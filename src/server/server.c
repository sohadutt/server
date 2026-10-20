#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <cups/cups.h>

#include "connection.h"
#include "logger.h"
#include "socket.h"
#include "thread_pool.h"

#define SERVER_BACKLOG 128
#define CONNECTION_QUEUE_PER_WORKER 8U

typedef struct {
  int client;
  struct sockaddr_storage peer_address;
  socklen_t peer_address_length;
  const ServerConfig *config;
  const StaticFileServer *static_files;
} ConnectionTask;

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
                    task->static_files);
  free(task);
}

int server_start(const ServerConfig *config) {
  if (config == NULL || config->port < 1 || config->port > 65535 ||
      config->host[0] == '\0' || config->worker_threads < 1 ||
      config->worker_threads > 64) {
    log_error("Cannot start server with invalid configuration");
    return -1;
  }

  StaticFileServer static_files;
  if (static_file_server_init(&static_files, config) != 0)
    return -1;

  int listener = server_socket_listen(
      config->host, (unsigned short)config->port, SERVER_BACKLOG);
  if (listener < 0) {
    static_file_server_destroy(&static_files);
    return -1;
  }

  size_t worker_count = (size_t)config->worker_threads;
  ThreadPool *workers = thread_pool_create(
      worker_count, worker_count * CONNECTION_QUEUE_PER_WORKER);
  if (workers == NULL) {
    log_error("Could not start the connection worker pool");
    close(listener);
    static_file_server_destroy(&static_files);
    return -1;
  }

  char message[160];
  snprintf(message, sizeof(message), "Server listening on %s:%d", config->host,
           config->port);
  log_info(message);

  for (;;) {
    struct sockaddr_storage peer_address = {0};
    socklen_t peer_address_length = sizeof(peer_address);
    int client = server_socket_accept(listener, &peer_address,
                                      &peer_address_length);
    if (client < 0) {
      int accept_error = errno;
      if (accept_error == EINTR) {
        continue;
      }
      log_error("Failed to accept connection");
      if (accept_error == EBADF || accept_error == EINVAL) {
        close(listener);
        thread_pool_destroy(workers);
        static_file_server_destroy(&static_files);
        return -1;
      }
      continue;
    }

    ConnectionTask *task = malloc(sizeof(*task));
    if (task == NULL) {
      close(client);
      continue;
    }
    *task = (ConnectionTask){.client = client,
                             .peer_address = peer_address,
                             .peer_address_length = peer_address_length,
                             .config = config,
                             .static_files = &static_files};
    if (thread_pool_submit(workers, handle_connection_task, task) != 0) {
      close(client);
      free(task);
      log_debug("Connection queue full; closed incoming connection");
    }
  }
}
