#include "request.h"
#include "http.h"
#include "logger.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

void log_debug(const char *message) { (void)message; }

static HttpRequest make_request(const char *text) {
  HttpRequest request = {0};
  size_t length = strlen(text);
  assert(length < sizeof(request.data));
  memcpy(request.data, text, length + 1);
  request.length = length;
  return request;
}

int main(void) {
  HttpRequest request = make_request(
      "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
  assert(http_request_is_bodyless(&request) == 1);
  assert(http_request_headers_valid(&request, "HTTP/1.1") == 1);

  request = make_request(
      "POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
  assert(http_request_is_bodyless(&request) == 1);

  request = make_request(
      "POST / HTTP/1.1\r\nContent-Length: 1\r\n\r\nx");
  assert(http_request_is_bodyless(&request) == 0);

  request = make_request(
      "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
  assert(http_request_is_bodyless(&request) == 0);

  request = make_request(
      "POST / HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n");
  assert(http_request_is_bodyless(&request) == 0);

  request = make_request("GET / HTTP/1.1\r\nMalformed Header\r\n\r\n");
  assert(http_request_is_bodyless(&request) == 0);
  assert(http_request_headers_valid(&request, "HTTP/1.1") == 0);
  request = make_request("GET / HTTP/1.1\nHost: localhost\n\n");
  assert(http_request_headers_valid(&request, "HTTP/1.1") == 0);
  request = make_request("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n");
  assert(http_request_headers_valid(&request, "HTTP/1.1") == 0);
  request = make_request("GET / HTTP/1.1\r\nHost: localhost\r\n"
                         "Content-Length: 0\r\nContent-Length: 0\r\n\r\n");
  assert(http_request_headers_valid(&request, "HTTP/1.1") == 0);
  request = make_request("GET / HTTP/1.0\r\n\r\n");
  assert(http_request_headers_valid(&request, "HTTP/1.0") == 1);
  assert(http_request_is_bodyless(NULL) == 0);

  puts("HTTP request unit tests passed");
  return 0;
}
