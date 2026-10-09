#include <stdio.h>

#include "config.h"
#include "logger.h"
#include "server.h"

int main(int argc, char **argv) {
  if (argc > 2) {
    fprintf(stderr, "Usage: %s [config-file]\n", argv[0]);
    return 2;
  }

  const char *config_path = argc > 1 ? argv[1] : "config/server.conf";
  ServerConfig config;

  if (config_load(config_path, &config) != 0) {
    fprintf(stderr, "Error: Failed to load %s\n", config_path);
    return 1;
  }

  if (logger_init(&config) != 0) {
    fprintf(stderr, "Error: logger_init failed\n");
    return 1;
  }

  log_info("Starting Server");
  int result = server_start(&config);

  logger_close();
  return result == 0 ? 0 : 1;
}
