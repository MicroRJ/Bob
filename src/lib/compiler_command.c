#include "compiler_command.h"

static String executable_name(String path)
{
	u64 start = 0;
	u64 i;
	for (i = 0; i < path.size; ++i) {
		if (path.data[i] == '/' || path.data[i] == '\\') start = i + 1;
	}
	path = string_slice(path, start, path.size - start);
	if (string_ends_with_insensitive(path, STRING_LITERAL(".exe"))) {
		path.size -= sizeof(".exe") - 1;
	}
	return path;
}

static Compiler_Kind compiler_kind_from_executable(String executable)
{
	String name = executable_name(executable);
	if (string_equal_insensitive(name, STRING_LITERAL("clang-cl"))) return COMPILER_KIND_CLANG_CL;
	if (string_equal_insensitive(name, STRING_LITERAL("clang")) ||
		string_equal_insensitive(name, STRING_LITERAL("clang++"))) return COMPILER_KIND_CLANG;
	if (string_equal_insensitive(name, STRING_LITERAL("gcc")) ||
		string_equal_insensitive(name, STRING_LITERAL("g++")) ||
		string_equal_insensitive(name, STRING_LITERAL("cc")) ||
		string_equal_insensitive(name, STRING_LITERAL("c++"))) return COMPILER_KIND_GCC;
	if (string_equal_insensitive(name, STRING_LITERAL("cl"))) return COMPILER_KIND_MSVC;
	return COMPILER_KIND_UNKNOWN;
}

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
	b32 first = true;
	for (;;)
	{
		u64 mark = arena_mark(scratch.arena);
		String argument;
		if (!next_argument(scratch.arena, command_line, &offset, &argument)) break;

		if (first) {
			result->executable = arena_push_string_copy(arena, argument);
			result->kind = compiler_kind_from_executable(argument);
			first = false;
		}
		if (string_equal_insensitive(argument, STRING_LITERAL("/c")) ||
			string_equal(argument, STRING_LITERAL("-c"))) {
			result->compiles = true;
		}
		if (string_equal_insensitive(argument, STRING_LITERAL("/sourceDependencies")) ||
			string_equal(argument, STRING_LITERAL("-MMD")) ||
			string_equal(argument, STRING_LITERAL("-MD")) ||
			string_equal(argument, STRING_LITERAL("-MF")) ||
			string_starts_with(argument, STRING_LITERAL("-MF"))) {
			result->generates_dependencies = true;
		}

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

b32 compiler_command_add_dependencies(Arena *arena, String command_line, String dependency_file, String *result)
{
	Compiler_Command command;
	void *start;

	if (!arena || !result || !command_line.data || !dependency_file.data) return false;
	if (!compiler_command_parse(arena, command_line, &command) ||
		command.kind == COMPILER_KIND_UNKNOWN || !command.compiles ||
		command.generates_dependencies) {
		return false;
	}

	start = arena_top(arena);
	arena_append_str(arena, command_line);
	switch (command.kind) {
	case COMPILER_KIND_CLANG:
	case COMPILER_KIND_GCC:
		arena_append_text(arena, " -MD -MF \"");
		arena_append_str(arena, dependency_file);
		arena_append_char(arena, '"');
		break;
	case COMPILER_KIND_CLANG_CL:
		arena_append_text(arena, " /clang:-MD /clang:-MF\"");
		arena_append_str(arena, dependency_file);
		arena_append_char(arena, '"');
		break;
	case COMPILER_KIND_MSVC:
		arena_append_text(arena, " /sourceDependencies \"");
		arena_append_str(arena, dependency_file);
		arena_append_char(arena, '"');
		break;
	default:
		return false;
	}
	*result = arena_string_from(arena, start);
	arena_finalize_string(arena, *result);
	return true;
}
