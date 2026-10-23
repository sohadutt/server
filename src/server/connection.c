#include "connection.h"

#include "api.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include "logger.h"
#include "request.h"
#include "rate_limit.h"
#include "resource_monitor.h"
#include "response.h"
#include "static.h"
#include "transport.h"
#include "router.h"

static RateLimiter address_rate_limiter = RATE_LIMITER_INITIALIZER;
static RateLimiter session_rate_limiter = RATE_LIMITER_INITIALIZER;

#define HTTP_KEEP_ALIVE_MAX_REQUESTS 16U

static int peer_is_loopback(const struct sockaddr *address) {
  if (address == NULL)
    return 0;
  if (address->sa_family == AF_INET) {
    const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)address;
    return (ntohl(ipv4->sin_addr.s_addr) >> 24) == 127U;
  }
  if (address->sa_family == AF_INET6) {
    const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)address;
    return IN6_IS_ADDR_LOOPBACK(&ipv6->sin6_addr) ||
           (IN6_IS_ADDR_V4MAPPED(&ipv6->sin6_addr) &&
            ipv6->sin6_addr.s6_addr[12] == 127);
  }
  return 0;
}

static int normalized_ip(const char *input, size_t length, char *output,
                         size_t output_capacity) {
  if (length == 0 || length >= INET6_ADDRSTRLEN)
    return -1;
  char value[INET6_ADDRSTRLEN];
  memcpy(value, input, length);
  value[length] = '\0';
  unsigned char address[sizeof(struct in6_addr)];
  int family = strchr(value, ':') != NULL ? AF_INET6 : AF_INET;
  if (inet_pton(family, value, address) != 1 ||
      inet_ntop(family, address, output, (socklen_t)output_capacity) == NULL)
    return -1;
  return 0;
}

static int forwarded_client_ip(const HttpRequest *request, char *output,
                               size_t output_capacity) {
  if (request == NULL || output == NULL || output_capacity == 0)
    return -1;
  size_t end = 0;
  for (size_t i = 0; i + 3 < request->length; i++) {
    if (memcmp(request->data + i, "\r\n\r\n", 4) == 0) {
      end = i + 4;
      break;
    }
  }
  if (end == 0) {
    for (size_t i = 0; i + 1 < request->length; i++) {
      if (request->data[i] == '\n' && request->data[i + 1] == '\n') {
        end = i + 2;
        break;
      }
    }
  }
  if (end == 0)
    return -1;
  size_t position = 0;
  while (position < end && request->data[position] != '\n')
    position++;
  if (position < end)
    position++;

  while (position < end) {
    size_t line_end = position;
    while (line_end < end && request->data[line_end] != '\r' &&
           request->data[line_end] != '\n')
      line_end++;
    if (line_end == position)
      break;
    size_t colon = position;
    while (colon < line_end && request->data[colon] != ':')
      colon++;
    if (colon < line_end && colon - position == 15 &&
        strncasecmp(request->data + position, "x-forwarded-for", 15) == 0) {
      size_t first = colon + 1;
      while (first < line_end && isspace((unsigned char)request->data[first]))
        first++;
      size_t last = line_end;
      while (last > first && isspace((unsigned char)request->data[last - 1]))
        last--;
      for (size_t i = first; i < last; i++) {
        if (request->data[i] == ',')
          first = i + 1;
      }
      while (first < last && isspace((unsigned char)request->data[first]))
        first++;
      while (last > first && isspace((unsigned char)request->data[last - 1]))
        last--;
      return normalized_ip(request->data + first, last - first, output,
                           output_capacity);
    }
    while (line_end < end && (request->data[line_end] == '\r' ||
                              request->data[line_end] == '\n'))
      line_end++;
    position = line_end;
  }
  return -1;
}

