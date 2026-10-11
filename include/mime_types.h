#ifndef MIME_TYPES_H
#define MIME_TYPES_H

#include <stddef.h>

#define MIME_TYPES_MAX 128
#define MIME_EXTENSION_CAPACITY 32
#define MIME_CONTENT_TYPE_CAPACITY 128

typedef struct MimeTypeEntry {
  char extension[MIME_EXTENSION_CAPACITY];
  char content_type[MIME_CONTENT_TYPE_CAPACITY];
} MimeTypeEntry;

typedef struct MimeTypes {
  MimeTypeEntry entries[MIME_TYPES_MAX];
  size_t count;
} MimeTypes;

int mime_types_load(MimeTypes *types, const char *filename);
const char *mime_types_lookup(const MimeTypes *types, const char *extension,
                              const char *fallback);
int mime_content_type_valid(const char *content_type);

#endif
