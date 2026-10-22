#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <stddef.h>
#include "transport.h"

#define HTTP_REQUEST_CAPACITY 4096

typedef struct HttpRequest {
  char data[HTTP_REQUEST_CAPACITY];
  size_t length;
} HttpRequest;

/* Returns 1 when data was received, 0 on end-of-stream, and -1 on error. */
int http_request_receive(HttpTransport *transport, HttpRequest *request);
/* Validates HTTP/1 header syntax and rejects ambiguous body framing. */
int http_request_headers_valid(const HttpRequest *request,
                               const char *http_version);
/* True when the request has valid headers and no unsupported body framing. */
int http_request_is_bodyless(const HttpRequest *request);
void http_request_log_debug(const HttpRequest *request);

#endif
