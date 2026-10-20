#ifndef CONFIG_H
#define CONFIG_H

#include "logger.h"

#include <stddef.h>

typedef struct ServerConfig {
  LogLevel log_level;

  int log_console;
  int log_file;

  int port;
  char host[64];
  int worker_threads;

  char log_directory[256];
  char document_root[256];
  char index_file[128];
  char not_found_page[128];
  char mime_types_file[256];
  char default_mime_type[128];
  int max_static_file_size_mb;

  char cups_server[256];
  int cups_encryption_required;
  char database_path[512];
  char admin_api_token[256];
} ServerConfig;

void config_defaults(ServerConfig *config);
int config_load(const char *filename, ServerConfig *config);

#endif
