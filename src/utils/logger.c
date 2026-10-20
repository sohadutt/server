#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "logger.h"

static FILE *log_file = NULL;
static LogLevel minimum_level = LOG_INFO;
static int log_to_console = 1;
static pthread_mutex_t logger_mutex = PTHREAD_MUTEX_INITIALIZER;

#define LOG_LINE_CAPACITY 8192

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#error "Secure log file opening requires O_NOFOLLOW support"
#endif

static int ensure_dir(const char *path) {
  struct stat st;

  if (path == NULL || *path == '\0') {
    fprintf(stderr, "Invalid log directory\n");
    return -1;
  }

  if (lstat(path, &st) == 0) {
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

  if (mkdir(path, 0700) == 0) {
    return 0;
  }

  if (errno == EEXIST) {
    if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
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

  minimum_level = config->log_level;
  log_to_console = config->log_console || config->log_level == LOG_DEBUG;
  log_file = NULL;

  if (!config->log_file)
    return 0;

  const char *log_dir = config->log_directory;
  if (ensure_dir(log_dir) != 0)
    return -1;

  time_t now = time(NULL);
  struct tm tm_now;

  if (localtime_r(&now, &tm_now) == NULL) {
    perror("localtime_r");
    return -1;
  }

  char filename[512];

  int written =
      snprintf(filename, sizeof(filename), "%s/%04d-%02d-%02d.log", log_dir,
               tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);

  if (written < 0 || (size_t)written >= sizeof(filename)) {
    fprintf(stderr, "Log filename is too long\n");
    return -1;
  }

  int log_fd = open(filename, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW |
                                    O_CLOEXEC,
                    0600);
  if (log_fd < 0) {
    fprintf(stderr, "Failed to open log file %s: %s\n", filename,
            strerror(errno));
    return -1;
  }

  struct stat log_status;
  if (fstat(log_fd, &log_status) != 0 || !S_ISREG(log_status.st_mode) ||
      fchmod(log_fd, 0600) != 0) {
    close(log_fd);
    fprintf(stderr, "Log path is not a regular file with safe permissions\n");
    return -1;
  }
  log_file = fdopen(log_fd, "a");
  if (log_file == NULL) {
    close(log_fd);
    fprintf(stderr, "Failed to initialize log stream: %s\n",
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
  pthread_mutex_lock(&logger_mutex);
  if (level < minimum_level)
    goto done;

  if (message == NULL) {
    message = "(null)";
  }

  time_t now = time(NULL);
  struct tm tm_now;

  if (localtime_r(&now, &tm_now) == NULL) {
    goto done;
  }

  char line[LOG_LINE_CAPACITY];
  int length = snprintf(
      line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d [%s] %s\n",
      tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour,
      tm_now.tm_min, tm_now.tm_sec, level_to_string(level), message);
  if (length < 0 || (size_t)length >= sizeof(line))
    goto done;
  if (log_to_console)
    fputs(line, stderr);
  if (log_file != NULL) {
    fputs(line, log_file);
    fflush(log_file);
  }

done:
  pthread_mutex_unlock(&logger_mutex);
}

void log_debug(const char *message) { log_message(LOG_DEBUG, message); }

void log_info(const char *message) { log_message(LOG_INFO, message); }

void log_warn(const char *message) { log_message(LOG_WARN, message); }

void log_error(const char *message) { log_message(LOG_ERROR, message); }

void logger_close(void) {
  pthread_mutex_lock(&logger_mutex);
  if (log_file != NULL) {
    fclose(log_file);
    log_file = NULL;
  }
  pthread_mutex_unlock(&logger_mutex);
}
