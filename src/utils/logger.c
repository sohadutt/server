#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "logger.h"

static FILE *log_file = NULL;
static _Atomic int minimum_level = LOG_INFO;
static int log_to_console = 1;
static char log_directory[256];
static char active_log_date[11];
static pthread_mutex_t logger_mutex = PTHREAD_MUTEX_INITIALIZER;

#define LOG_LINE_CAPACITY 8192
#define LOG_FILE_BUFFER_CAPACITY (64U * 1024U)

static char log_file_buffer[LOG_FILE_BUFFER_CAPACITY];
static _Thread_local uint64_t current_request_id;

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

static int daily_log_name(const char *name) {
  if (name == NULL || strlen(name) != 14 || name[4] != '-' ||
      name[7] != '-' || strcmp(name + 10, ".log") != 0)
    return 0;
  for (size_t i = 0; i < 10; i++) {
    if (i != 4 && i != 7 && (name[i] < '0' || name[i] > '9'))
      return 0;
  }
  int month = (name[5] - '0') * 10 + name[6] - '0';
  int day = (name[8] - '0') * 10 + name[9] - '0';
  return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

static void prune_old_logs(int retention_days, const struct tm *today) {
  if (retention_days < 1 || today == NULL)
    return;
  struct tm cutoff = *today;
  cutoff.tm_hour = 0;
  cutoff.tm_min = 0;
  cutoff.tm_sec = 0;
  cutoff.tm_mday -= retention_days;
  time_t cutoff_time = mktime(&cutoff);
  struct tm cutoff_date;
  char cutoff_text[11];
  if (cutoff_time == (time_t)-1 || localtime_r(&cutoff_time, &cutoff_date) == NULL ||
      snprintf(cutoff_text, sizeof(cutoff_text), "%04d-%02d-%02d",
               cutoff_date.tm_year + 1900, cutoff_date.tm_mon + 1,
               cutoff_date.tm_mday) != 10)
    return;

  DIR *directory = opendir(log_directory);
  if (directory == NULL)
    return;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (!daily_log_name(entry->d_name) ||
        strncmp(entry->d_name, cutoff_text, 10) >= 0)
      continue;
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/%s", log_directory,
                          entry->d_name);
    if (length < 0 || (size_t)length >= sizeof(path))
      continue;
    struct stat status;
    if (lstat(path, &status) == 0 && S_ISREG(status.st_mode) &&
        unlink(path) != 0)
      fprintf(stderr, "Could not remove expired log %s: %s\n", path,
              strerror(errno));
  }
  closedir(directory);
}

static FILE *open_daily_log(const struct tm *date, char date_text[11]) {
  int date_length = snprintf(date_text, 11, "%04d-%02d-%02d",
                             date->tm_year + 1900, date->tm_mon + 1,
                             date->tm_mday);
  if (date_length != 10) {
    errno = EOVERFLOW;
    return NULL;
  }

  char filename[512];
  int written = snprintf(filename, sizeof(filename), "%s/%s.log",
                         log_directory, date_text);
  if (written < 0 || (size_t)written >= sizeof(filename)) {
    errno = ENAMETOOLONG;
    return NULL;
  }

  int log_fd = open(filename, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW |
                                 O_CLOEXEC,
                    0600);
  if (log_fd < 0)
    return NULL;

  struct stat log_status;
  if (fstat(log_fd, &log_status) != 0 || !S_ISREG(log_status.st_mode) ||
      fchmod(log_fd, 0600) != 0) {
    close(log_fd);
    errno = EPERM;
    return NULL;
  }

  FILE *stream = fdopen(log_fd, "a");
  if (stream == NULL) {
    close(log_fd);
  } else if (setvbuf(stream, log_file_buffer, _IOFBF,
                     sizeof(log_file_buffer)) != 0) {
    fclose(stream);
    stream = NULL;
  }
  return stream;
}

static void rotate_daily_log(const struct tm *date) {
  if (log_file == NULL)
    return;

  char date_text[11];
  int date_length = snprintf(date_text, sizeof(date_text), "%04d-%02d-%02d",
                             date->tm_year + 1900, date->tm_mon + 1,
                             date->tm_mday);
  if (date_length != 10 || strcmp(date_text, active_log_date) == 0)
    return;

  FILE *next_file = open_daily_log(date, date_text);
  if (next_file == NULL) {
    fprintf(stderr, "Failed to rotate daily log: %s\n", strerror(errno));
    fclose(log_file);
    log_file = NULL;
    active_log_date[0] = '\0';
    return;
  }

  fclose(log_file);
  log_file = next_file;
  memcpy(active_log_date, date_text, sizeof(active_log_date));
}

