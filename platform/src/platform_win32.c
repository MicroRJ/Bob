#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <limits.h>
#include <string.h>

static HANDLE win32_handle_from_file(Platform_File file)
{
	return (HANDLE)file.value;
}

static U64 win32_file_time(FILETIME time)
{
	ULARGE_INTEGER value;
	value.LowPart = time.dwLowDateTime;
	value.HighPart = time.dwHighDateTime;
	return value.QuadPart;
}

void *platform_virtual_reserve(U64 size)
{
	if (size > SIZE_MAX) return NULL;
	return VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_READWRITE);
}

B32 platform_virtual_commit(void *memory, U64 size)
{
	return memory && size <= SIZE_MAX && VirtualAlloc(memory, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

B32 platform_virtual_decommit(void *memory, U64 size)
{
	return memory && size <= SIZE_MAX && VirtualFree(memory, (SIZE_T)size, MEM_DECOMMIT) != 0;
}

void platform_virtual_release(void *memory)
{
	if (memory) VirtualFree(memory, 0, MEM_RELEASE);
}

U64 platform_counter(void)
{
	LARGE_INTEGER value;
	return QueryPerformanceCounter(&value) ? (U64)value.QuadPart : 0;
}

U64 platform_counter_frequency(void)
{
	LARGE_INTEGER value;
	return QueryPerformanceFrequency(&value) ? (U64)value.QuadPart : 0;
}

void platform_sleep(U64 milliseconds)
{
	while (milliseconds > MAXDWORD) {
		Sleep(MAXDWORD);
		milliseconds -= MAXDWORD;
	}
	Sleep((DWORD)milliseconds);
}

Platform_File platform_access_file(const char *path, Platform_File_Intent intent, Platform_File_Access access)
{
	DWORD desired_access = 0;
	DWORD share_mode = 0;
	DWORD disposition = OPEN_EXISTING;
	DWORD attributes = FILE_ATTRIBUTE_NORMAL;

	if (!path) return (Platform_File){0};
	if (access & PLATFORM_FILE_READ) desired_access |= GENERIC_READ;
	if (access & PLATFORM_FILE_WRITE) desired_access |= GENERIC_WRITE;
	if (access & PLATFORM_FILE_EXECUTE) desired_access |= GENERIC_EXECUTE;
	if (access & PLATFORM_FILE_SHARE_READ) share_mode |= FILE_SHARE_READ;
	if (access & PLATFORM_FILE_SHARE_WRITE) share_mode |= FILE_SHARE_WRITE;
	if (access & PLATFORM_FILE_SHARE_DELETE) share_mode |= FILE_SHARE_DELETE;
	if (access & PLATFORM_FILE_NO_BUFFERING) attributes |= FILE_FLAG_NO_BUFFERING;

	switch (intent) {
	case PLATFORM_FILE_CREATE_ALWAYS: disposition = CREATE_ALWAYS; break;
	case PLATFORM_FILE_CREATE_NEW: disposition = CREATE_NEW; break;
	case PLATFORM_FILE_OPEN_ALWAYS: disposition = OPEN_ALWAYS; break;
	case PLATFORM_FILE_OPEN_EXISTING: disposition = OPEN_EXISTING; break;
	case PLATFORM_FILE_TRUNCATE_EXISTING: disposition = TRUNCATE_EXISTING; break;
	default: return (Platform_File){0};
	}

	HANDLE handle = CreateFileA(path, desired_access, share_mode, NULL, disposition, attributes, NULL);
	if (handle == INVALID_HANDLE_VALUE) return (Platform_File){0};
	return (Platform_File){ .value = (UPtr)handle };
}

B32 platform_file_is_valid(Platform_File file)
{
	return file.value != 0 && win32_handle_from_file(file) != INVALID_HANDLE_VALUE;
}

void platform_close_file(Platform_File file)
{
	if (platform_file_is_valid(file)) CloseHandle(win32_handle_from_file(file));
}

B32 platform_get_file_info(const char *path, Platform_File_Info *info)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!path || !info || !GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return PLATFORM_FALSE;

	ULARGE_INTEGER size;
	size.LowPart = data.nFileSizeLow;
	size.HighPart = data.nFileSizeHigh;
	*info = (Platform_File_Info){
		.size = size.QuadPart,
		.creation_time = win32_file_time(data.ftCreationTime),
		.access_time = win32_file_time(data.ftLastAccessTime),
		.write_time = win32_file_time(data.ftLastWriteTime),
		.is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
		.is_symbolic_link = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0,
	};
	return PLATFORM_TRUE;
}

B32 platform_remove_file(const char *path)
{
	if (!path) return PLATFORM_FALSE;
	if (DeleteFileA(path)) return PLATFORM_TRUE;
	DWORD error = GetLastError();
	return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

B32 platform_get_file_size(Platform_File file, U64 *size)
{
	LARGE_INTEGER value;
	if (!size || !platform_file_is_valid(file) || !GetFileSizeEx(win32_handle_from_file(file), &value) || value.QuadPart < 0) return PLATFORM_FALSE;
	*size = (U64)value.QuadPart;
	return PLATFORM_TRUE;
}

B32 platform_set_file_cursor(Platform_File file, Platform_Seek_Origin origin, I64 distance, U64 *position)
{
	static const DWORD origins[] = { FILE_BEGIN, FILE_CURRENT, FILE_END };
	LARGE_INTEGER move;
	LARGE_INTEGER result;
	if (!platform_file_is_valid(file) || origin < PLATFORM_SEEK_BEGIN || origin > PLATFORM_SEEK_END) return PLATFORM_FALSE;
	move.QuadPart = distance;
	if (!SetFilePointerEx(win32_handle_from_file(file), move, &result, origins[origin]) || result.QuadPart < 0) return PLATFORM_FALSE;
	if (position) *position = (U64)result.QuadPart;
	return PLATFORM_TRUE;
}

B32 platform_read_file(Platform_File file, void *data, U64 size, U64 *bytes_read)
{
	U64 total = 0;
	if (bytes_read) *bytes_read = 0;
	if (!platform_file_is_valid(file) || (!data && size)) return PLATFORM_FALSE;
	while (total < size) {
		U64 remaining = size - total;
		DWORD request = remaining > MAXDWORD ? MAXDWORD : (DWORD)remaining;
		DWORD received = 0;
		if (!ReadFile(win32_handle_from_file(file), (char *)data + total, request, &received, NULL)) return PLATFORM_FALSE;
		total += received;
		if (received < request) break;
	}
	if (bytes_read) *bytes_read = total;
	return PLATFORM_TRUE;
}

B32 platform_write_file(Platform_File file, const void *data, U64 size, U64 *bytes_written)
{
	U64 total = 0;
	if (bytes_written) *bytes_written = 0;
	if (!platform_file_is_valid(file) || (!data && size)) return PLATFORM_FALSE;
	while (total < size) {
		U64 remaining = size - total;
		DWORD request = remaining > MAXDWORD ? MAXDWORD : (DWORD)remaining;
		DWORD written = 0;
		if (!WriteFile(win32_handle_from_file(file), (const char *)data + total, request, &written, NULL)) return PLATFORM_FALSE;
		total += written;
		if (written == 0) return PLATFORM_FALSE;
	}
	if (bytes_written) *bytes_written = total;
	return PLATFORM_TRUE;
}

static void win32_close_handle(HANDLE *handle)
{
	if (*handle) CloseHandle(*handle);
	*handle = NULL;
}

static B32 win32_create_process_pipe(HANDLE *read_pipe, HANDLE *write_pipe)
{
	SECURITY_ATTRIBUTES security = {0};
	security.nLength = sizeof(security);
	security.bInheritHandle = TRUE;
	if (!CreatePipe(read_pipe, write_pipe, &security, 0)) return PLATFORM_FALSE;
	if (SetHandleInformation(*read_pipe, HANDLE_FLAG_INHERIT, 0)) return PLATFORM_TRUE;
	win32_close_handle(read_pipe);
	win32_close_handle(write_pipe);
	return PLATFORM_FALSE;
}

static Platform_Error win32_platform_error(DWORD error)
{
	switch (error) {
	case ERROR_SUCCESS: return PLATFORM_ERROR_NONE;
	case ERROR_INVALID_PARAMETER: return PLATFORM_ERROR_INVALID_ARGUMENT;
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND: return PLATFORM_ERROR_NOT_FOUND;
	case ERROR_ACCESS_DENIED: return PLATFORM_ERROR_ACCESS_DENIED;
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS: return PLATFORM_ERROR_ALREADY_EXISTS;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY: return PLATFORM_ERROR_OUT_OF_MEMORY;
	case ERROR_NOT_SUPPORTED:
	case ERROR_CALL_NOT_IMPLEMENTED: return PLATFORM_ERROR_NOT_SUPPORTED;
	case ERROR_BROKEN_PIPE:
	case ERROR_NO_DATA: return PLATFORM_ERROR_BROKEN_PIPE;
	default: return PLATFORM_ERROR_UNKNOWN;
	}
}

Platform_Process_Start_Result platform_start_process(const char *command_line, Platform_Process_Options options)
{
	Platform_Process_Start_Result result = {0};
	STARTUPINFOA startup = {0};
	PROCESS_INFORMATION process = {0};
	HANDLE output_read = NULL;
	HANDLE output_write = NULL;
	HANDLE error_read = NULL;
	HANDLE error_write = NULL;
	char *mutable_command_line = NULL;
	B32 inherit_handles = options.capture_standard_output || options.capture_standard_error;

	if (!command_line) {
		result.error = PLATFORM_ERROR_INVALID_ARGUMENT;
		return result;
	}
	if (options.capture_standard_output && !win32_create_process_pipe(&output_read, &output_write)) goto failure;
	if (options.capture_standard_error && !win32_create_process_pipe(&error_read, &error_write)) goto failure;

	startup.cb = sizeof(startup);
	if (inherit_handles) {
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		startup.hStdOutput = output_write ? output_write : GetStdHandle(STD_OUTPUT_HANDLE);
		startup.hStdError = error_write ? error_write : GetStdHandle(STD_ERROR_HANDLE);
	}

	SIZE_T command_size = strlen(command_line) + 1;
	mutable_command_line = HeapAlloc(GetProcessHeap(), 0, command_size);
	if (!mutable_command_line) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		goto failure;
	}
	memcpy(mutable_command_line, command_line, command_size);
	if (!CreateProcessA(NULL, mutable_command_line, NULL, NULL, inherit_handles, options.hide_window ? CREATE_NO_WINDOW : 0, NULL, options.working_directory, &startup, &process)) goto failure;

	HeapFree(GetProcessHeap(), 0, mutable_command_line);
	win32_close_handle(&process.hThread);
	win32_close_handle(&output_write);
	win32_close_handle(&error_write);
	result.process.handle = (UPtr)process.hProcess;
	result.process.standard_output = (UPtr)output_read;
	result.process.standard_error = (UPtr)error_read;
	return result;

failure:
	DWORD error = GetLastError();
	if (mutable_command_line) HeapFree(GetProcessHeap(), 0, mutable_command_line);
	win32_close_handle(&process.hThread);
	win32_close_handle(&process.hProcess);
	win32_close_handle(&output_read);
	win32_close_handle(&output_write);
	win32_close_handle(&error_read);
	win32_close_handle(&error_write);
	result.error = win32_platform_error(error);
	result.os_error = error;
	return result;
}

B32 platform_process_is_valid(Platform_Process process)
{
	return process.handle != 0;
}

static Platform_Process_Read_Result win32_read_process_pipe(UPtr *pipe_value, void *data, U64 capacity)
{
	Platform_Process_Read_Result result = {0};
	HANDLE pipe = (HANDLE)*pipe_value;
	DWORD available = 0;
	DWORD received = 0;
	if (!pipe) {
		result.end_of_stream = PLATFORM_TRUE;
		return result;
	}
	if (!data && capacity) {
		result.error = PLATFORM_ERROR_INVALID_ARGUMENT;
		return result;
	}
	if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) {
		DWORD error = GetLastError();
		if (error != ERROR_BROKEN_PIPE) {
			result.error = win32_platform_error(error);
			result.os_error = error;
			return result;
		}
		CloseHandle(pipe);
		*pipe_value = 0;
		result.end_of_stream = PLATFORM_TRUE;
		return result;
	}
	if (available == 0 || capacity == 0) return result;
	DWORD request = capacity < available ? (DWORD)capacity : available;
	if (capacity > MAXDWORD && available == MAXDWORD) request = MAXDWORD;
	if (!ReadFile(pipe, data, request, &received, NULL)) {
		DWORD error = GetLastError();
		result.error = win32_platform_error(error);
		result.os_error = error;
		return result;
	}
	result.size = received;
	return result;
}

