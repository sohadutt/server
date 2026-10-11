#define _XOPEN_SOURCE 700

#include "static.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "logger.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define STATIC_FILE_MAX_SIZE (1024ULL * 1024ULL * 1024ULL)
#define BYTES_PER_MEGABYTE (1024ULL * 1024ULL)

static int copy_string(char *destination, size_t capacity, const char *source) {
  size_t length = strlen(source);
  if (length == 0 || length >= capacity)
    return -1;
  memcpy(destination, source, length + 1);
  return 0;
}

static int is_safe_relative_path(const char *path) {
  if (path == NULL || path[0] == '\0' || path[0] == '/' ||
      strchr(path, '\\') != NULL) {
    return 0;
  }

  const char *segment = path;
  for (const char *cursor = path;; cursor++) {
    unsigned char character = (unsigned char)*cursor;
    if (character != '\0' && character != '/' &&
        (character < 0x20 || character == 0x7f)) {
      return 0;
    }
    if (character == '/' || character == '\0') {
      size_t length = (size_t)(cursor - segment);
      if (length == 0 || (length == 1 && segment[0] == '.') ||
          (length == 2 && segment[0] == '.' && segment[1] == '.')) {
        return 0;
      }
      if (character == '\0')
        break;
      segment = cursor + 1;
    }
  }
  return 1;
}

static int hex_value(char character) {
  if (character >= '0' && character <= '9')
    return character - '0';
  if (character >= 'a' && character <= 'f')
    return character - 'a' + 10;
  if (character >= 'A' && character <= 'F')
    return character - 'A' + 10;
  return -1;
}

static int decode_request_path(const char *target, char *relative_path,
                               size_t capacity) {
  if (target == NULL || target[0] != '/' || capacity == 0)
    return -1;

  size_t output = 0;
  for (size_t input = 1;
       target[input] != '\0' && target[input] != '?' && target[input] != '#';
       input++) {
    unsigned char character = (unsigned char)target[input];
    if (character == '%') {
      int high = hex_value(target[input + 1]);
      int low = target[input + 1] == '\0' ? -1 : hex_value(target[input + 2]);
      if (high < 0 || low < 0)
        return -1;
      character = (unsigned char)((high << 4) | low);
      input += 2;
    }
    if (character == '\0' || character == '\\' || character < 0x20 ||
        character == 0x7f || output + 1 >= capacity) {
      return -1;
    }
    relative_path[output++] = (char)character;
  }
  relative_path[output] = '\0';

  while (*relative_path == '/') {
    memmove(relative_path, relative_path + 1, strlen(relative_path));
  }
  if (*relative_path == '\0') {
    return 0;
  }
  char *segment = relative_path;
  while (*segment != '\0') {
    while (*segment == '/')
      segment++;
    if (*segment == '\0')
      break;
    char *end = strchr(segment, '/');
    if (end == NULL)
      end = segment + strlen(segment);
    size_t length = (size_t)(end - segment);
    if ((length == 1 && segment[0] == '.') ||
        (length == 2 && segment[0] == '.' && segment[1] == '.')) {
      return -1;
    }
    segment = end;
  }
  return 0;
}

static int path_is_inside_root(const char *root, const char *path) {
  size_t root_length = strlen(root);
  if (strncmp(root, path, root_length) != 0)
    return 0;
  if (root_length == 1 && root[0] == '/')
    return path[0] == '/';
  return path[root_length] == '/' || path[root_length] == '\0';
}

static const char *content_type_for_path(const StaticFileServer *server,
                                         const char *path) {
  const char *extension = strrchr(path, '.');
  if (extension == NULL || extension[1] == '\0') {
    return server->default_mime_type;
  }
  return mime_types_lookup(&server->mime_types, extension + 1,
                           server->default_mime_type);
}

static int resolve_file_path(const StaticFileServer *server,
                             const char *relative_path, char *resolved_path,
                             size_t capacity) {
  char candidate[PATH_MAX];
  int written = snprintf(candidate, sizeof(candidate), "%s/%s",
                         server->document_root, relative_path);
  if (written < 0 || (size_t)written >= sizeof(candidate) ||
      realpath(candidate, resolved_path) == NULL ||
      !path_is_inside_root(server->document_root, resolved_path)) {
    return -1;
  }

  struct stat status;
  if (stat(resolved_path, &status) != 0)
    return -1;
  if (S_ISDIR(status.st_mode)) {
    written = snprintf(candidate, sizeof(candidate), "%s/%s", resolved_path,
                       server->index_file);
    if (written < 0 || (size_t)written >= sizeof(candidate) ||
        realpath(candidate, resolved_path) == NULL ||
        !path_is_inside_root(server->document_root, resolved_path)) {
      return -1;
    }
    if (stat(resolved_path, &status) != 0)
      return -1;
  }
  if (!S_ISREG(status.st_mode) || strlen(resolved_path) >= capacity)
    return -1;
  return 0;
}

