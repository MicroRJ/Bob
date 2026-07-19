#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>

static SRWLOCK output_lock = SRWLOCK_INIT;

b32 platform_executable_resolves(String name)
{
   Scratch scratch = get_scratch();
	DWORD capacity = KILOBYTES(32);
   char *resolved = arena_reserve(scratch.arena, capacity);
   DWORD length = resolved ? SearchPathA(NULL, name.data, ".exe", capacity, resolved, NULL) : 0;
	b32 found = length > 0 && length < capacity;
	end_scratch(scratch);
	return found;
}

b32 platform_file_info(const char *path, Platform_File_Info *info)
{
   WIN32_FILE_ATTRIBUTE_DATA attributes;
   ULARGE_INTEGER write_time;

   if (!path || !info || !GetFileAttributesExA(path, GetFileExInfoStandard, &attributes)) {
      return false;
   }

   write_time.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
   write_time.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
   info->write_time = write_time.QuadPart;
   info->is_directory = (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
   return true;
}

b32 platform_current_directory(Arena *arena, String *result)
{
   u64 mark;
   DWORD required;
   DWORD length;
   char *data;
   if (!arena || !result) return false;
   mark = arena_mark(arena);
   result->data = NULL;
   result->size = 0;
   for (;;)
   {
      required = GetCurrentDirectoryA(0, NULL);
      if (required == 0) { goto failure; }
      data = arena_push(arena, required);
      if (!data) { goto failure; }
      length = GetCurrentDirectoryA(required, data);
      if (length > 0 && length < required)
      {
         result->data = data;
         result->size = length;
         return true;
      }
      arena_restore(arena, mark);
      if (length == 0) { goto failure; }
   }
   failure:
   arena_restore(arena, mark);
   return false;
}

b32 platform_absolute_path(Arena *arena, const char *path, String *result)
{
   u64 mark;
   DWORD required;
   DWORD length;
   char *data;
   if (!arena || !path || !result) return false;
   mark = arena_mark(arena);
   result->data = NULL;
   result->size = 0;
   for (;;)
   {
      required = GetFullPathNameA(path, 0, NULL, NULL);
      if (required == 0) { goto failure; }
      data = arena_push(arena, required);
      if (!data) { goto failure; }
      length = GetFullPathNameA(path, required, data, NULL);
      if (length > 0 && length < required)
      {
         result->data = data;
         result->size = length;
         return true;
      }
      arena_restore(arena, mark);
      if (length == 0) { goto failure; }
   }
   failure:
   arena_restore(arena, mark);
   return false;
}

b32 platform_read_entire_file(Arena *arena, const char *path, String *result)
{
   HANDLE file;
   LARGE_INTEGER file_size;
   char *data;
   size_t total = 0;
   u64 mark;

   if (!arena || !path || !result) return false;
   mark = arena_mark(arena);
   result->data = NULL;
   result->size = 0;
   file = CreateFileA(
      path,
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      NULL
   );
   if (file == INVALID_HANDLE_VALUE) return false;

   u64 size = !GetFileSizeEx(file, &file_size);
   if (size ||file_size.QuadPart < 0 || (u64) file_size.QuadPart > SIZE_MAX - 1) {
      CloseHandle(file);
      return false;
   }
   data = arena_push(arena, (u64)file_size.QuadPart + 1);
   if (!data) {
      CloseHandle(file);
      return false;
   }
   while (total < (size_t)file_size.QuadPart)
   {
      size_t remaining = (size_t)file_size.QuadPart - total;
      DWORD request = remaining > UINT32_MAX ? UINT32_MAX : (DWORD)remaining;
      DWORD received = 0;
      if (!ReadFile(file, data + total, request, &received, NULL) || received == 0)
      {
         CloseHandle(file);
         arena_restore(arena, mark);
         return false;
      }
      total += received;
   }
   CloseHandle(file);
   data[total] = 0;
   result->data = data;
   result->size = total;
   return true;
}

b32 platform_write_entire_file(const char *path, const void *data, size_t size)
{
   HANDLE file;
   size_t total = 0;

   if (!path || (!data && size)) return false;
   file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE) return false;
   while (total < size)
   {
      size_t remaining = size - total;
      DWORD request = remaining > UINT32_MAX ? UINT32_MAX : (DWORD)remaining;
      DWORD written = 0;
      if (!WriteFile(file, (const char *)data + total, request, &written, NULL) || written == 0) {
         CloseHandle(file);
         return false;
      }
      total += written;
   }
   CloseHandle(file);
   return true;
}

