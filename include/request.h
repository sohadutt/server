#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include "transport.h"
#include <stddef.h>

#define HTTP_REQUEST_CAPACITY 4096
#define HTTP_METHOD_CAPACITY 16
#define HTTP_TARGET_CAPACITY 2048
#define HTTP_VERSION_CAPACITY 16

typedef struct HttpRequestLine {
  char method[HTTP_METHOD_CAPACITY];
  char target[HTTP_TARGET_CAPACITY];
  char version[HTTP_VERSION_CAPACITY];
} HttpRequestLine;

typedef struct HttpRequest {
  char data[HTTP_REQUEST_CAPACITY];
  size_t length;
} HttpRequest;

/* Returns 1 when data was received, 0 on end-of-stream, and -1 on error. */
int http_request_receive(HttpTransport *transport, HttpRequest *request);
int http_request_receive_timeout(HttpTransport *transport,
                                 HttpRequest *request, int timeout_ms);
/* Validates HTTP/1 header syntax and rejects ambiguous body framing. */
int http_request_headers_valid(const HttpRequest *request,
                               const char *http_version);
int http_request_keep_alive(const HttpRequest *request,
                            const char *http_version);
int http_request_has_extra_data(const HttpRequest *request);
int http_request_accepts_gzip(const HttpRequest *request);
/* True when the request has valid headers and no unsupported body framing. */
int http_request_is_bodyless(const HttpRequest *request);
/* Drain a small, framed request body before closing a rejected connection. */
int http_request_drain_body(HttpTransport *transport, const HttpRequest *request,
                            size_t *bytes_received);
void http_request_log_debug(const HttpRequest *request);
int http_request_line_parse(const HttpRequest *request,
                            HttpRequestLine *request_line);
#endif