Platform_Process_Read_Result platform_read_process_output(Platform_Process *process, void *data, U64 capacity)
{
	if (!process) return (Platform_Process_Read_Result){ .error = PLATFORM_ERROR_INVALID_ARGUMENT };
	return win32_read_process_pipe(&process->standard_output, data, capacity);
}

Platform_Process_Read_Result platform_read_process_error(Platform_Process *process, void *data, U64 capacity)
{
	if (!process) return (Platform_Process_Read_Result){ .error = PLATFORM_ERROR_INVALID_ARGUMENT };
	return win32_read_process_pipe(&process->standard_error, data, capacity);
}

Platform_Process_Wait_Result platform_wait_process(Platform_Process process, U32 milliseconds)
{
	Platform_Process_Wait_Result result = { .status = PLATFORM_PROCESS_WAIT_FAILED };
	if (!platform_process_is_valid(process)) {
		result.error = PLATFORM_ERROR_INVALID_ARGUMENT;
		return result;
	}
	DWORD wait = WaitForSingleObject((HANDLE)process.handle, milliseconds);
	if (wait == WAIT_TIMEOUT) {
		result.status = PLATFORM_PROCESS_WAIT_TIMED_OUT;
		return result;
	}
	if (wait != WAIT_OBJECT_0) {
		DWORD error = GetLastError();
		result.error = win32_platform_error(error);
		result.os_error = error;
		return result;
	}
	DWORD code = 0;
	if (!GetExitCodeProcess((HANDLE)process.handle, &code)) {
		DWORD error = GetLastError();
		result.error = win32_platform_error(error);
		result.os_error = error;
		return result;
	}
	result.status = PLATFORM_PROCESS_WAIT_COMPLETED;
	result.exit_code = code;
	return result;
}

