#ifndef SERVER_CONNECTION_H
#define SERVER_CONNECTION_H

#include <sys/socket.h>

#include "config.h"
#include "data.h"
#include "static.h"
#include "tls.h"
#include "router.h"

void connection_handle(int client, const struct sockaddr *peer_address,
                       socklen_t peer_address_length,
                       const ServerConfig *config,
                       const StaticFileServer *static_files,
                       DataStore *data_store, TlsServer *tls_server,
                       const HttpRouter *router);

#endif
