#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

static char *trim(char *value) {
  while (*value == ' ' || *value == '\t' || *value == '\r' ||
         *value == '\n') {
    value++;
  }

  char *end = value + strlen(value);
  while (end > value &&
         (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
          end[-1] == '\n')) {
    *--end = '\0';
  }
  return value;
}

static int copy_value(char *destination, size_t capacity, const char *value) {
  size_t length = strlen(value);
  if (length == 0 || length >= capacity) {
    return -1;
  }
  memcpy(destination, value, length + 1);
  return 0;
}

static int parse_int(const char *value, int minimum, int maximum, int *result) {
  char *end;
  errno = 0;
  long parsed = strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < minimum ||
      parsed > maximum || parsed < INT_MIN || parsed > INT_MAX) {
    return -1;
  }
  *result = (int)parsed;
  return 0;
}

static int parse_bool(const char *value, int *result) {
  if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
    *result = 1;
    return 0;
  }
  if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
    *result = 0;
    return 0;
  }
  return -1;
}

static int set_value(ServerConfig *config, const char *key,
                     const char *value) {
  if (strcmp(key, "port") == 0) {
    return parse_int(value, 1, 65535, &config->port);
  }
  if (strcmp(key, "host") == 0) {
    return copy_value(config->host, sizeof(config->host), value);
  }
  if (strcmp(key, "worker_threads") == 0) {
    return parse_int(value, 1, 1024, &config->worker_threads);
  }
  if (strcmp(key, "document_root") == 0) {
    return copy_value(config->document_root, sizeof(config->document_root),
                      value);
  }
  if (strcmp(key, "log_directory") == 0) {
    return copy_value(config->log_directory, sizeof(config->log_directory),
                      value);
  }
  if (strcmp(key, "log_level") == 0) {
    if (strcmp(value, "debug") == 0) config->log_level = LOG_DEBUG;
    else if (strcmp(value, "info") == 0) config->log_level = LOG_INFO;
    else if (strcmp(value, "warn") == 0) config->log_level = LOG_WARN;
    else if (strcmp(value, "error") == 0) config->log_level = LOG_ERROR;
    else return -1;
    return 0;
  }
  if (strcmp(key, "log_console") == 0) {
    return parse_bool(value, &config->log_console);
  }
  if (strcmp(key, "log_file") == 0) {
    return parse_bool(value, &config->log_file);
  }
  return -1;
}

void config_defaults(ServerConfig *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->log_level = LOG_INFO;
  config->log_console = 1;
  config->log_file = 1;
  config->port = 8080;
  config->worker_threads = 4;
  snprintf(config->host, sizeof(config->host), "%s", "0.0.0.0");
  snprintf(config->log_directory, sizeof(config->log_directory), "%s", "logs");
  snprintf(config->document_root, sizeof(config->document_root), "%s", "public");
}

int config_load(const char *filename, ServerConfig *config) {
  if (filename == NULL || config == NULL) return -1;

  FILE *file = fopen(filename, "r");
  if (file == NULL) {
    fprintf(stderr, "Cannot open config '%s': %s\n", filename,
            strerror(errno));
    return -1;
  }

  ServerConfig parsed;
  config_defaults(&parsed);
  char line[512];
  unsigned int line_number = 0;
  int result = 0;

  while (fgets(line, sizeof(line), file) != NULL) {
    line_number++;
    if (strchr(line, '\n') == NULL && !feof(file)) {
      fprintf(stderr, "%s:%u: line is too long\n", filename, line_number);
      result = -1;
      break;
    }

    char *content = trim(line);
    if (*content == '\0' || *content == '#') continue;

    char *separator = strchr(content, '=');
    if (separator == NULL) {
      fprintf(stderr, "%s:%u: expected key = value\n", filename, line_number);
      result = -1;
      break;
    }
    *separator = '\0';
    char *key = trim(content);
    char *value = trim(separator + 1);
    if (set_value(&parsed, key, value) != 0) {
      fprintf(stderr, "%s:%u: invalid or unknown setting '%s'\n", filename,
              line_number, key);
      result = -1;
      break;
    }
  }

  if (ferror(file)) {
    fprintf(stderr, "Failed reading config '%s': %s\n", filename,
            strerror(errno));
    result = -1;
  }
  fclose(file);

  if (result == 0) *config = parsed;
  return result;
}
