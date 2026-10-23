#include "response.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "buffer.h"
#include "compression.h"
#include "logger.h"
#include "transport.h"

#define GZIP_MINIMUM_BODY_SIZE 256U
#define GZIP_MAXIMUM_BODY_SIZE (4U * 1024U * 1024U)
#define GZIP_MAXIMUM_FILE_SIZE (1024U * 1024U)

static size_t send_all(HttpTransport *transport, const void *data,
                       size_t length) {
  size_t sent_total = 0;
  const unsigned char *bytes = data;
  while (sent_total < length) {
    ssize_t sent = http_transport_write(transport, bytes + sent_total,
                                        length - sent_total);
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent <= 0) {
      int write_error = errno;
      if (sent == 0)
        log_debugf("Client closed connection during response write (%zu/%zu bytes)",
                   sent_total, length);
      else
        log_debugf("Response write failed after %zu/%zu bytes: %s", sent_total,
                   length, write_error != 0 ? strerror(write_error)
                                            : "I/O error");
      break;
    }
    sent_total += (size_t)sent;
  }
  return sent_total;
}

static int has_line_break(const char *value) {
  return strchr(value, '\r') != NULL || strchr(value, '\n') != NULL;
}

static int content_type_is_compressible(const char *content_type) {
  return strncmp(content_type, "text/", 5) == 0 ||
         strncmp(content_type, "application/json", 16) == 0 ||
         strncmp(content_type, "application/javascript", 22) == 0 ||
         strncmp(content_type, "application/xml", 15) == 0 ||
         strncmp(content_type, "image/svg+xml", 13) == 0;
}

static size_t send_headers(HttpTransport *transport, int status_code,
                           const char *reason_phrase, const char *content_type,
                           size_t body_length, const char *cache_control,
                           const char *content_encoding, int vary_encoding) {
  if (transport == NULL || transport->socket_fd < 0 || reason_phrase == NULL ||
      content_type == NULL || cache_control == NULL || status_code < 100 ||
      status_code > 599 || has_line_break(reason_phrase) ||
      has_line_break(content_type) || has_line_break(cache_control)) {
    return 0;
  }
  transport->last_status_code = status_code;

#ifdef SO_NOSIGPIPE
  int no_sigpipe = 1;
  (void)setsockopt(transport->socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
                   sizeof(no_sigpipe));
#endif

  ByteBuffer headers;
  if (byte_buffer_init(&headers, 8192) != 0)
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
          "Content-Security-Policy: default-src 'self'; base-uri 'none'; "
          "object-src 'none'; frame-ancestors 'none'; form-action 'self'; "
          "upgrade-insecure-requests\r\n"
          "Cache-Control: %s\r\n"
          "%s"
          "%s"
          "%s"
          "Content-Length: %zu\r\n"
          "Connection: %s\r\n"
          "%s"
          "\r\n",
          status_code, reason_phrase, content_type, cache_control,
          vary_encoding ? "Vary: Accept-Encoding\r\n" : "",
          content_encoding != NULL ? "Content-Encoding: gzip\r\n" : "",
          transport->tls != NULL
              ? "Strict-Transport-Security: max-age=31536000\r\n"
              : "",
          body_length, transport->keep_alive ? "keep-alive" : "close",
          transport->keep_alive ? "Keep-Alive: timeout=1, max=16\r\n" : "") !=
      0) {
    byte_buffer_destroy(&headers);
    return 0;
  }

  size_t sent_headers = send_all(transport, headers.data, headers.length);
  if (sent_headers != headers.length) {
    byte_buffer_destroy(&headers);
    return sent_headers;
  }
  byte_buffer_destroy(&headers);
  return sent_headers;
}

