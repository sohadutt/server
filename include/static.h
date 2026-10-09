#ifndef STATIC_FILES_H
#define STATIC_FILES_H

#include <stddef.h>

typedef struct StaticFile {
  unsigned char *data;
  size_t length;
  const char *content_type;
} StaticFile;

/* Returns 0 when loaded and -1 when the path cannot be served. */
int static_file_load(const char *document_root, const char *request_target,
                     StaticFile *file);
void static_file_release(StaticFile *file);

#endif
