#include "logger.h"
#include "platform.h"

#include <stdio.h>

typedef struct Logger_State {
   Log_Level minimum_level;
   i32 verbosity;
   b32 colors;
} Logger_State;

static Logger_State logger = { LOG_LEVEL_TRACE, -1, true };

static const char *level_color(Log_Level level)
{
   switch (level)
   {
      case LOG_LEVEL_TRACE: return "\x1b[90m";
      case LOG_LEVEL_INFO: return "\x1b[36m";
      case LOG_LEVEL_SUCCESS: return "\x1b[32m";
      case LOG_LEVEL_WARNING: return "\x1b[33m";
      case LOG_LEVEL_ERROR: return "\x1b[31m";
      case LOG_LEVEL_FATAL: return "\x1b[1;31m";
   }
   return "";
}

void logger_init(void) {
   platform_enable_console_colors();
}

void logger_set_minimum_level(Log_Level level) {
   logger.minimum_level = level;
}

void logger_set_colors(b32 enabled) {
   logger.colors = enabled;
}

void logger_set_verbosity(i32 verbosity) {
	logger.verbosity = verbosity;
}

i32 logger_get_verbosity(void) {
	return logger.verbosity;
}

b32 logger_has_verbosity(i32 verbosity) {
	return logger.verbosity >= verbosity;
}

void logger_logv(Log_Level level, const char *tag, const char *format, va_list arguments)
{
   Scratch scratch;
   char *start;
   String message;
   FILE *stream;
   b32 colors;

   if (level < logger.minimum_level) { return; }
   stream = level >= LOG_LEVEL_WARNING ? stderr : stdout;

   platform_output_lock();
   scratch = get_scratch();
   start = arena_top(scratch.arena);
   colors = logger.colors &&
   platform_console_supports_colors(level >= LOG_LEVEL_WARNING);
   if (colors) { arena_push_text(scratch.arena, level_color(level)); }
   arena_pushf(scratch.arena, "[%s]", tag);
   if (colors) { arena_push_text(scratch.arena, "\x1b[0m"); }
   arena_push_char(scratch.arena, ' ');
   arena_pushfv(scratch.arena, format, arguments);
   arena_push_char(scratch.arena, '\n');
   message = string_from_range(start, arena_top(scratch.arena));
   fwrite(message.data, 1, (size_t)message.size, stream);
   fflush(stream);
   end_scratch(scratch);
   platform_output_unlock();
}

void logger_log(Log_Level level, const char *tag, const char *format, ...)
{
   va_list arguments;
   va_start(arguments, format);
   logger_logv(level, tag, format, arguments);
   va_end(arguments);
}
