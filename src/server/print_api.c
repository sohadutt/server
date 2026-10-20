#include "print_api.h"

#include <openssl/crypto.h>
#include <cups/cups.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "logger.h"
#include "printing.h"
#include "response.h"

#define PRINT_UPLOAD_MAX_BYTES (64U * 1024U * 1024U)
#define PRINT_UPLOAD_TIMEOUT_SECONDS 120
#define PRINT_HEADER_VALUE_CAPACITY 512U

static pthread_mutex_t print_submission_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
  const char *name;
  char *value;
  size_t capacity;
  int seen;
} HeaderValue;

typedef struct {
  size_t content_length;
  size_t body_offset;
  char content_type[PRINT_HEADER_VALUE_CAPACITY];
  char authorization[PRINT_HEADER_VALUE_CAPACITY];
  char copies[16];
  char sides[64];
  char pages[256];
  int pages_seen;
} PrintRequestHeaders;

static size_t header_end(const HttpRequest *request) {
  for (size_t i = 0; i + 3 < request->length; i++) {
    if (request->data[i] == '\r' && request->data[i + 1] == '\n' &&
        request->data[i + 2] == '\r' && request->data[i + 3] == '\n')
      return i + 4;
  }
  for (size_t i = 0; i + 1 < request->length; i++) {
    if (request->data[i] == '\n' && request->data[i + 1] == '\n')
      return i + 2;
  }
  return 0;
}

static int header_name_char(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int ascii_digit(unsigned char c) {
  return c >= '0' && c <= '9';
}

static int append_header_value(HeaderValue *header, const char *value,
                               size_t length) {
  if (header == NULL || header->seen || length == 0 ||
      length >= header->capacity)
    return -1;
  for (size_t i = 0; i < length; i++) {
    if ((unsigned char)value[i] < 32 && value[i] != '\t')
      return -1;
  }
  memcpy(header->value, value, length);
  header->value[length] = '\0';
  header->seen = 1;
  return 0;
}

static int parse_headers(const HttpRequest *request,
                         PrintRequestHeaders *headers) {
  size_t end = header_end(request);
  if (end == 0)
    return -1;
  headers->body_offset = end;
  HeaderValue values[] = {
      {.name = "content-type", .value = headers->content_type,
       .capacity = sizeof(headers->content_type)},
      {.name = "authorization", .value = headers->authorization,
       .capacity = sizeof(headers->authorization)},
      {.name = "x-print-copies", .value = headers->copies,
       .capacity = sizeof(headers->copies)},
      {.name = "x-print-sides", .value = headers->sides,
       .capacity = sizeof(headers->sides)},
      {.name = "x-print-pages", .value = headers->pages,
       .capacity = sizeof(headers->pages)},
  };

  size_t position = 0;
  while (position < end && request->data[position] != '\r' &&
         request->data[position] != '\n')
    position++;
  while (position < end &&
         (request->data[position] == '\r' || request->data[position] == '\n'))
    position++;

  int content_length_seen = 0;
  while (position < end) {
    size_t line_end = position;
    while (line_end < end && request->data[line_end] != '\r' &&
           request->data[line_end] != '\n')
      line_end++;
    if (line_end == position)
      break;
    size_t colon = position;
    while (colon < line_end && request->data[colon] != ':') {
      if (!header_name_char((unsigned char)request->data[colon]))
        return -1;
      colon++;
    }
    if (colon == position || colon == line_end)
      return -1;
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
    size_t value_length = value_end - value_start;

    if (name_length == 14 &&
        strncasecmp(request->data + position, "content-length", 14) == 0) {
      if (content_length_seen || value_length == 0)
        return -1;
      content_length_seen = 1;
      size_t parsed = 0;
      for (size_t i = value_start; i < value_end; i++) {
        unsigned char digit = (unsigned char)request->data[i];
        if (!ascii_digit(digit) ||
            parsed > (SIZE_MAX - (size_t)(digit - '0')) / 10)
          return -1;
        parsed = parsed * 10 + (size_t)(digit - '0');
      }
      headers->content_length = parsed;
    } else if (name_length == 17 &&
               strncasecmp(request->data + position, "transfer-encoding",
                           17) == 0) {
      return -1;
    } else {
      for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (name_length == strlen(values[i].name) &&
            strncasecmp(request->data + position, values[i].name,
                        name_length) == 0 &&
            append_header_value(&values[i], request->data + value_start,
                                value_length) != 0)
          return -1;
      }
    }
    while (line_end < end &&
           (request->data[line_end] == '\r' || request->data[line_end] == '\n'))
      line_end++;
    position = line_end;
  }
  headers->pages_seen = values[4].seen;
  return content_length_seen ? 0 : -1;
}

