#include "response.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static size_t send_all(int socket_fd, const void *data, size_t length) {
  size_t sent_total = 0;
  const unsigned char *bytes = data;
  while (sent_total < length) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    ssize_t sent =
        send(socket_fd, bytes + sent_total, length - sent_total, flags);
    if (sent < 0 && errno == EINTR) continue;
    if (sent <= 0) break;
    sent_total += (size_t)sent;
  }
  return sent_total;
}

static int has_line_break(const char *value) {
  return strchr(value, '\r') != NULL || strchr(value, '\n') != NULL;
}

size_t http_response_send(int client, int status_code,
                          const char *reason_phrase, const char *content_type,
                          const void *body, size_t body_length) {
  if (client < 0 || reason_phrase == NULL || content_type == NULL ||
      (body == NULL && body_length > 0) || status_code < 100 ||
      status_code > 599 ||
      has_line_break(reason_phrase) || has_line_break(content_type)) {
    return 0;
  }

#ifdef SO_NOSIGPIPE
  int no_sigpipe = 1;
  (void)setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
                   sizeof(no_sigpipe));
#endif

  char headers[512];
  int header_length = snprintf(headers, sizeof(headers),
                               "HTTP/1.1 %d %s\r\n"
                               "Content-Type: %s\r\n"
                               "Content-Length: %zu\r\n"
                               "Connection: close\r\n\r\n",
                               status_code, reason_phrase, content_type,
                               body_length);
  if (header_length < 0 || (size_t)header_length >= sizeof(headers)) return 0;

  size_t sent_headers =
      send_all(client, headers, (size_t)header_length);
  if (sent_headers != (size_t)header_length) return sent_headers;
  return sent_headers + send_all(client, body, body_length);
}
