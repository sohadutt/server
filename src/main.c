#include "config.h"
#include "logger.h"

int main(void) {
  ServerConfig config = {0};

  if (config_load("config/server.conf", &config) != 0) {
    return 1;
  }

  if (logger_init(&config) != 0) {
    return 1;
  }

  log_info("Server started");
  log_debug("Initializing network");
  log_warn("Test warning");
  log_error("Test error");

  logger_close();

  return 0;
}
