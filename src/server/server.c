#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "connection.h"
#include "logger.h"
#include "socket.h"

#define SERVER_BACKLOG 128

int server_start(const ServerConfig *config) {
  if (config == NULL || config->port < 1 || config->port > 65535 ||
      config->host[0] == '\0') {
    log_error("Cannot start server with invalid configuration");
    return -1;
  }

  int listener = server_socket_listen(
      config->host, (unsigned short)config->port, SERVER_BACKLOG);
  if (listener < 0)
    return -1;

  char message[160];
  snprintf(message, sizeof(message), "Server listening on %s:%d", config->host,
           config->port);
  log_info(message);

  for (;;) {
    struct sockaddr_storage peer_address;
    socklen_t peer_address_length;
    int client =
        server_socket_accept(listener, &peer_address, &peer_address_length);
    if (client < 0) {
      int accept_error = errno;
      log_error("Failed to accept connection");
      if (accept_error == EBADF || accept_error == EINVAL) {
        close(listener);
        return -1;
      }
      continue;
    }

    connection_handle(client, (struct sockaddr *)&peer_address,
                      peer_address_length, config);
  }
}
