#include "response.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "transport.h"
#include "buffer.h"

static size_t send_all(HttpTransport *transport, const void *data,
                       size_t length) {
  size_t sent_total = 0;
  const unsigned char *bytes = data;
  while (sent_total < length) {
    ssize_t sent = http_transport_write(transport, bytes + sent_total,
                                        length - sent_total);
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent <= 0)
      break;
    sent_total += (size_t)sent;
  }
  return sent_total;
}

static int has_line_break(const char *value) {
  return strchr(value, '\r') != NULL || strchr(value, '\n') != NULL;
}

size_t http_response_send(HttpTransport *transport, int status_code,
                          const char *reason_phrase, const char *content_type,
                          const void *body, size_t body_length) {
  if (transport == NULL || transport->socket_fd < 0 || reason_phrase == NULL ||
      content_type == NULL ||
      (body == NULL && body_length > 0) || status_code < 100 ||
      status_code > 599 || has_line_break(reason_phrase) ||
      has_line_break(content_type)) {
    return 0;
  }

#ifdef SO_NOSIGPIPE
  int no_sigpipe = 1;
  (void)setsockopt(transport->socket_fd, SOL_SOCKET, SO_NOSIGPIPE,
                   &no_sigpipe, sizeof(no_sigpipe));
#endif

  ByteBuffer headers;
  if (byte_buffer_init(&headers, 1024) != 0)
    return 0;
  if (byte_buffer_appendf(
          &headers,
          "HTTP/1.1 %d %s\r\n"
          "Content-Type: %s\r\n"
          "X-Content-Type-Options: nosniff\r\n"
          "X-Frame-Options: DENY\r\n"
          "Referrer-Policy: no-referrer\r\n"
          "Permissions-Policy: camera=(), microphone=(), geolocation=()\r\n"
          "Cross-Origin-Resource-Policy: same-origin\r\n"
          "Content-Security-Policy: default-src 'self'; base-uri 'none'; object-src 'none'; frame-ancestors 'none'; form-action 'self'; upgrade-insecure-requests\r\n"
          "Cache-Control: no-store\r\n"
          "%s"
          "Content-Length: %zu\r\n"
          "Connection: close\r\n\r\n",
          status_code, reason_phrase, content_type,
          transport->tls != NULL
              ? "Strict-Transport-Security: max-age=31536000\r\n"
              : "",
          body_length) != 0) {
    byte_buffer_destroy(&headers);
    return 0;
  }

  size_t sent_headers = send_all(transport, headers.data, headers.length);
  if (sent_headers != headers.length) {
    byte_buffer_destroy(&headers);
    return sent_headers;
  }
  byte_buffer_destroy(&headers);
  return sent_headers + send_all(transport, body, body_length);
}
