#include "logger.h"
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BACKLOG 10

int server_start(void) {
  struct addrinfo hints, *res;
  char err_buf[256];
  memset(&hints, 0, sizeof hints);

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  int status = getaddrinfo(NULL, "3490", &hints, &res);
  if (status != 0) {
    snprintf(err_buf, sizeof(err_buf), "Failed to get address info: %s", gai_strerror(status));
    log_error(err_buf);
    exit(-1);
  }

  int socketfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (socketfd < 0) {
    snprintf(err_buf, sizeof(err_buf), "Failed to create socket: %s", strerror(errno));
    log_error(err_buf);
    freeaddrinfo(res);
    exit(-1);
  }

  int opt = 1;
  setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  if (bind(socketfd, res->ai_addr, res->ai_addrlen) < 0) {
    snprintf(err_buf, sizeof(err_buf), "Failed to bind socket: %s", strerror(errno));
    log_error(err_buf);
    freeaddrinfo(res);
    close(socketfd);
    exit(-1);
  }

  if (listen(socketfd, BACKLOG) < 0) {
    snprintf(err_buf, sizeof(err_buf), "Failed to listen on socket: %s", strerror(errno));
    log_error(err_buf);
    freeaddrinfo(res);
    close(socketfd);
    exit(-1);
  }

  freeaddrinfo(res);
  log_info("Server bound and listening on port 3490");

  while (1) {
      struct sockaddr_storage client_addr;
      socklen_t addr_size = sizeof(client_addr);

      int client_fd = accept(socketfd, (struct sockaddr *)&client_addr, &addr_size);
      if (client_fd < 0) {
        log_error("Failed to accept connection");
        continue;
      }

      printf("Client connected!\n");

      const char *http_response =
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: text/plain\r\n"
          "Content-Length: 13\r\n"
          "Connection: close\r\n"
          "\r\n"
          "Hello, World!";

      send(client_fd, http_response, strlen(http_response), 0);
      close(client_fd);
    }

    close(socketfd);

  return 0;
}
