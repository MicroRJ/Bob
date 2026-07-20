b32 platform_executable_resolves(String name)
{
	if (!string_is_terminated(name)) return false;
	Scratch scratch = begin_scratch();
	DWORD capacity = KILOBYTES(32);
	char *resolved = arena_reserve(scratch.arena, capacity);
	DWORD length = resolved ? SearchPathA(NULL, name.data, ".exe", capacity, resolved, NULL) : 0;
	b32 found = length > 0 && length < capacity;
	end_scratch(scratch);
	return found;
}

b32 platform_file_info(String path, Platform_File_Info *info)
{
	WIN32_FILE_ATTRIBUTE_DATA attributes;
	ULARGE_INTEGER write_time;

	if (!string_is_terminated(path) || !info) return false;
	if (!GetFileAttributesExA(path.data, GetFileExInfoStandard, &attributes)) return false;

	write_time.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
	write_time.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
	info->write_time = write_time.QuadPart;
	info->is_directory = (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	return true;
}

b32 platform_list_directory(Arena *arena, String path, Platform_Directory_Entries *result)
{
	typedef struct Entry_Node Entry_Node;
	struct Entry_Node
	{
		Entry_Node *next;
		String name;
		b32 is_directory;
		b32 is_symbolic_link;
	};

	if (!arena || !result) return false;
	*result = (Platform_Directory_Entries){0};
	Scratch scratch = begin_different_scratch(arena);
	void *start = arena_top(scratch.arena);
	arena_append_str(scratch.arena, path);
	if (path.size && path.data[path.size - 1] != '/' && path.data[path.size - 1] != '\\') arena_append_char(scratch.arena, '\\');
	arena_append_char(scratch.arena, '*');
	String search = arena_string_from(scratch.arena, start);
	arena_finalize_string(scratch.arena, search);

	WIN32_FIND_DATAA data;
	HANDLE find = FindFirstFileA(search.data, &data);
	if (find == INVALID_HANDLE_VALUE) {
		end_scratch(scratch);
		return false;
	}

	Entry_Node *first = NULL;
	Entry_Node *last = NULL;
	do
	{
		if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
		Entry_Node *node = arena_push_zero_aligned(scratch.arena, sizeof(*node), _Alignof(Entry_Node));
		node->name = arena_push_cstring(scratch.arena, data.cFileName);
		node->is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		node->is_symbolic_link = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
		if (last) last->next = node;
		else first = node;
		last = node;
		++result->count;
	}
	while (FindNextFileA(find, &data));
	FindClose(find);

	result->items = arena_push_zero_aligned(arena, result->count * sizeof(*result->items), _Alignof(Platform_Directory_Entry));
	u32 index = 0;
	for (Entry_Node *node = first; node; node = node->next)
	{
		result->items[index].name = arena_push_string_copy(arena, node->name);
		result->items[index].is_directory = node->is_directory;
		result->items[index].is_symbolic_link = node->is_symbolic_link;
		++index;
	}
	end_scratch(scratch);
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
	for (;;) {
		required = GetCurrentDirectoryA(0, NULL);
		if (required == 0) goto failure;
		data = arena_push(arena, required);
		if (!data) goto failure;
		length = GetCurrentDirectoryA(required, data);
		if (length > 0 && length < required) {
			result->data = data;
			result->size = length;
			return true;
		}
		arena_restore(arena, mark);
		if (length == 0) goto failure;
	}

failure:
	arena_restore(arena, mark);
	return false;
}

b32 platform_absolute_path(Arena *arena, String path, String *result)
{
	u64 mark;
	DWORD required;
	DWORD length;
	char *data;
	if (!arena || !string_is_terminated(path) || !result) return false;
	mark = arena_mark(arena);
	result->data = NULL;
	result->size = 0;
	for (;;) {
		required = GetFullPathNameA(path.data, 0, NULL, NULL);
		if (required == 0) goto failure;
		data = arena_push(arena, required);
		if (!data) goto failure;
		length = GetFullPathNameA(path.data, required, data, NULL);
		if (length > 0 && length < required) {
			result->data = data;
			result->size = length;
			return true;
		}
		arena_restore(arena, mark);
		if (length == 0) goto failure;
	}

failure:
	arena_restore(arena, mark);
	return false;
}

b32 platform_read_entire_file(Arena *arena, String path, String *result)
{
	HANDLE file;
	LARGE_INTEGER file_size;
	char *data;
	size_t total = 0;
	u64 mark;

	if (!arena || !string_is_terminated(path) || !result) return false;
	mark = arena_mark(arena);
	result->data = NULL;
	result->size = 0;
	file = CreateFileA(path.data, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) return false;

	u64 size = !GetFileSizeEx(file, &file_size);
	if (size || file_size.QuadPart < 0 || (u64)file_size.QuadPart > SIZE_MAX - 1) {
		CloseHandle(file);
		return false;
	}
	data = arena_push(arena, (u64)file_size.QuadPart + 1);
	if (!data) {
		CloseHandle(file);
		return false;
	}
	while (total < (size_t)file_size.QuadPart) {
		size_t remaining = (size_t)file_size.QuadPart - total;
		DWORD request = remaining > UINT32_MAX ? UINT32_MAX : (DWORD)remaining;
		DWORD received = 0;
		if (!ReadFile(file, data + total, request, &received, NULL) || received == 0) {
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

b32 platform_write_entire_file(String path, const void *data, size_t size)
{
	HANDLE file;
	size_t total = 0;

	if (!string_is_terminated(path) || (!data && size)) return false;
	file = CreateFileA(path.data, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) return false;
	while (total < size) {
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

b32 platform_create_directory(String path)
{
	if (!string_is_terminated(path)) return false;
	if (CreateDirectoryA(path.data, NULL)) return true;
	return GetLastError() == ERROR_ALREADY_EXISTS;
}
