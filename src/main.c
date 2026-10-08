#include "config.h"
#include "logger.h"
#include "server.h"
#include <stdio.h>

int main(void) {
  ServerConfig config = {0};

  if (config_load("config/server.conf", &config) != 0) {
    fprintf(stderr, "Error: Failed to load config/server.conf\n");
    return 1;
  }

  if (logger_init(&config) != 0) {
    fprintf(stderr, "Error: logger_init failed\n");
    return 1;
  }

  log_info("Starting Server");
  fflush(stdout);

  server_start();

  logger_close();
  return 0;
}
