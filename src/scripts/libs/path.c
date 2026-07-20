#include "scripts/libs/path.h"

String script_path_join(Arena *arena, String left, String right)
{
	while (left.size > 1 && (left.data[left.size - 1] == '/' || left.data[left.size - 1] == '\\')) --left.size;
	while (left.size && right.size && (right.data[0] == '/' || right.data[0] == '\\')) {
		++right.data;
		--right.size;
	}
	void *start = arena_top(arena);
	for (u64 index = 0; index < left.size; ++index) arena_append_char(arena, left.data[index] == '\\' ? '/' : left.data[index]);
	if (left.size && right.size) arena_append_char(arena, '/');
	for (u64 index = 0; index < right.size; ++index) arena_append_char(arena, right.data[index] == '\\' ? '/' : right.data[index]);
	String result = arena_string_from(arena, start);
	arena_finalize_string(arena, result);
	return result;
}
