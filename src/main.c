#include "config.h"
#include "logger.h"
#include "server.h"
#include <stdio.h>

int main(int argc, char **argv) {
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
  fflush(stdout);

  int result = server_start(&config);

  logger_close();
  return result == 0 ? 0 : 1;
}
