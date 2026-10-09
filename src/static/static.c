#define _XOPEN_SOURCE 700

#include "static.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define STATIC_FILE_MAX_SIZE (64 * 1024 * 1024)

static int hex_value(char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

static int decode_request_path(const char *target, char *relative_path,
                               size_t capacity) {
  if (target == NULL || target[0] != '/' || capacity == 0) return -1;

  size_t output = 0;
  for (size_t input = 1; target[input] != '\0' && target[input] != '?' &&
                          target[input] != '#';
       input++) {
    unsigned char character = (unsigned char)target[input];
    if (character == '%') {
      int high = hex_value(target[input + 1]);
      int low = target[input + 1] == '\0' ? -1 : hex_value(target[input + 2]);
      if (high < 0 || low < 0) return -1;
      character = (unsigned char)((high << 4) | low);
      input += 2;
    }
    if (character == '\0' || character == '\\' || !isprint(character) ||
        output + 1 >= capacity) {
      return -1;
    }
    relative_path[output++] = (char)character;
  }
  relative_path[output] = '\0';

  char *segment = relative_path;
  while (*segment != '\0') {
    while (*segment == '/') segment++;
    char *end = strchr(segment, '/');
    if (end == NULL) end = segment + strlen(segment);
    size_t length = (size_t)(end - segment);
    if ((length == 1 && segment[0] == '.') ||
        (length == 2 && segment[0] == '.' && segment[1] == '.')) {
      return -1;
    }
    segment = end;
  }

  while (*relative_path == '/') memmove(relative_path, relative_path + 1,
                                        strlen(relative_path));
  if (*relative_path == '\0') {
    if (sizeof("index.html") > capacity) return -1;
    strcpy(relative_path, "index.html");
  }
  return 0;
}

static int path_is_inside_root(const char *root, const char *path) {
  size_t root_length = strlen(root);
  if (strncmp(root, path, root_length) != 0) return 0;
  if (root_length == 1 && root[0] == '/') return path[0] == '/';
  return path[root_length] == '/' || path[root_length] == '\0';
}

static const char *content_type_for_path(const char *path) {
  const char *extension = strrchr(path, '.');
  if (extension == NULL) return "application/octet-stream";
  extension++;

  if (strcasecmp(extension, "html") == 0 || strcasecmp(extension, "htm") == 0)
    return "text/html; charset=utf-8";
  if (strcasecmp(extension, "css") == 0) return "text/css; charset=utf-8";
  if (strcasecmp(extension, "js") == 0) return "text/javascript; charset=utf-8";
  if (strcasecmp(extension, "json") == 0) return "application/json";
  if (strcasecmp(extension, "txt") == 0) return "text/plain; charset=utf-8";
  if (strcasecmp(extension, "svg") == 0) return "image/svg+xml";
  if (strcasecmp(extension, "png") == 0) return "image/png";
  if (strcasecmp(extension, "jpg") == 0 || strcasecmp(extension, "jpeg") == 0)
    return "image/jpeg";
  if (strcasecmp(extension, "gif") == 0) return "image/gif";
  if (strcasecmp(extension, "ico") == 0) return "image/vnd.microsoft.icon";
  if (strcasecmp(extension, "pdf") == 0) return "application/pdf";
  if (strcasecmp(extension, "woff2") == 0) return "font/woff2";
  return "application/octet-stream";
}

int static_file_load(const char *document_root, const char *request_target,
                     StaticFile *file) {
  if (document_root == NULL || request_target == NULL || file == NULL) return -1;
  file->data = NULL;
  file->length = 0;
  file->content_type = NULL;

  char relative_path[PATH_MAX];
  char root_path[PATH_MAX];
  char candidate[PATH_MAX];
  char resolved_path[PATH_MAX];
  if (decode_request_path(request_target, relative_path,
                          sizeof(relative_path)) != 0 ||
      realpath(document_root, root_path) == NULL) {
    return -1;
  }

  int written = snprintf(candidate, sizeof(candidate), "%s/%s", root_path,
                         relative_path);
  if (written < 0 || (size_t)written >= sizeof(candidate) ||
      realpath(candidate, resolved_path) == NULL ||
      !path_is_inside_root(root_path, resolved_path)) {
    return -1;
  }

  FILE *input = fopen(resolved_path, "rb");
  if (input == NULL) return -1;

  struct stat file_status;
  if (fstat(fileno(input), &file_status) != 0 ||
      !S_ISREG(file_status.st_mode) || file_status.st_size < 0 ||
      (unsigned long long)file_status.st_size > STATIC_FILE_MAX_SIZE) {
    fclose(input);
    return -1;
  }

  size_t length = (size_t)file_status.st_size;
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
  file->content_type = content_type_for_path(relative_path);
  return 0;
}

void static_file_release(StaticFile *file) {
  if (file == NULL) return;
  free(file->data);
  file->data = NULL;
  file->length = 0;
  file->content_type = NULL;
}
