#include "platform.h"

#include <assert.h>
#include <string.h>

typedef struct Thread_Test_Context {
	Platform_Mutex mutex;
	Platform_Condition condition;
	U32 value;
} Thread_Test_Context;

static U32 thread_test_main(void *context_pointer)
{
	Thread_Test_Context *context = context_pointer;
	platform_lock_mutex(&context->mutex);
	context->value = 42;
	platform_signal_condition(&context->condition);
	platform_unlock_mutex(&context->mutex);
	return 7;
}

int main(void)
{
	void *memory = platform_virtual_reserve(4096);
	assert(memory);
	assert(platform_virtual_commit(memory, 4096));
	memset(memory, 0x5a, 4096);
	assert(platform_virtual_decommit(memory, 4096));
	platform_virtual_release(memory);

	assert(platform_counter_frequency() > 0);
	U64 before = platform_counter();
	platform_sleep(1);
	assert(platform_counter() >= before);

	const char *path = "build/platform_test_file.tmp";
	const char expected[] = "platapuss";
	char actual[sizeof(expected)] = {0};
	U64 transferred = 0;
	U64 size = 0;

	Platform_File file = platform_access_file(path, PLATFORM_FILE_CREATE_ALWAYS, PLATFORM_FILE_READ | PLATFORM_FILE_WRITE | PLATFORM_FILE_SHARE_READ);
	assert(platform_file_is_valid(file));
	assert(platform_write_file(file, expected, sizeof(expected), &transferred));
	assert(transferred == sizeof(expected));
	assert(platform_get_file_size(file, &size));
	assert(size == sizeof(expected));
	assert(platform_set_file_cursor(file, PLATFORM_SEEK_BEGIN, 0, NULL));
	assert(platform_read_file(file, actual, sizeof(actual), &transferred));
	assert(transferred == sizeof(actual));
	assert(memcmp(actual, expected, sizeof(expected)) == 0);
	platform_close_file(file);

	Platform_File_Info info;
	assert(platform_get_file_info(path, &info));
	assert(info.size == sizeof(expected));
	assert(!info.is_directory);
	assert(platform_remove_file(path));

	Platform_Process_Start_Result start = platform_start_process("cmd.exe /d /c echo platapuss", (Platform_Process_Options){ .capture_standard_output = PLATFORM_TRUE, .capture_standard_error = PLATFORM_TRUE, .hide_window = PLATFORM_TRUE });
	assert(start.error == PLATFORM_ERROR_NONE);
	assert(start.os_error == 0);
	Platform_Process process = start.process;
	assert(platform_process_is_valid(process));
	Platform_Process_Wait_Result wait = platform_wait_process(process, PLATFORM_WAIT_INFINITE);
	assert(wait.status == PLATFORM_PROCESS_WAIT_COMPLETED);
	assert(wait.error == PLATFORM_ERROR_NONE);
	assert(wait.os_error == 0);
	assert(wait.exit_code == 0);
	char process_output[64] = {0};
	Platform_Process_Read_Result read = platform_read_process_output(&process, process_output, sizeof(process_output));
	assert(read.error == PLATFORM_ERROR_NONE);
	assert(read.os_error == 0);
	assert(read.size >= sizeof("platapuss") - 1);
	assert(memcmp(process_output, "platapuss", sizeof("platapuss") - 1) == 0);
	platform_close_process(&process);
	assert(!platform_process_is_valid(process));

	Thread_Test_Context thread_context = {0};
	platform_init_mutex(&thread_context.mutex);
	platform_init_condition(&thread_context.condition);
	platform_lock_mutex(&thread_context.mutex);
	Platform_Thread_Start_Result thread_start = platform_start_thread(thread_test_main, &thread_context);
	assert(thread_start.error == PLATFORM_ERROR_NONE);
	while (thread_context.value == 0) {
		Platform_Result condition_wait = platform_wait_condition(&thread_context.condition, &thread_context.mutex);
		assert(condition_wait.error == PLATFORM_ERROR_NONE);
	}
	platform_unlock_mutex(&thread_context.mutex);
	assert(thread_context.value == 42);
	assert(platform_current_thread_id() != 0);
	Platform_Thread_Join_Result thread_join = platform_join_thread(thread_start.thread);
	assert(thread_join.error == PLATFORM_ERROR_NONE);
	assert(thread_join.return_code == 7);
	platform_close_thread(&thread_start.thread);
	assert(!platform_thread_is_valid(thread_start.thread));
	platform_destroy_condition(&thread_context.condition);
	platform_destroy_mutex(&thread_context.mutex);
	return 0;
}
