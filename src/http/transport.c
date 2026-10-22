#include "transport.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

int http_transport_wait_readable(HttpTransport *transport, int timeout_ms) {
  if (transport == NULL || transport->socket_fd < 0) {
    errno = EINVAL;
    return -1;
  }
  if (transport->tls != NULL && SSL_pending(transport->tls) > 0)
    return 1;
  struct pollfd descriptor = {.fd = transport->socket_fd, .events = POLLIN};
  int result;
  do {
    result = poll(&descriptor, 1, timeout_ms);
  } while (result < 0 && errno == EINTR);
  return result;
}

static int wait_for_ssl_event(HttpTransport *transport, short event) {
  struct pollfd descriptor = {.fd = transport->socket_fd, .events = event};
  int result;
  do {
    result = poll(&descriptor, 1, 15000);
  } while (result < 0 && errno == EINTR);
  if (result == 0)
    errno = ETIMEDOUT;
  return result;
}

ssize_t http_transport_read(HttpTransport *transport, void *buffer,
                            size_t capacity) {
  if (transport == NULL || transport->socket_fd < 0 || buffer == NULL ||
      capacity == 0) {
    errno = EINVAL;
    return -1;
  }
  if (transport->tls == NULL)
    return recv(transport->socket_fd, buffer, capacity, 0);

  for (;;) {
    size_t received = 0;
    ERR_clear_error();
    int result = SSL_read_ex(transport->tls, buffer, capacity, &received);
    if (result == 1)
      return (ssize_t)received;
    int ssl_error = SSL_get_error(transport->tls, result);
    if (ssl_error == SSL_ERROR_ZERO_RETURN)
      return 0;
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
      short event = ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
      if (wait_for_ssl_event(transport, event) > 0)
        continue;
      return -1;
    }
    if (ssl_error == SSL_ERROR_SYSCALL && errno == 0)
      errno = ECONNRESET;
    else if (ssl_error != SSL_ERROR_SYSCALL)
      errno = EPROTO;
    return -1;
  }
}

ssize_t http_transport_write(HttpTransport *transport, const void *buffer,
                             size_t length) {
  if (transport == NULL || transport->socket_fd < 0 ||
      (buffer == NULL && length != 0)) {
    errno = EINVAL;
    return -1;
  }
  if (length == 0)
    return 0;
  if (transport->tls == NULL) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return send(transport->socket_fd, buffer, length, flags);
  }
  for (;;) {
    size_t sent = 0;
    ERR_clear_error();
    int result = SSL_write_ex(transport->tls, buffer, length, &sent);
    if (result == 1)
      return (ssize_t)sent;
    int ssl_error = SSL_get_error(transport->tls, result);
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
      short event = ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
      if (wait_for_ssl_event(transport, event) > 0)
        continue;
      return -1;
    }
    if (ssl_error == SSL_ERROR_SYSCALL && errno == 0)
      errno = EPIPE;
    else if (ssl_error != SSL_ERROR_SYSCALL)
      errno = EPROTO;
    return -1;
  }
}
