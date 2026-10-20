#ifndef PRINT_API_H
#define PRINT_API_H

#include "config.h"
#include "request.h"

#include <stddef.h>

/* Handles POST /api/print and returns the number of response bytes written. */
size_t print_api_handle(int client, const HttpRequest *request,
                        const ServerConfig *config, size_t *bytes_received);

#endif
