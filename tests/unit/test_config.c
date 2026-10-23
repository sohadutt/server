#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_config(const char *path, const char *content) {
  FILE *file = fopen(path, "w");
  assert(file != NULL);
  assert(fputs(content, file) >= 0);
  assert(fclose(file) == 0);
}

int main(void) {
  ServerConfig config;
  config_defaults(&config);
  assert(config.port == 8080);
  assert(config.worker_threads >= 1 && config.worker_threads <= 64);
  assert(config.signup_enabled == 0);

  char path[] = "/tmp/server-config-test-XXXXXX";
  int descriptor = mkstemp(path);
  assert(descriptor >= 0);
  close(descriptor);

  write_config(path,
               "port = 9091\n"
               "host = 127.0.0.1\n"
               "worker_threads = 12\n"
               "log_level = warn\n"
               "log_console = false\n"
               "log_file = false\n"
               "signup_enabled = true\n"
               "static_mount = /app|apps/app|spa\n"
               "static_mount = /files|files|files\n");
  assert(config_load(path, &config) == 0);
  assert(config.port == 9091);
  assert(strcmp(config.host, "127.0.0.1") == 0);
  assert(config.worker_threads == 12);
  assert(config.log_level == LOG_WARN);
  assert(config.log_console == 0);
  assert(config.log_file == 0);
  assert(config.signup_enabled == 1);
  assert(config.static_mount_count == 2);
  assert(strcmp(config.static_mounts[0].url_prefix, "/app") == 0);
  assert(strcmp(config.static_mounts[0].directory, "apps/app") == 0);
  assert(config.static_mounts[0].spa_fallback == 1);

  write_config(path, "port = 70000\n");
  assert(config_load(path, &config) == -1);
  assert(config.port == 9091);
  write_config(path, "static_mount = /app|../outside|spa\n");
  assert(config_load(path, &config) == -1);
  unlink(path);

  puts("configuration unit tests passed");
  return 0;
}