static int load_relative_file(const StaticFileServer *server,
                              const char *relative_path, StaticFile *file) {
  file->data = NULL;
  file->length = 0;
  file->content_type = NULL;
  file->relative_path[0] = '\0';

  char resolved_path[PATH_MAX];
  if (resolve_file_path(server, relative_path, resolved_path,
                        sizeof(resolved_path)) != 0) {
    return -1;
  }

  FILE *input = fopen(resolved_path, "rb");
  if (input == NULL)
    return -1;

  struct stat status;
  if (fstat(fileno(input), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 ||
      (unsigned long long)status.st_size > server->max_file_size_bytes) {
    fclose(input);
    return -1;
  }

  size_t length = (size_t)status.st_size;
  unsigned char *data = malloc(length == 0 ? 1 : length);
  if (data == NULL) {
    fclose(input);
    return -1;
  }

  size_t bytes_read = fread(data, 1, length, input);
  int read_failed = ferror(input) || bytes_read != length;
  fclose(input);
  if (read_failed) {
    free(data);
    return -1;
  }

  file->data = data;
  file->length = length;
  file->content_type = content_type_for_path(server, resolved_path);
  const char *relative_file = resolved_path + strlen(server->document_root);
  if (*relative_file == '/')
    relative_file++;
  if (copy_string(file->relative_path, sizeof(file->relative_path),
                  relative_file) != 0) {
    free(data);
    file->data = NULL;
    file->length = 0;
    file->content_type = NULL;
    return -1;
  }
  return 0;
}

int static_file_server_init(StaticFileServer *server,
                            const struct ServerConfig *config) {
  if (server == NULL || config == NULL || config->max_static_file_size_mb < 1 ||
      config->max_static_file_size_mb > 1024) {
    return -1;
  }

  memset(server, 0, sizeof(*server));
  char resolved_root[PATH_MAX];
  struct stat root_status;
  if (realpath(config->document_root, resolved_root) == NULL ||
      strlen(resolved_root) >= sizeof(server->document_root) ||
      copy_string(server->document_root, sizeof(server->document_root),
                  resolved_root) != 0 ||
      stat(server->document_root, &root_status) != 0 ||
      !S_ISDIR(root_status.st_mode)) {
    char message[384];
    snprintf(message, sizeof(message), "Invalid document root '%s'",
             config->document_root);
    log_error(message);
    return -1;
  }

  if (!is_safe_relative_path(config->index_file) ||
      !is_safe_relative_path(config->not_found_page) ||
      !mime_content_type_valid(config->default_mime_type) ||
      copy_string(server->index_file, sizeof(server->index_file),
                  config->index_file) != 0 ||
      copy_string(server->not_found_page, sizeof(server->not_found_page),
                  config->not_found_page) != 0 ||
      copy_string(server->default_mime_type, sizeof(server->default_mime_type),
                  config->default_mime_type) != 0) {
    log_error("Invalid static file configuration");
    return -1;
  }

  server->max_file_size_bytes =
      (size_t)config->max_static_file_size_mb * BYTES_PER_MEGABYTE;
  if (server->max_file_size_bytes > STATIC_FILE_MAX_SIZE) {
    log_error("Configured static file limit exceeds the server maximum");
    return -1;
  }

  if (mime_types_load(&server->mime_types, config->mime_types_file) != 0)
    return -1;

  StaticFile check = {0};
  if (load_relative_file(server, server->index_file, &check) != 0) {
    log_error("Configured index file cannot be loaded from the document root");
    return -1;
  }
  static_file_release(&check);

  if (load_relative_file(server, server->not_found_page, &check) != 0) {
    log_error("Configured 404 page cannot be loaded from the document root");
    return -1;
  }
  static_file_release(&check);
  return 0;
}

void static_file_server_destroy(StaticFileServer *server) {
  if (server != NULL)
    memset(server, 0, sizeof(*server));
}

int static_file_load(const StaticFileServer *server, const char *request_target,
                     StaticFile *file) {
  if (server == NULL || request_target == NULL || file == NULL)
    return -1;
  file->data = NULL;
  file->length = 0;
  file->content_type = NULL;
  file->relative_path[0] = '\0';

  char relative_path[PATH_MAX];
  if (decode_request_path(request_target, relative_path,
                          sizeof(relative_path)) != 0) {
    return -1;
  }
  const char *path_to_load =
      relative_path[0] == '\0' ? server->index_file : relative_path;
  return load_relative_file(server, path_to_load, file);
}

int static_file_load_not_found(const StaticFileServer *server,
                               StaticFile *file) {
  if (server == NULL || file == NULL)
    return -1;
  file->data = NULL;
  file->length = 0;
  file->content_type = NULL;
  file->relative_path[0] = '\0';
  return load_relative_file(server, server->not_found_page, file);
}

void static_file_release(StaticFile *file) {
  if (file == NULL)
    return;
  free(file->data);
  file->data = NULL;
  file->length = 0;
  file->content_type = NULL;
  file->relative_path[0] = '\0';
}
