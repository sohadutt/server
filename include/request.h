#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <stddef.h>

#define HTTP_REQUEST_CAPACITY 4096

typedef struct HttpRequest {
  char data[HTTP_REQUEST_CAPACITY];
  size_t length;
} HttpRequest;

/* Returns 1 when data was received, 0 on end-of-stream, and -1 on error. */
int http_request_receive(int client, HttpRequest *request);
void http_request_log_debug(const HttpRequest *request);

#endif
