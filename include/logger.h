#ifndef LOGGER_H
#define LOGGER_H

typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;

typedef struct ServerConfig ServerConfig;

int logger_init(const ServerConfig *config);

void log_message(LogLevel level, const char *message);

void log_debug(const char *message);
void log_info(const char *message);
void log_warn(const char *message);
void log_error(const char *message);

void logger_close(void);

#endif
