#include "socket.h"

#include <errno.h>
#include <fcntl.h>
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
    log_errorf("Cannot resolve listen address %s:%s: %s",
               host == NULL ? "*" : host, service, gai_strerror(status));
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
      int flags = fcntl(sockfd, F_GETFL, 0);
      if (flags < 0 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        last_error = errno;
        close(sockfd);
        continue;
      }
      listener = sockfd;
      break;
    }

    last_error = errno;
    close(sockfd);
  }
  freeaddrinfo(addresses);

  if (listener < 0) {
    log_errorf("Cannot listen on %s:%s: %s", host == NULL ? "*" : host,
               service, strerror(last_error));
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
  if (client >= 0) {
    int flags = fcntl(client, F_GETFL, 0);
    if (flags < 0 || fcntl(client, F_SETFL, flags & ~O_NONBLOCK) < 0) {
      int saved_error = errno;
      close(client);
      errno = saved_error;
      return -1;
    }
  }
  return client;
}
