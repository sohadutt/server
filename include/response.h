#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>

size_t http_response_send(int client, int status_code, const char *reason_phrase,
                          const char *content_type, const void *body,
                          size_t body_length);

#endif
