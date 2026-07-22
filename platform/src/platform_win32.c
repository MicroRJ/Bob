#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <limits.h>
#include <string.h>

static Platform_Error win32_platform_error(DWORD error);

static HANDLE win32_handle_from_file(Platform_File file)
{
	return (HANDLE)file.value;
}

static I64 win32_file_time_to_unix_ms(FILETIME time)
{
	ULARGE_INTEGER value;
	value.LowPart = time.dwLowDateTime;
	value.HighPart = time.dwHighDateTime;
	const U64 unix_epoch = 116444736000000000ull;
	if (value.QuadPart < unix_epoch) return 0;
	return (I64)((value.QuadPart - unix_epoch) / 10000ull);
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

B32 platform_debug_break(void)
{
	DebugBreak();
	return PLATFORM_TRUE;
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
		.created_unix_ms = win32_file_time_to_unix_ms(data.ftCreationTime),
		.accessed_unix_ms = win32_file_time_to_unix_ms(data.ftLastAccessTime),
		.modified_unix_ms = win32_file_time_to_unix_ms(data.ftLastWriteTime),
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

B32 platform_copy_file(const char *source, const char *destination, B32 overwrite)
{
	return source && destination && CopyFileA(source, destination, !overwrite) != 0;
}

B32 platform_move_file(const char *source, const char *destination, B32 overwrite)
{
	if (!source || !destination) return PLATFORM_FALSE;
	DWORD flags = MOVEFILE_COPY_ALLOWED;
	if (overwrite) flags |= MOVEFILE_REPLACE_EXISTING;
	return MoveFileExA(source, destination, flags) != 0;
}

static B32 win32_create_directory(const char *path)
{
	if (CreateDirectoryA(path, NULL)) return PLATFORM_TRUE;
	if (GetLastError() != ERROR_ALREADY_EXISTS) return PLATFORM_FALSE;
	DWORD attributes = GetFileAttributesA(path);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

B32 platform_create_directory(const char *path)
{
	return path && win32_create_directory(path);
}

B32 platform_create_directories(const char *path)
{
	if (!path || !path[0]) return PLATFORM_FALSE;
	SIZE_T size = strlen(path) + 1;
	char *copy = HeapAlloc(GetProcessHeap(), 0, size);
	if (!copy) return PLATFORM_FALSE;
	memcpy(copy, path, size);
	SIZE_T root = 0;
	if (copy[0] && copy[1] == ':') root = 2;
	else if (copy[0] == '/' || copy[0] == '\\') root = 1;
	if ((copy[0] == '/' || copy[0] == '\\') && (copy[1] == '/' || copy[1] == '\\')) {
		root = 2;
		U32 components = 0;
		while (copy[root] && components < 2) {
			if (copy[root] == '/' || copy[root] == '\\') ++components;
			++root;
		}
	}
	B32 result = PLATFORM_TRUE;
	for (SIZE_T index = root; copy[index]; ++index) {
		if (copy[index] != '/' && copy[index] != '\\') continue;
		if (index == root) continue;
		char separator = copy[index];
		copy[index] = 0;
		if (!win32_create_directory(copy)) result = PLATFORM_FALSE;
		copy[index] = separator;
		if (!result) break;
	}
	if (result) result = win32_create_directory(copy);
	HeapFree(GetProcessHeap(), 0, copy);
	return result;
}

B32 platform_remove_directory(const char *path)
{
	if (!path) return PLATFORM_FALSE;
	if (RemoveDirectoryA(path)) return PLATFORM_TRUE;
	DWORD error = GetLastError();
	return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

B32 platform_executable_resolves(const char *name)
{
	if (!name) return PLATFORM_FALSE;
	char buffer[32768];
	DWORD length = SearchPathA(NULL, name, ".exe", sizeof(buffer), buffer, NULL);
	return length > 0 && length < sizeof(buffer);
}

Platform_String_Result platform_get_current_directory(char *buffer, U64 capacity)
{
	Platform_String_Result result = {0};
	DWORD required = GetCurrentDirectoryA(0, NULL);
	if (!required) {
		result.os_error = GetLastError();
		result.error = win32_platform_error(result.os_error);
		return result;
	}
	result.required_capacity = required;
	result.size = required - 1;
	if (!buffer) return result;
	if (capacity < required || capacity > MAXDWORD) {
		result.error = PLATFORM_ERROR_BUFFER_TOO_SMALL;
		return result;
	}
	DWORD length = GetCurrentDirectoryA((DWORD)capacity, buffer);
	if (!length || length >= capacity) {
		result.os_error = GetLastError();
		result.error = result.os_error ? win32_platform_error(result.os_error) : PLATFORM_ERROR_BUFFER_TOO_SMALL;
		return result;
	}
	result.size = length;
	return result;
}

B32 platform_set_current_directory(const char *path)
{
	return path && SetCurrentDirectoryA(path) != 0;
}

Platform_String_Result platform_get_absolute_path(const char *path, char *buffer, U64 capacity)
{
	Platform_String_Result result = {0};
	if (!path) {
		result.error = PLATFORM_ERROR_INVALID_ARGUMENT;
		return result;
	}
	DWORD required = GetFullPathNameA(path, 0, NULL, NULL);
	if (!required) {
		result.os_error = GetLastError();
		result.error = win32_platform_error(result.os_error);
		return result;
	}
	result.required_capacity = required;
	result.size = required - 1;
	if (!buffer) return result;
	if (capacity < required || capacity > MAXDWORD) {
		result.error = PLATFORM_ERROR_BUFFER_TOO_SMALL;
		return result;
	}
	DWORD length = GetFullPathNameA(path, (DWORD)capacity, buffer, NULL);
	if (!length || length >= capacity) {
		result.os_error = GetLastError();
		result.error = result.os_error ? win32_platform_error(result.os_error) : PLATFORM_ERROR_BUFFER_TOO_SMALL;
		return result;
	}
	result.size = length;
	return result;
}

typedef struct Win32_Directory {
	HANDLE find;
	WIN32_FIND_DATAA data;
	B32 pending;
} Win32_Directory;

Platform_Directory_Open_Result platform_open_directory(const char *path)
{
	Platform_Directory_Open_Result result = {0};
	if (!path) {
		result.error = PLATFORM_ERROR_INVALID_ARGUMENT;
		return result;
	}
	Win32_Directory *directory = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*directory));
	if (!directory) {
		result.error = PLATFORM_ERROR_OUT_OF_MEMORY;
		result.os_error = ERROR_NOT_ENOUGH_MEMORY;
		return result;
	}
	SIZE_T path_size = strlen(path);
	SIZE_T search_size = path_size + 3;
	char *search = HeapAlloc(GetProcessHeap(), 0, search_size);
	if (!search) {
		HeapFree(GetProcessHeap(), 0, directory);
		result.error = PLATFORM_ERROR_OUT_OF_MEMORY;
		result.os_error = ERROR_NOT_ENOUGH_MEMORY;
		return result;
	}
	memcpy(search, path, path_size);
	SIZE_T cursor = path_size;
	if (cursor && search[cursor - 1] != '/' && search[cursor - 1] != '\\') search[cursor++] = '\\';
	search[cursor++] = '*';
	search[cursor] = 0;
	directory->find = FindFirstFileA(search, &directory->data);
	HeapFree(GetProcessHeap(), 0, search);
	if (directory->find == INVALID_HANDLE_VALUE) {
		result.os_error = GetLastError();
		result.error = win32_platform_error(result.os_error);
		HeapFree(GetProcessHeap(), 0, directory);
		return result;
	}
	directory->pending = PLATFORM_TRUE;
	result.directory.handle = (UPtr)directory;
	return result;
}

Platform_Directory_Next_Result platform_next_directory(Platform_Directory *directory_value, char *name, U64 capacity)
{
	Platform_Directory_Next_Result result = {0};
	if (!directory_value || !directory_value->handle) {
		result.error = PLATFORM_ERROR_INVALID_ARGUMENT;
		return result;
	}
	Win32_Directory *directory = (Win32_Directory *)directory_value->handle;
	for (;;) {
		if (!directory->pending) {
			if (!FindNextFileA(directory->find, &directory->data)) {
				DWORD error = GetLastError();
				if (error != ERROR_NO_MORE_FILES) {
					result.error = win32_platform_error(error);
					result.os_error = error;
				}
				return result;
			}
			directory->pending = PLATFORM_TRUE;
		}
		if (strcmp(directory->data.cFileName, ".") != 0 && strcmp(directory->data.cFileName, "..") != 0) break;
		directory->pending = PLATFORM_FALSE;
	}
	result.name_size = strlen(directory->data.cFileName);
	if (!name || capacity <= result.name_size) {
		result.error = PLATFORM_ERROR_BUFFER_TOO_SMALL;
		return result;
	}
	memcpy(name, directory->data.cFileName, result.name_size + 1);
	ULARGE_INTEGER size;
	size.LowPart = directory->data.nFileSizeLow;
	size.HighPart = directory->data.nFileSizeHigh;
	result.info.size = size.QuadPart;
	result.info.created_unix_ms = win32_file_time_to_unix_ms(directory->data.ftCreationTime);
	result.info.accessed_unix_ms = win32_file_time_to_unix_ms(directory->data.ftLastAccessTime);
	result.info.modified_unix_ms = win32_file_time_to_unix_ms(directory->data.ftLastWriteTime);
	result.info.is_directory = (directory->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	result.info.is_symbolic_link = (directory->data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
	result.has_entry = PLATFORM_TRUE;
	directory->pending = PLATFORM_FALSE;
	return result;
}

void platform_close_directory(Platform_Directory *directory_value)
{
	if (!directory_value || !directory_value->handle) return;
	Win32_Directory *directory = (Win32_Directory *)directory_value->handle;
	FindClose(directory->find);
	HeapFree(GetProcessHeap(), 0, directory);
	*directory_value = (Platform_Directory){0};
}

Platform_Environment_Result platform_get_environment(const char *name, char *buffer, U64 capacity)
{
	Platform_Environment_Result result = {0};
	if (!name) {
		result.error = PLATFORM_ERROR_INVALID_ARGUMENT;
		return result;
	}
	SetLastError(ERROR_SUCCESS);
	DWORD required = GetEnvironmentVariableA(name, NULL, 0);
	if (required == 0) {
		DWORD error = GetLastError();
		if (error == ERROR_ENVVAR_NOT_FOUND) return result;
		if (error != ERROR_SUCCESS) {
			result.error = win32_platform_error(error);
			result.os_error = error;
			return result;
		}
		result.found = PLATFORM_TRUE;
		result.required_capacity = 1;
		if (buffer && capacity) buffer[0] = 0;
		else if (buffer) result.error = PLATFORM_ERROR_BUFFER_TOO_SMALL;
		return result;
	}
	result.found = PLATFORM_TRUE;
	result.size = required - 1;
	result.required_capacity = required;
	if (!buffer) return result;
	if (capacity < required || capacity > MAXDWORD) {
		result.error = PLATFORM_ERROR_BUFFER_TOO_SMALL;
		return result;
	}
	DWORD length = GetEnvironmentVariableA(name, buffer, (DWORD)capacity);
	if (length >= capacity) {
		result.error = PLATFORM_ERROR_BUFFER_TOO_SMALL;
		result.required_capacity = (U64)length + 1;
		return result;
	}
	if (length == 0 && GetLastError() != ERROR_SUCCESS) {
		result.os_error = GetLastError();
		result.error = win32_platform_error(result.os_error);
		return result;
	}
	result.size = length;
	return result;
}

Platform_Result platform_set_environment(const char *name, const char *value)
{
	Platform_Result result = {0};
	if (!name) {
		result.error = PLATFORM_ERROR_INVALID_ARGUMENT;
		return result;
	}
	if (!SetEnvironmentVariableA(name, value)) {
		result.os_error = GetLastError();
		result.error = win32_platform_error(result.os_error);
	}
	return result;
}

Platform_String_Result platform_get_environment_block(char *buffer, U64 capacity)
{
	Platform_String_Result result = {0};
	LPCH environment = GetEnvironmentStringsA();
	if (!environment) {
		result.os_error = GetLastError();
		result.error = win32_platform_error(result.os_error);
		return result;
	}
	LPCH cursor = environment;
	while (*cursor) cursor += strlen(cursor) + 1;
	result.size = (U64)(cursor - environment);
	result.required_capacity = result.size + 1;
	if (buffer) {
		if (capacity < result.required_capacity) result.error = PLATFORM_ERROR_BUFFER_TOO_SMALL;
		else memcpy(buffer, environment, (SIZE_T)result.required_capacity);
	}
	FreeEnvironmentStringsA(environment);
	return result;
}

static HANDLE win32_standard_stream(Platform_Standard_Stream stream)
{
	if (stream == PLATFORM_STANDARD_OUTPUT) return GetStdHandle(STD_OUTPUT_HANDLE);
	if (stream == PLATFORM_STANDARD_ERROR) return GetStdHandle(STD_ERROR_HANDLE);
	return NULL;
}

B32 platform_stream_is_console(Platform_Standard_Stream stream)
{
	HANDLE handle = win32_standard_stream(stream);
	DWORD mode = 0;
	return handle && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) != 0;
}

B32 platform_console_supports_colors(Platform_Standard_Stream stream)
{
	HANDLE handle = win32_standard_stream(stream);
	DWORD mode = 0;
	return handle && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) && (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

Platform_Result platform_enable_console_colors(Platform_Standard_Stream stream)
{
	Platform_Result result = {0};
	HANDLE handle = win32_standard_stream(stream);
	DWORD mode = 0;
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		result.error = PLATFORM_ERROR_INVALID_ARGUMENT;
		return result;
	}
	if (!GetConsoleMode(handle, &mode)) {
		result.os_error = GetLastError();
		result.error = win32_platform_error(result.os_error);
		return result;
	}
	if (!SetConsoleMode(handle, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
		result.os_error = GetLastError();
		result.error = win32_platform_error(result.os_error);
	}
	return result;
}

Platform_Write_Result platform_write_console(Platform_Standard_Stream stream, const void *data, U64 size)
{
	Platform_Write_Result result = {0};
	HANDLE handle = win32_standard_stream(stream);
	if (!handle || handle == INVALID_HANDLE_VALUE || (!data && size)) {
		result.error = PLATFORM_ERROR_INVALID_ARGUMENT;
		return result;
	}
	while (result.size < size) {
		U64 remaining = size - result.size;
		DWORD request = remaining > MAXDWORD ? MAXDWORD : (DWORD)remaining;
		DWORD written = 0;
		if (!WriteFile(handle, (const char *)data + result.size, request, &written, NULL)) {
			result.os_error = GetLastError();
			result.error = win32_platform_error(result.os_error);
			return result;
		}
		if (written == 0) break;
		result.size += written;
	}
	return result;
}

Platform_String_Result platform_error_message(U32 os_error, char *buffer, U64 capacity)
{
	Platform_String_Result result = {0};
	if (!os_error) {
		result.error = PLATFORM_ERROR_INVALID_ARGUMENT;
		return result;
	}
	char *message = NULL;
	DWORD length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, os_error, 0, (char *)&message, 0, NULL);
	if (!length) {
		result.os_error = GetLastError();
		result.error = win32_platform_error(result.os_error);
		return result;
	}
	while (length && (message[length - 1] == '\r' || message[length - 1] == '\n' || message[length - 1] == ' ')) --length;
	result.size = length;
	result.required_capacity = (U64)length + 1;
	if (buffer) {
		if (capacity < result.required_capacity) result.error = PLATFORM_ERROR_BUFFER_TOO_SMALL;
		else {
			memcpy(buffer, message, length);
			buffer[length] = 0;
		}
	}
	LocalFree(message);
	return result;
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
	case ERROR_INSUFFICIENT_BUFFER:
	case ERROR_MORE_DATA: return PLATFORM_ERROR_BUFFER_TOO_SMALL;
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

U64 platform_current_process_id(void)
{
	return GetCurrentProcessId();
}

void platform_exit_process(I32 exit_code)
{
	ExitProcess((UINT)exit_code);
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
