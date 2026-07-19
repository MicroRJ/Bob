#ifndef PLATFORM_H
#define PLATFORM_H

#include "base.h"

#include <stddef.h>

typedef struct Platform_File_Info {
   u64 write_time;
   b32 is_directory;
} Platform_File_Info;

typedef struct Platform_Process_Result {
	String output;
	u32 exit_code;
	u32 error_code;
	b32 launched;
} Platform_Process_Result;

b32 platform_file_info(String path, Platform_File_Info *info);
b32 platform_current_directory(Arena *arena, String *result);
b32 platform_absolute_path(Arena *arena, String path, String *result);
b32 platform_read_entire_file(Arena *arena, String path, String *result);
b32 platform_write_entire_file(String path, const void *data, size_t size);
b32 platform_create_directory(String path);
b32 platform_local_app_data(Arena *arena, String *result);
b32 platform_get_environment(String name, Arena *arena, String *value);
b32 platform_get_environment_block(Arena *arena, String *block);
b32 platform_set_environment(String name, String value);
b32 platform_executable_resolves(String string);
b32 platform_run_command(const char *command_line, Arena *arena, Platform_Process_Result *result);
b32 platform_error_message(u32 error_code, Arena *arena, String *result);
b32 platform_capture_stdout(const char *command_line, Arena *arena, String *output, u32 *exit_code);
u64 platform_performance_counter(void);
u64 platform_performance_frequency(void);
void *platform_virtual_reserve(u64 size);
b32 platform_virtual_commit(void *memory, u64 size);
void platform_virtual_free(void *memory);
void platform_enable_console_colors(void);
b32 platform_console_supports_colors(b32 error_stream);
void platform_output_lock(void);
void platform_output_unlock(void);

#endif
