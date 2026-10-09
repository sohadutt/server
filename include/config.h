#ifndef CONFIG_H
#define CONFIG_H

#include "logger.h"

typedef struct ServerConfig {
  LogLevel log_level;

  int log_console;
  int log_file;

  int port;
  char host[64];
  int worker_threads;

  char log_directory[256];
  char document_root[256];

} ServerConfig;

void config_defaults(ServerConfig *config);
int config_load(const char *filename, ServerConfig *config);

#endif
