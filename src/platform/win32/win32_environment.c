b32 platform_local_app_data(Arena *arena, String *result)
{
	return platform_get_environment(STRING_LITERAL("LOCALAPPDATA"), arena, result) && result->size > 0;
}

b32 platform_get_environment(String name, Arena *arena, String *value)
{
	DWORD required;
	DWORD length;
	char *data;
	if (!string_is_terminated(name) || !arena || !value) return false;
	value->data = NULL;
	value->size = 0;
	SetLastError(ERROR_SUCCESS);
	required = GetEnvironmentVariableA(name.data, NULL, 0);
	if (required == 0) {
		DWORD error = GetLastError();
		return error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND;
	}
	data = arena_reserve(arena, required);
	if (!data) return false;
	length = GetEnvironmentVariableA(name.data, data, required);
	if (length == 0 || length >= required || !arena_push(arena, required)) return false;
	value->data = data;
	value->size = length;
	return true;
}

b32 platform_get_environment_block(Arena *arena, String *block)
{
	LPCH environment;
	LPCH cursor;
	u64 size;
	char *copy;

	if (!arena || !block) return false;
	block->data = NULL;
	block->size = 0;
	environment = GetEnvironmentStringsA();
	if (!environment) return false;
	cursor = environment;
	while (*cursor) cursor += strlen(cursor) + 1;
	size = (u64)(cursor - environment);
	copy = arena_push_copy(arena, size + 1, environment);
	FreeEnvironmentStringsA(environment);
	if (!copy) return false;
	block->data = copy;
	block->size = size;
	return true;
}

b32 platform_set_environment(String name, String value)
{
	if (!string_is_terminated(name)) return false;
	if (value.data && !string_is_terminated(value)) return false;
	return SetEnvironmentVariableA(name.data, value.data) != 0;
}
