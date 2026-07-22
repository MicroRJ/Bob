typedef struct Shared_Platform_Process {
	u64 handle;
	u64 standard_output;
	u64 standard_error;
} Shared_Platform_Process;

typedef struct Shared_Platform_Process_Options {
	const char *working_directory;
	i32 capture_standard_output;
	i32 capture_standard_error;
	i32 hide_window;
} Shared_Platform_Process_Options;

typedef struct Shared_Platform_Process_Start_Result {
	Shared_Platform_Process process;
	i32 error;
	u32 os_error;
} Shared_Platform_Process_Start_Result;

typedef struct Shared_Platform_Process_Read_Result {
	u64 size;
	i32 error;
	u32 os_error;
	i32 end_of_stream;
} Shared_Platform_Process_Read_Result;

typedef struct Shared_Platform_Process_Wait_Result {
	i32 status;
	i32 error;
	u32 os_error;
	u32 exit_code;
} Shared_Platform_Process_Wait_Result;

typedef struct Shared_Platform_Thread {
	u64 handle;
} Shared_Platform_Thread;

typedef struct Shared_Platform_Mutex {
	u64 storage[8];
} Shared_Platform_Mutex;

typedef struct Shared_Platform_Condition {
	u64 storage[8];
} Shared_Platform_Condition;

typedef struct Shared_Platform_Thread_Start_Result {
	Shared_Platform_Thread thread;
	i32 error;
	u32 os_error;
} Shared_Platform_Thread_Start_Result;

typedef struct Shared_Platform_Thread_Join_Result {
	u32 return_code;
	i32 error;
	u32 os_error;
} Shared_Platform_Thread_Join_Result;

typedef struct Shared_Platform_Result {
	i32 error;
	u32 os_error;
} Shared_Platform_Result;

typedef u32 Shared_Platform_Thread_Function(void *context);

enum {
	SHARED_PROCESS_WAIT_COMPLETED,
	SHARED_PROCESS_WAIT_TIMED_OUT,
	SHARED_PROCESS_WAIT_FAILED,
};

void platform_virtual_release(void *memory);
u64 platform_counter(void);
u64 platform_counter_frequency(void);
Shared_Platform_Process_Start_Result platform_start_process(const char *command_line, Shared_Platform_Process_Options options);
Shared_Platform_Process_Read_Result platform_read_process_output(Shared_Platform_Process *process, void *data, u64 capacity);
Shared_Platform_Process_Read_Result platform_read_process_error(Shared_Platform_Process *process, void *data, u64 capacity);
Shared_Platform_Process_Wait_Result platform_wait_process(Shared_Platform_Process process, u32 milliseconds);
void platform_close_process(Shared_Platform_Process *process);
Shared_Platform_Thread_Start_Result platform_start_thread(Shared_Platform_Thread_Function *function, void *context);
Shared_Platform_Thread_Join_Result platform_join_thread(Shared_Platform_Thread thread);
void platform_close_thread(Shared_Platform_Thread *thread);
void platform_init_mutex(Shared_Platform_Mutex *mutex);
void platform_lock_mutex(Shared_Platform_Mutex *mutex);
void platform_unlock_mutex(Shared_Platform_Mutex *mutex);
void platform_destroy_mutex(Shared_Platform_Mutex *mutex);
void platform_init_condition(Shared_Platform_Condition *condition);
Shared_Platform_Result platform_wait_condition(Shared_Platform_Condition *condition, Shared_Platform_Mutex *mutex);
void platform_signal_condition(Shared_Platform_Condition *condition);
void platform_broadcast_condition(Shared_Platform_Condition *condition);
void platform_destroy_condition(Shared_Platform_Condition *condition);

struct Platform_Thread {
	Shared_Platform_Thread thread;
};

struct Platform_Mutex {
	Shared_Platform_Mutex mutex;
};

struct Platform_Condition {
	Shared_Platform_Condition condition;
};

void platform_virtual_free(void *memory)
{
	platform_virtual_release(memory);
}

u64 platform_performance_counter(void)
{
	return platform_counter();
}

u64 platform_performance_frequency(void)
{
	return platform_counter_frequency();
}

static b32 append_process_pipe(Shared_Platform_Process *process, Arena *arena, b32 standard_error, u32 *error_code)
{
	char buffer[4096];
	Shared_Platform_Process_Read_Result read = standard_error ? platform_read_process_error(process, buffer, sizeof(buffer)) : platform_read_process_output(process, buffer, sizeof(buffer));
	if (read.error) {
		*error_code = read.os_error ? read.os_error : (u32)read.error;
		return false;
	}
	if (read.size && !arena_push_copy(arena, read.size, buffer)) {
		*error_code = ERROR_NOT_ENOUGH_MEMORY;
		return false;
	}
	return read.size != 0;
}