static int bearer_token(const char *header, char token[256]) {
  static const char prefix[] = "Bearer ";
  if (strncasecmp(header, prefix, 7) != 0 || strlen(header + 7) >= 256)
    return 0;
  memcpy(token, header + 7, strlen(header + 7) + 1);
  return token[0] != '\0';
}

static int parse_copies(const char *value, int *copies) {
  if (value[0] == '\0') {
    *copies = 1;
    return 0;
  }
  int parsed = 0;
  for (const unsigned char *cursor = (const unsigned char *)value; *cursor;
       cursor++) {
    if (!ascii_digit(*cursor))
      return -1;
    parsed = parsed * 10 + (int)(*cursor - '0');
    if (parsed > 99)
      return -1;
  }
  if (parsed < 1)
    return -1;
  *copies = parsed;
  return 0;
}

static int valid_pages(const char *value) {
  if (value[0] == '\0')
    return 0;
  const char *cursor = value;
  while (*cursor != '\0') {
    if (!ascii_digit((unsigned char)*cursor))
      return 0;
    unsigned long first = 0;
    while (ascii_digit((unsigned char)*cursor)) {
      unsigned long digit = (unsigned long)(*cursor++ - '0');
      if (first > ((unsigned long)INT_MAX - digit) / 10)
        return 0;
      first = first * 10 + digit;
    }
    if (first == 0)
      return 0;
    if (*cursor == '-') {
      cursor++;
      if (!ascii_digit((unsigned char)*cursor))
        return 0;
      unsigned long last = 0;
      while (ascii_digit((unsigned char)*cursor)) {
        unsigned long digit = (unsigned long)(*cursor++ - '0');
        if (last > ((unsigned long)INT_MAX - digit) / 10)
          return 0;
        last = last * 10 + digit;
      }
      if (last < first)
        return 0;
    }
    if (*cursor == '\0')
      return 1;
    if (*cursor++ != ',' || *cursor == '\0')
      return 0;
  }
  return 0;
}

static int wait_for_upload(int client, const struct timespec *deadline) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return -1;
  long long ms = ((long long)deadline->tv_sec - now.tv_sec) * 1000LL +
                 ((long long)deadline->tv_nsec - now.tv_nsec + 999999LL) /
                     1000000LL;
  if (ms <= 0)
    return -1;
  struct pollfd descriptor = {.fd = client, .events = POLLIN};
  int ready;
  do {
    ready = poll(&descriptor, 1, ms > INT_MAX ? INT_MAX : (int)ms);
  } while (ready < 0 && errno == EINTR);
  return ready > 0 ? 0 : -1;
}

static int spool_request_body(int client, const HttpRequest *request,
                              const PrintRequestHeaders *headers, FILE *file,
                              size_t *body_bytes_received) {
  struct timespec deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
    return -1;
  deadline.tv_sec += PRINT_UPLOAD_TIMEOUT_SECONDS;
  size_t copied = 0;
  if (request->length > headers->body_offset) {
    copied = request->length - headers->body_offset;
    if (copied > headers->content_length ||
        fwrite(request->data + headers->body_offset, 1, copied, file) != copied)
      return -1;
  }
  if (body_bytes_received != NULL)
    *body_bytes_received = copied;
  unsigned char buffer[16384];
  while (copied < headers->content_length) {
    if (wait_for_upload(client, &deadline) != 0)
      return -1;
    size_t remaining = headers->content_length - copied;
    size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    ssize_t received;
    do {
      received = recv(client, buffer, wanted, 0);
    } while (received < 0 && errno == EINTR);
    if (received <= 0 || fwrite(buffer, 1, (size_t)received, file) !=
                             (size_t)received)
      return -1;
    copied += (size_t)received;
    if (body_bytes_received != NULL)
      *body_bytes_received = copied;
  }
  return fflush(file) == 0 && fseek(file, 0, SEEK_SET) == 0 ? 0 : -1;
}

static size_t send_json(int client, int status, const char *reason,
                        const char *json) {
  return http_response_send(client, status, reason,
                            "application/json; charset=utf-8", json,
                            strlen(json));
}

