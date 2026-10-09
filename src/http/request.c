#include "request.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "logger.h"

int http_request_receive(int client, HttpRequest *request) {
  if (client < 0 || request == NULL) {
    errno = EINVAL;
    return -1;
  }

  request->length = 0;
  ssize_t received;
  do {
    received = recv(client, request->data, sizeof(request->data) - 1, 0);
  } while (received < 0 && errno == EINTR);

  if (received < 0) return -1;
  request->length = (size_t)received;
  request->data[request->length] = '\0';
  return received == 0 ? 0 : 1;
}

void http_request_log_debug(const HttpRequest *request) {
  if (request == NULL || request->length == 0) {
    log_debug("Client message: <empty>");
    return;
  }

  char message[HTTP_REQUEST_CAPACITY + 64];
  int prefix_length = snprintf(message, sizeof(message),
                               "Client message (%zu bytes):\n",
                               request->length);
  if (prefix_length < 0 || (size_t)prefix_length >= sizeof(message)) return;

  size_t output = (size_t)prefix_length;
  for (size_t input = 0; input < request->length &&
                          output < sizeof(message) - 1;
       input++) {
    unsigned char character = (unsigned char)request->data[input];
    message[output++] = character == '\r' || character == '\n' ||
                                character == '\t' ||
                                (character >= 0x20 && character < 0x7f)
                            ? (char)character
                            : '?';
  }
  message[output] = '\0';
  log_debug(message);
}
