#include "http.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (data == NULL || size == 0 || size >= HTTP_REQUEST_CAPACITY)
    return 0;

  HttpRequest request = {0};
  memcpy(request.data, data, size);
  request.length = size;
  HttpRequestLine line = {0};
  (void)http_request_line_parse(&request, &line);
  return 0;
}