static int submit_document(FILE *file, const PrintRequestHeaders *headers,
                           int copies, const char *media_type,
                           char *printer_name, size_t printer_name_capacity,
                           int *job_id) {
  PrintService service = {0};
  print_service_init(&service);
  cups_dest_t *destination = NULL;
  cups_dinfo_t *info = NULL;
  cups_option_t *options = NULL;
  int option_count = 0;
  int result = -1;
  int created = 0;
  Printer *selected = NULL;

  if (get_printers(&service) != 0 ||
      (selected = select_printer(&service)) == NULL)
    goto cleanup;
  if (selected->name == NULL ||
      strlen(selected->name) >= printer_name_capacity)
    goto cleanup;
  destination = cupsGetNamedDest(CUPS_HTTP_DEFAULT, selected->name,
                                 selected->instance);
  if (destination == NULL)
    goto cleanup;
  info = cupsCopyDestInfo(CUPS_HTTP_DEFAULT, destination);
  if (info == NULL)
    goto cleanup;

  char copies_text[16];
  snprintf(copies_text, sizeof(copies_text), "%d", copies);
  int previous_count = option_count;
  option_count = cupsAddOption("copies", copies_text, option_count, &options);
  if (option_count != previous_count + 1)
    goto cleanup;
  previous_count = option_count;
  const char *sides = headers->sides[0] != '\0' ? headers->sides : "one-sided";
  option_count = cupsAddOption("sides", sides, option_count, &options);
  if (option_count != previous_count + 1)
    goto cleanup;
  if (headers->pages_seen) {
    previous_count = option_count;
    option_count = cupsAddOption("page-ranges", headers->pages, option_count,
                                 &options);
    if (option_count != previous_count + 1)
      goto cleanup;
  }

  ipp_status_t status = cupsCreateDestJob(CUPS_HTTP_DEFAULT, destination, info,
                                          job_id, "Browser Print", option_count,
                                          options);
  if (status != IPP_STATUS_OK &&
      status != IPP_STATUS_OK_IGNORED_OR_SUBSTITUTED)
    goto cleanup;
  created = 1;
  if (cupsStartDestDocument(CUPS_HTTP_DEFAULT, destination, info, *job_id,
                            "uploaded-document", media_type, 0, NULL, 1) !=
      HTTP_STATUS_CONTINUE)
    goto cleanup;

  unsigned char buffer[65536];
  size_t bytes;
  while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (cupsWriteRequestData(CUPS_HTTP_DEFAULT, (const char *)buffer, bytes) !=
        HTTP_STATUS_CONTINUE)
      goto cleanup;
  }
  ipp_status_t finish_status =
      cupsFinishDestDocument(CUPS_HTTP_DEFAULT, destination, info);
  if (ferror(file) ||
      (finish_status != IPP_STATUS_OK &&
       finish_status != IPP_STATUS_OK_IGNORED_OR_SUBSTITUTED))
    goto cleanup;

  memcpy(printer_name, selected->name, strlen(selected->name) + 1);
  result = 0;

cleanup:
  if (created && result != 0)
    (void)cupsCancelDestJob(CUPS_HTTP_DEFAULT, destination, *job_id);
  cupsFreeOptions(option_count, options);
  if (info != NULL)
    cupsFreeDestInfo(info);
  if (destination != NULL)
    cupsFreeDests(1, destination);
  print_service_destroy(&service);
  return result;
}

static size_t json_escape(char *output, size_t capacity, const char *input) {
  size_t used = 0;
  for (const unsigned char *p = (const unsigned char *)input; *p != '\0'; p++) {
    if (*p == '"' || *p == '\\') {
      if (used + 2 >= capacity)
        return 0;
      output[used++] = '\\';
      output[used++] = (char)*p;
    } else if (*p < 32) {
      if (used + 6 >= capacity)
        return 0;
      int written = snprintf(output + used, capacity - used, "\\u%04x", *p);
      if (written != 6)
        return 0;
      used += 6;
    } else {
      if (used + 1 >= capacity)
        return 0;
      output[used++] = (char)*p;
    }
  }
  output[used] = '\0';
  return used;
}

