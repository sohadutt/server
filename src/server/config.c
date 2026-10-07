#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

static char *trim(char *str) {
  while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') {
    str++;
  }

  if (*str == '\0') {
    return str;
  }

  char *end = str + strlen(str) - 1;

  while (end > str &&
         (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
    *end = '\0';
    end--;
  }

  if (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
    *end = '\0';
  }

  return str;
}

static LogLevel parse_log_level(const char *value) {
  if (strcmp(value, "debug") == 0) {
    return LOG_DEBUG;
  }

  if (strcmp(value, "info") == 0) {
    return LOG_INFO;
  }

  if (strcmp(value, "warn") == 0) {
    return LOG_WARN;
  }

  if (strcmp(value, "error") == 0) {
    return LOG_ERROR;
  }

  return LOG_INFO;
}

static int parse_bool(const char *value) {
  return strcmp(value, "true") == 0 || strcmp(value, "1") == 0;
}

static int parse_int(const char *value, int min, int max, int *result) {
  char *end;
  errno = 0;
  long parsed = strtol(value, &end, 10);

  if (errno != 0 || end == value || *trim(end) != '\0' || parsed < min ||
      parsed > max || parsed > INT_MAX || parsed < INT_MIN) {
    return -1;
  }

  *result = (int)parsed;
  return 0;
}

int config_load(const char *path, ServerConfig *config) {
  if (path == NULL || config == NULL) {
    return -1;
  }

  FILE *file = fopen(path, "r");

  if (file == NULL) {
    perror("fopen");
    return -1;
  }

  char line[512];

  while (fgets(line, sizeof(line), file) != NULL) {

    char *trimmed = trim(line);

    if (*trimmed == '\0' || *trimmed == '#') {
      continue;
    }

    char *separator = strchr(trimmed, '=');

    if (separator == NULL) {
      continue;
    }

    *separator = '\0';

    char *key = trim(trimmed);
    char *value = trim(separator + 1);

    if (strcmp(key, "port") == 0) {
      if (parse_int(value, 1, 65535, &config->port) != 0) {
        fclose(file);
        return -1;
      }
    } else if (strcmp(key, "host") == 0) {
      snprintf(config->host, sizeof(config->host), "%s", value);
    } else if (strcmp(key, "worker_threads") == 0) {
      if (parse_int(value, 1, INT_MAX, &config->worker_threads) != 0) {
        fclose(file);
        return -1;
      }
    } else if (strcmp(key, "document_root") == 0) {
      snprintf(config->document_root, sizeof(config->document_root), "%s",
               value);
    } else if (strcmp(key, "log_level") == 0) {
      config->log_level = parse_log_level(value);
    } else if (strcmp(key, "log_console") == 0) {
      config->log_console = parse_bool(value);
    } else if (strcmp(key, "log_file") == 0) {
      config->log_file = parse_bool(value);
    } else if (strcmp(key, "log_directory") == 0) {
      snprintf(config->log_directory, sizeof(config->log_directory), "%s",
               value);
    }
  }

  fclose(file);

  return 0;
}