void platform_close_process(Platform_Process *process)
{
	if (!process) return;
	HANDLE handle = (HANDLE)process->handle;
	HANDLE output = (HANDLE)process->standard_output;
	HANDLE error = (HANDLE)process->standard_error;
	win32_close_handle(&handle);
	win32_close_handle(&output);
	win32_close_handle(&error);
	*process = (Platform_Process){0};
}

typedef struct Win32_Thread_Start {
	Platform_Thread_Function *function;
	void *context;
} Win32_Thread_Start;

_Static_assert(sizeof(Platform_Mutex) >= sizeof(SRWLOCK), "Platform_Mutex storage is too small");
_Static_assert(_Alignof(Platform_Mutex) >= _Alignof(SRWLOCK), "Platform_Mutex storage is under-aligned");
_Static_assert(sizeof(Platform_Condition) >= sizeof(CONDITION_VARIABLE), "Platform_Condition storage is too small");
_Static_assert(_Alignof(Platform_Condition) >= _Alignof(CONDITION_VARIABLE), "Platform_Condition storage is under-aligned");

static DWORD WINAPI win32_thread_entry(void *context)
{
	Win32_Thread_Start start = *(Win32_Thread_Start *)context;
	HeapFree(GetProcessHeap(), 0, context);
	return start.function(start.context);
}

