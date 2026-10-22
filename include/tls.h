#ifndef TLS_H
#define TLS_H

#include <openssl/ssl.h>

typedef struct TlsServer TlsServer;

TlsServer *tls_server_create(const char *certificate_file,
                             const char *private_key_file);
void tls_server_destroy(TlsServer *server);
SSL *tls_server_accept(TlsServer *server, int socket_fd);
void tls_connection_destroy(SSL *connection);

#endif
