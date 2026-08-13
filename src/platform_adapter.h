#ifndef BOB_PLATFORM_ADAPTER_H
#define BOB_PLATFORM_ADAPTER_H

#include "base.h"

#include <stddef.h>

typedef struct Bob_Platform_File_Info {
	u64 size;
   i64 modified_unix_ms;
} Bob_Platform_File_Info;

typedef struct Bob_Platform_Process_Result {
	String output;
	u32 exit_code;
	u32 error_code;
	b32 launched;
} Bob_Platform_Process_Result;

typedef struct Bob_Platform_Process_Options {
	String working_directory;
	b32 capture_stderr;
	b32 hide_window;
} Bob_Platform_Process_Options;

b32 bob_platform_file_info(String path, Bob_Platform_File_Info *info);
b32 bob_platform_current_directory(Arena *arena, String *result);
b32 bob_platform_absolute_path(Arena *arena, String path, String *result);
b32 bob_platform_read_entire_file(Arena *arena, String path, String *result);
b32 bob_platform_write_entire_file(String path, const void *data, size_t size);
b32 bob_platform_create_directory(String path);
b32 bob_platform_local_app_data(Arena *arena, String *result);
b32 bob_platform_get_environment(String name, Arena *arena, String *value);
b32 bob_platform_get_environment_block(Arena *arena, String *block);
b32 bob_platform_set_environment(String name, String value);
b32 bob_platform_executable_resolves(String string);
b32 bob_platform_run_command(String command_line, Arena *arena, Bob_Platform_Process_Options options, Bob_Platform_Process_Result *result);
b32 bob_platform_error_message(u32 error_code, Arena *arena, String *result);
void bob_platform_enable_console_colors(void);
b32 bob_platform_console_supports_colors(b32 error_stream);
void bob_platform_output_lock(void);
void bob_platform_output_unlock(void);

#endif
