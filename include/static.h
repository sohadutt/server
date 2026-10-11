#ifndef STATIC_FILES_H
#define STATIC_FILES_H

#include <stddef.h>

#include "mime_types.h"

struct ServerConfig;

#define STATIC_ROOT_CAPACITY 4096
#define STATIC_INDEX_CAPACITY 128
typedef struct StaticFileServer {
  char document_root[STATIC_ROOT_CAPACITY];
  char index_file[STATIC_INDEX_CAPACITY];
  char not_found_page[STATIC_INDEX_CAPACITY];
  char default_mime_type[MIME_CONTENT_TYPE_CAPACITY];
  size_t max_file_size_bytes;
  MimeTypes mime_types;
} StaticFileServer;

typedef struct StaticFile {
  unsigned char *data;
  size_t length;
  const char *content_type;
  char relative_path[STATIC_ROOT_CAPACITY];
} StaticFile;

/* Loads the MIME table and verifies configured index and 404 files. */
int static_file_server_init(StaticFileServer *server,
                            const struct ServerConfig *config);
void static_file_server_destroy(StaticFileServer *server);
/* On success, file owns a buffer that must be released with static_file_release. */
int static_file_load(const StaticFileServer *server, const char *request_target,
                     StaticFile *file);
int static_file_load_not_found(const StaticFileServer *server,
                               StaticFile *file);
void static_file_release(StaticFile *file);

#endif
