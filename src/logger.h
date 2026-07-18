#ifndef LOGGER_H
#define LOGGER_H

#include "base.h"

typedef enum Log_Level {
    LOG_LEVEL_TRACE,
    LOG_LEVEL_INFO,
    LOG_LEVEL_SUCCESS,
    LOG_LEVEL_WARNING,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL,
} Log_Level;

void logger_init(void);
void logger_set_minimum_level(Log_Level level);
void logger_set_colors(b32 enabled);
void logger_logv(Log_Level level, const char *tag, const char *format,
                 va_list arguments);
void logger_log(Log_Level level, const char *tag, const char *format, ...);

#define log_trace(...)   logger_log(LOG_LEVEL_TRACE,   "trace",   __VA_ARGS__)
#define log_info(...)    logger_log(LOG_LEVEL_INFO,    "info",    __VA_ARGS__)
#define log_success(...) logger_log(LOG_LEVEL_SUCCESS, "success", __VA_ARGS__)
#define log_warning(...) logger_log(LOG_LEVEL_WARNING, "warning", __VA_ARGS__)
#define log_error(...)   logger_log(LOG_LEVEL_ERROR,   "error",   __VA_ARGS__)
#define log_fatal(...)   logger_log(LOG_LEVEL_FATAL,   "fatal",   __VA_ARGS__)

#endif
