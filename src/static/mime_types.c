#include "mime_types.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "logger.h"

#define MIME_LINE_CAPACITY 512

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

static int valid_extension(const char *extension) {
  if (extension == NULL || *extension == '\0') return 0;
  for (const unsigned char *cursor = (const unsigned char *)extension;
       *cursor != '\0'; cursor++) {
    if (!isalnum(*cursor) && *cursor != '-' && *cursor != '_') return 0;
  }
  return 1;
}

int mime_content_type_valid(const char *content_type) {
  const char *separator = content_type == NULL ? NULL : strchr(content_type, '/');
  if (separator == NULL || separator == content_type || separator[1] == '\0' ||
      strchr(separator + 1, '/') != NULL) {
    return 0;
  }

  for (const unsigned char *cursor = (const unsigned char *)content_type;
       *cursor != '\0'; cursor++) {
    if (!isalnum(*cursor) && !strchr("/!#$&^_.+-", *cursor)) return 0;
  }
  return 1;
}

static int add_entry(MimeTypes *types, const char *extension,
                     const char *content_type) {
  if (!valid_extension(extension) ||
      !mime_content_type_valid(content_type)) {
    return -1;
  }

  size_t index = types->count;
  for (size_t i = 0; i < types->count; i++) {
    if (strcasecmp(types->entries[i].extension, extension) == 0) {
      index = i;
      break;
    }
  }
  if (index == types->count) {
    if (types->count >= MIME_TYPES_MAX) return -1;
    types->count++;
  }

  MimeTypeEntry *entry = &types->entries[index];
  size_t extension_length = strlen(extension);
  size_t content_type_length = strlen(content_type);
  if (extension_length >= sizeof(entry->extension) ||
      content_type_length >= sizeof(entry->content_type)) {
    return -1;
  }
  memcpy(entry->extension, extension, extension_length + 1);
  memcpy(entry->content_type, content_type, content_type_length + 1);
  for (char *cursor = entry->extension; *cursor != '\0'; cursor++) {
    *cursor = (char)tolower((unsigned char)*cursor);
  }
  return 0;
}

int mime_types_load(MimeTypes *types, const char *filename) {
  if (types == NULL || filename == NULL || *filename == '\0') return -1;
  memset(types, 0, sizeof(*types));

  FILE *file = fopen(filename, "r");
  if (file == NULL) {
    int file_error = errno;
    log_errorf("Cannot open MIME types file '%s': %s", filename,
               strerror(file_error));
    return -1;
  }

  char line[MIME_LINE_CAPACITY];
  unsigned int line_number = 0;
  int result = 0;
  while (fgets(line, sizeof(line), file) != NULL) {
    line_number++;
    if (strchr(line, '\n') == NULL && !feof(file)) {
      result = -1;
    } else {
      char *content = trim(line);
      if (*content == '\0' || *content == '#') continue;

      char *comment = strchr(content, '#');
      if (comment != NULL) *comment = '\0';
      content = trim(content);
      char *save = NULL;
      char *extension = strtok_r(content, " \t", &save);
      char *content_type = strtok_r(NULL, " \t", &save);
      char *extra = strtok_r(NULL, " \t", &save);
      if (extension == NULL || content_type == NULL || extra != NULL ||
          add_entry(types, extension, content_type) != 0) {
        result = -1;
      }
    }

    if (result != 0) {
      log_errorf("%s:%u: expected '<extension> <media/type>'", filename,
                 line_number);
      break;
    }
  }

  if (ferror(file)) {
    int read_error = errno;
    log_errorf("Failed reading MIME types file '%s': %s", filename,
               read_error != 0 ? strerror(read_error) : "I/O error");
    result = -1;
  }
  fclose(file);
  if (result == 0 && types->count == 0) {
    log_error("MIME types file contains no mappings");
    return -1;
  }
  if (result != 0) memset(types, 0, sizeof(*types));
  return result;
}

const char *mime_types_lookup(const MimeTypes *types, const char *extension,
                              const char *fallback) {
  if (types == NULL || extension == NULL) return fallback;
  for (size_t i = 0; i < types->count; i++) {
    if (strcasecmp(types->entries[i].extension, extension) == 0) {
      return types->entries[i].content_type;
    }
  }
  return fallback;
}
