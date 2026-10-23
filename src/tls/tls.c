#include "tls.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"

struct TlsServer {
  SSL_CTX *context;
};

/* This service speaks HTTP/1.1 only; do not negotiate an unsupported ALPN. */
static int select_http11(SSL *ssl, const unsigned char **out,
                         unsigned char *out_length, const unsigned char *input,
                         unsigned int input_length, void *argument) {
  (void)ssl;
  (void)argument;
  static const unsigned char http11[] = "http/1.1";
  size_t position = 0;
  while (position < input_length) {
    size_t protocol_length = input[position++];
    if (protocol_length == 0 || protocol_length > input_length - position)
      return SSL_TLSEXT_ERR_ALERT_FATAL;
    if (protocol_length == sizeof(http11) - 1 &&
        memcmp(input + position, http11, sizeof(http11) - 1) == 0) {
      *out = http11;
      *out_length = (unsigned char)(sizeof(http11) - 1);
      return SSL_TLSEXT_ERR_OK;
    }
    position += protocol_length;
  }
  return SSL_TLSEXT_ERR_NOACK;
}

TlsServer *tls_server_create(const char *certificate_file,
                             const char *private_key_file) {
  if (certificate_file == NULL || private_key_file == NULL ||
      certificate_file[0] == '\0' || private_key_file[0] == '\0')
    return NULL;

  TlsServer *server = calloc(1, sizeof(*server));
  if (server == NULL)
    return NULL;

  server->context = SSL_CTX_new(TLS_server_method());
  if (server->context == NULL)
    goto fail;

  if (SSL_CTX_set_min_proto_version(server->context, TLS1_2_VERSION) != 1)
    goto fail;
  SSL_CTX_set_options(server->context, SSL_OP_NO_COMPRESSION);
#ifdef SSL_OP_NO_RENEGOTIATION
  SSL_CTX_set_options(server->context, SSL_OP_NO_RENEGOTIATION);
#endif
  SSL_CTX_set_security_level(server->context, 2);
  SSL_CTX_set_mode(server->context, SSL_MODE_RELEASE_BUFFERS |
                                        SSL_MODE_ENABLE_PARTIAL_WRITE |
                                        SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  SSL_CTX_set_timeout(server->context, 300);
  SSL_CTX_set_session_cache_mode(server->context, SSL_SESS_CACHE_SERVER);
  static const unsigned char session_context[] = "low-level-server-http";
  if (SSL_CTX_set_session_id_context(server->context, session_context,
                                     sizeof(session_context) - 1) != 1)
    goto fail;
  if (SSL_CTX_set_max_early_data(server->context, 0) != 1)
    goto fail;
  SSL_CTX_set_alpn_select_cb(server->context, select_http11, NULL);

  if (SSL_CTX_use_certificate_chain_file(server->context, certificate_file) !=
          1 ||
      SSL_CTX_use_PrivateKey_file(server->context, private_key_file,
                                  SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_check_private_key(server->context) != 1)
    goto fail;

  return server;

fail: {
  unsigned long error = ERR_get_error();
  if (error != 0) {
    char message[256];
    ERR_error_string_n(error, message, sizeof(message));
    log_errorf("TLS context setup failed: %s", message);
  } else {
    log_error("TLS context setup failed without an OpenSSL error detail");
  }
  tls_server_destroy(server);
  return NULL;
}
}

void tls_server_destroy(TlsServer *server) {
  if (server == NULL)
    return;
  SSL_CTX_free(server->context);
  free(server);
}

SSL *tls_server_accept(TlsServer *server, int socket_fd) {
  if (server == NULL || socket_fd < 0)
    return NULL;
  SSL *connection = SSL_new(server->context);
  if (connection == NULL) {
    unsigned long error = ERR_get_error();
    char detail[256];
    if (error != 0) {
      ERR_error_string_n(error, detail, sizeof(detail));
      log_debugf("Could not allocate TLS connection state: %s", detail);
    } else {
      log_debug("Could not allocate TLS connection state");
    }
    return NULL;
  }
  ERR_clear_error();
  if (SSL_set_fd(connection, socket_fd) != 1) {
    unsigned long error = ERR_get_error();
    char detail[256];
    if (error != 0) {
      ERR_error_string_n(error, detail, sizeof(detail));
      log_debugf("Could not attach socket to TLS connection: %s", detail);
    } else {
      log_debug("Could not attach socket to TLS connection");
    }
    SSL_free(connection);
    return NULL;
  }
  errno = 0;
  int accept_result = SSL_accept(connection);
  if (accept_result != 1) {
    int socket_error = errno;
    int ssl_error = SSL_get_error(connection, accept_result);
    unsigned long error = ERR_get_error();
    if (error != 0) {
      char detail[256];
      ERR_error_string_n(error, detail, sizeof(detail));
      log_debugf("TLS handshake failed (SSL error %d): %s", ssl_error,
                 detail);
    } else if (socket_error != 0) {
      log_debugf("TLS handshake failed (SSL error %d): %s", ssl_error,
                 strerror(socket_error));
    } else {
      log_debugf("TLS handshake failed (SSL error %d)", ssl_error);
    }
    SSL_free(connection);
    return NULL;
  }
  return connection;
}

void tls_connection_destroy(SSL *connection) {
  if (connection == NULL)
    return;
  /* Send close_notify once without waiting for the peer's shutdown. */
  (void)SSL_shutdown(connection);
  SSL_free(connection);
}
