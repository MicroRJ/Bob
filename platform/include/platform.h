#ifndef PLATFORM_H
#define PLATFORM_H

#include "platform_types.h"

typedef struct Platform_File {
	UPtr value;
} Platform_File;

typedef enum Platform_File_Access {
	PLATFORM_FILE_READ          = 1 << 0,
	PLATFORM_FILE_WRITE         = 1 << 1,
	PLATFORM_FILE_EXECUTE       = 1 << 2,
	PLATFORM_FILE_SHARE_READ    = 1 << 3,
	PLATFORM_FILE_SHARE_WRITE   = 1 << 4,
	PLATFORM_FILE_SHARE_DELETE  = 1 << 5,
	PLATFORM_FILE_NO_BUFFERING  = 1 << 6,
} Platform_File_Access;

typedef enum Platform_File_Intent {
	PLATFORM_FILE_CREATE_ALWAYS,
	PLATFORM_FILE_CREATE_NEW,
	PLATFORM_FILE_OPEN_ALWAYS,
	PLATFORM_FILE_OPEN_EXISTING,
	PLATFORM_FILE_TRUNCATE_EXISTING,
} Platform_File_Intent;

typedef enum Platform_Seek_Origin {
	PLATFORM_SEEK_BEGIN,
	PLATFORM_SEEK_CURRENT,
	PLATFORM_SEEK_END,
} Platform_Seek_Origin;

typedef struct Platform_File_Info {
	U64 size;
	U64 creation_time;
	U64 access_time;
	U64 write_time;
	B32 is_directory;
	B32 is_symbolic_link;
} Platform_File_Info;

typedef struct Platform_Process {
	UPtr handle;
	UPtr standard_output;
	UPtr standard_error;
} Platform_Process;

typedef struct Platform_Process_Options {
	const char *working_directory;
	B32 capture_standard_output;
	B32 capture_standard_error;
	B32 hide_window;
} Platform_Process_Options;

typedef enum Platform_Error {
	PLATFORM_ERROR_NONE,
	PLATFORM_ERROR_UNKNOWN,
	PLATFORM_ERROR_INVALID_ARGUMENT,
	PLATFORM_ERROR_NOT_FOUND,
	PLATFORM_ERROR_ACCESS_DENIED,
	PLATFORM_ERROR_ALREADY_EXISTS,
	PLATFORM_ERROR_OUT_OF_MEMORY,
	PLATFORM_ERROR_NOT_SUPPORTED,
	PLATFORM_ERROR_BROKEN_PIPE,
} Platform_Error;

typedef struct Platform_Process_Start_Result {
	Platform_Process process;
	Platform_Error error;
	U32 os_error;
} Platform_Process_Start_Result;

typedef struct Platform_Process_Read_Result {
	U64 size;
	Platform_Error error;
	U32 os_error;
	B32 end_of_stream;
} Platform_Process_Read_Result;

typedef enum Platform_Process_Wait_Status {
	PLATFORM_PROCESS_WAIT_COMPLETED,
	PLATFORM_PROCESS_WAIT_TIMED_OUT,
	PLATFORM_PROCESS_WAIT_FAILED,
} Platform_Process_Wait_Status;

typedef struct Platform_Process_Wait_Result {
	Platform_Process_Wait_Status status;
	Platform_Error error;
	U32 os_error;
	U32 exit_code;
} Platform_Process_Wait_Result;

typedef U32 Platform_Thread_Function(void *context);

typedef struct Platform_Thread {
	UPtr handle;
} Platform_Thread;

typedef struct Platform_Mutex {
	U64 storage[8];
} Platform_Mutex;

typedef struct Platform_Condition {
	U64 storage[8];
} Platform_Condition;

typedef struct Platform_Thread_Start_Result {
	Platform_Thread thread;
	Platform_Error error;
	U32 os_error;
} Platform_Thread_Start_Result;

typedef struct Platform_Thread_Join_Result {
	U32 return_code;
	Platform_Error error;
	U32 os_error;
} Platform_Thread_Join_Result;

typedef struct Platform_Result {
	Platform_Error error;
	U32 os_error;
} Platform_Result;

#define PLATFORM_WAIT_INFINITE ((U32)-1)

void *platform_virtual_reserve(U64 size);
B32 platform_virtual_commit(void *memory, U64 size);
B32 platform_virtual_decommit(void *memory, U64 size);
void platform_virtual_release(void *memory);

U64 platform_counter(void);
U64 platform_counter_frequency(void);
void platform_sleep(U64 milliseconds);

Platform_File platform_access_file(const char *path, Platform_File_Intent intent, Platform_File_Access access);
B32 platform_file_is_valid(Platform_File file);
void platform_close_file(Platform_File file);
B32 platform_get_file_info(const char *path, Platform_File_Info *info);
B32 platform_remove_file(const char *path);
B32 platform_get_file_size(Platform_File file, U64 *size);
B32 platform_set_file_cursor(Platform_File file, Platform_Seek_Origin origin, I64 distance, U64 *position);
B32 platform_read_file(Platform_File file, void *data, U64 size, U64 *bytes_read);
B32 platform_write_file(Platform_File file, const void *data, U64 size, U64 *bytes_written);

Platform_Process_Start_Result platform_start_process(const char *command_line, Platform_Process_Options options);
B32 platform_process_is_valid(Platform_Process process);
Platform_Process_Read_Result platform_read_process_output(Platform_Process *process, void *data, U64 capacity);
Platform_Process_Read_Result platform_read_process_error(Platform_Process *process, void *data, U64 capacity);
Platform_Process_Wait_Result platform_wait_process(Platform_Process process, U32 milliseconds);
void platform_close_process(Platform_Process *process);

Platform_Thread_Start_Result platform_start_thread(Platform_Thread_Function *function, void *context);
B32 platform_thread_is_valid(Platform_Thread thread);
Platform_Thread_Join_Result platform_join_thread(Platform_Thread thread);
void platform_close_thread(Platform_Thread *thread);
U64 platform_current_thread_id(void);

void platform_init_mutex(Platform_Mutex *mutex);
void platform_lock_mutex(Platform_Mutex *mutex);
void platform_unlock_mutex(Platform_Mutex *mutex);
void platform_destroy_mutex(Platform_Mutex *mutex);

void platform_init_condition(Platform_Condition *condition);
Platform_Result platform_wait_condition(Platform_Condition *condition, Platform_Mutex *mutex);
void platform_signal_condition(Platform_Condition *condition);
void platform_broadcast_condition(Platform_Condition *condition);
void platform_destroy_condition(Platform_Condition *condition);

#endif
