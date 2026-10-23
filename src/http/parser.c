#include "request.h"

#include <stdio.h>
#include <string.h>

static int token_character(unsigned char character) {
  return (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z') ||
         (character >= '0' && character <= '9') ||
         strchr("!#$%&'*+-.^_`|~", character) != NULL;
}

int http_request_line_parse(const HttpRequest *request,
                            HttpRequestLine *request_line) {
  if (request == NULL || request_line == NULL || request->length == 0) {
    return -1;
  }

  *request_line = (HttpRequestLine){0};
  size_t line_length = 0;
  while (line_length < request->length && request->data[line_length] != '\r' &&
         request->data[line_length] != '\n')
    line_length++;
  if (line_length == 0 || line_length >= HTTP_REQUEST_CAPACITY)
    return -1;

  size_t first_space = 0;
  while (first_space < line_length && request->data[first_space] != ' ') {
    if (!token_character((unsigned char)request->data[first_space]))
      return -1;
    first_space++;
  }
  if (first_space == 0 || first_space >= sizeof(request_line->method) ||
      first_space == line_length)
    return -1;
  size_t target_start = first_space + 1;
  size_t second_space = target_start;
  while (second_space < line_length && request->data[second_space] != ' ') {
    unsigned char character = (unsigned char)request->data[second_space];
    if (character <= 0x20 || character >= 0x7f || character == '#' ||
        character == '\\')
      return -1;
    second_space++;
  }
  size_t target_length = second_space - target_start;
  if (target_length == 0 || target_length >= sizeof(request_line->target) ||
      request->data[target_start] != '/' || second_space == line_length)
    return -1;
  size_t version_start = second_space + 1;
  size_t version_length = line_length - version_start;
  if (version_length >= sizeof(request_line->version) || version_length == 0)
    return -1;
  for (size_t i = version_start; i < line_length; i++) {
    if (request->data[i] <= 0x20 || request->data[i] >= 0x7f)
      return -1;
  }

  memcpy(request_line->method, request->data, first_space);
  memcpy(request_line->target, request->data + target_start, target_length);
  memcpy(request_line->version, request->data + version_start, version_length);
  if (strcmp(request_line->version, "HTTP/1.0") != 0 &&
      strcmp(request_line->version, "HTTP/1.1") != 0) {
    return -1;
  }
  return 0;
}