static void client_identity(const struct sockaddr *peer_address,
                            const HttpRequest *request, char *identity,
                            size_t identity_capacity) {
  if (request != NULL && peer_is_loopback(peer_address) &&
      forwarded_client_ip(request, identity, identity_capacity) == 0)
    return;
  if (peer_address != NULL &&
      getnameinfo(peer_address, peer_address->sa_family == AF_INET
                                   ? sizeof(struct sockaddr_in)
                                   : sizeof(struct sockaddr_in6),
                  identity, (socklen_t)identity_capacity, NULL, 0,
                  NI_NUMERICHOST) == 0)
    return;
  snprintf(identity, identity_capacity, "unknown");
}

static int rate_limit_kind(const char *target, RateLimitKind *kind) {
  size_t path_length = strcspn(target, "?");
  if (path_length == sizeof("/api/login") - 1 &&
      memcmp(target, "/api/login", path_length) == 0) {
    *kind = RATE_LIMIT_LOGIN;
    return 1;
  }
  if (path_length == sizeof("/api/signup") - 1 &&
      memcmp(target, "/api/signup", path_length) == 0) {
    *kind = RATE_LIMIT_SIGNUP;
    return 1;
  }
  if (path_length >= sizeof("/api/admin/") - 1 &&
      memcmp(target, "/api/admin/", sizeof("/api/admin/") - 1) == 0) {
    *kind = RATE_LIMIT_ADMIN;
    return 1;
  }
  *kind = RATE_LIMIT_API;
  return 1;
}

static int request_session_key(const HttpRequest *request, DataStore *store,
                               char *key, size_t key_capacity) {
  if (request == NULL || key == NULL || key_capacity < 70)
    return -1;
  size_t header_limit = 0;
  for (size_t i = 0; i + 3 < request->length; i++) {
    if (memcmp(request->data + i, "\r\n\r\n", 4) == 0) {
      header_limit = i + 4;
      break;
    }
  }
  if (header_limit == 0)
    return -1;
  size_t position = 0;
  while (position + 1 < header_limit &&
         !(request->data[position] == '\r' &&
           request->data[position + 1] == '\n'))
    position++;
  position += 2;
  char token[256] = {0};
  int authorization_seen = 0;
  int valid = 0;
  while (position + 1 < header_limit) {
    size_t end = position;
    while (end + 1 < header_limit &&
           !(request->data[end] == '\r' && request->data[end + 1] == '\n'))
      end++;
    if (end == position)
      break;
    size_t colon = position;
    while (colon < end && request->data[colon] != ':')
      colon++;
    if (colon < end && colon - position == sizeof("authorization") - 1 &&
        strncasecmp(request->data + position, "authorization",
                    sizeof("authorization") - 1) == 0) {
      if (authorization_seen)
        break;
      authorization_seen = 1;
      size_t first = colon + 1;
      while (first < end && (request->data[first] == ' ' ||
                             request->data[first] == '\t'))
        first++;
      size_t last = end;
      while (last > first && (request->data[last - 1] == ' ' ||
                              request->data[last - 1] == '\t'))
        last--;
      if (last - first <= 7 || last - first - 7 >= sizeof(token) ||
          strncasecmp(request->data + first, "Bearer ", 7) != 0)
        break;
      size_t token_length = last - first - 7;
      valid = token_length > 0;
      for (size_t i = 0; i < token_length; i++) {
        unsigned char character = (unsigned char)request->data[first + 7 + i];
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '-' ||
              character == '_')) {
          valid = 0;
          break;
        }
      }
      if (valid) {
        memcpy(token, request->data + first + 7, token_length);
        token[token_length] = '\0';
      }
    }
    position = end + 2;
  }
  if (!valid) {
    OPENSSL_cleanse(token, sizeof(token));
    return -1;
  }

  DataUser user = {0};
  DataResult authentication =
      data_token_authenticate(store, token, &user);
  OPENSSL_cleanse(&user, sizeof(user));
  if (authentication != DATA_OK) {
    OPENSSL_cleanse(token, sizeof(token));
    return -1;
  }

  unsigned char digest[SHA256_DIGEST_LENGTH];
  if (SHA256((const unsigned char *)token, strlen(token), digest) == NULL) {
    OPENSSL_cleanse(token, sizeof(token));
    return -1;
  }
  static const char hex[] = "0123456789abcdef";
  if (key_capacity < 70) {
    OPENSSL_cleanse(token, sizeof(token));
    OPENSSL_cleanse(digest, sizeof(digest));
    return -1;
  }
  memcpy(key, "sess:", 5);
  for (size_t i = 0; i < sizeof(digest); i++) {
    key[5 + i * 2] = hex[digest[i] >> 4];
    key[6 + i * 2] = hex[digest[i] & 0x0fU];
  }
  key[69] = '\0';
  OPENSSL_cleanse(token, sizeof(token));
  OPENSSL_cleanse(digest, sizeof(digest));
  return 0;
}

