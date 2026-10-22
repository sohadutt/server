#ifndef PRINT_API_H
#define PRINT_API_H

#include "config.h"
#include "data.h"
#include "request.h"
#include "transport.h"

#include <stddef.h>

/* Handles POST /api/print and returns the number of response bytes written. */
size_t print_api_handle(HttpTransport *transport, const HttpRequest *request,
                        const ServerConfig *config, DataStore *data_store,
                        size_t *bytes_received);

#endif
