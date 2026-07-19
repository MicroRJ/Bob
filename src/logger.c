#include "logger.h"
#include "platform/platform.h"

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

static b32 logger_has_verbosity(i32 verbosity) {
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
   scratch = begin_scratch();
   start = arena_top(scratch.arena);
   colors = logger.colors &&
   platform_console_supports_colors(level >= LOG_LEVEL_WARNING);
   if (colors) { arena_append_text(scratch.arena, level_color(level)); }
   arena_appendf(scratch.arena, "[%s]", tag);
   if (colors) { arena_append_text(scratch.arena, "\x1b[0m"); }
   arena_append_char(scratch.arena, ' ');
   arena_appendfv(scratch.arena, format, arguments);
   arena_append_char(scratch.arena, '\n');
	message = arena_string_from(scratch.arena, start);
	arena_finalize_string(scratch.arena, message);
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

void logger_log_at(i32 verbosity, Log_Level level, const char *tag, const char *format, ...)
{
	va_list arguments;
	if (!logger_has_verbosity(verbosity)) return;
	va_start(arguments, format);
	logger_logv(level, tag, format, arguments);
	va_end(arguments);
}

static void logger_append_output(Arena *arena, String input)
{
	u64 start = 0;
	for (u64 i = 0; i + 1 < input.size; ++i) {
		if (input.data[i] == '\r' && input.data[i + 1] == '\n') {
			arena_append_str(arena, string_slice(input, start, i - start));
			start = i + 1;
		}
	}
	arena_append_str(arena, string_slice(input, start, input.size - start));
}

void logger_log_string(Log_Level level, const char *tag, String input)
{
	Scratch scratch;
	char *start;
	String message;
	FILE *stream;
	b32 colors;

	if (level < logger.minimum_level) return;
	stream = level >= LOG_LEVEL_WARNING ? stderr : stdout;
	platform_output_lock();
	scratch = begin_scratch();
	start = arena_top(scratch.arena);
	colors = logger.colors &&
		platform_console_supports_colors(level >= LOG_LEVEL_WARNING);
	if (colors) arena_append_text(scratch.arena, level_color(level));
	arena_appendf(scratch.arena, "[%s]", tag);
	if (colors) arena_append_text(scratch.arena, "\x1b[0m");
	arena_append_char(scratch.arena, ' ');
	logger_append_output(scratch.arena, input);
	if (input.size == 0 || input.data[input.size - 1] != '\n') {
		arena_append_char(scratch.arena, '\n');
	}
	message = arena_string_from(scratch.arena, start);
	arena_finalize_string(scratch.arena, message);
	fwrite(message.data, 1, (size_t)message.size, stream);
	fflush(stream);
	end_scratch(scratch);
	platform_output_unlock();
}

void logger_log_string_at(i32 verbosity, Log_Level level, const char *tag, String message)
{
	if (!logger_has_verbosity(verbosity)) return;
	logger_log_string(level, tag, message);
}
