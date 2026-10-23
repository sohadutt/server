#include "request.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "logger.h"

#define HTTP_REQUEST_TIMEOUT_MS 10000
#define HTTP_REQUEST_DRAIN_LIMIT (64U * 1024U)

static size_t skip_line_ending(const HttpRequest *request, size_t position,
                               size_t limit);

static size_t request_header_length(const HttpRequest *request) {
  for (size_t i = 0; i + 3 < request->length; i++) {
    if (request->data[i] == '\r' && request->data[i + 1] == '\n' &&
        request->data[i + 2] == '\r' && request->data[i + 3] == '\n') {
      return i + 4;
    }
  }
  for (size_t i = 0; i + 1 < request->length; i++) {
    if (request->data[i] == '\n' && request->data[i + 1] == '\n') {
      return i + 2;
    }
  }
  return 0;
}

static int request_has_header_end(const HttpRequest *request) {
  return request_header_length(request) != 0;
}

static int is_header_name_character(unsigned char character) {
  return isalnum(character) || strchr("!#$%&'*+-.^_`|~", character) != NULL;
}

int http_request_headers_valid(const HttpRequest *request,
                               const char *http_version) {
  if (request == NULL || http_version == NULL)
    return 0;
  size_t limit = 0;
  for (size_t i = 0; i + 3 < request->length; i++) {
    if (memcmp(request->data + i, "\r\n\r\n", 4) == 0) {
      limit = i + 4;
      break;
    }
  }
  if (limit == 0)
    return 0;

  size_t position = 0;
  while (position + 1 < limit &&
         !(request->data[position] == '\r' &&
           request->data[position + 1] == '\n')) {
    unsigned char character = (unsigned char)request->data[position++];
    if (character < 0x20 || character == 0x7f)
      return 0;
  }
  if (position + 1 >= limit)
    return 0;
  position += 2;

  unsigned header_count = 0;
  unsigned host_count = 0;
  unsigned length_count = 0;
  while (position < limit - 2) {
    size_t line_end = position;
    while (line_end + 1 < limit &&
           !(request->data[line_end] == '\r' &&
             request->data[line_end + 1] == '\n')) {
      unsigned char character = (unsigned char)request->data[line_end];
      if (character == '\n' || character == '\r' || character == 0 ||
          (character < 0x20 && character != '\t') || character == 0x7f)
        return 0;
      line_end++;
    }
    if (line_end + 1 >= limit || line_end == position ||
        request->data[position] == ' ' || request->data[position] == '\t' ||
        ++header_count > 64)
      return 0;

    size_t colon = position;
    while (colon < line_end && request->data[colon] != ':') {
      if (!is_header_name_character((unsigned char)request->data[colon]))
        return 0;
      colon++;
    }
    if (colon == position || colon == line_end)
      return 0;
    size_t value_start = colon + 1;
    while (value_start < line_end &&
           (request->data[value_start] == ' ' ||
            request->data[value_start] == '\t'))
      value_start++;
    size_t value_end = line_end;
    while (value_end > value_start &&
           (request->data[value_end - 1] == ' ' ||
            request->data[value_end - 1] == '\t'))
      value_end--;
    size_t name_length = colon - position;

    if (name_length == 4 &&
        strncasecmp(request->data + position, "host", 4) == 0) {
      if (++host_count > 1 || value_start == value_end)
        return 0;
    } else if (name_length == sizeof("content-length") - 1 &&
               strncasecmp(request->data + position, "content-length",
                           name_length) == 0) {
      if (++length_count > 1 || value_start == value_end)
        return 0;
      for (size_t i = value_start; i < value_end; i++) {
        if (request->data[i] < '0' || request->data[i] > '9')
          return 0;
      }
    } else if (name_length == sizeof("transfer-encoding") - 1 &&
               strncasecmp(request->data + position, "transfer-encoding",
                           name_length) == 0) {
      return 0;
    }
    position = line_end + 2;
  }

  return strcmp(http_version, "HTTP/1.1") != 0 || host_count == 1;
}

