#ifndef PLATFORM_H
#define PLATFORM_H

#include "base.h"

#include <stddef.h>

typedef struct Platform_File_Info {
   u64 write_time;
   b32 is_directory;
   b32 is_symbolic_link;
} Platform_File_Info;

typedef struct Platform_Directory_Entry {
	String name;
	b32 is_directory;
	b32 is_symbolic_link;
} Platform_Directory_Entry;

typedef struct Platform_Directory_Entries {
	Platform_Directory_Entry *items;
	u32 count;
} Platform_Directory_Entries;

typedef struct Platform_Process_Result {
	String output;
	u32 exit_code;
	u32 error_code;
	b32 launched;
} Platform_Process_Result;

typedef struct Platform_Process_Options {
	b32 capture_stderr;
	b32 hide_window;
} Platform_Process_Options;

typedef struct Platform_Thread Platform_Thread;
typedef struct Platform_Mutex Platform_Mutex;
typedef struct Platform_Condition Platform_Condition;
typedef u32 Platform_Thread_Function(void *data);

b32 platform_file_info(String path, Platform_File_Info *info);
b32 platform_list_directory(Arena *arena, String path, Platform_Directory_Entries *result);
b32 platform_current_directory(Arena *arena, String *result);
b32 platform_absolute_path(Arena *arena, String path, String *result);
b32 platform_read_entire_file(Arena *arena, String path, String *result);
b32 platform_write_entire_file(String path, const void *data, size_t size);
b32 bob_platform_copy_file(String source, String destination, b32 overwrite);
b32 bob_platform_move_file(String source, String destination, b32 overwrite);
b32 bob_platform_remove_file(String path);
b32 bob_platform_remove_directory(String path);
b32 bob_platform_create_directory(String path);
b32 bob_platform_create_directories(String path);
b32 bob_platform_local_app_data(Arena *arena, String *result);
b32 bob_platform_get_environment(String name, Arena *arena, String *value);
b32 bob_platform_get_environment_block(Arena *arena, String *block);
b32 bob_platform_set_environment(String name, String value);
b32 bob_platform_executable_resolves(String string);
b32 platform_run_command(String command_line, Arena *arena, Platform_Process_Options options, Platform_Process_Result *result);
b32 bob_platform_error_message(u32 error_code, Arena *arena, String *result);
u64 platform_performance_counter(void);
u64 platform_performance_frequency(void);
void *platform_virtual_reserve(u64 size);
b32 platform_virtual_commit(void *memory, u64 size);
void platform_virtual_free(void *memory);
void bob_platform_enable_console_colors(void);
b32 platform_console_supports_colors(b32 error_stream);
void platform_output_lock(void);
void platform_output_unlock(void);
Platform_Thread *platform_thread_create(Platform_Thread_Function *function, void *data);
b32 platform_thread_join(Platform_Thread *thread);
void platform_thread_destroy(Platform_Thread *thread);
u64 platform_current_thread_id(void);
Platform_Mutex *platform_mutex_create(void);
void platform_mutex_destroy(Platform_Mutex *mutex);
void platform_mutex_lock(Platform_Mutex *mutex);
void platform_mutex_unlock(Platform_Mutex *mutex);
Platform_Condition *platform_condition_create(void);
void platform_condition_destroy(Platform_Condition *condition);
b32 platform_condition_wait(Platform_Condition *condition, Platform_Mutex *mutex);
void platform_condition_signal(Platform_Condition *condition);
void platform_condition_broadcast(Platform_Condition *condition);

#endif