b32 platform_create_directory(const char *path)
{
   if (!path) return false;
   if (CreateDirectoryA(path, NULL)) return true;
   return GetLastError() == ERROR_ALREADY_EXISTS;
}

b32 platform_local_app_data(Arena *arena, String *result) {
   return platform_get_environment("LOCALAPPDATA", arena, result) &&
   result->size > 0;
}

b32 platform_get_environment(const char *name, Arena *arena, String *value)
{
   DWORD required;
   DWORD length;
   char *data;
   if (!name || !arena || !value) return false;
   value->data = NULL;
   value->size = 0;
   SetLastError(ERROR_SUCCESS);
   required = GetEnvironmentVariableA(name, NULL, 0);
   if (required == 0) {
      DWORD error = GetLastError();
      return error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND;
   }
   data = arena_reserve(arena, required);
   if (!data) return false;
   length = GetEnvironmentVariableA(name, data, required);
   if (length == 0 || length >= required || !arena_push(arena, required)) return false;
   value->data = data;
   value->size = length;
   return true;
}

b32 platform_set_environment(const char *name, const char *value) {
   return name && SetEnvironmentVariableA(name, value) != 0;
}

b32 platform_capture_stdout(const char *command_line, Arena *arena, String *output, u32 *exit_code)
{
   SECURITY_ATTRIBUTES security = {0};
   STARTUPINFOA startup = {0};
   PROCESS_INFORMATION process = {0};
   HANDLE read_pipe = NULL;
   HANDLE write_pipe = NULL;
   Scratch scratch;
   String mutable_command;
   u64 mark;
   b32 success = false;

   if (!command_line || !arena || !output || !exit_code) return false;
   mark = arena_mark(arena);
   output->data = NULL;
   output->size = 0;
   *exit_code = UINT32_MAX;

   security.nLength = sizeof(security);
   security.bInheritHandle = TRUE;
   if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
      goto escape;
   }
   if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
   	goto escape;
   }

   startup.cb = sizeof(startup);
   startup.dwFlags = STARTF_USESTDHANDLES;
   startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
   startup.hStdOutput = write_pipe;
   startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

   scratch = get_scratch();
   mutable_command = arena_push_cstring(scratch.arena, command_line);
   if (
      !mutable_command.data
      ||  !CreateProcessA(NULL, mutable_command.data, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process)
   ) {
      end_scratch(scratch);
      goto escape;
   }
   end_scratch(scratch);

   CloseHandle(write_pipe);
   write_pipe = NULL;
   CloseHandle(process.hThread);
   process.hThread = NULL;

   for (;;)
   {
      char *destination = arena_reserve(arena, KILOBYTES(16));
      DWORD received = 0;
      if (!destination) { goto escape; }
      if (!ReadFile(read_pipe, destination, KILOBYTES(16), &received, NULL)) {
         if (GetLastError() == ERROR_BROKEN_PIPE) { break; }
         goto escape;
      }
      if (received && !arena_push(arena, received)) { goto escape; }
   }

   if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0) { goto escape; }
   {
      DWORD process_exit_code = UINT32_MAX;
      if (!GetExitCodeProcess(process.hProcess, &process_exit_code)) { goto escape; }
      *exit_code = process_exit_code;
   }
   output->data = (char *)arena->data + mark;
   output->size = arena->used - mark;
   success = true;

   escape:
   if (!success) { arena_restore(arena, mark); }
   if (process.hThread) { CloseHandle(process.hThread); }
   if (process.hProcess) { CloseHandle(process.hProcess); }
   if (read_pipe) { CloseHandle(read_pipe); }
   if (write_pipe) { CloseHandle(write_pipe); }
   return success;
}

