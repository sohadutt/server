#include "socket.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "logger.h"

int server_socket_listen(const char *host, unsigned short port, int backlog) {
  char service[6];
  snprintf(service, sizeof(service), "%hu", port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = host == NULL ? AI_PASSIVE : 0;
  struct addrinfo *addresses = NULL;

  int status = getaddrinfo(host, service, &hints, &addresses);
  if (status != 0) {
    char message[256];
    snprintf(message, sizeof(message), "Cannot resolve %s:%s: %s",
             host == NULL ? "*" : host, service, gai_strerror(status));
    log_error(message);
    return -1;
  }

  int listener = -1;
  int last_error = EADDRNOTAVAIL;
  for (struct addrinfo *address = addresses; address != NULL;
       address = address->ai_next) {
    int sockfd =
        socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (sockfd < 0) {
      last_error = errno;
      continue;
    }

    int enabled = 1;
    (void)setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                     sizeof(enabled));
    if (bind(sockfd, address->ai_addr, address->ai_addrlen) == 0 &&
        listen(sockfd, backlog) == 0) {
      listener = sockfd;
      break;
    }

    last_error = errno;
    close(sockfd);
  }
  freeaddrinfo(addresses);

  if (listener < 0) {
    char message[256];
    snprintf(message, sizeof(message), "Cannot listen on %s:%s: %s",
             host == NULL ? "*" : host, service, strerror(last_error));
    log_error(message);
  }
  return listener;
}

int server_socket_accept(int listener, struct sockaddr_storage *peer_address,
                         socklen_t *peer_address_length) {
  if (peer_address == NULL || peer_address_length == NULL) {
    errno = EINVAL;
    return -1;
  }

  int client;
  do {
    *peer_address_length = sizeof(*peer_address);
    client =
        accept(listener, (struct sockaddr *)peer_address, peer_address_length);
  } while (client < 0 && errno == EINTR);
  return client;
}
