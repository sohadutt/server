#ifndef SERVER_CONNECTION_H
#define SERVER_CONNECTION_H

#include <stdint.h>
#include <sys/socket.h>

#include "config.h"
#include "data.h"
#include "static.h"
#include "tls.h"
#include "router.h"

typedef struct Connection Connection;

typedef struct ConnectionTimings {
  uint64_t tls_handshake_ns;
  uint64_t request_read_ns;
  uint64_t socket_write_ns;
  uint64_t socket_writable_wait_ns;
} ConnectionTimings;

Connection *connection_create(int client, const struct sockaddr *peer_address,
                              socklen_t peer_address_length,
                              uint64_t request_id, const ServerConfig *config,
                              const StaticFileServer *static_files,
                              DataStore *data_store, TlsServer *tls_server,
                              const HttpRouter *router);
/* Process one complete request. Returns 1 to wait for a keep-alive request. */
int connection_process_one(Connection *connection, ConnectionTimings *timings);
int connection_file_descriptor(const Connection *connection);
void connection_destroy(Connection *connection);

#endif
