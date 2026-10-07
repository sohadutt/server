#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "config.h"
#include "logger.h"

static FILE *log_file = NULL;

static int ensure_dir(const char *path) {
  struct stat st;

  if (path == NULL || *path == '\0') {
    fprintf(stderr, "Invalid log directory\n");
    return -1;
  }

  if (stat(path, &st) == 0) {
    if (!S_ISDIR(st.st_mode)) {
      fprintf(stderr, "Path exists but is not a directory: %s\n", path);
      return -1;
    }

    return 0;
  }

  if (errno != ENOENT) {
    perror("stat");
    return -1;
  }

  if (mkdir(path, 0755) == 0) {
    return 0;
  }

  if (errno == EEXIST) {
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
      return 0;
    }
  }

  perror("mkdir");
  return -1;
}

int logger_init(const ServerConfig *config) {
  if (config == NULL) {
    fprintf(stderr, "Invalid server configuration\n");
    return -1;
  }

  const char *log_dir = config->log_directory;

  if (ensure_dir(log_dir) != 0) {
    return -1;
  }

  time_t now = time(NULL);
  struct tm tm_now;

  if (localtime_r(&now, &tm_now) == NULL) {
    perror("localtime_r");
    return -1;
  }

  char filename[512];

  int written = snprintf(
      filename, sizeof(filename), "%s/%04d-%02d-%02d_%02d-%02d-%02d.log",
      log_dir, tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
      tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);

  if (written < 0 || (size_t)written >= sizeof(filename)) {
    fprintf(stderr, "Log filename is too long\n");
    return -1;
  }

  log_file = fopen(filename, "a");

  if (log_file == NULL) {
    fprintf(stderr, "Failed to open log file %s: %s\n", filename,
            strerror(errno));
    return -1;
  }

  return 0;
}

static const char *level_to_string(LogLevel level) {
  switch (level) {
  case LOG_DEBUG:
    return "DEBUG";

  case LOG_INFO:
    return "INFO";

  case LOG_WARN:
    return "WARN";

  case LOG_ERROR:
    return "ERROR";

  default:
    return "UNKNOWN";
  }
}

void log_message(LogLevel level, const char *message) {
  if (log_file == NULL) {
    fprintf(stderr, "Logger has not been initialized\n");
    return;
  }

  if (message == NULL) {
    message = "(null)";
  }

  time_t now = time(NULL);
  struct tm tm_now;

  if (localtime_r(&now, &tm_now) == NULL) {
    return;
  }

  fprintf(log_file, "%04d-%02d-%02d %02d:%02d:%02d [%s] %s\n",
          tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
          tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, level_to_string(level),
          message);

  fflush(log_file);
}

void log_debug(const char *message) { log_message(LOG_DEBUG, message); }

void log_info(const char *message) { log_message(LOG_INFO, message); }

void log_warn(const char *message) { log_message(LOG_WARN, message); }

void log_error(const char *message) { log_message(LOG_ERROR, message); }

void logger_close(void) {
  if (log_file != NULL) {
    fclose(log_file);
    log_file = NULL;
  }
}
