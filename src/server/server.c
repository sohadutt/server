#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cups/cups.h>

#include "connection.h"
#include "data.h"
#include "event_loop.h"
#include "logger.h"
#include "router.h"
#include "socket.h"
#include "thread_pool.h"
#include "tls.h"

#define SERVER_BACKLOG 256
#define CONNECTION_QUEUE_PER_WORKER 64U
#define KEEP_ALIVE_IDLE_SECONDS 15
#define METRICS_LOG_INTERVAL_SECONDS 5

static volatile sig_atomic_t shutdown_requested = 0;
static atomic_uint_fast64_t next_request_id = 1;

static void request_shutdown(int signal_number) {
  (void)signal_number;
  shutdown_requested = 1;
}

static int set_nonblocking_cloexec(int descriptor) {
  int status = fcntl(descriptor, F_GETFL, 0);
  int descriptor_flags = fcntl(descriptor, F_GETFD, 0);
  return status >= 0 && descriptor_flags >= 0 &&
                 fcntl(descriptor, F_SETFL, status | O_NONBLOCK) == 0 &&
                 fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0
             ? 0
             : -1;
}

typedef struct ConnectionJob {
  Connection *connection;
  struct AcceptContext *context;
  int keep_alive;
  uint64_t queued_at_ns;
  struct ConnectionJob *next;
} ConnectionJob;

typedef struct IdleConnection {
  Connection *connection;
  struct AcceptContext *context;
  time_t idle_since;
  struct IdleConnection *next;
} IdleConnection;

typedef struct AcceptContext {
  const ServerConfig *config;
  const StaticFileServer *static_files;
  DataStore *data_store;
  TlsServer *tls_server;
  ThreadPool *workers;
  const HttpRouter *router;
  EventLoop *loop;
  pthread_mutex_t completion_mutex;
  ConnectionJob *completion_head;
  ConnectionJob *completion_tail;
  int wake_pipe[2];
  IdleConnection *idle_connections;
  atomic_size_t active_connections;
  atomic_size_t idle_connections_count;
  atomic_size_t busy_workers;
  atomic_uint_fast64_t accepted_connections;
  atomic_uint_fast64_t worker_tasks_completed;
  atomic_uint_fast64_t rejected_connections;
  atomic_uint_fast64_t metric_tasks;
  atomic_uint_fast64_t metric_queue_wait_ns;
  atomic_uint_fast64_t metric_worker_wall_ns;
  atomic_uint_fast64_t metric_tls_handshake_ns;
  atomic_uint_fast64_t metric_request_read_ns;
  atomic_uint_fast64_t metric_socket_write_ns;
  atomic_uint_fast64_t metric_socket_writable_wait_ns;
  struct rusage previous_usage;
  uint64_t previous_cpu_sample_ns;
  time_t next_metrics_log;
  int fatal_error;
} AcceptContext;

static uint64_t monotonic_nanoseconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
         (uint64_t)now.tv_nsec;
}

static uint64_t timeval_nanoseconds(struct timeval value) {
  return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
         (uint64_t)value.tv_usec * UINT64_C(1000);
}

static uint64_t usage_cpu_nanoseconds(const struct rusage *usage) {
  return timeval_nanoseconds(usage->ru_utime) +
         timeval_nanoseconds(usage->ru_stime);
}

static void queue_completion(AcceptContext *context, ConnectionJob *job) {
  pthread_mutex_lock(&context->completion_mutex);
  if (context->completion_tail != NULL)
    context->completion_tail->next = job;
  else
    context->completion_head = job;
  context->completion_tail = job;
  pthread_mutex_unlock(&context->completion_mutex);
  const unsigned char wake = 1;
  ssize_t ignored;
  do {
    ignored = write(context->wake_pipe[1], &wake, sizeof(wake));
  } while (ignored < 0 && errno == EINTR);
  (void)ignored;
}

static ConnectionJob *take_completion(AcceptContext *context) {
  pthread_mutex_lock(&context->completion_mutex);
  ConnectionJob *job = context->completion_head;
  if (job != NULL) {
    context->completion_head = job->next;
    if (context->completion_head == NULL)
      context->completion_tail = NULL;
    job->next = NULL;
  }
  pthread_mutex_unlock(&context->completion_mutex);
  return job;
}