size_t print_api_handle(int client, const HttpRequest *request,
                        const ServerConfig *config, DataStore *data_store,
                        size_t *bytes_received) {
  if (client < 0 || request == NULL || config == NULL)
    return 0;
  if (bytes_received != NULL)
    *bytes_received = request->length;

  size_t response_bytes = 0;
  PrintRequestHeaders headers = {0};
  FILE *upload = NULL;
  const char *failure = NULL;
  int status = 0;
  const char *reason = NULL;
  int copies = 1;
  const char *media_type = NULL;
  char token[256] = {0};
  DataUser user = {0};
  if (parse_headers(request, &headers) != 0) {
    failure = "Malformed print request headers";
    status = 400;
    reason = "Bad Request";
  } else if (!bearer_token(headers.authorization, token) ||
             data_token_authenticate(data_store, token, &user) != DATA_OK) {
    failure = "Login with a valid phone number and password first";
    status = 401;
    reason = "Unauthorized";
  } else if (headers.content_length == 0 ||
             headers.content_length > PRINT_UPLOAD_MAX_BYTES) {
    failure = "Document must be between 1 byte and 64 MiB";
    status = 413;
    reason = "Content Too Large";
  } else if (strcmp(headers.content_type, "application/pdf") == 0) {
    media_type = "application/pdf";
  } else if (strcmp(headers.content_type, "image/jpeg") == 0 ||
             strcmp(headers.content_type, "image/png") == 0) {
    media_type = headers.content_type;
  } else {
    failure = "Supported formats are PDF, JPEG, and PNG";
    status = 415;
    reason = "Unsupported Media Type";
  }

  if (failure == NULL && pthread_mutex_trylock(&print_submission_mutex) != 0) {
    OPENSSL_cleanse(token, sizeof(token));
    return send_json(client, 503, "Service Unavailable",
                     "{\"error\":\"A print request is already being processed\"}");
  }
  if (failure != NULL) {
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", failure);
    response_bytes = send_json(client, status, reason, body);
    OPENSSL_cleanse(token, sizeof(token));
    return response_bytes;
  }

  if (failure == NULL && parse_copies(headers.copies, &copies) != 0) {
    failure = "Copies must be between 1 and 99";
    status = 400;
    reason = "Bad Request";
  }
  if (failure == NULL && headers.sides[0] != '\0' &&
      strcmp(headers.sides, "one-sided") != 0 &&
      strcmp(headers.sides, "two-sided-long-edge") != 0 &&
      strcmp(headers.sides, "two-sided-short-edge") != 0) {
    failure = "Unsupported duplex setting";
    status = 400;
    reason = "Bad Request";
  }
  if (failure == NULL && headers.pages_seen &&
      (strcmp(media_type, "application/pdf") != 0 ||
       !valid_pages(headers.pages))) {
    failure = "Page ranges require a PDF and use a format like 1-3,5";
    status = 400;
    reason = "Bad Request";
  }
  if (failure == NULL &&
      (upload = tmpfile()) == NULL) {
    failure = "Could not prepare document upload";
    status = 500;
    reason = "Internal Server Error";
  }
  if (failure == NULL) {
    size_t body_bytes = 0;
    int spooled = spool_request_body(client, request, &headers, upload,
                                     &body_bytes);
    if (bytes_received != NULL)
      *bytes_received = headers.body_offset + body_bytes;
    if (spooled != 0) {
      failure = "Document upload was incomplete or timed out";
      status = 400;
      reason = "Bad Request";
    }
  }

  char printer_name[256] = {0};
  int job_id = 0;
  if (failure == NULL &&
      submit_document(upload, &headers, copies, media_type, printer_name,
                      sizeof(printer_name), &job_id) != 0) {
    log_warn("CUPS could not accept the submitted print document");
    failure = "No available printer or CUPS submission failed";
    status = 503;
    reason = "Service Unavailable";
  }

  if (upload != NULL)
    fclose(upload);
  if (failure != NULL) {
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", failure);
    response_bytes = send_json(client, status, reason, body);
  } else {
    char escaped_name[512];
    char body[768];
    if (json_escape(escaped_name, sizeof(escaped_name), printer_name) == 0) {
      response_bytes = send_json(client, 500, "Internal Server Error",
                                 "{\"error\":\"Print response encoding failed\"}");
    } else {
      snprintf(body, sizeof(body),
               "{\"job_id\":%d,\"printer\":\"%s\"}", job_id,
               escaped_name);
      response_bytes = send_json(client, 201, "Created", body);
      char message[384];
      snprintf(message, sizeof(message), "Submitted print job %d", job_id);
      log_info(message);
    }
  }
  pthread_mutex_unlock(&print_submission_mutex);
  OPENSSL_cleanse(token, sizeof(token));
  return response_bytes;
}
