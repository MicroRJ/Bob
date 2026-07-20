#include "compiler_command.h"

static b32 next_argument(Arena *arena, String command_line, u64 *offset, String *result)
{
	u64 cursor = *offset;
	while (cursor < command_line.size && (command_line.data[cursor] == ' ' || command_line.data[cursor] == '\t')) ++cursor;
	if (cursor == command_line.size) {
		*offset = cursor;
		return false;
	}

	void *start = arena_top(arena);
	b32 quoted = false;
	while (cursor < command_line.size && (quoted || (command_line.data[cursor] != ' ' && command_line.data[cursor] != '\t')))
	{
		char character = command_line.data[cursor++];
		if (character == '"') quoted = !quoted;
		else arena_append_char(arena, character);
	}
	*result = arena_string_from(arena, start);
	arena_finalize_string(arena, *result);
	*offset = cursor;
	return true;
}

b32 compiler_command_parse(Arena *arena, String command_line, Compiler_Command *result)
{
	if (!arena || !result) return false;
	*result = (Compiler_Command){0};
	if (!command_line.data) return true;

	u64 maximum_count = command_line.size / 2 + 1;
	if (maximum_count > UINT32_MAX) return false;
	result->include_directories.items = arena_push_zero_aligned(arena, maximum_count * sizeof(String), _Alignof(String));

	Scratch scratch = begin_different_scratch(arena);
	u64 offset = 0;
	for (;;)
	{
		u64 mark = arena_mark(scratch.arena);
		String argument;
		if (!next_argument(scratch.arena, command_line, &offset, &argument)) break;

		String directory = {0};
		if (string_is(argument, "/I") || string_is(argument, "-I") || string_is(argument, "-isystem")) {
			arena_restore(scratch.arena, mark);
			mark = arena_mark(scratch.arena);
			if (!next_argument(scratch.arena, command_line, &offset, &directory)) break;
		}
		else if (argument.size > 2 && (argument.data[0] == '/' || argument.data[0] == '-') && argument.data[1] == 'I') {
			directory = string_slice(argument, 2, argument.size - 2);
		}

		if (directory.size) {
			result->include_directories.items[result->include_directories.count++] = arena_push_string_copy(arena, directory);
		}
		arena_restore(scratch.arena, mark);
	}
	end_scratch(scratch);
	return true;
}
