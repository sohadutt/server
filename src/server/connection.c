#include "connection.h"

#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "http.h"
#include "logger.h"
#include "print_api.h"
#include "request.h"
#include "resource_monitor.h"
#include "response.h"
#include "static.h"

static void log_peer(const struct sockaddr *address, socklen_t address_length) {
  char host[128];
  char service[16];
  if (address != NULL &&
      getnameinfo(address, address_length, host, sizeof(host), service,
                  sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
    char message[256];
    snprintf(message, sizeof(message), "Connection from %s:%s", host, service);
    log_info(message);
    return;
  }
  log_info("Connection from unknown source");
}

void connection_handle(int client, const struct sockaddr *peer_address,
                       socklen_t peer_address_length,
                       const ServerConfig *config,
                       const StaticFileServer *static_files) {
  if (client < 0)
    return;
  if (config == NULL || static_files == NULL) {
    close(client);
    return;
  }

  const struct timeval receive_timeout = {.tv_sec = 10, .tv_usec = 0};
  const struct timeval send_timeout = {.tv_sec = 15, .tv_usec = 0};
  if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                 sizeof(receive_timeout)) != 0 ||
      setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                 sizeof(send_timeout)) != 0) {
    log_warn("Could not set client socket timeouts");
    close(client);
    return;
  }

  log_peer(peer_address, peer_address_length);

  ResourceUsageSnapshot request_start;
  int monitor_request = config->log_level == LOG_DEBUG &&
                        resource_monitor_capture(&request_start) == 0;

  HttpRequest request = {0};
  int read_result = http_request_receive(client, &request);
  if (read_result < 0) {
    log_warn("Failed to read request from client");
  } else {
    http_request_log_debug(&request);
  }

  HttpRequestLine request_line = {0};
  int request_line_valid =
      read_result > 0 && http_request_line_parse(&request, &request_line) == 0;
  if (request_line_valid && strcmp(request_line.target, "/api/print") == 0) {
    size_t bytes_sent = 0;
    size_t api_bytes_received = request.length;
    if (strcmp(request_line.method, "POST") == 0) {
      bytes_sent = print_api_handle(client, &request, config,
                                    &api_bytes_received);
    } else {
      static const char body[] = "{\"error\":\"Use POST to submit a print job\"}";
      bytes_sent = http_response_send(client, 405, "Method Not Allowed",
                                      "application/json; charset=utf-8", body,
                                      sizeof(body) - 1);
    }
    if (monitor_request) {
      resource_monitor_log_request(&request_start, request.data,
                                   api_bytes_received, bytes_sent);
    }
    close(client);
    return;
  }

  int parsed_request = request_line_valid && http_request_is_bodyless(&request);
  StaticFile response_file = {0};
  int status_code = 200;
  const char *reason_phrase = "OK";

  if (!parsed_request) {
    status_code = 400;
    reason_phrase = "Bad Request";
  } else if (strcmp(request_line.method, "GET") != 0) {
    status_code = 405;
    reason_phrase = "Method Not Allowed";
  } else if (static_file_load(static_files, request_line.target,
                              &response_file) != 0) {
    status_code = 404;
    reason_phrase = "Not Found";
  }

  if (status_code != 200 &&
      static_file_load_not_found(static_files, &response_file) != 0) {
    log_error("Could not load the configured 404 page");
  }

  if (response_file.data != NULL) {
    char message[512];
    snprintf(message, sizeof(message), "Serving '%s' as %s (HTTP %d)",
             response_file.relative_path, response_file.content_type,
             status_code);
    log_debug(message);
  }

  const char *content_type = response_file.content_type != NULL
                                 ? response_file.content_type
                                 : static_files->default_mime_type;
  size_t bytes_sent =
      http_response_send(client, status_code, reason_phrase, content_type,
                         response_file.data, response_file.length);

  if (monitor_request) {
    resource_monitor_log_request(&request_start, request.data, request.length,
                                 bytes_sent);
  }
  static_file_release(&response_file);
  close(client);
}
