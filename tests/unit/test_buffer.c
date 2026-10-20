#include "response.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  static const char body[] = "hello";
  size_t sent = http_response_send(sockets[0], 200, "OK", "text/plain",
                                   body, sizeof(body) - 1);
  assert(sent > sizeof(body) - 1);

  char response[1024] = {0};
  ssize_t received = read(sockets[1], response, sizeof(response) - 1);
  assert(received > 0);
  assert(strstr(response, "HTTP/1.1 200 OK\r\n") != NULL);
  assert(strstr(response, "Content-Type: text/plain\r\n") != NULL);
  assert(strstr(response, "X-Content-Type-Options: nosniff\r\n") != NULL);
  assert(strstr(response, "Content-Length: 5\r\n") != NULL);
  assert(strstr(response, "\r\n\r\nhello") != NULL);
  close(sockets[0]);
  close(sockets[1]);

  assert(http_response_send(-1, 200, "OK", "text/plain", body,
                            sizeof(body) - 1) == 0);
  assert(http_response_send(1, 200, "OK\r\nInjected: yes", "text/plain",
                            body, sizeof(body) - 1) == 0);
  assert(http_response_send(1, 200, "OK", "text/plain\r\nInjected: yes",
                            body, sizeof(body) - 1) == 0);
  assert(http_response_send(1, 200, "OK", "text/plain", NULL, 1) == 0);

  puts("HTTP response unit tests passed");
  return 0;
}