Platform_Thread_Start_Result platform_start_thread(Platform_Thread_Function *function, void *context)
{
	Platform_Thread_Start_Result result = {0};
	if (!function) {
		result.error = PLATFORM_ERROR_INVALID_ARGUMENT;
		return result;
	}
	Win32_Thread_Start *start = HeapAlloc(GetProcessHeap(), 0, sizeof(*start));
	if (!start) {
		result.error = PLATFORM_ERROR_OUT_OF_MEMORY;
		result.os_error = ERROR_NOT_ENOUGH_MEMORY;
		return result;
	}
	start->function = function;
	start->context = context;
	HANDLE thread = CreateThread(NULL, 0, win32_thread_entry, start, 0, NULL);
	if (!thread) {
		DWORD error = GetLastError();
		HeapFree(GetProcessHeap(), 0, start);
		result.error = win32_platform_error(error);
		result.os_error = error;
		return result;
	}
	result.thread.handle = (UPtr)thread;
	return result;
}

B32 platform_thread_is_valid(Platform_Thread thread)
{
	return thread.handle != 0;
}

Platform_Thread_Join_Result platform_join_thread(Platform_Thread thread)
{
	Platform_Thread_Join_Result result = {0};
	if (!platform_thread_is_valid(thread)) {
		result.error = PLATFORM_ERROR_INVALID_ARGUMENT;
		return result;
	}
	if (WaitForSingleObject((HANDLE)thread.handle, INFINITE) != WAIT_OBJECT_0) {
		DWORD error = GetLastError();
		result.error = win32_platform_error(error);
		result.os_error = error;
		return result;
	}
	DWORD return_code = 0;
	if (!GetExitCodeThread((HANDLE)thread.handle, &return_code)) {
		DWORD error = GetLastError();
		result.error = win32_platform_error(error);
		result.os_error = error;
		return result;
	}
	result.return_code = return_code;
	return result;
}

