#ifndef HTTP_H
#define HTTP_H

#include "request.h"

#define HTTP_METHOD_CAPACITY 16
#define HTTP_TARGET_CAPACITY 2048
#define HTTP_VERSION_CAPACITY 16

typedef struct HttpRequestLine {
  char method[HTTP_METHOD_CAPACITY];
  char target[HTTP_TARGET_CAPACITY];
  char version[HTTP_VERSION_CAPACITY];
} HttpRequestLine;

int http_request_line_parse(const HttpRequest *request,
                            HttpRequestLine *request_line);

#endif
