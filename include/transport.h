#ifndef HTTP_TRANSPORT_H
#define HTTP_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct ssl_st SSL;

typedef struct HttpTransport {
  int socket_fd;
  SSL *tls;
  int last_status_code;
  int gzip_enabled;
  int accepts_gzip;
  int keep_alive;
  uint64_t write_time_ns;
  uint64_t write_wait_ns;
} HttpTransport;

ssize_t http_transport_read(HttpTransport *transport, void *buffer,
                            size_t capacity);
ssize_t http_transport_write(HttpTransport *transport, const void *buffer,
                             size_t length);
int http_transport_wait_readable(HttpTransport *transport, int timeout_ms);

#endif
