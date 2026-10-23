#include "print_api.h"

#include <cups/cups.h>
#include <errno.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
  char batch_id[DATA_PRINT_BATCH_CAPACITY];
  char document_name[DATA_PRINT_NAME_CAPACITY];
  char request_id[DATA_PRINT_REQUEST_CAPACITY];
  char pages_applied[8];
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

static int ascii_digit(unsigned char c) { return c >= '0' && c <= '9'; }

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
      {.name = "content-type",
       .value = headers->content_type,
       .capacity = sizeof(headers->content_type)},
      {.name = "authorization",
       .value = headers->authorization,
       .capacity = sizeof(headers->authorization)},
      {.name = "x-print-copies",
       .value = headers->copies,
       .capacity = sizeof(headers->copies)},
      {.name = "x-print-sides",
       .value = headers->sides,
       .capacity = sizeof(headers->sides)},
      {.name = "x-print-pages",
       .value = headers->pages,
       .capacity = sizeof(headers->pages)},
      {.name = "x-print-batch-id",
       .value = headers->batch_id,
       .capacity = sizeof(headers->batch_id)},
      {.name = "x-print-name",
       .value = headers->document_name,
       .capacity = sizeof(headers->document_name)},
      {.name = "x-print-request-id",
       .value = headers->request_id,
       .capacity = sizeof(headers->request_id)},
      {.name = "x-print-pages-applied",
       .value = headers->pages_applied,
       .capacity = sizeof(headers->pages_applied)},
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
    while (value_start < line_end && (request->data[value_start] == ' ' ||
                                      request->data[value_start] == '\t'))
      value_start++;
    size_t value_end = line_end;
    while (value_end > value_start && (request->data[value_end - 1] == ' ' ||
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
    } else if (name_length == 17 && strncasecmp(request->data + position,
                                                "transfer-encoding", 17) == 0) {
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

static int normalize_batch_id(const char *input, char output[65]) {
  size_t length = strlen(input);
  if (length == 0) {
    unsigned char random_bytes[16];
    static const char hex[] = "0123456789abcdef";
    if (RAND_bytes(random_bytes, sizeof(random_bytes)) != 1)
      return -1;
    for (size_t i = 0; i < sizeof(random_bytes); i++) {
      output[i * 2] = hex[random_bytes[i] >> 4];
      output[i * 2 + 1] = hex[random_bytes[i] & 0x0fU];
    }
    output[32] = '\0';
    OPENSSL_cleanse(random_bytes, sizeof(random_bytes));
    return 0;
  }
  if (length >= DATA_PRINT_BATCH_CAPACITY)
    return -1;
  for (size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)input[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-'))
      return -1;
  }
  memcpy(output, input, length + 1);
  return 0;
}

static void normalize_document_name(const char *input, char output[256]) {
  size_t used = 0;
  const char *base = input;
  for (const char *cursor = input; *cursor != '\0'; cursor++) {
    if (*cursor == '/' || *cursor == '\\')
      base = cursor + 1;
  }
  for (const unsigned char *cursor = (const unsigned char *)base;
       *cursor != '\0' && used < 254; cursor++) {
    unsigned char c = *cursor;
    output[used++] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                      c == '_')
                         ? (char)c
                         : '_';
  }
  while (used > 0 && output[used - 1] == '.')
    used--;
  if (used == 0)
    memcpy(output, "document", sizeof("document"));
  else
    output[used] = '\0';
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
    int digit = (int)(*cursor - '0');
    if (parsed > (99 - digit) / 10)
      return -1;
    parsed = parsed * 10 + digit;
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

static int wait_for_upload(HttpTransport *transport,
                           const struct timespec *deadline) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return -1;
  long long ms =
      ((long long)deadline->tv_sec - now.tv_sec) * 1000LL +
      ((long long)deadline->tv_nsec - now.tv_nsec + 999999LL) / 1000000LL;
  if (ms <= 0)
    return -1;
  int ready =
      http_transport_wait_readable(transport, ms > INT_MAX ? INT_MAX : (int)ms);
  return ready > 0 ? 0 : -1;
}

static int spool_request_body(HttpTransport *transport,
                              const HttpRequest *request,
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
    if (wait_for_upload(transport, &deadline) != 0)
      return -1;
    size_t remaining = headers->content_length - copied;
    size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    ssize_t received;
    do {
      received = http_transport_read(transport, buffer, wanted);
    } while (received < 0 && errno == EINTR);
    if (received <= 0 ||
        fwrite(buffer, 1, (size_t)received, file) != (size_t)received)
      return -1;
    copied += (size_t)received;
    if (body_bytes_received != NULL)
      *body_bytes_received = copied;
  }
  return fflush(file) == 0 && fseek(file, 0, SEEK_SET) == 0 ? 0 : -1;
}

