#include "response.h"
#include "buffer.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  ByteBuffer buffer;
  assert(byte_buffer_init(&buffer, 700) == 0);
  assert(buffer.data == buffer.inline_data);
  assert(byte_buffer_append(&buffer, "abc", 3) == 0);
  assert(byte_buffer_append(&buffer, buffer.data, buffer.length) == 0);
  assert(buffer.length == 6 && memcmp(buffer.data, "abcabc", 6) == 0);
  char growth[600];
  memset(growth, 'x', sizeof(growth));
  assert(byte_buffer_append(&buffer, growth, sizeof(growth)) == 0);
  assert(buffer.data != buffer.inline_data && buffer.length == 606);
  assert(byte_buffer_append(&buffer, growth, 95) == -1);
  byte_buffer_reset(&buffer);
  assert(buffer.length == 0 && buffer.data[0] == '\0');
  byte_buffer_destroy(&buffer);

  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  HttpTransport transport = {.socket_fd = sockets[0], .tls = NULL};
  static const char body[] = "hello";
  size_t sent = http_response_send(&transport, 200, "OK", "text/plain",
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

  transport.socket_fd = -1;
  assert(http_response_send(&transport, 200, "OK", "text/plain", body,
                            sizeof(body) - 1) == 0);
  transport.socket_fd = 1;
  assert(http_response_send(&transport, 200, "OK\r\nInjected: yes", "text/plain",
                            body, sizeof(body) - 1) == 0);
  assert(http_response_send(&transport, 200, "OK", "text/plain\r\nInjected: yes",
                            body, sizeof(body) - 1) == 0);
  assert(http_response_send(&transport, 200, "OK", "text/plain", NULL, 1) == 0);

  puts("HTTP response unit tests passed");
  return 0;
}
