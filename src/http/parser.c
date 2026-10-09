#include "http.h"

#include <stdio.h>
#include <string.h>

int http_request_line_parse(const HttpRequest *request,
                            HttpRequestLine *request_line) {
  if (request == NULL || request_line == NULL || request->length == 0) {
    return -1;
  }

  size_t line_length = 0;
  while (line_length < request->length && request->data[line_length] != '\r' &&
         request->data[line_length] != '\n') {
    line_length++;
  }
  if (line_length == 0 || line_length >= HTTP_REQUEST_CAPACITY) return -1;

  char line[HTTP_REQUEST_CAPACITY];
  memcpy(line, request->data, line_length);
  line[line_length] = '\0';

  char extra;
  int parsed = sscanf(line, "%15s %2047s %15s %c", request_line->method,
                      request_line->target, request_line->version, &extra);
  if (parsed != 3 || request_line->target[0] != '/' ||
      (strcmp(request_line->version, "HTTP/1.0") != 0 &&
       strcmp(request_line->version, "HTTP/1.1") != 0)) {
    return -1;
  }
  return 0;
}