static void process_connection_job(void *argument) {
  ConnectionJob *job = argument;
  if (job == NULL || job->connection == NULL)
    return;
  AcceptContext *context = job->context;
  if (context == NULL)
    return;
  cupsSetServer(context->config->cups_server[0] != '\0'
                    ? context->config->cups_server
                    : NULL);
  cupsSetEncryption(context->config->cups_encryption_required
                        ? HTTP_ENCRYPTION_REQUIRED
                        : HTTP_ENCRYPTION_IF_REQUESTED);
  atomic_fetch_add_explicit(&context->busy_workers, 1, memory_order_relaxed);
  uint64_t started_ns = monotonic_nanoseconds();
  if (started_ns >= job->queued_at_ns)
    atomic_fetch_add_explicit(&context->metric_queue_wait_ns,
                              started_ns - job->queued_at_ns,
                              memory_order_relaxed);
  ConnectionTimings timings = {0};
  job->keep_alive = connection_process_one(job->connection, &timings);
  uint64_t finished_ns = monotonic_nanoseconds();
  if (finished_ns >= started_ns)
    atomic_fetch_add_explicit(&context->metric_worker_wall_ns,
                              finished_ns - started_ns,
                              memory_order_relaxed);
  atomic_fetch_add_explicit(&context->metric_tls_handshake_ns,
                            timings.tls_handshake_ns, memory_order_relaxed);
  atomic_fetch_add_explicit(&context->metric_request_read_ns,
                            timings.request_read_ns, memory_order_relaxed);
  atomic_fetch_add_explicit(&context->metric_socket_write_ns,
                            timings.socket_write_ns, memory_order_relaxed);
  atomic_fetch_add_explicit(&context->metric_socket_writable_wait_ns,
                            timings.socket_writable_wait_ns,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&context->metric_tasks, 1, memory_order_relaxed);
  atomic_fetch_sub_explicit(&context->busy_workers, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&context->worker_tasks_completed, 1,
                            memory_order_relaxed);
  logger_set_request_id(0);
  queue_completion(context, job);
}

static void idle_remove(AcceptContext *context, IdleConnection *idle) {
  IdleConnection **cursor = &context->idle_connections;
  while (*cursor != NULL && *cursor != idle)
    cursor = &(*cursor)->next;
  if (*cursor == idle) {
    *cursor = idle->next;
    atomic_fetch_sub_explicit(&context->idle_connections_count, 1,
                              memory_order_relaxed);
  }
}

static void connection_ready(int descriptor, short events, void *argument) {
  (void)descriptor;
  IdleConnection *idle = argument;
  if (idle == NULL)
    return;
  AcceptContext *context = idle->context;
  (void)event_loop_remove(context->loop,
                          connection_file_descriptor(idle->connection));
  idle_remove(context, idle);
  ConnectionJob *job = calloc(1, sizeof(*job));
  if ((events & POLLIN) == 0 || job == NULL) {
    connection_destroy(idle->connection);
    atomic_fetch_sub_explicit(&context->active_connections, 1,
                              memory_order_relaxed);
    free(job);
    free(idle);
    return;
  }
  job->connection = idle->connection;
  job->context = context;
  job->queued_at_ns = monotonic_nanoseconds();
  free(idle);
  if (thread_pool_submit(context->workers, process_connection_job, job) != 0) {
    connection_destroy(job->connection);
    atomic_fetch_sub_explicit(&context->active_connections, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&context->rejected_connections, 1,
                              memory_order_relaxed);
    free(job);
  }
}

static void completions_ready(int descriptor, short events, void *argument) {
  (void)events;
  AcceptContext *context = argument;
  unsigned char buffer[256];
  while (read(descriptor, buffer, sizeof(buffer)) > 0) {
  }
  ConnectionJob *job;
  while ((job = take_completion(context)) != NULL) {
    int fd = connection_file_descriptor(job->connection);
    if (job->keep_alive && !shutdown_requested) {
      IdleConnection *idle = calloc(1, sizeof(*idle));
      if (idle != NULL) {
        idle->connection = job->connection;
        idle->idle_since = time(NULL);
        idle->context = context;
        if (event_loop_add(context->loop, fd, POLLIN, connection_ready, idle) ==
            0) {
          idle->next = context->idle_connections;
          context->idle_connections = idle;
          atomic_fetch_add_explicit(&context->idle_connections_count, 1,
                                    memory_order_relaxed);
          free(job);
          continue;
        }
        free(idle);
      }
    }
    connection_destroy(job->connection);
    atomic_fetch_sub_explicit(&context->active_connections, 1,
                              memory_order_relaxed);
    free(job);
  }
}

