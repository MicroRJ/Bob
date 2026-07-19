b32 platform_run_command(String command_line, Arena *arena, Platform_Process_Options options, Platform_Process_Result *result)
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

	if (!command_line.data || !arena || !result) return false;
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
	startup.hStdError = options.capture_stderr ? write_pipe : GetStdHandle(STD_ERROR_HANDLE);

	mutable_command = arena_push_string_copy(arena, command_line);
	if (!mutable_command.data) {
		result->error_code = ERROR_NOT_ENOUGH_MEMORY;
		goto escape;
	}
	if (!CreateProcessA(NULL, mutable_command.data, NULL, NULL, TRUE, options.hide_window ? CREATE_NO_WINDOW : 0, NULL, NULL, &startup, &process)) {
		result->error_code = GetLastError();
		goto escape;
	}

	arena_restore(arena, mark);
	result->launched = true;
	CloseHandle(write_pipe);
	write_pipe = NULL;
	CloseHandle(process.hThread);
	process.hThread = NULL;

	for (;;) {
		char buffer[4096];
		DWORD received = 0;
		if (!ReadFile(read_pipe, buffer, sizeof(buffer), &received, NULL)) {
			if (GetLastError() == ERROR_BROKEN_PIPE) break;
			result->error_code = GetLastError();
			goto escape;
		}
		if (received && output_ok && !arena_push_copy(arena, received, buffer)) output_ok = false;
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