static int upload_signature_matches(FILE *file, const char *media_type) {
  if (file == NULL || media_type == NULL || fseek(file, 0, SEEK_SET) != 0)
    return 0;

  unsigned char prefix[1024];
  size_t count = fread(prefix, 1, sizeof(prefix), file);
  int valid = 0;
  if (strcmp(media_type, "application/pdf") == 0) {
    static const unsigned char marker[] = "%PDF-";
    for (size_t i = 0; i + sizeof(marker) - 1 <= count; i++) {
      if (memcmp(prefix + i, marker, sizeof(marker) - 1) == 0) {
        valid = 1;
        break;
      }
    }
  } else if (strcmp(media_type, "image/jpeg") == 0) {
    valid = count >= 3 && prefix[0] == 0xff && prefix[1] == 0xd8 &&
            prefix[2] == 0xff;
  } else if (strcmp(media_type, "image/png") == 0) {
    static const unsigned char signature[] = {0x89, 'P',  'N',  'G',
                                              0x0d, 0x0a, 0x1a, 0x0a};
    valid = count >= sizeof(signature) &&
            memcmp(prefix, signature, sizeof(signature)) == 0;
  }
  OPENSSL_cleanse(prefix, sizeof(prefix));
  return valid && fseek(file, 0, SEEK_SET) == 0;
}

static size_t send_json(HttpTransport *transport, int status,
                        const char *reason, const char *json) {
  return http_response_send(transport, status, reason,
                            "application/json; charset=utf-8", json,
                            strlen(json));
}

static void make_cups_job_title(char *title, size_t capacity,
                                const DataUser *user,
                                const PrintRequestHeaders *headers) {
  if (title == NULL || capacity == 0 || user == NULL || headers == NULL)
    return;
  char prefix[128];
  int prefix_length = snprintf(prefix, sizeof(prefix), "serverjob:%lld:%s:",
                               (long long)user->id, headers->request_id);
  if (prefix_length < 0 || (size_t)prefix_length >= sizeof(prefix)) {
    title[0] = '\0';
    return;
  }
  size_t used = (size_t)prefix_length;
  size_t name_length = strlen(headers->document_name);
  if (used >= capacity)
    used = capacity - 1;
  memcpy(title, prefix, used);
  size_t available = capacity - used - 1;
  if (name_length > available)
    name_length = available;
  memcpy(title + used, headers->document_name, name_length);
  title[used + name_length] = '\0';
}

/*
 * Recover the CUPS-accepted/SQLite-not-yet-recorded crash window on a retry.
 * CUPS must retain the job in its history for this best-effort lookup to work.
 */
static int recover_cups_job(const DataUser *user,
                            const PrintRequestHeaders *headers, int copies,
                            const char *media_type, DataStore *data_store,
                            char *printer_name, size_t printer_capacity,
                            int *cups_job_id, int64_t *stored_job_id) {
  char prefix[128];
  int written = snprintf(prefix, sizeof(prefix), "serverjob:%lld:%s:",
                         (long long)user->id, headers->request_id);
  if (written < 0 || (size_t)written >= sizeof(prefix))
    return -1;

  int result = -1;
  cups_dest_t *destinations = NULL;
  int destination_count = cupsGetDests2(CUPS_HTTP_DEFAULT, &destinations);
  for (int destination_index = 0;
       destination_count > 0 && destinations != NULL &&
       destination_index < destination_count && result != 0;
       destination_index++) {
    const char *destination_name = destinations[destination_index].name;
    if (destination_name == NULL)
      continue;
    cups_job_t *jobs = NULL;
    int job_count = cupsGetJobs2(CUPS_HTTP_DEFAULT, &jobs, destination_name,
                                 0, CUPS_WHICHJOBS_ALL);
    for (int index = 0; job_count > 0 && jobs != NULL && index < job_count;
         index++) {
      cups_job_t *job = &jobs[index];
      if (job->title == NULL || job->dest == NULL ||
          strncmp(job->title, prefix, (size_t)written) != 0 ||
          strlen(job->dest) >= printer_capacity)
        continue;
      DataResult recorded = data_print_job_record(
          data_store, user->phone_e164, job->id, job->dest, media_type, copies,
          headers->sides[0] != '\0' ? headers->sides : "one-sided",
          headers->pages_seen ? headers->pages : "", headers->batch_id,
          headers->document_name, headers->request_id, stored_job_id);
      if (recorded == DATA_DUPLICATE) {
        DataPrintJob existing = {0};
        if (data_print_job_find_request(data_store, user->phone_e164,
                                        headers->request_id,
                                        &existing) == DATA_OK &&
            existing.cups_job_id == job->id &&
            strcmp(existing.printer, job->dest) == 0) {
          *stored_job_id = existing.id;
          recorded = DATA_OK;
        }
        OPENSSL_cleanse(&existing, sizeof(existing));
      }
      if (recorded == DATA_OK) {
        memcpy(printer_name, job->dest, strlen(job->dest) + 1);
        *cups_job_id = job->id;
        result = 0;
      }
      break;
    }
    if (jobs != NULL && job_count > 0)
      cupsFreeJobs(job_count, jobs);
  }
  if (destinations != NULL && destination_count > 0)
    cupsFreeDests(destination_count, destinations);
  return result;
}