static size_t response_send_body(HttpTransport *transport, int status_code,
                                 const char *reason_phrase,
                                 const char *content_type, const void *body,
                                 size_t body_length,
                                 const char *cache_control,
                                 int allow_compression) {
  if (body == NULL && body_length > 0)
    return 0;
  unsigned char *compressed_body = NULL;
  size_t compressed_length = 0;
  int compressible = allow_compression && transport != NULL &&
                     transport->gzip_enabled &&
                     content_type != NULL &&
                     content_type_is_compressible(content_type);
  if (compressible && transport->accepts_gzip && body_length >=
                                                    GZIP_MINIMUM_BODY_SIZE &&
      body_length <= GZIP_MAXIMUM_BODY_SIZE &&
      compress_gzip(body, body_length, &compressed_body,
                    &compressed_length) == 0 &&
      compressed_length < body_length) {
    body = compressed_body;
    body_length = compressed_length;
  }
  const char *encoding = compressed_body != NULL && body == compressed_body
                             ? "gzip"
                             : NULL;
  size_t headers_sent = send_headers(transport, status_code, reason_phrase,
                                     content_type, body_length, cache_control,
                                     encoding, compressible);
  if (headers_sent == 0) {
    free(compressed_body);
    return 0;
  }
  size_t sent = headers_sent + send_all(transport, body, body_length);
  free(compressed_body);
  return sent;
}

size_t http_response_send(HttpTransport *transport, int status_code,
                          const char *reason_phrase, const char *content_type,
                          const void *body, size_t body_length) {
  /* Dynamic no-store responses can contain credentials; do not compress them. */
  return response_send_body(transport, status_code, reason_phrase, content_type,
                            body, body_length, "no-store", 0);
}

size_t http_response_send_static(HttpTransport *transport, int status_code,
                                 const char *reason_phrase,
                                 const char *content_type, const void *body,
                                 size_t body_length,
                                 const char *cache_control) {
  return response_send_body(transport, status_code, reason_phrase, content_type,
                            body, body_length, cache_control, 1);
}

size_t http_response_send_file(HttpTransport *transport, int status_code,
                               const char *reason_phrase,
                               const char *content_type, int file_fd,
                               size_t body_length, const char *cache_control) {
  if (file_fd < 0)
    return 0;
  unsigned char *file_data = NULL;
  unsigned char *compressed_body = NULL;
  size_t compressed_length = 0;
  int compressible = transport != NULL && transport->gzip_enabled &&
                     content_type != NULL &&
                     content_type_is_compressible(content_type);
  if (compressible && transport->accepts_gzip &&
      body_length >= GZIP_MINIMUM_BODY_SIZE &&
      body_length <= GZIP_MAXIMUM_FILE_SIZE) {
    file_data = malloc(body_length);
    if (file_data != NULL) {
      size_t offset = 0;
      while (offset < body_length) {
        ssize_t count;
        do {
          count = pread(file_fd, file_data + offset, body_length - offset,
                        (off_t)offset);
        } while (count < 0 && errno == EINTR);
        if (count <= 0)
          break;
        offset += (size_t)count;
      }
      if (offset == body_length &&
          compress_gzip(file_data, body_length, &compressed_body,
                        &compressed_length) == 0 &&
          compressed_length < body_length) {
        const char *encoding = "gzip";
        size_t headers_sent = send_headers(
            transport, status_code, reason_phrase, content_type,
            compressed_length, cache_control, encoding, compressible);
        free(file_data);
        if (headers_sent == 0) {
          free(compressed_body);
          return 0;
        }
        size_t sent = headers_sent +
                      send_all(transport, compressed_body, compressed_length);
        free(compressed_body);
        return sent;
      }
    }
  }
  free(file_data);
  size_t headers_sent = send_headers(transport, status_code, reason_phrase,
                                     content_type, body_length, cache_control,
                                     NULL, compressible);
  if (headers_sent == 0)
    return 0;

  unsigned char buffer[16 * 1024];
  size_t total = headers_sent;
  size_t offset = 0;
  while (offset < body_length) {
    size_t remaining = body_length - offset;
    size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    ssize_t count;
    do {
      count = pread(file_fd, buffer, chunk, (off_t)offset);
    } while (count < 0 && errno == EINTR);
    if (count <= 0)
      break;
    size_t written = send_all(transport, buffer, (size_t)count);
    total += written;
    offset += written;
    if (written != (size_t)count)
      break;
  }
  return total;
}