int http_request_accepts_gzip(const HttpRequest *request) {
  if (request == NULL)
    return 0;

  size_t limit = request_header_length(request);
  if (limit == 0)
    return 0;

  size_t position = 0;
  while (position < limit && request->data[position] != '\r' &&
         request->data[position] != '\n')
    position++;
  position = skip_line_ending(request, position, limit);

  while (position < limit) {
    size_t end = position;
    while (end < limit && request->data[end] != '\r' &&
           request->data[end] != '\n')
      end++;
    if (end == position)
      break;

    size_t colon = position;
    while (colon < end && request->data[colon] != ':')
      colon++;
    if (colon < end && colon - position == sizeof("accept-encoding") - 1 &&
        strncasecmp(request->data + position, "accept-encoding",
                    sizeof("accept-encoding") - 1) == 0) {
      size_t item = colon + 1;
      while (item < end) {
        size_t item_end = item;
        while (item_end < end && request->data[item_end] != ',')
          item_end++;
        size_t coding_end = item;
        while (coding_end < item_end && request->data[coding_end] != ';')
          coding_end++;
        while (item < coding_end &&
               (request->data[item] == ' ' || request->data[item] == '\t'))
          item++;
        while (coding_end > item &&
               (request->data[coding_end - 1] == ' ' ||
                request->data[coding_end - 1] == '\t'))
          coding_end--;

        if (coding_end - item == sizeof("gzip") - 1 &&
            strncasecmp(request->data + item, "gzip", sizeof("gzip") - 1) ==
                0) {
          int quality_valid = 1;
          double quality = 1.0;
          size_t parameter = coding_end;
          while (parameter < item_end) {
            parameter++;
            while (parameter < item_end &&
                   (request->data[parameter] == ' ' ||
                    request->data[parameter] == '\t'))
              parameter++;
            size_t parameter_end = parameter;
            while (parameter_end < item_end &&
                   request->data[parameter_end] != ';')
              parameter_end++;
            size_t value_start = parameter;
            while (value_start < parameter_end &&
                   request->data[value_start] != '=')
              value_start++;
            if (value_start < parameter_end) {
              size_t name_end = value_start;
              while (name_end > parameter &&
                     (request->data[name_end - 1] == ' ' ||
                      request->data[name_end - 1] == '\t'))
                name_end--;
              value_start++;
              while (value_start < parameter_end &&
                     (request->data[value_start] == ' ' ||
                      request->data[value_start] == '\t'))
                value_start++;
              size_t value_end = parameter_end;
              while (value_end > value_start &&
                     (request->data[value_end - 1] == ' ' ||
                      request->data[value_end - 1] == '\t'))
                value_end--;
              if (name_end - parameter == 1 &&
                  (request->data[parameter] == 'q' ||
                   request->data[parameter] == 'Q')) {
                char quality_text[16];
                size_t quality_length = value_end - value_start;
                if (quality_length == 0 ||
                    quality_length >= sizeof(quality_text)) {
                  quality_valid = 0;
                } else {
                  memcpy(quality_text, request->data + value_start,
                         quality_length);
                  quality_text[quality_length] = '\0';
                  char *quality_end = NULL;
                  quality = strtod(quality_text, &quality_end);
                  if (quality_end == quality_text || *quality_end != '\0' ||
                      !(quality >= 0.0 && quality <= 1.0))
                    quality_valid = 0;
                }
              }
            }
            parameter = parameter_end;
          }
          if (quality_valid && quality > 0.0)
            return 1;
        }
        item = item_end + (item_end < end ? 1U : 0U);
      }
    }
    position = skip_line_ending(request, end, limit);
  }
  return 0;
}

static size_t skip_line_ending(const HttpRequest *request, size_t position,
                               size_t limit) {
  if (position < limit && request->data[position] == '\r') {
    position++;
  }
  if (position < limit && request->data[position] == '\n') {
    position++;
  }
  return position;
}

int http_request_is_bodyless(const HttpRequest *request) {
  if (request == NULL) {
    return 0;
  }

  size_t limit = request_header_length(request);
  if (limit == 0) {
    return 0;
  }

  size_t position = 0;
  while (position < limit && request->data[position] != '\r' &&
         request->data[position] != '\n') {
    position++;
  }
  position = skip_line_ending(request, position, limit);
  int content_length_seen = 0;

  while (position < limit) {
    size_t end = position;
    while (end < limit && request->data[end] != '\r' &&
           request->data[end] != '\n') {
      end++;
    }
    if (end == position) {
      return 1;
    }

    size_t colon = position;
    while (colon < end && request->data[colon] != ':') {
      if (!is_header_name_character((unsigned char)request->data[colon])) {
        return 0;
      }
      colon++;
    }
    if (colon == position || colon == end) {
      return 0;
    }

    size_t value_start = colon + 1;
    while (value_start < end &&
           (request->data[value_start] == ' ' ||
            request->data[value_start] == '\t')) {
      value_start++;
    }
    size_t value_end = end;
    while (value_end > value_start &&
           (request->data[value_end - 1] == ' ' ||
            request->data[value_end - 1] == '\t')) {
      value_end--;
    }
    size_t name_length = colon - position;

    if (name_length == sizeof("transfer-encoding") - 1 &&
        strncasecmp(request->data + position, "transfer-encoding",
                    name_length) == 0) {
      return 0;
    }
    if (name_length == sizeof("content-length") - 1 &&
        strncasecmp(request->data + position, "content-length",
                    name_length) == 0) {
      if (content_length_seen || value_start == value_end) {
        return 0;
      }
      content_length_seen = 1;
      for (size_t i = value_start; i < value_end; i++) {
        if (request->data[i] != '0') {
          return 0;
        }
      }
    }

    position = skip_line_ending(request, end, limit);
  }

  return 0;
}