b32 platform_run_command(String command_line, Arena *arena, Platform_Process_Options options, Platform_Process_Result *result)
{
	u64 mark;
	Shared_Platform_Process_Start_Result start;
	Shared_Platform_Process_Wait_Result wait = {0};
	if (!string_is_terminated(command_line) || !arena || !result) return false;
	mark = arena_mark(arena);
	*result = (Platform_Process_Result){ .exit_code = UINT32_MAX };
	start = platform_start_process(command_line.data, (Shared_Platform_Process_Options){ .capture_standard_output = true, .capture_standard_error = options.capture_stderr, .hide_window = options.hide_window });
	if (start.error) {
		result->error_code = start.os_error ? start.os_error : (u32)start.error;
		return false;
	}
	result->launched = true;
	for (;;) {
		while (append_process_pipe(&start.process, arena, false, &result->error_code)) {}
		if (options.capture_stderr) while (append_process_pipe(&start.process, arena, true, &result->error_code)) {}
		if (result->error_code) goto failure;
		wait = platform_wait_process(start.process, 1);
		if (wait.status == SHARED_PROCESS_WAIT_COMPLETED) break;
		if (wait.status == SHARED_PROCESS_WAIT_FAILED) {
			result->error_code = wait.os_error ? wait.os_error : (u32)wait.error;
			goto failure;
		}
	}
	while (append_process_pipe(&start.process, arena, false, &result->error_code)) {}
	if (options.capture_stderr) while (append_process_pipe(&start.process, arena, true, &result->error_code)) {}
	if (result->error_code) goto failure;
	result->exit_code = wait.exit_code;
	result->output.data = (char *)arena->data + mark;
	result->output.size = arena->used - mark;
	platform_close_process(&start.process);
	return true;

failure:
	platform_close_process(&start.process);
	arena_restore(arena, mark);
	result->output = (String){0};
	return false;
}

b32 platform_error_message(u32 error_code, Arena *arena, String *result)
{
	u64 mark;
	char *data;
	DWORD length;
	if (!error_code || !arena || !result) return false;
	mark = arena_mark(arena);
	data = arena_reserve(arena, KILOBYTES(4));
	if (!data) return false;
	length = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error_code, 0, data, KILOBYTES(4), NULL);
	if (!length) return false;
	while (length && (data[length - 1] == '\r' || data[length - 1] == '\n' || data[length - 1] == ' ')) --length;
	data[length] = 0;
	if (!arena_push(arena, length + 1)) {
		arena_restore(arena, mark);
		return false;
	}
	result->data = data;
	result->size = length;
	return true;
}

Platform_Thread *platform_thread_create(Platform_Thread_Function *function, void *data)
{
	Platform_Thread *thread = calloc(1, sizeof(*thread));
	if (!thread) return NULL;
	Shared_Platform_Thread_Start_Result start = platform_start_thread(function, data);
	if (start.error) {
		free(thread);
		return NULL;
	}
	thread->thread = start.thread;
	return thread;
}

b32 platform_thread_join(Platform_Thread *thread)
{
	return thread && platform_join_thread(thread->thread).error == 0;
}

void platform_thread_destroy(Platform_Thread *thread)
{
	if (!thread) return;
	platform_close_thread(&thread->thread);
	free(thread);
}

Platform_Mutex *platform_mutex_create(void)
{
	Platform_Mutex *mutex = calloc(1, sizeof(*mutex));
	if (mutex) platform_init_mutex(&mutex->mutex);
	return mutex;
}

void platform_mutex_destroy(Platform_Mutex *mutex)
{
	if (!mutex) return;
	platform_destroy_mutex(&mutex->mutex);
	free(mutex);
}

void platform_mutex_lock(Platform_Mutex *mutex)
{
	platform_lock_mutex(&mutex->mutex);
}

void platform_mutex_unlock(Platform_Mutex *mutex)
{
	platform_unlock_mutex(&mutex->mutex);
}

Platform_Condition *platform_condition_create(void)
{
	Platform_Condition *condition = calloc(1, sizeof(*condition));
	if (condition) platform_init_condition(&condition->condition);
	return condition;
}

void platform_condition_destroy(Platform_Condition *condition)
{
	if (!condition) return;
	platform_destroy_condition(&condition->condition);
	free(condition);
}

b32 platform_condition_wait(Platform_Condition *condition, Platform_Mutex *mutex)
{
	return platform_wait_condition(&condition->condition, &mutex->mutex).error == 0;
}

void platform_condition_signal(Platform_Condition *condition)
{
	platform_signal_condition(&condition->condition);
}

void platform_condition_broadcast(Platform_Condition *condition)
{
	platform_broadcast_condition(&condition->condition);
}
