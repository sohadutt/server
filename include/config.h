#ifndef CONFIG_H
#define CONFIG_H

#include "logger.h"

#include <stddef.h>

#define SERVER_STATIC_MOUNTS_MAX 16U
#define SERVER_STATIC_MOUNT_PATH_CAPACITY 128U
#define SERVER_STATIC_MOUNT_DIRECTORY_CAPACITY 256U

typedef struct StaticMountConfig {
  char url_prefix[SERVER_STATIC_MOUNT_PATH_CAPACITY];
  char directory[SERVER_STATIC_MOUNT_DIRECTORY_CAPACITY];
  int spa_fallback;
} StaticMountConfig;

typedef struct ServerConfig {
  LogLevel log_level;

  int log_console;
  int log_file;
  int log_retention_days;

  int port;
  char host[64];
  int worker_threads;
  int tls_enabled;
  int rate_limiting_enabled;
  int gzip_enabled;
  char tls_certificate_file[512];
  char tls_private_key_file[512];

  char log_directory[256];
  char document_root[256];
  char index_file[128];
  char not_found_page[128];
  char mime_types_file[256];
  char default_mime_type[128];
  int max_static_file_size_mb;
  StaticMountConfig static_mounts[SERVER_STATIC_MOUNTS_MAX];
  size_t static_mount_count;
  int signup_enabled;

  char cups_server[256];
  int cups_encryption_required;
  char database_path[512];
  char admin_api_token[256];
} ServerConfig;

void config_defaults(ServerConfig *config);
int config_load(const char *filename, ServerConfig *config);

#endif