int logger_init(const ServerConfig *config) {
  if (config == NULL) {
    fprintf(stderr, "Invalid server configuration\n");
    return -1;
  }

  atomic_store_explicit(&minimum_level, config->log_level,
                        memory_order_relaxed);
  log_to_console = config->log_console || config->log_level == LOG_DEBUG;
  log_file = NULL;
  active_log_date[0] = '\0';
  if (strlen(config->log_directory) >= sizeof(log_directory)) {
    fprintf(stderr, "Log directory path is too long\n");
    return -1;
  }
  snprintf(log_directory, sizeof(log_directory), "%s", config->log_directory);

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
  prune_old_logs(config->log_retention_days, &tm_now);
  log_file = open_daily_log(&tm_now, active_log_date);
  if (log_file == NULL) {
    fprintf(stderr, "Failed to open daily log in %s: %s\n", log_dir,
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

void logger_set_request_id(uint64_t request_id) {
  current_request_id = request_id;
}

int logger_is_enabled(LogLevel level) {
  return (int)level >=
         atomic_load_explicit(&minimum_level, memory_order_relaxed);
}

static void log_vmessage(LogLevel level, const char *format,
                         va_list arguments)
    __attribute__((format(printf, 2, 0)));

static void log_vmessage(LogLevel level, const char *format, va_list arguments) {
  if (!logger_is_enabled(level))
    return;
  pthread_mutex_lock(&logger_mutex);
  if (!logger_is_enabled(level))
    goto done;

  struct timespec now;
  struct tm tm_now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0 ||
      localtime_r(&now.tv_sec, &tm_now) == NULL) {
    goto done;
  }
  rotate_daily_log(&tm_now);

  char line[LOG_LINE_CAPACITY];
  int prefix_length = snprintf(
      line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s]%s",
      tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour,
      tm_now.tm_min, tm_now.tm_sec, now.tv_nsec / 1000000L,
      level_to_string(level), current_request_id != 0 ? " [request=" : "");
  if (prefix_length < 0 || (size_t)prefix_length >= sizeof(line))
    goto done;
  size_t used = (size_t)prefix_length;
  if (current_request_id != 0) {
    int request_length = snprintf(line + used, sizeof(line) - used,
                                  "%" PRIu64 "]", current_request_id);
    if (request_length < 0 || (size_t)request_length >= sizeof(line) - used)
      goto done;
    used += (size_t)request_length;
  }
  if (used + 1 >= sizeof(line))
    goto done;
  line[used++] = ' ';
  size_t message_start = used;
  int message_length = vsnprintf(line + used, sizeof(line) - used - 1,
                                 format != NULL ? format : "(null)",
                                 arguments);
  if (message_length < 0)
    goto done;
  if ((size_t)message_length >= sizeof(line) - used - 1) {
    static const char truncated[] = "... [truncated]\n";
    size_t marker_length = sizeof(truncated) - 1;
    size_t marker_position = sizeof(line) - marker_length - 1;
    memcpy(line + marker_position, truncated, marker_length);
    used = marker_position + marker_length;
  } else {
    used += (size_t)message_length;
    line[used++] = '\n';
  }
  for (size_t index = message_start; index + 1 < used; index++) {
    unsigned char character = (unsigned char)line[index];
    if (character < 0x20 || character == 0x7f)
      line[index] = ' ';
  }
  line[used] = '\0';
  if (log_to_console)
    fputs(line, stderr);
  if (log_file != NULL) {
    errno = 0;
    int write_failed = fputs(line, log_file) == EOF;
    if (!write_failed && level >= LOG_WARN && fflush(log_file) != 0)
      write_failed = 1;
    if (write_failed) {
      int write_error = errno;
      FILE *failed_file = log_file;
      log_file = NULL;
      active_log_date[0] = '\0';
      (void)fclose(failed_file);
      fprintf(stderr, "Daily log write failed: %s\n",
              write_error != 0 ? strerror(write_error) : "I/O error");
    }
  }

done:
  pthread_mutex_unlock(&logger_mutex);
}

void log_messagef(LogLevel level, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  log_vmessage(level, format, arguments);
  va_end(arguments);
}

void log_message(LogLevel level, const char *message) {
  log_messagef(level, "%s", message != NULL ? message : "(null)");
}

void log_debug(const char *message) { log_message(LOG_DEBUG, message); }
void log_debugf(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  log_vmessage(LOG_DEBUG, format, arguments);
  va_end(arguments);
}

void log_info(const char *message) { log_message(LOG_INFO, message); }
void log_infof(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  log_vmessage(LOG_INFO, format, arguments);
  va_end(arguments);
}

void log_warn(const char *message) { log_message(LOG_WARN, message); }
void log_warnf(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  log_vmessage(LOG_WARN, format, arguments);
  va_end(arguments);
}

void log_error(const char *message) { log_message(LOG_ERROR, message); }
void log_errorf(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  log_vmessage(LOG_ERROR, format, arguments);
  va_end(arguments);
}

void logger_close(void) {
  pthread_mutex_lock(&logger_mutex);
  if (log_file != NULL) {
    fclose(log_file);
    log_file = NULL;
  }
  active_log_date[0] = '\0';
  pthread_mutex_unlock(&logger_mutex);
}
