static SRWLOCK output_lock = SRWLOCK_INIT;

static void enable_virtual_terminal(HANDLE output)
{
	DWORD mode;
	if (output != NULL && output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode)) SetConsoleMode(output, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

void platform_enable_console_colors(void)
{
	enable_virtual_terminal(GetStdHandle(STD_OUTPUT_HANDLE));
	enable_virtual_terminal(GetStdHandle(STD_ERROR_HANDLE));
}

b32 platform_console_supports_colors(b32 error_stream)
{
	HANDLE output = GetStdHandle(error_stream ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
	DWORD mode;
	return output != NULL && output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode) && (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

void platform_output_lock(void)
{
	AcquireSRWLockExclusive(&output_lock);
}

void platform_output_unlock(void)
{
	ReleaseSRWLockExclusive(&output_lock);
}
