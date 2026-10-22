#ifndef API_H
#define API_H

#include "config.h"
#include "data.h"
#include "request.h"
#include "transport.h"

#include <stddef.h>

/* Handles application API paths. Returns 1 if the path was handled. */
int api_handle_request(HttpTransport *transport, const HttpRequest *request,
                       const char *method, const char *target,
                       const ServerConfig *config, DataStore *data_store,
                       size_t *bytes_received, size_t *bytes_sent);

#endif
