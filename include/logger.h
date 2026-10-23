#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define LOGGER_PRINTF_FORMAT(format_index, first_argument) \
  __attribute__((format(printf, format_index, first_argument)))
#else
#define LOGGER_PRINTF_FORMAT(format_index, first_argument)
#endif

typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;

typedef struct ServerConfig ServerConfig;

int logger_init(const ServerConfig *config);
void logger_set_request_id(uint64_t request_id);
int logger_is_enabled(LogLevel level);

void log_message(LogLevel level, const char *message);
void log_messagef(LogLevel level, const char *format, ...)
    LOGGER_PRINTF_FORMAT(2, 3);

void log_debug(const char *message);
void log_debugf(const char *format, ...) LOGGER_PRINTF_FORMAT(1, 2);
void log_info(const char *message);
void log_infof(const char *format, ...) LOGGER_PRINTF_FORMAT(1, 2);
void log_warn(const char *message);
void log_warnf(const char *format, ...) LOGGER_PRINTF_FORMAT(1, 2);
void log_error(const char *message);
void log_errorf(const char *format, ...) LOGGER_PRINTF_FORMAT(1, 2);

void logger_close(void);

#endif
