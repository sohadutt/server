#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

static char *trim(char *value) {
  while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
    value++;
  }

  char *end = value + strlen(value);
  while (end > value && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
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

static int safe_path_components(const char *path, int absolute) {
  if (path == NULL || path[0] == '\0' || (absolute && path[0] != '/') ||
      (!absolute && path[0] == '/') || strchr(path, '\\') != NULL)
    return 0;
  const char *segment = path + (absolute ? 1 : 0);
  for (const char *cursor = segment;; cursor++) {
    unsigned char character = (unsigned char)*cursor;
    if (character != '\0' && character != '/' &&
        (character < 0x20 || character == 0x7f || character == '?' ||
         character == '#'))
      return 0;
    if (character == '/' || character == '\0') {
      size_t length = (size_t)(cursor - segment);
      if (length == 0 || (length == 1 && segment[0] == '.') ||
          (length == 2 && segment[0] == '.' && segment[1] == '.'))
        return 0;
      if (character == '\0')
        break;
      segment = cursor + 1;
    }
  }
  return 1;
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

static int automatic_worker_count(void) {
  long processor_count = sysconf(_SC_NPROCESSORS_ONLN);
  if (processor_count < 2)
    return 4;
  if (processor_count >= 32)
    return 64;
  return (int)(processor_count * 2);
}

static int set_value(ServerConfig *config, const char *key, const char *value) {
  if (strcmp(key, "static_mount") == 0) {
    if (config->static_mount_count >= SERVER_STATIC_MOUNTS_MAX)
      return -1;
    char mount_value[512];
    if (copy_value(mount_value, sizeof(mount_value), value) != 0)
      return -1;
    char *separator = strchr(mount_value, '|');
    if (separator == NULL)
      return -1;
    *separator++ = '\0';
    char *fallback = strchr(separator, '|');
    if (fallback == NULL)
      return -1;
    *fallback++ = '\0';
    if (strchr(fallback, '|') != NULL ||
        (strcmp(fallback, "spa") != 0 && strcmp(fallback, "files") != 0))
      return -1;
    StaticMountConfig mount = {0};
    if (copy_value(mount.url_prefix, sizeof(mount.url_prefix), mount_value) != 0 ||
        copy_value(mount.directory, sizeof(mount.directory), separator) != 0 ||
        !safe_path_components(mount.url_prefix, 1) ||
        mount.url_prefix[strlen(mount.url_prefix) - 1] == '/' ||
        !safe_path_components(mount.directory, 0))
      return -1;
    mount.spa_fallback = strcmp(fallback, "spa") == 0;
    for (size_t index = 0; index < config->static_mount_count; index++) {
      if (strcmp(config->static_mounts[index].url_prefix, mount.url_prefix) == 0)
        return -1;
    }
    config->static_mounts[config->static_mount_count++] = mount;
    return 0;
  }
  if (strcmp(key, "tls_enabled") == 0) {
    return parse_bool(value, &config->tls_enabled);
  }
  if (strcmp(key, "tls_certificate_file") == 0) {
    return copy_value(config->tls_certificate_file,
                      sizeof(config->tls_certificate_file), value);
  }
  if (strcmp(key, "tls_private_key_file") == 0) {
    return copy_value(config->tls_private_key_file,
                      sizeof(config->tls_private_key_file), value);
  }
  if (strcmp(key, "port") == 0) {
    return parse_int(value, 1, 65535, &config->port);
  }
  if (strcmp(key, "host") == 0) {
    return copy_value(config->host, sizeof(config->host), value);
  }
  if (strcmp(key, "worker_threads") == 0) {
    if (strcmp(value, "auto") == 0) {
      config->worker_threads = automatic_worker_count();
      return 0;
    }
    return parse_int(value, 1, 64, &config->worker_threads);
  }
  if (strcmp(key, "rate_limiting_enabled") == 0 ||
      strcmp(key, "throttling") == 0) {
    return parse_bool(value, &config->rate_limiting_enabled);
  }
  if (strcmp(key, "gzip_enabled") == 0) {
    return parse_bool(value, &config->gzip_enabled);
  }
  if (strcmp(key, "document_root") == 0) {
    return copy_value(config->document_root, sizeof(config->document_root),
                      value);
  }
  if (strcmp(key, "index_file") == 0) {
    return copy_value(config->index_file, sizeof(config->index_file), value);
  }
  if (strcmp(key, "not_found_page") == 0) {
    return copy_value(config->not_found_page, sizeof(config->not_found_page),
                      value);
  }
  if (strcmp(key, "mime_types_file") == 0) {
    return copy_value(config->mime_types_file, sizeof(config->mime_types_file),
                      value);
  }
  if (strcmp(key, "default_mime_type") == 0) {
    return copy_value(config->default_mime_type,
                      sizeof(config->default_mime_type), value);
  }
  if (strcmp(key, "max_static_file_size_mb") == 0) {
    return parse_int(value, 1, 256, &config->max_static_file_size_mb);
  }
  if (strcmp(key, "signup_enabled") == 0) {
    return parse_bool(value, &config->signup_enabled);
  }
  if (strcmp(key, "cups_server") == 0) {
    if (value[0] == '\0')
      return -1;
    return copy_value(config->cups_server, sizeof(config->cups_server), value);
  }
  if (strcmp(key, "cups_encryption_required") == 0) {
    return parse_bool(value, &config->cups_encryption_required);
  }
  if (strcmp(key, "database_path") == 0) {
    return copy_value(config->database_path, sizeof(config->database_path), value);
  }
  if (strcmp(key, "admin_api_token") == 0) {
    if (strlen(value) < 32 || strlen(value) >= sizeof(config->admin_api_token))
      return -1;
    return copy_value(config->admin_api_token,
                      sizeof(config->admin_api_token), value);
  }
  if (strcmp(key, "log_directory") == 0) {
    return copy_value(config->log_directory, sizeof(config->log_directory),
                      value);
  }
  if (strcmp(key, "log_level") == 0) {
    if (strcmp(value, "debug") == 0)
      config->log_level = LOG_DEBUG;
    else if (strcmp(value, "info") == 0)
      config->log_level = LOG_INFO;
    else if (strcmp(value, "warn") == 0)
      config->log_level = LOG_WARN;
    else if (strcmp(value, "error") == 0)
      config->log_level = LOG_ERROR;
    else
      return -1;
    return 0;
  }
  if (strcmp(key, "log_console") == 0) {
    return parse_bool(value, &config->log_console);
  }
  if (strcmp(key, "log_file") == 0) {
    return parse_bool(value, &config->log_file);
  }
  if (strcmp(key, "log_retention_days") == 0) {
    return parse_int(value, 1, 3650, &config->log_retention_days);
  }
  return -1;
}

void config_defaults(ServerConfig *config) {
  if (config == NULL)
    return;
  memset(config, 0, sizeof(*config));
  config->log_level = LOG_INFO;
  config->log_console = 1;
  config->log_file = 1;
  config->log_retention_days = 30;
  config->port = 8080;
  config->worker_threads = automatic_worker_count();
  config->rate_limiting_enabled = 1;
  config->gzip_enabled = 1;
  snprintf(config->host, sizeof(config->host), "%s", "0.0.0.0");
  snprintf(config->log_directory, sizeof(config->log_directory), "%s", "logs");
  snprintf(config->document_root, sizeof(config->document_root), "%s",
           "public");
  snprintf(config->index_file, sizeof(config->index_file), "%s", "index.html");
  snprintf(config->not_found_page, sizeof(config->not_found_page), "%s",
           "404.html");
  snprintf(config->mime_types_file, sizeof(config->mime_types_file), "%s",
           "config/mime.types");
  snprintf(config->default_mime_type, sizeof(config->default_mime_type), "%s",
           "application/octet-stream");
  config->max_static_file_size_mb = 64;
  config->signup_enabled = 0;
  config->cups_server[0] = '\0';
  config->cups_encryption_required = 0;
  snprintf(config->database_path, sizeof(config->database_path), "%s",
           "data/printing.sqlite");
}

int config_load(const char *filename, ServerConfig *config) {
  if (filename == NULL || config == NULL)
    return -1;

  FILE *file = fopen(filename, "r");
  if (file == NULL) {
    fprintf(stderr, "Cannot open config '%s': %s\n", filename, strerror(errno));
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
    if (*content == '\0' || *content == '#')
      continue;

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

  if (result == 0 && parsed.tls_enabled &&
      (parsed.tls_certificate_file[0] == '\0' ||
       parsed.tls_private_key_file[0] == '\0')) {
    fprintf(stderr, "%s: tls_enabled requires tls_certificate_file and "
                    "tls_private_key_file\n", filename);
    result = -1;
  }
  if (result == 0)
    *config = parsed;
  return result;
}