void platform_close_thread(Platform_Thread *thread)
{
	if (!thread) return;
	HANDLE handle = (HANDLE)thread->handle;
	win32_close_handle(&handle);
	*thread = (Platform_Thread){0};
}

U64 platform_current_thread_id(void)
{
	return GetCurrentThreadId();
}

void platform_init_mutex(Platform_Mutex *mutex)
{
	if (mutex) InitializeSRWLock((SRWLOCK *)mutex->storage);
}

void platform_lock_mutex(Platform_Mutex *mutex)
{
	if (mutex) AcquireSRWLockExclusive((SRWLOCK *)mutex->storage);
}

void platform_unlock_mutex(Platform_Mutex *mutex)
{
	if (mutex) ReleaseSRWLockExclusive((SRWLOCK *)mutex->storage);
}

void platform_destroy_mutex(Platform_Mutex *mutex)
{
	if (mutex) *mutex = (Platform_Mutex){0};
}

void platform_init_condition(Platform_Condition *condition)
{
	if (condition) InitializeConditionVariable((CONDITION_VARIABLE *)condition->storage);
}

Platform_Result platform_wait_condition(Platform_Condition *condition, Platform_Mutex *mutex)
{
	Platform_Result result = {0};
	if (!condition || !mutex) {
		result.error = PLATFORM_ERROR_INVALID_ARGUMENT;
		return result;
	}
	if (!SleepConditionVariableSRW((CONDITION_VARIABLE *)condition->storage, (SRWLOCK *)mutex->storage, INFINITE, 0)) {
		DWORD error = GetLastError();
		result.error = win32_platform_error(error);
		result.os_error = error;
	}
	return result;
}

void platform_signal_condition(Platform_Condition *condition)
{
	if (condition) WakeConditionVariable((CONDITION_VARIABLE *)condition->storage);
}

void platform_broadcast_condition(Platform_Condition *condition)
{
	if (condition) WakeAllConditionVariable((CONDITION_VARIABLE *)condition->storage);
}

void platform_destroy_condition(Platform_Condition *condition)
{
	if (condition) *condition = (Platform_Condition){0};
}
