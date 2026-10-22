#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <poll.h>
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

#define SERVER_BACKLOG 128
#define CONNECTION_QUEUE_PER_WORKER 8U

static volatile sig_atomic_t shutdown_requested = 0;

static void request_shutdown(int signal_number) {
  (void)signal_number;
  shutdown_requested = 1;
}

typedef struct {
  int client;
  struct sockaddr_storage peer_address;
  socklen_t peer_address_length;
  const ServerConfig *config;
  const StaticFileServer *static_files;
  DataStore *data_store;
  TlsServer *tls_server;
  const HttpRouter *router;
} ConnectionTask;

typedef struct {
  const ServerConfig *config;
  const StaticFileServer *static_files;
  DataStore *data_store;
  TlsServer *tls_server;
  ThreadPool *workers;
  const HttpRouter *router;
  int fatal_error;
} AcceptContext;

static void handle_connection_task(void *argument) {
  ConnectionTask *task = argument;
  if (task == NULL) {
    return;
  }

  cupsSetServer(task->config->cups_server[0] != '\0'
                    ? task->config->cups_server
                    : NULL);
  cupsSetEncryption(task->config->cups_encryption_required
                        ? HTTP_ENCRYPTION_REQUIRED
                        : HTTP_ENCRYPTION_IF_REQUESTED);

  connection_handle(task->client, (const struct sockaddr *)&task->peer_address,
                    task->peer_address_length, task->config,
                    task->static_files, task->data_store, task->tls_server,
                    task->router);
  free(task);
}

static void accept_ready(int listener, short events, void *argument) {
  AcceptContext *context = argument;
  if (context == NULL || (events & POLLIN) == 0) {
    log_error("Server listener reported an unexpected event");
    if (context != NULL)
      context->fatal_error = 1;
    return;
  }

  struct sockaddr_storage peer_address = {0};
  socklen_t peer_address_length = sizeof(peer_address);
  int client = server_socket_accept(listener, &peer_address,
                                    &peer_address_length);
  if (client < 0) {
    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
      log_error("Failed to accept connection");
      context->fatal_error = 1;
    }
    return;
  }

  ConnectionTask *task = malloc(sizeof(*task));
  if (task == NULL) {
    close(client);
    return;
  }
  *task = (ConnectionTask){
      .client = client,
      .peer_address = peer_address,
      .peer_address_length = peer_address_length,
      .config = context->config,
      .static_files = context->static_files,
      .data_store = context->data_store,
      .tls_server = context->tls_server,
      .router = context->router,
  };
  if (thread_pool_submit(context->workers, handle_connection_task, task) != 0) {
    close(client);
    free(task);
    log_debug("Connection queue full; closed incoming connection");
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
    log_error("Could not open the configured user database");
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
  if (http_router_add(&router, "/api", HTTP_ROUTE_EXACT, api_methods,
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
  };
  EventLoop *loop = event_loop_create();
  if (loop == NULL ||
      event_loop_add(loop, listener, POLLIN, accept_ready, &accept_context) !=
          0) {
    log_error("Could not register the listener with the event loop");
    event_loop_destroy(loop);
    close(listener);
    thread_pool_destroy(workers);
    data_store_close(data_store);
    static_file_server_destroy(&static_files);
    tls_server_destroy(tls_server);
    return -1;
  }

  char message[160];
  snprintf(message, sizeof(message), "Server listening on %s://%s:%d",
           config->tls_enabled ? "https" : "http", config->host,
           config->port);
  log_info(message);

  int loop_result = 0;
  while (!accept_context.fatal_error && !shutdown_requested) {
    loop_result = event_loop_run_once(loop, 1000);
    if (loop_result < 0) {
      log_error("Listener event loop stopped unexpectedly");
      accept_context.fatal_error = 1;
    }
  }
  if (shutdown_requested)
    log_info("Shutdown requested; draining active connections");
  event_loop_destroy(loop);
  close(listener);
  thread_pool_destroy(workers);
  data_store_close(data_store);
  static_file_server_destroy(&static_files);
  tls_server_destroy(tls_server);
  return accept_context.fatal_error ? -1 : 0;
}
