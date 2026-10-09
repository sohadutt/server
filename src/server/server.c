#include "server.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "logger.h"
#include "utils.h"

#define BACKLOG 128

static int open_listener(const ServerConfig *config) {
  struct addrinfo hints;
  struct addrinfo *addresses = NULL;
  char service[6];
  snprintf(service, sizeof(service), "%d", config->port);

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  int status = getaddrinfo(config->host, service, &hints, &addresses);
  if (status != 0) {
    char message[256];
    snprintf(message, sizeof(message), "Failed to resolve %s:%s: %s",
             config->host, service, gai_strerror(status));
    log_error(message);
    return -1;
  }

  int listener = -1;
  for (struct addrinfo *address = addresses; address != NULL;
       address = address->ai_next) {
    listener =
        socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (listener < 0)
      continue;

    int enabled = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
                     sizeof(enabled));
    if (bind(listener, address->ai_addr, address->ai_addrlen) == 0 &&
        listen(listener, BACKLOG) == 0) {
      break;
    }
    close(listener);
    listener = -1;
  }

  freeaddrinfo(addresses);
  if (listener < 0) {
    char message[256];
    snprintf(message, sizeof(message), "Failed to listen on %s:%s: %s",
             config->host, service, strerror(errno));
    log_error(message);
  }
  return listener;
}

static void log_client_address(const struct sockaddr *address,
                              socklen_t address_length) {
  char host[NI_MAXHOST];
  char service[NI_MAXSERV];
  if (getnameinfo(address, address_length, host, sizeof(host), service,
                  sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
    char message[256];
    snprintf(message, sizeof(message), "Connection from %s:%s", host, service);
    log_info(message);
  } else {
    log_info("Connection from unknown source");
  }
}

static ssize_t receive_request(int client, char *buffer, size_t capacity) {
  ssize_t received;
  do {
    received = recv(client, buffer, capacity - 1, 0);
  } while (received < 0 && errno == EINTR);

  if (received >= 0) buffer[received] = '\0';
  return received;
}

static size_t send_response(int client) {
  static const char response[] = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/plain\r\n"
                                 "Content-Length: 13\r\n"
                                 "Connection: close\r\n"
                                 "\r\n"
                                 "Hello, World!";
  size_t total_sent = 0;
  while (total_sent < sizeof(response) - 1) {
    ssize_t sent = send(client, response + total_sent,
                        sizeof(response) - 1 - total_sent, 0);
    if (sent < 0 && errno == EINTR) continue;
    if (sent <= 0) break;
    total_sent += (size_t)sent;
  }
  return total_sent;
}

static void log_client_message(const char *request, size_t length) {
  if (length == 0) {
    log_debug("Client message: <empty>");
    return;
  }

  char message[1100];
  int prefix_length = snprintf(message, sizeof(message), "Client message:\n");
  if (prefix_length < 0 || (size_t)prefix_length >= sizeof(message)) return;
  size_t remaining = sizeof(message) - (size_t)prefix_length - 1;
  if (length > remaining) length = remaining;
  memcpy(message + prefix_length, request, length);
  message[prefix_length + length] = '\0';
  log_debug(message);
}

static void handle_client(int client, const ServerConfig *config) {
  ResourceUsageSnapshot request_usage;
  int monitor_request = config->log_level == LOG_DEBUG &&
                        resource_usage_capture(&request_usage) == 0;

  char request[1024] = {0};
  ssize_t received = receive_request(client, request, sizeof(request));
  if (received < 0) {
    log_warn("Failed to read request from client");
  } else {
    log_client_message(request, (size_t)received);
  }

  size_t sent = send_response(client);
  if (monitor_request) {
    resource_usage_log_request(&request_usage, request,
                               received > 0 ? (size_t)received : 0, sent);
  }
  close(client);
}

int server_start(const ServerConfig *config) {
  if (config == NULL) {
    log_error("Cannot start server without configuration");
    return -1;
  }

  int listener = open_listener(config);
  if (listener < 0)
    return -1;

  char message[160];
  snprintf(message, sizeof(message), "Server listening on %s:%d", config->host,
           config->port);
  log_info(message);

  for (;;) {
    struct sockaddr_storage client_address;
    socklen_t address_length = sizeof(client_address);
    int client =
        accept(listener, (struct sockaddr *)&client_address, &address_length);
    if (client < 0) {
      if (errno == EINTR)
        continue;
      log_error("Failed to accept connection");
      continue;
    }

    log_client_address((struct sockaddr *)&client_address, address_length);
    handle_client(client, config);
  }
}