static int submit_document(FILE *file, const PrintRequestHeaders *headers,
                           int copies, const char *media_type,
                           const char *phone_e164, DataStore *data_store,
                           char *printer_name, size_t printer_name_capacity,
                           int *job_id, int64_t *stored_job_id) {
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
  if (selected->name == NULL || strlen(selected->name) >= printer_name_capacity)
    goto cleanup;
  destination =
      cupsGetNamedDest(CUPS_HTTP_DEFAULT, selected->name, selected->instance);
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
  if (headers->pages_seen && strcmp(headers->pages_applied, "true") != 0) {
    previous_count = option_count;
    option_count =
        cupsAddOption("page-ranges", headers->pages, option_count, &options);
    if (option_count != previous_count + 1)
      goto cleanup;
  }

  for (int i = 0; i < option_count; i++) {
    if (!cupsCheckDestSupported(CUPS_HTTP_DEFAULT, destination, info,
                                options[i].name, options[i].value))
      goto cleanup;
  }

  char cups_title[256];
  DataUser title_user = {0};
  if (data_user_get(data_store, phone_e164, &title_user) != DATA_OK)
    goto cleanup;
  make_cups_job_title(cups_title, sizeof(cups_title), &title_user, headers);
  OPENSSL_cleanse(&title_user, sizeof(title_user));
  if (cups_title[0] == '\0')
    goto cleanup;
  ipp_status_t status =
      cupsCreateDestJob(CUPS_HTTP_DEFAULT, destination, info, job_id,
                        cups_title, option_count, options);
  OPENSSL_cleanse(cups_title, sizeof(cups_title));
  if (status != IPP_STATUS_OK && status != IPP_STATUS_OK_IGNORED_OR_SUBSTITUTED)
    goto cleanup;
  created = 1;
  if (cupsStartDestDocument(CUPS_HTTP_DEFAULT, destination, info, *job_id,
                            headers->document_name, media_type, 0, NULL,
                            1) != HTTP_STATUS_CONTINUE)
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
  if (ferror(file) || (finish_status != IPP_STATUS_OK &&
                       finish_status != IPP_STATUS_OK_IGNORED_OR_SUBSTITUTED))
    goto cleanup;

  memcpy(printer_name, selected->name, strlen(selected->name) + 1);
  DataResult record_result = data_print_job_record(
      data_store, phone_e164, *job_id, selected->name, media_type, copies,
      sides, headers->pages_seen ? headers->pages : "", headers->batch_id,
      headers->document_name, headers->request_id, stored_job_id);
  if (record_result != DATA_OK) {
    log_errorf("CUPS job %d was accepted but its database record failed (%d)",
               *job_id, record_result);
    goto cleanup;
  }
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

static size_t send_job_result(HttpTransport *transport, int status,
                              const char *reason, int64_t stored_job_id,
                              int cups_job_id, const char *printer,
                              const char *batch_id) {
  char escaped_name[512];
  char escaped_batch[DATA_PRINT_BATCH_CAPACITY * 2];
  if (json_escape(escaped_name, sizeof(escaped_name), printer) == 0 ||
      json_escape(escaped_batch, sizeof(escaped_batch), batch_id) == 0)
    return send_json(transport, 500, "Internal Server Error",
                     "{\"error\":\"Print response encoding failed\"}");
  char body[1024];
  snprintf(body, sizeof(body),
           "{\"job_id\":%lld,\"cups_job_id\":%d,\"printer\":\"%s\","
           "\"batch_id\":\"%s\"}",
           (long long)stored_job_id, cups_job_id, escaped_name, escaped_batch);
  return send_json(transport, status, reason, body);
}

static const char *cups_job_status(ipp_jstate_t state) {
  switch (state) {
  case IPP_JSTATE_PENDING:
  case IPP_JSTATE_HELD:
    return "submitted";
  case IPP_JSTATE_PROCESSING:
    return "processing";
  case IPP_JSTATE_COMPLETED:
    return "completed";
  case IPP_JSTATE_CANCELED:
    return "canceled";
  case IPP_JSTATE_ABORTED:
  case IPP_JSTATE_STOPPED:
  default:
    return "failed";
  }
}

static int stored_job_is_terminal(const DataPrintJob *job) {
  return job != NULL &&
         (strcmp(job->status, "completed") == 0 ||
          strcmp(job->status, "canceled") == 0 ||
          strcmp(job->status, "failed") == 0);
}

void print_api_sync_jobs(DataStore *data_store, const char *phone_e164) {
  if (data_store == NULL || phone_e164 == NULL)
    return;
  DataPrintJob *stored_jobs = NULL;
  size_t stored_count = 0;
  if (data_print_job_list(data_store, phone_e164, &stored_jobs,
                          &stored_count) != DATA_OK)
    return;
  for (size_t i = 0; i < stored_count; i++) {
    if (stored_job_is_terminal(&stored_jobs[i]))
      continue;
    int printer_seen = 0;
    for (size_t previous = 0; previous < i; previous++) {
      if (strcmp(stored_jobs[previous].printer, stored_jobs[i].printer) == 0) {
        printer_seen = 1;
        break;
      }
    }
    if (printer_seen)
      continue;

    /* Keep CUPS result memory limited to one printer at a time. */
    cups_job_t *cups_jobs = NULL;
    int cups_count = cupsGetJobs2(CUPS_HTTP_DEFAULT, &cups_jobs,
                                  stored_jobs[i].printer, 0,
                                  CUPS_WHICHJOBS_ALL);
    if (cups_count > 0 && cups_jobs != NULL) {
      for (size_t job_index = i; job_index < stored_count; job_index++) {
        if (stored_job_is_terminal(&stored_jobs[job_index]))
          continue;
        if (strcmp(stored_jobs[job_index].printer,
                   stored_jobs[i].printer) != 0)
          continue;
        for (int cups_index = 0; cups_index < cups_count; cups_index++) {
          if (cups_jobs[cups_index].id == stored_jobs[job_index].cups_job_id &&
              cups_jobs[cups_index].dest != NULL &&
              strcmp(cups_jobs[cups_index].dest,
                     stored_jobs[job_index].printer) == 0) {
            (void)data_print_job_set_status(
                data_store, phone_e164, stored_jobs[job_index].id,
                cups_job_status(cups_jobs[cups_index].state));
            break;
          }
        }
      }
    }
    if (cups_jobs != NULL && cups_count > 0)
      cupsFreeJobs(cups_count, cups_jobs);
  }
  data_print_job_list_free(stored_jobs);
}

DataResult print_api_cancel_job(DataStore *data_store, const char *phone_e164,
                                int64_t job_id) {
  DataPrintJob job = {0};
  DataResult result =
      data_print_job_get(data_store, phone_e164, job_id, &job);
  if (result != DATA_OK)
    return result;
  if (strcmp(job.status, "completed") == 0 ||
      strcmp(job.status, "canceled") == 0 ||
      strcmp(job.status, "failed") == 0)
    return DATA_INVALID;
  ipp_status_t status = cupsCancelJob2(CUPS_HTTP_DEFAULT, job.printer,
                                       job.cups_job_id, 0);
  if (status != IPP_STATUS_OK &&
      status != IPP_STATUS_OK_IGNORED_OR_SUBSTITUTED)
    return DATA_ERROR;
  return data_print_job_set_status(data_store, phone_e164, job_id, "canceled");
}

size_t print_api_handle(HttpTransport *transport, const HttpRequest *request,
                        const ServerConfig *config, DataStore *data_store,
                        size_t *bytes_received) {
  if (transport == NULL || transport->socket_fd < 0 || request == NULL ||
      config == NULL)
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
  } else if (transport->tls == NULL) {
    failure = "HTTPS is required to submit print jobs";
    status = 426;
    reason = "Upgrade Required";
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

  if (failure == NULL &&
      (normalize_batch_id(headers.batch_id, headers.batch_id) != 0 ||
       normalize_batch_id(headers.request_id, headers.request_id) != 0)) {
    failure = "Invalid print batch or request identifier";
    status = 400;
    reason = "Bad Request";
  }
  if (failure == NULL) {
    char supplied_name[DATA_PRINT_NAME_CAPACITY];
    memcpy(supplied_name, headers.document_name, sizeof(supplied_name));
    normalize_document_name(supplied_name, headers.document_name);
    OPENSSL_cleanse(supplied_name, sizeof(supplied_name));
    DataPrintJob previous = {0};
    DataResult previous_result = data_print_job_find_request(
        data_store, user.phone_e164, headers.request_id, &previous);
    if (previous_result == DATA_OK) {
      response_bytes = send_job_result(transport, 200, "OK", previous.id,
                                       previous.cups_job_id, previous.printer,
                                       previous.batch_id);
      OPENSSL_cleanse(&previous, sizeof(previous));
      OPENSSL_cleanse(&user, sizeof(user));
      OPENSSL_cleanse(token, sizeof(token));
      return response_bytes;
    }
    OPENSSL_cleanse(&previous, sizeof(previous));
    if (previous_result != DATA_NOT_FOUND) {
      failure = "Could not check print request status";
      status = 500;
      reason = "Internal Server Error";
    }
  }

  if (failure == NULL && pthread_mutex_trylock(&print_submission_mutex) != 0) {
    OPENSSL_cleanse(token, sizeof(token));
    OPENSSL_cleanse(&user, sizeof(user));
    return send_json(
        transport, 503, "Service Unavailable",
        "{\"error\":\"A print request is already being processed\"}");
  }
  if (failure != NULL) {
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", failure);
    response_bytes = send_json(transport, status, reason, body);
    OPENSSL_cleanse(token, sizeof(token));
    OPENSSL_cleanse(&user, sizeof(user));
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
  if (failure == NULL && headers.pages_applied[0] != '\0' &&
      strcmp(headers.pages_applied, "true") != 0 &&
      strcmp(headers.pages_applied, "false") != 0) {
    failure = "Invalid page-range processing flag";
    status = 400;
    reason = "Bad Request";
  }
  if (failure == NULL && (upload = tmpfile()) == NULL) {
    failure = "Could not prepare document upload";
    status = 500;
    reason = "Internal Server Error";
  }
  if (failure == NULL) {
    size_t body_bytes = 0;
    int spooled =
        spool_request_body(transport, request, &headers, upload, &body_bytes);
    if (bytes_received != NULL)
      *bytes_received = headers.body_offset + body_bytes;
    if (spooled != 0) {
      failure = "Document upload was incomplete or timed out";
      status = 400;
      reason = "Bad Request";
    } else if (!upload_signature_matches(upload, media_type)) {
      failure = "The uploaded file does not match its declared format";
      status = 415;
      reason = "Unsupported Media Type";
    }
  }

  char printer_name[256] = {0};
  int job_id = 0;
  int64_t stored_job_id = 0;
  int recovered = 0;
  if (failure == NULL) {
    recovered = recover_cups_job(&user, &headers, copies, media_type,
                                 data_store, printer_name,
                                 sizeof(printer_name), &job_id,
                                 &stored_job_id) == 0;
    if (!recovered &&
        submit_document(upload, &headers, copies, media_type,
                        user.phone_e164, data_store, printer_name,
                        sizeof(printer_name), &job_id, &stored_job_id) != 0) {
      log_warn("Print submission failed; inspect the request-scoped details");
      failure = "No available printer or CUPS submission failed";
      status = 503;
      reason = "Service Unavailable";
    }
  }

  if (upload != NULL)
    fclose(upload);
  if (failure != NULL) {
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", failure);
    response_bytes = send_json(transport, status, reason, body);
  } else {
    response_bytes = send_job_result(transport, recovered ? 200 : 201,
                                     recovered ? "OK" : "Created", stored_job_id,
                                     job_id, printer_name, headers.batch_id);
    log_infof("Submitted print job %d to printer '%s' (stored job %lld)",
              job_id, printer_name, (long long)stored_job_id);
  }
  pthread_mutex_unlock(&print_submission_mutex);
  OPENSSL_cleanse(token, sizeof(token));
  OPENSSL_cleanse(&user, sizeof(user));
  return response_bytes;
}
