#include "http.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  HttpRequest request = {0};
  HttpRequestLine line = {0};

  const char valid[] = "GET /folder/page.html?x=1 HTTP/1.1\r\nHost: local\r\n\r\n";
  memcpy(request.data, valid, sizeof(valid));
  request.length = sizeof(valid) - 1;
  assert(http_request_line_parse(&request, &line) == 0);
  assert(strcmp(line.method, "GET") == 0);
  assert(strcmp(line.target, "/folder/page.html?x=1") == 0);
  assert(strcmp(line.version, "HTTP/1.1") == 0);

  const char invalid_version[] = "GET / HTTP/2.0\r\n\r\n";
  memset(&request, 0, sizeof(request));
  memcpy(request.data, invalid_version, sizeof(invalid_version));
  request.length = sizeof(invalid_version) - 1;
  assert(http_request_line_parse(&request, &line) == -1);

  const char absolute_target[] = "GET http://example.test/ HTTP/1.1\r\n\r\n";
  memset(&request, 0, sizeof(request));
  memcpy(request.data, absolute_target, sizeof(absolute_target));
  request.length = sizeof(absolute_target) - 1;
  assert(http_request_line_parse(&request, &line) == -1);
  assert(http_request_line_parse(NULL, &line) == -1);
  assert(http_request_line_parse(&request, NULL) == -1);

  puts("parser unit tests passed");
  return 0;
}
