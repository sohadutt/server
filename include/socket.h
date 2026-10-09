#ifndef SERVER_SOCKET_H
#define SERVER_SOCKET_H

#include <sys/socket.h>

int server_socket_listen(const char *host, unsigned short port, int backlog);
int server_socket_accept(int listener, struct sockaddr_storage *peer_address,
                         socklen_t *peer_address_length);

#endif
