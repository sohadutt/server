#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>
#include "transport.h"

size_t http_response_send(HttpTransport *transport, int status_code, const char *reason_phrase,
                          const char *content_type, const void *body,
                          size_t body_length);
size_t http_response_send_file(HttpTransport *transport, int status_code,
                               const char *reason_phrase,
                               const char *content_type, int file_fd,
                               size_t body_length);

#endif