static int remaining_timeout_ms(const struct timespec *deadline) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return -1;
  }

  long long remaining_ns =
      ((long long)deadline->tv_sec - (long long)now.tv_sec) * 1000000000LL +
      ((long long)deadline->tv_nsec - (long long)now.tv_nsec);
  if (remaining_ns <= 0) {
    return 0;
  }
  long long remaining_ms = (remaining_ns + 999999LL) / 1000000LL;
  return remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms;
}

int http_request_receive(HttpTransport *transport, HttpRequest *request) {
  if (transport == NULL || transport->socket_fd < 0 || request == NULL) {
    errno = EINVAL;
    return -1;
  }

  request->length = 0;
  struct timespec deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
    return -1;
  }
  deadline.tv_sec += HTTP_REQUEST_TIMEOUT_MS / 1000;

  while (!request_has_header_end(request)) {
    if (request->length == sizeof(request->data) - 1) {
      errno = EMSGSIZE;
      return -1;
    }

    int timeout_ms = remaining_timeout_ms(&deadline);
    if (timeout_ms <= 0) {
      if (timeout_ms == 0) {
        errno = ETIMEDOUT;
      }
      return -1;
    }

    int ready = http_transport_wait_readable(transport, timeout_ms);
    if (ready == 0) {
      errno = ETIMEDOUT;
      return -1;
    }
    if (ready < 0) {
      return -1;
    }

    ssize_t received;
    do {
      received = http_transport_read(
          transport, request->data + request->length,
          sizeof(request->data) - 1 - request->length);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
      return -1;
    }
    if (received == 0) {
      request->data[request->length] = '\0';
      return request->length == 0 ? 0 : -1;
    }

    request->length += (size_t)received;
    request->data[request->length] = '\0';
  }

  return 1;
}

int http_request_drain_body(HttpTransport *transport, const HttpRequest *request,
                            size_t *bytes_received) {
  if (transport == NULL || request == NULL || transport->socket_fd < 0)
    return -1;

  size_t end = request_header_length(request);
  if (end == 0)
    return -1;

  size_t content_length = 0;
  int length_seen = 0;
  size_t position = 0;
  while (position < end && request->data[position] != '\n')
    position++;
  if (position < end)
    position++;
  while (position < end) {
    size_t line_end = position;
    while (line_end < end && request->data[line_end] != '\r' &&
           request->data[line_end] != '\n')
      line_end++;
    if (line_end == position)
      break;
    size_t colon = position;
    while (colon < line_end && request->data[colon] != ':')
      colon++;
    if (colon == line_end)
      return -1;
    if (colon - position == sizeof("content-length") - 1 &&
        strncasecmp(request->data + position, "content-length",
                    sizeof("content-length") - 1) == 0) {
      if (length_seen)
        return -1;
      length_seen = 1;
      size_t value = colon + 1;
      while (value < line_end &&
             (request->data[value] == ' ' || request->data[value] == '\t'))
        value++;
      size_t value_end = line_end;
      while (value_end > value &&
             (request->data[value_end - 1] == ' ' ||
              request->data[value_end - 1] == '\t'))
        value_end--;
      if (value == value_end)
        return -1;
      for (size_t index = value; index < value_end; index++) {
        unsigned char character = (unsigned char)request->data[index];
        if (character < '0' || character > '9' ||
            content_length > (HTTP_REQUEST_DRAIN_LIMIT -
                              (size_t)(character - '0')) /
                                 10U) {
          errno = EMSGSIZE;
          return -1;
        }
        content_length = content_length * 10U + (size_t)(character - '0');
      }
    }
    while (line_end < end && (request->data[line_end] == '\r' ||
                              request->data[line_end] == '\n'))
      line_end++;
    position = line_end;
  }

  size_t received_total = request->length;
  if (bytes_received != NULL && *bytes_received > received_total)
    received_total = *bytes_received;
  size_t received_body = received_total > end ? received_total - end : 0;
  if (received_body > content_length)
    return -1;
  size_t remaining = content_length - received_body;
  unsigned char discard[1024];
  struct timespec deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
    return -1;
  deadline.tv_nsec += 250000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  while (remaining > 0) {
    int timeout_ms = remaining_timeout_ms(&deadline);
    if (timeout_ms <= 0) {
      if (timeout_ms == 0)
        errno = ETIMEDOUT;
      return -1;
    }
    int ready = http_transport_wait_readable(transport, timeout_ms);
    if (ready <= 0) {
      if (ready == 0)
        errno = ETIMEDOUT;
      return -1;
    }
    size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
    ssize_t count = http_transport_read(transport, discard, chunk);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return -1;
    remaining -= (size_t)count;
  }
  if (bytes_received != NULL)
    *bytes_received = end + content_length;
  return 0;
}