static int monotonic_seconds(uint64_t *seconds) {
  struct timespec now;
  if (seconds == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
      now.tv_sec < 0)
    return -1;
  *seconds = (uint64_t)now.tv_sec;
  return 0;
}

static void log_peer(const struct sockaddr *address, socklen_t address_length) {
  char host[128];
  char service[16];
  if (address != NULL &&
      getnameinfo(address, address_length, host, sizeof(host), service,
                  sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
    log_debugf("Connection from %s:%s", host, service);
    return;
  }
  log_debug("Connection from unknown source");
}

static void log_request_summary(const HttpRequestLine *request_line,
                                int status_code, size_t bytes_received,
                                size_t bytes_sent,
                                const struct timespec *started) {
  struct timespec finished;
  if (clock_gettime(CLOCK_MONOTONIC, &finished) != 0)
    return;
  double elapsed_ms = 0.0;
  if (started != NULL) {
    elapsed_ms = (double)(finished.tv_sec - started->tv_sec) * 1000.0 +
                 (double)(finished.tv_nsec - started->tv_nsec) / 1000000.0;
  }
  char status_text[16];
  if (status_code <= 0)
    snprintf(status_text, sizeof(status_text), "n/a");
  else
    snprintf(status_text, sizeof(status_text), "%d", status_code);
  if (request_line == NULL) {
    log_debugf("Request completed status=%s bytes_in=%zu bytes_out=%zu duration_ms=%.3f",
               status_text, bytes_received, bytes_sent, elapsed_ms);
    return;
  }
  size_t path_length = strcspn(request_line->target, "?");
  log_debugf("Request %s %.*s completed status=%s bytes_in=%zu bytes_out=%zu duration_ms=%.3f",
             request_line->method, (int)path_length, request_line->target,
             status_text, bytes_received, bytes_sent, elapsed_ms);
}

static void drain_rejected_body(HttpTransport *transport,
                                const HttpRequest *request, int headers_valid,
                                size_t *bytes_received) {
  if (!headers_valid)
    return;
  errno = 0;
  if (http_request_drain_body(transport, request, bytes_received) != 0 &&
      errno != EMSGSIZE)
    log_debugf("Could not drain rejected request body: %s",
               errno != 0 ? strerror(errno) : "incomplete body");
}

static void close_transport(HttpTransport *transport) {
  if (transport == NULL)
    return;
  tls_connection_destroy(transport->tls);
  close(transport->socket_fd);
  transport->socket_fd = -1;
  transport->tls = NULL;
}

struct Connection {
  uint64_t request_id;
  unsigned request_count;
  int tls_initialized;
  HttpTransport transport;
  struct sockaddr_storage peer_address;
  socklen_t peer_address_length;
  const ServerConfig *config;
  const StaticFileServer *static_files;
  DataStore *data_store;
  TlsServer *tls_server;
  const HttpRouter *router;
  uint64_t tls_handshake_ns;
  uint64_t request_read_ns;
};

static uint64_t monotonic_nanoseconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
         (uint64_t)now.tv_nsec;
}

