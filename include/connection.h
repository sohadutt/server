#ifndef SERVER_CONNECTION_H
#define SERVER_CONNECTION_H

#include <sys/socket.h>

#include "config.h"

void connection_handle(int client, const struct sockaddr *peer_address,
                       socklen_t peer_address_length,
                       const ServerConfig *config);

#endif