b32 platform_run_command(const char *command_line, Arena *arena, Platform_Process_Result *result)
{
	SECURITY_ATTRIBUTES security = {0};
	STARTUPINFOA startup = {0};
	PROCESS_INFORMATION process = {0};
	HANDLE read_pipe = NULL;
	HANDLE write_pipe = NULL;
	String mutable_command = {0};
	u64 mark;
	b32 output_ok = true;
	b32 completed = false;

	if (!command_line || !arena || !result) return false;
	mark = arena_mark(arena);
	memset(result, 0, sizeof(*result));
	result->exit_code = UINT32_MAX;

	security.nLength = sizeof(security);
	security.bInheritHandle = TRUE;
	if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
		result->error_code = GetLastError();
		goto escape;
	}
	if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
		result->error_code = GetLastError();
		goto escape;
	}

	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	startup.hStdOutput = write_pipe;
	startup.hStdError = write_pipe;

	mutable_command = arena_push_cstring(arena, command_line);
	if (!mutable_command.data) {
		result->error_code = ERROR_NOT_ENOUGH_MEMORY;
		goto escape;
	}
	if (!CreateProcessA(NULL, mutable_command.data, NULL, NULL, TRUE, 0,
		NULL, NULL, &startup, &process))
	{
		result->error_code = GetLastError();
		goto escape;
	}
	arena_restore(arena, mark);
	result->launched = true;

	CloseHandle(write_pipe);
	write_pipe = NULL;
	CloseHandle(process.hThread);
	process.hThread = NULL;

	for (;;)
	{
		char buffer[4096];
		DWORD received = 0;
		if (!ReadFile(read_pipe, buffer, sizeof(buffer), &received, NULL)) {
			if (GetLastError() == ERROR_BROKEN_PIPE) break;
			result->error_code = GetLastError();
			goto escape;
		}
		if (received && output_ok && !arena_push_copy(arena, received, buffer)) {
			output_ok = false;
		}
	}

	if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0) {
		result->error_code = GetLastError();
		goto escape;
	}
	{
		DWORD exit_code = UINT32_MAX;
		if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
			result->error_code = GetLastError();
			goto escape;
		}
		result->exit_code = exit_code;
	}
	if (!output_ok) {
		result->error_code = ERROR_NOT_ENOUGH_MEMORY;
		goto escape;
	}

	result->output.data = (char *)arena->data + mark;
	result->output.size = arena->used - mark;
	completed = true;

	escape:
	if (!completed) {
		arena_restore(arena, mark);
		result->output = (String){0};
	}
	if (process.hThread) CloseHandle(process.hThread);
	if (process.hProcess) CloseHandle(process.hProcess);
	if (read_pipe) CloseHandle(read_pipe);
	if (write_pipe) CloseHandle(write_pipe);
	return completed;
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
	length = FormatMessageA(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, error_code, 0, data, KILOBYTES(4), NULL
	);
	if (!length) return false;
	while (length && (data[length - 1] == '\r' || data[length - 1] == '\n' || data[length - 1] == ' ')) {
		--length;
	}
	data[length] = 0;
	if (!arena_push(arena, length + 1)) {
		arena_restore(arena, mark);
		return false;
	}
	result->data = data;
	result->size = length;
	return true;
}

u64 platform_performance_counter(void)
{
   LARGE_INTEGER counter;
   if (!QueryPerformanceCounter(&counter)) return 0;
   return (u64)counter.QuadPart;
}

u64 platform_performance_frequency(void)
{
   LARGE_INTEGER frequency;
   if (!QueryPerformanceFrequency(&frequency)) return 0;
   return (u64)frequency.QuadPart;
}

void *platform_virtual_reserve(u64 size) {
   if (size > SIZE_MAX) return NULL;
   return VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_READWRITE);
}

b32 platform_virtual_commit(void *memory, u64 size) {
   if (!memory || size > SIZE_MAX) return false;
   return VirtualAlloc(memory, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

void platform_virtual_free(void *memory) {
   if (memory) { VirtualFree(memory, 0, MEM_RELEASE); }
}

static void enable_virtual_terminal(HANDLE output)
{
   DWORD mode;

   if (output != NULL && output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode)) {
      SetConsoleMode(output, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
   }
}

void platform_enable_console_colors(void) {
   enable_virtual_terminal(GetStdHandle(STD_OUTPUT_HANDLE));
   enable_virtual_terminal(GetStdHandle(STD_ERROR_HANDLE));
}

b32 platform_console_supports_colors(b32 error_stream)
{
   HANDLE output = GetStdHandle(error_stream ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
   DWORD mode;
   return output != NULL && output != INVALID_HANDLE_VALUE &&
   GetConsoleMode(output, &mode) &&
   (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

void platform_output_lock(void) {
   AcquireSRWLockExclusive(&output_lock);
}

void platform_output_unlock(void) {
   ReleaseSRWLockExclusive(&output_lock);
}