static void reap_idle_connections(AcceptContext *context, time_t now) {
  IdleConnection *idle = context->idle_connections;
  while (idle != NULL) {
    IdleConnection *next = idle->next;
    if (now - idle->idle_since >= KEEP_ALIVE_IDLE_SECONDS) {
      (void)event_loop_remove(context->loop,
                              connection_file_descriptor(idle->connection));
      idle_remove(context, idle);
      connection_destroy(idle->connection);
      atomic_fetch_sub_explicit(&context->active_connections, 1,
                                memory_order_relaxed);
      free(idle);
    }
    idle = next;
  }
}

static void log_server_metrics(AcceptContext *context, time_t now) {
  if (now < context->next_metrics_log)
    return;
  context->next_metrics_log = now + METRICS_LOG_INTERVAL_SECONDS;
  uint64_t sample_ns = monotonic_nanoseconds();
  uint64_t elapsed_ns = sample_ns >= context->previous_cpu_sample_ns
                            ? sample_ns - context->previous_cpu_sample_ns
                            : 0;
  context->previous_cpu_sample_ns = sample_ns;
  struct rusage current_usage = {0};
  uint64_t cpu_ns = 0;
  if (getrusage(RUSAGE_SELF, &current_usage) == 0) {
    uint64_t current_cpu_ns = usage_cpu_nanoseconds(&current_usage);
    uint64_t previous_cpu_ns = usage_cpu_nanoseconds(&context->previous_usage);
    if (current_cpu_ns >= previous_cpu_ns)
      cpu_ns = current_cpu_ns - previous_cpu_ns;
    context->previous_usage = current_usage;
  }
  uint64_t tasks = atomic_exchange_explicit(&context->metric_tasks, 0,
                                             memory_order_relaxed);
  uint64_t queue_ns = atomic_exchange_explicit(&context->metric_queue_wait_ns,
                                                0, memory_order_relaxed);
  uint64_t worker_ns = atomic_exchange_explicit(&context->metric_worker_wall_ns,
                                                 0, memory_order_relaxed);
  uint64_t handshake_ns = atomic_exchange_explicit(
      &context->metric_tls_handshake_ns, 0, memory_order_relaxed);
  uint64_t read_ns = atomic_exchange_explicit(&context->metric_request_read_ns,
                                               0, memory_order_relaxed);
  uint64_t write_ns = atomic_exchange_explicit(&context->metric_socket_write_ns,
                                                0, memory_order_relaxed);
  uint64_t writable_ns = atomic_exchange_explicit(
      &context->metric_socket_writable_wait_ns, 0, memory_order_relaxed);
  double cpu_percent = elapsed_ns > 0
                           ? (double)cpu_ns * 100.0 / (double)elapsed_ns
                           : 0.0;
  double divisor = tasks > 0 ? (double)tasks * 1000000.0 : 0.0;
  log_infof("Runtime metrics: active_connections=%zu idle_keep_alive=%zu "
            "busy_workers=%zu accepted=%llu worker_tasks_completed=%llu "
            "rejected_connections=%llu interval_tasks=%llu "
            "process_cpu=%.1f%% queue_avg=%.3fms worker_avg=%.3fms "
            "tls_avg=%.3fms request_read_avg=%.3fms "
            "socket_write_avg=%.3fms writable_wait_avg=%.3fms",
            atomic_load_explicit(&context->active_connections,
                                 memory_order_relaxed),
            atomic_load_explicit(&context->idle_connections_count,
                                 memory_order_relaxed),
            atomic_load_explicit(&context->busy_workers,
                                 memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(
                &context->accepted_connections, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(
                &context->worker_tasks_completed, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(
                &context->rejected_connections, memory_order_relaxed),
            (unsigned long long)tasks, cpu_percent,
            divisor > 0 ? (double)queue_ns / divisor : 0.0,
            divisor > 0 ? (double)worker_ns / divisor : 0.0,
            divisor > 0 ? (double)handshake_ns / divisor : 0.0,
            divisor > 0 ? (double)read_ns / divisor : 0.0,
            divisor > 0 ? (double)write_ns / divisor : 0.0,
            divisor > 0 ? (double)writable_ns / divisor : 0.0);
}

static void accept_ready(int listener, short events, void *argument) {
  AcceptContext *context = argument;
  if (context == NULL || (events & POLLIN) == 0) {
    log_error("Server listener reported an unexpected event");
    if (context != NULL)
      context->fatal_error = 1;
    return;
  }

  for (;;) {
    struct sockaddr_storage peer_address = {0};
    socklen_t peer_address_length = sizeof(peer_address);
    int client = server_socket_accept(listener, &peer_address,
                                      &peer_address_length);
    if (client < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;

      int accept_error = errno;
      log_warnf("Accept failed: %s", strerror(accept_error));
      /* Resource pressure is transient; only a broken listener is fatal. */
      if (accept_error == EBADF || accept_error == EINVAL ||
          accept_error == ENOTSOCK)
        context->fatal_error = 1;
      return;
    }

    Connection *connection = connection_create(
        client, (const struct sockaddr *)&peer_address, peer_address_length,
        atomic_fetch_add_explicit(&next_request_id, 1, memory_order_relaxed),
        context->config, context->static_files, context->data_store,
        context->tls_server, context->router);
    ConnectionJob *job = calloc(1, sizeof(*job));
    if (connection == NULL || job == NULL) {
      connection_destroy(connection);
      free(job);
      log_warn("Could not allocate a connection task; connection closed");
      continue;
    }
    job->connection = connection;
    job->context = context;
    job->queued_at_ns = monotonic_nanoseconds();
    atomic_fetch_add_explicit(&context->accepted_connections, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&context->active_connections, 1,
                              memory_order_relaxed);
    if (thread_pool_submit(context->workers, process_connection_job, job) != 0) {
      connection_destroy(connection);
      atomic_fetch_sub_explicit(&context->active_connections, 1,
                                memory_order_relaxed);
      atomic_fetch_add_explicit(&context->rejected_connections, 1,
                                memory_order_relaxed);
      free(job);
      log_debug("Connection queue full; closed incoming connection");
    }
  }
}

int server_start(const ServerConfig *config) {
  if (config == NULL || config->port < 1 || config->port > 65535 ||
      config->host[0] == '\0' || config->worker_threads < 1 ||
      config->worker_threads > 64 ||
      (config->tls_enabled &&
       (config->tls_certificate_file[0] == '\0' ||
        config->tls_private_key_file[0] == '\0'))) {
    log_error("Cannot start server with invalid configuration");
    return -1;
  }

  struct sigaction ignore_sigpipe = {.sa_handler = SIG_IGN};
  sigemptyset(&ignore_sigpipe.sa_mask);
  if (sigaction(SIGPIPE, &ignore_sigpipe, NULL) != 0) {
    log_error("Could not ignore SIGPIPE for closed client connections");
    return -1;
  }

  struct sigaction stop_action = {.sa_handler = request_shutdown};
  sigemptyset(&stop_action.sa_mask);
  stop_action.sa_flags = 0;
  if (sigaction(SIGINT, &stop_action, NULL) != 0 ||
      sigaction(SIGTERM, &stop_action, NULL) != 0) {
    log_error("Could not install graceful shutdown handlers");
    return -1;
  }
  shutdown_requested = 0;

  TlsServer *tls_server = NULL;
  if (config->tls_enabled) {
    tls_server = tls_server_create(config->tls_certificate_file,
                                   config->tls_private_key_file);
    if (tls_server == NULL) {
      log_error("Could not initialize TLS; check certificate and key files");
      return -1;
    }
  }

  StaticFileServer static_files;
  if (static_file_server_init(&static_files, config) != 0) {
    tls_server_destroy(tls_server);
    return -1;
  }

  DataStore *data_store = data_store_open(config->database_path);
  if (data_store == NULL) {
    log_errorf("Could not initialize user database '%s'",
               config->database_path);
    static_file_server_destroy(&static_files);
    tls_server_destroy(tls_server);
    return -1;
  }

  int listener = server_socket_listen(
      config->host, (unsigned short)config->port, SERVER_BACKLOG);
  if (listener < 0) {
    data_store_close(data_store);
    static_file_server_destroy(&static_files);
    tls_server_destroy(tls_server);
    return -1;
  }

  size_t worker_count = (size_t)config->worker_threads;
  ThreadPool *workers = thread_pool_create(
      worker_count, worker_count * CONNECTION_QUEUE_PER_WORKER);
  if (workers == NULL) {
    log_error("Could not start the connection worker pool");
    close(listener);
    data_store_close(data_store);
    static_file_server_destroy(&static_files);
    tls_server_destroy(tls_server);
    return -1;
  }

  HttpRouter router;
  http_router_init(&router);
  uint32_t api_methods = HTTP_ROUTE_ALL_METHODS;
  if (http_router_add(&router, "/healthz", HTTP_ROUTE_EXACT, HTTP_ROUTE_GET,
                      HTTP_ROUTE_API) != 0 ||
      http_router_add(&router, "/readyz", HTTP_ROUTE_EXACT, HTTP_ROUTE_GET,
                      HTTP_ROUTE_API) != 0 ||
      http_router_add(&router, "/api", HTTP_ROUTE_EXACT, api_methods,
                      HTTP_ROUTE_API) != 0 ||
      http_router_add(&router, "/api/", HTTP_ROUTE_PREFIX, api_methods,
                      HTTP_ROUTE_API) != 0 ||
      http_router_add(&router, "/", HTTP_ROUTE_FALLBACK, HTTP_ROUTE_GET,
                      HTTP_ROUTE_STATIC) != 0) {
    log_error("Could not configure the HTTP request router");
    close(listener);
    thread_pool_destroy(workers);
    data_store_close(data_store);
    static_file_server_destroy(&static_files);
    tls_server_destroy(tls_server);
    return -1;
  }

  AcceptContext accept_context = {
      .config = config,
      .static_files = &static_files,
      .data_store = data_store,
      .tls_server = tls_server,
      .workers = workers,
      .router = &router,
      .wake_pipe = {-1, -1},
  };
  int mutex_initialized =
      pthread_mutex_init(&accept_context.completion_mutex, NULL) == 0;
  EventLoop *loop = event_loop_create();
  if (!mutex_initialized || pipe(accept_context.wake_pipe) != 0 ||
      set_nonblocking_cloexec(accept_context.wake_pipe[0]) != 0 ||
      set_nonblocking_cloexec(accept_context.wake_pipe[1]) != 0 ||
      loop == NULL) {
    log_error("Could not initialize the connection completion queue");
    event_loop_destroy(loop);
    if (accept_context.wake_pipe[0] >= 0)
      close(accept_context.wake_pipe[0]);
    if (accept_context.wake_pipe[1] >= 0)
      close(accept_context.wake_pipe[1]);
    if (mutex_initialized)
      pthread_mutex_destroy(&accept_context.completion_mutex);
    close(listener);
    thread_pool_destroy(workers);
    data_store_close(data_store);
    static_file_server_destroy(&static_files);
    tls_server_destroy(tls_server);
    return -1;
  }
  accept_context.loop = loop;
  if (event_loop_add(loop, listener, POLLIN, accept_ready, &accept_context) !=
          0 ||
      event_loop_add(loop, accept_context.wake_pipe[0], POLLIN,
                     completions_ready, &accept_context) != 0) {
    log_error("Could not register server descriptors with the event loop");
    event_loop_destroy(loop);
    close(accept_context.wake_pipe[0]);
    close(accept_context.wake_pipe[1]);
    pthread_mutex_destroy(&accept_context.completion_mutex);
    close(listener);
    thread_pool_destroy(workers);
    data_store_close(data_store);
    static_file_server_destroy(&static_files);
    tls_server_destroy(tls_server);
    return -1;
  }

  log_infof("Listening on %s://%s:%d with %zu worker(s) and queue capacity %zu",
            config->tls_enabled ? "https" : "http", config->host,
            config->port, worker_count,
            worker_count * CONNECTION_QUEUE_PER_WORKER);

  int loop_result = 0;
  accept_context.next_metrics_log = time(NULL) + METRICS_LOG_INTERVAL_SECONDS;
  (void)getrusage(RUSAGE_SELF, &accept_context.previous_usage);
  accept_context.previous_cpu_sample_ns = monotonic_nanoseconds();
  while (!accept_context.fatal_error && !shutdown_requested) {
    loop_result = event_loop_run_once(loop, 250);
    if (loop_result < 0) {
      log_error("Listener event loop stopped unexpectedly");
      accept_context.fatal_error = 1;
    }
    time_t now = time(NULL);
    reap_idle_connections(&accept_context, now);
    log_server_metrics(&accept_context, now);
  }
  if (shutdown_requested)
    log_info("Shutdown requested; draining active connections");
  close(listener);
  while (accept_context.idle_connections != NULL) {
    IdleConnection *idle = accept_context.idle_connections;
    accept_context.idle_connections = idle->next;
    (void)event_loop_remove(loop,
                            connection_file_descriptor(idle->connection));
    connection_destroy(idle->connection);
    atomic_fetch_sub_explicit(&accept_context.active_connections, 1,
                              memory_order_relaxed);
    free(idle);
  }
  thread_pool_destroy(workers);
  ConnectionJob *finished;
  while ((finished = take_completion(&accept_context)) != NULL) {
    connection_destroy(finished->connection);
    atomic_fetch_sub_explicit(&accept_context.active_connections, 1,
                              memory_order_relaxed);
    free(finished);
  }
  event_loop_destroy(loop);
  close(accept_context.wake_pipe[0]);
  close(accept_context.wake_pipe[1]);
  pthread_mutex_destroy(&accept_context.completion_mutex);
  data_store_close(data_store);
  static_file_server_destroy(&static_files);
  tls_server_destroy(tls_server);
  return accept_context.fatal_error ? -1 : 0;
}