Connection *connection_create(int client, const struct sockaddr *peer_address,
                              socklen_t peer_address_length,
                              uint64_t request_id, const ServerConfig *config,
                              const StaticFileServer *static_files,
                              DataStore *data_store, TlsServer *tls_server,
                              const HttpRouter *router) {
  if (client < 0 || peer_address == NULL ||
      peer_address_length > sizeof(struct sockaddr_storage) || config == NULL ||
      static_files == NULL || router == NULL) {
    if (client >= 0)
      close(client);
    errno = EINVAL;
    return NULL;
  }
  Connection *connection = calloc(1, sizeof(*connection));
  if (connection == NULL) {
    close(client);
    return NULL;
  }
  connection->request_id = request_id;
  connection->transport.socket_fd = client;
  connection->peer_address_length = peer_address_length;
  memcpy(&connection->peer_address, peer_address,
         peer_address_length <= sizeof(connection->peer_address)
             ? peer_address_length
             : sizeof(connection->peer_address));
  connection->config = config;
  connection->static_files = static_files;
  connection->data_store = data_store;
  connection->tls_server = tls_server;
  connection->router = router;
  const struct timeval receive_timeout = {.tv_sec = 10, .tv_usec = 0};
  const struct timeval send_timeout = {.tv_sec = 15, .tv_usec = 0};
  if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                 sizeof(receive_timeout)) != 0 ||
      setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                 sizeof(send_timeout)) != 0) {
    int socket_error = errno;
    log_warnf("Could not set client socket timeouts: %s",
              strerror(socket_error));
    connection_destroy(connection);
    errno = socket_error;
    return NULL;
  }
  log_peer(peer_address, peer_address_length);
  return connection;
}

int connection_file_descriptor(const Connection *connection) {
  return connection != NULL ? connection->transport.socket_fd : -1;
}

void connection_destroy(Connection *connection) {
  if (connection == NULL)
    return;
  close_transport(&connection->transport);
  free(connection);
}