void http_request_log_debug(const HttpRequest *request) {
  if (request == NULL || request->length == 0) {
    log_debug("Client message: <empty>");
    return;
  }
  size_t logged_length = request_header_length(request);
  if (logged_length == 0) {
    logged_length = request->length;
  }
  char message[HTTP_REQUEST_CAPACITY + 64];
  int length = snprintf(message, sizeof(message),
                        "Client headers (%zu bytes):\n", logged_length);
  if (length < 0 || (size_t)length >= sizeof(message))
    return;
  size_t output = (size_t)length;
  size_t line_start = 0;
  int first_line = 1;
  while (line_start < logged_length && output < sizeof(message) - 1) {
    size_t line_end = line_start;
    while (line_end < logged_length && request->data[line_end] != '\r' &&
           request->data[line_end] != '\n') {
      line_end++;
    }

    size_t colon = line_start;
    while (colon < line_end && request->data[colon] != ':') {
      colon++;
    }
    size_t name_length = colon - line_start;
    static const char *const sensitive_headers[] = {
        "authorization", "proxy-authorization", "cookie", "set-cookie",
        "x-api-key", "api-key", "x-auth-token"};
    int redact = 0;
    if (!first_line && colon < line_end) {
      for (size_t i = 0;
           i < sizeof(sensitive_headers) / sizeof(sensitive_headers[0]); i++) {
        if (strlen(sensitive_headers[i]) == name_length &&
            strncasecmp(request->data + line_start, sensitive_headers[i],
                        name_length) == 0) {
          redact = 1;
          break;
        }
      }
    }

    size_t query_position = line_end;
    size_t query_end = line_end;
    if (first_line) {
      size_t method_end = line_start;
      while (method_end < line_end && request->data[method_end] != ' ')
        method_end++;
      size_t target_end = method_end < line_end ? method_end + 1 : line_end;
      while (target_end < line_end && request->data[target_end] != ' ')
        target_end++;
      for (size_t i = method_end + 1; i < target_end; i++) {
        if (request->data[i] == '?') {
          query_position = i;
          query_end = target_end;
          break;
        }
      }
    }

    size_t content_end = redact ? colon : line_end;
    for (size_t i = line_start; i < content_end && output < sizeof(message) - 1;
         i++) {
      if (first_line && i == query_position) {
        static const char redacted_query[] = "?<redacted>";
        size_t redacted_length = sizeof(redacted_query) - 1;
        if (redacted_length > sizeof(message) - 1 - output)
          break;
        memcpy(message + output, redacted_query, redacted_length);
        output += redacted_length;
        i = query_end - 1;
        continue;
      }
      unsigned char c = (unsigned char)request->data[i];
      if (c < 0x20 || c == 0x7f) {
        message[output++] = '?';
      } else {
        message[output++] = (char)c;
      }
    }
    if (redact && output < sizeof(message) - 1) {
      static const char redacted[] = ": <redacted>";
      size_t redacted_length = sizeof(redacted) - 1;
      if (redacted_length > sizeof(message) - 1 - output) {
        break;
      }
      memcpy(message + output, redacted, redacted_length);
      output += redacted_length;
    }

    while (line_end < logged_length &&
           (request->data[line_end] == '\r' || request->data[line_end] == '\n')) {
      if (output + 2 >= sizeof(message)) {
        break;
      }
      message[output++] = '\\';
      message[output++] = request->data[line_end++] == '\r' ? 'r' : 'n';
    }
    first_line = 0;
    line_start = line_end;
    if (line_start < request->length && request->data[line_start] == '\0') {
      break;
    }
  }
  message[output] = '\0';
  log_debug(message);
}
