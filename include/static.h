#ifndef STATIC_FILES_H
#define STATIC_FILES_H

#include <stddef.h>

#include "config.h"
#include "mime_types.h"

#define STATIC_ROOT_CAPACITY 4096
#define STATIC_INDEX_CAPACITY 128
typedef struct StaticFileServer {
  int document_root_fd;
  char document_root[STATIC_ROOT_CAPACITY];
  char index_file[STATIC_INDEX_CAPACITY];
  char not_found_page[STATIC_INDEX_CAPACITY];
  char default_mime_type[MIME_CONTENT_TYPE_CAPACITY];
  size_t max_file_size_bytes;
  MimeTypes mime_types;
  StaticMountConfig mounts[SERVER_STATIC_MOUNTS_MAX];
  size_t mount_count;
} StaticFileServer;

typedef struct StaticFile {
  /* Streaming files own fd; buffered files own data. */
  int fd;
  int is_stream;
  unsigned char *data;
  size_t length;
  size_t memory_charge;
  const char *content_type;
  const char *cache_control;
  char relative_path[STATIC_ROOT_CAPACITY];
} StaticFile;

/* Loads the MIME table and verifies configured index and 404 files. */
int static_file_server_init(StaticFileServer *server,
                            const ServerConfig *config);
void static_file_server_destroy(StaticFileServer *server);
/* Pass a zero-initialized/empty file; release its owned buffer before reuse. */
int static_file_load(const StaticFileServer *server, const char *request_target,
                     StaticFile *file);
int static_file_load_not_found(const StaticFileServer *server,
                               StaticFile *file);
/* Opens a validated static file without copying it into process memory. */
int static_file_open(const StaticFileServer *server, const char *request_target,
                     StaticFile *file);
int static_file_open_not_found(const StaticFileServer *server,
                               StaticFile *file);
void static_file_release(StaticFile *file);

#endif
