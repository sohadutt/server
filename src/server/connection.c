#include "connection.h"

#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "http.h"
#include "logger.h"
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
                       const ServerConfig *config) {
  if (client < 0 || config == NULL)
    return;
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
  int parsed_request =
      read_result > 0 && http_request_line_parse(&request, &request_line) == 0;
  StaticFile response_file = {0};
  int status_code = 200;
  const char *reason_phrase = "OK";

  if (!parsed_request) {
    status_code = 400;
    reason_phrase = "Bad Request";
  } else if (strcmp(request_line.method, "GET") != 0) {
    status_code = 405;
    reason_phrase = "Method Not Allowed";
  } else if (static_file_load(config->document_root, request_line.target,
                              &response_file) != 0) {
    status_code = 404;
    reason_phrase = "Not Found";
  }

  if (status_code != 200 && static_file_load(config->document_root, "/404.html",
                                             &response_file) != 0) {
    log_error("Could not load the configured 404 page");
  }

  const char *content_type = response_file.content_type != NULL
                                 ? response_file.content_type
                                 : "text/html; charset=utf-8";
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