static int connection_process_one_impl(Connection *connection) {
  if (connection == NULL)
    return 0;
  HttpTransport *transport = &connection->transport;
  if (!connection->tls_initialized && connection->config->tls_enabled) {
    uint64_t handshake_started = monotonic_nanoseconds();
    transport->tls = tls_server_accept(connection->tls_server,
                                        transport->socket_fd);
    uint64_t handshake_finished = monotonic_nanoseconds();
    if (handshake_finished >= handshake_started)
      connection->tls_handshake_ns = handshake_finished - handshake_started;
    if (transport->tls == NULL)
      return 0;
  }
  connection->tls_initialized = 1;
  logger_set_request_id(connection->request_id);
  const struct sockaddr *peer_address =
      (const struct sockaddr *)&connection->peer_address;
  const ServerConfig *config = connection->config;
  const StaticFileServer *static_files = connection->static_files;
  DataStore *data_store = connection->data_store;
  const HttpRouter *router = connection->router;
  ResourceUsageSnapshot request_start;
  unsigned request_number = connection->request_count;
  struct timespec request_started = {0};
  int have_request_start =
      clock_gettime(CLOCK_MONOTONIC, &request_started) == 0;
  int monitor_request = config->log_level == LOG_DEBUG &&
                        resource_monitor_capture(&request_start) == 0;

  HttpRequest request = {0};
  uint64_t read_started = monotonic_nanoseconds();
  int read_result = http_request_receive_timeout(transport, &request, 10000);
  uint64_t read_finished = monotonic_nanoseconds();
  if (read_finished >= read_started)
    connection->request_read_ns = read_finished - read_started;
  if (read_result == 0)
    return 0;
  transport->keep_alive = 0;
  transport->gzip_enabled = config->gzip_enabled;
  transport->accepts_gzip = http_request_accepts_gzip(&request);
  if (read_result < 0) {
    int read_error = errno;
    log_debugf("Could not read complete request from client: %s",
               read_error != 0 ? strerror(read_error) : "connection closed");
  } else {
    http_request_log_debug(&request);
  }

  HttpRequestLine request_line = {0};
  int request_line_valid =
      read_result > 0 && http_request_line_parse(&request, &request_line) == 0;
  int headers_valid = request_line_valid &&
                      http_request_headers_valid(&request, request_line.version);
  int request_valid = request_line_valid && headers_valid;
  if (!request_valid) {
    char identity[INET6_ADDRSTRLEN] = {0};
    uint64_t now = 0;
    client_identity(peer_address, NULL, identity, sizeof(identity));
    int may_reply = !config->rate_limiting_enabled ||
                    (monotonic_seconds(&now) == 0 &&
                     rate_limiter_allow(&address_rate_limiter, identity,
                                        RATE_LIMIT_MALFORMED, now));
    size_t bytes_sent = may_reply
                            ? http_response_send(
                                  transport, 400, "Bad Request",
                                  "text/plain; charset=utf-8",
                                  "Bad Request\n", sizeof("Bad Request\n") - 1)
                            : 0;
    size_t bytes_received = request.length;
    drain_rejected_body(transport, &request, headers_valid,
                        &bytes_received);
    log_debug(may_reply ? "Rejected malformed HTTP request"
                        : "Dropped malformed request from throttled client");
    if (monitor_request)
      resource_monitor_log_request(&request_start, request.data,
                                   bytes_received, bytes_sent);
    log_request_summary(request_line_valid ? &request_line : NULL, 400,
                        bytes_received, bytes_sent,
                        have_request_start ? &request_started : NULL);
    return 0;
  }

  const HttpRoute *route = NULL;
  HttpRouteResult route_result =
      http_router_resolve(router, request_line.method, request_line.target,
                          &route);

  if (request_line_valid && route_result == HTTP_ROUTE_METHOD_NOT_ALLOWED) {
    size_t bytes_sent = http_response_send(
        transport, 405, "Method Not Allowed", "text/plain; charset=utf-8",
        "Method Not Allowed\n", sizeof("Method Not Allowed\n") - 1);
    size_t bytes_received = request.length;
    drain_rejected_body(transport, &request, headers_valid,
                        &bytes_received);
    if (monitor_request)
      resource_monitor_log_request(&request_start, request.data,
                                   bytes_received, bytes_sent);
    log_request_summary(&request_line, 405, bytes_received, bytes_sent,
                        have_request_start ? &request_started : NULL);
    return 0;
  }

  if (route_result == HTTP_ROUTE_MATCHED &&
      route->action == HTTP_ROUTE_API) {
    RateLimitKind limit_kind;
    char identity[INET6_ADDRSTRLEN] = {0};
    uint64_t now = 0;
    client_identity(peer_address, &request, identity, sizeof(identity));
    int rate_limited = config->rate_limiting_enabled &&
                       (monotonic_seconds(&now) != 0 ||
                        !rate_limit_kind(request_line.target, &limit_kind) ||
                        !rate_limiter_allow(&address_rate_limiter, identity,
                                            RATE_LIMIT_API, now) ||
                        (limit_kind != RATE_LIMIT_API &&
                         !rate_limiter_allow(&address_rate_limiter, identity,
                                             limit_kind, now)));
    size_t path_length = strcspn(request_line.target, "?");
    int is_print = path_length == sizeof("/api/print") - 1 &&
                   memcmp(request_line.target, "/api/print", path_length) == 0;
    int is_login = path_length == sizeof("/api/login") - 1 &&
                   memcmp(request_line.target, "/api/login", path_length) == 0;
    int is_signup = path_length == sizeof("/api/signup") - 1 &&
                    memcmp(request_line.target, "/api/signup", path_length) == 0;
    if (config->rate_limiting_enabled && !rate_limited && !is_login &&
        !is_signup &&
        strncmp(request_line.target, "/api/admin/", 11) != 0) {
      char session_key[RATE_LIMITER_KEY_CAPACITY] = {0};
      if (request_session_key(&request, data_store, session_key,
                              sizeof(session_key)) == 0) {
        rate_limited =
            !rate_limiter_allow(&session_rate_limiter, session_key,
                                RATE_LIMIT_SESSION, now) ||
            (is_print &&
             !rate_limiter_allow(&session_rate_limiter, session_key,
                                 RATE_LIMIT_PRINT, now));
      }
      OPENSSL_cleanse(session_key, sizeof(session_key));
    }
    if (rate_limited) {
        static const char response[] = "{\"error\":\"rate limit exceeded\"}\n";
        size_t bytes_sent = http_response_send(
            transport, 429, "Too Many Requests", "application/json",
            response, sizeof(response) - 1);
        size_t bytes_received = request.length;
        drain_rejected_body(transport, &request, headers_valid,
                            &bytes_received);
        if (monitor_request) {
          resource_monitor_log_request(&request_start, request.data,
                                       bytes_received, bytes_sent);
        }
        log_request_summary(&request_line, 429, bytes_received, bytes_sent,
                            have_request_start ? &request_started : NULL);
        return 0;
    }
    size_t bytes_sent = 0;
    size_t api_bytes_received = request.length;
    (void)api_handle_request(transport, &request, request_line.method,
                             request_line.target, config, data_store,
                             &api_bytes_received, &bytes_sent);
    drain_rejected_body(transport, &request, headers_valid,
                        &api_bytes_received);
    if (monitor_request) {
      resource_monitor_log_request(&request_start, request.data,
                                   api_bytes_received, bytes_sent);
    }
    log_request_summary(&request_line, transport->last_status_code,
                        api_bytes_received, bytes_sent,
                        have_request_start ? &request_started : NULL);
    return 0;
  }

  int parsed_request = request_line_valid && route != NULL &&
                       route->action == HTTP_ROUTE_STATIC &&
                       http_request_is_bodyless(&request);
  transport->keep_alive = parsed_request &&
                         request_number + 1 < HTTP_KEEP_ALIVE_MAX_REQUESTS &&
                         !http_request_has_extra_data(&request) &&
                         http_request_keep_alive(&request,
                                                 request_line.version);
  StaticFile response_file = {0};
  int status_code = 200;
  const char *reason_phrase = "OK";

  if (!parsed_request) {
    status_code = 400;
    reason_phrase = "Bad Request";
  } else if (route_result == HTTP_ROUTE_NO_MATCH) {
    status_code = 404;
    reason_phrase = "Not Found";
  } else if (static_file_open(static_files, request_line.target,
                              &response_file) != 0) {
    status_code = 404;
    reason_phrase = "Not Found";
  }

  if (status_code != 200 &&
      static_file_open_not_found(static_files, &response_file) != 0) {
    log_error("Could not load the configured 404 page");
  }

  if (response_file.data != NULL || response_file.is_stream) {
    log_debugf("Serving '%s' as %s (HTTP %d)", response_file.relative_path,
               response_file.content_type, status_code);
  }

  const char *content_type = response_file.content_type != NULL
                                 ? response_file.content_type
                                 : static_files->default_mime_type;
  size_t bytes_sent = response_file.is_stream
                          ? http_response_send_file(
                                transport, status_code, reason_phrase,
                                content_type, response_file.fd,
                                response_file.length,
                                response_file.cache_control != NULL
                                    ? response_file.cache_control
                                    : "no-cache")
                          : http_response_send_static(
                                transport, status_code, reason_phrase,
                                content_type, response_file.data,
                                response_file.length,
                                response_file.cache_control != NULL
                                    ? response_file.cache_control
                                    : "no-cache");

  size_t bytes_received = request.length;
  drain_rejected_body(transport, &request, headers_valid, &bytes_received);

  if (monitor_request) {
    resource_monitor_log_request(&request_start, request.data, bytes_received,
                                 bytes_sent);
  }
  log_request_summary(&request_line, status_code, bytes_received, bytes_sent,
                      have_request_start ? &request_started : NULL);
  static_file_release(&response_file);
  connection->request_count++;
  return transport->keep_alive;
}

int connection_process_one(Connection *connection, ConnectionTimings *timings) {
  if (connection == NULL)
    return 0;
  connection->tls_handshake_ns = 0;
  connection->request_read_ns = 0;
  connection->transport.write_time_ns = 0;
  connection->transport.write_wait_ns = 0;
  int keep_alive = connection_process_one_impl(connection);
  if (timings != NULL) {
    *timings = (ConnectionTimings){
        .tls_handshake_ns = connection->tls_handshake_ns,
        .request_read_ns = connection->request_read_ns,
        .socket_write_ns = connection->transport.write_time_ns,
        .socket_writable_wait_ns = connection->transport.write_wait_ns,
    };
  }
  return keep_alive;
}
