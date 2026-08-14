#include "compiler_command.h"

static String executable_name(String path)
{
	u64 start = 0;
	u64 i;
	for (i = 0; i < path.size; ++i) {
		if (path.data[i] == '/' || path.data[i] == '\\') start = i + 1;
	}
	path = string_slice(path, start, path.size - start);
	if (string_ends_with_insensitive(path, LIT(".exe"))) {
		path.size -= sizeof(".exe") - 1;
	}
	return path;
}

// Match every start against the compiler, the start is either 0 or the position
// right after a '-'.
static b32 compiler_name_matches(String name, String compiler)
{
	for (u64 start = 0; start + compiler.size <= name.size; ++start)
	{
		if (start > 0 && name.data[start - 1] != '-') {
			continue;
		}
		if (!string_equal_insensitive(string_slice(name, start, compiler.size), compiler)) {
			continue;
		}

		u64 end = start + compiler.size;
		if (end == name.size || name.data[end] == '-') {
			return true;
		}
	}
	return false;
}

static Compiler_Kind compiler_kind_from_executable(String executable)
{
	String name = executable_name(executable);
	if (compiler_name_matches(name, LIT("clang-cl"))) return COMPILER_KIND_CLANG_CL;
	if (compiler_name_matches(name, LIT("clang")) ||
		compiler_name_matches(name, LIT("clang++"))) return COMPILER_KIND_CLANG;
	if (compiler_name_matches(name, LIT("gcc")) ||
		compiler_name_matches(name, LIT("g++")) ||
		compiler_name_matches(name, LIT("cc")) ||
		compiler_name_matches(name, LIT("c++"))) return COMPILER_KIND_GCC;
	if (string_equal_insensitive(name, LIT("cl"))) return COMPILER_KIND_MSVC;
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

	Scratch scratch = begin_different_scratch(arena);
	u64 offset = 0;
	u64 executable_mark = arena_mark(scratch.arena);
	String executable;
	if (!next_argument(scratch.arena, command_line, &offset, &executable)) {
		end_scratch(scratch);
		return true;
	}
	result->executable = arena_push_string_copy(arena, executable);
	if (!result->executable.data) {
		end_scratch(scratch);
		return false;
	}
	result->kind = compiler_kind_from_executable(executable);
	arena_restore(scratch.arena, executable_mark);

	for (;;)
	{
		u64 mark = arena_mark(scratch.arena);
		String argument;
		if (!next_argument(scratch.arena, command_line, &offset, &argument)) break;

		if (string_equal_insensitive(argument, LIT("/c")) ||
			string_equal(argument, LIT("-c"))) {
			result->compiles = true;
		}
		if (string_equal_insensitive(argument, LIT("/sourceDependencies")) ||
			string_equal(argument, LIT("-MMD")) ||
			string_equal(argument, LIT("-MD")) ||
			string_equal(argument, LIT("-MF")) ||
			string_starts_with(argument, LIT("-MF"))) {
			result->generates_dependencies = true;
		}
		arena_restore(scratch.arena, mark);
	}
	result->can_add_make_dependencies = result->compiles &&
		!result->generates_dependencies &&
		(result->kind == COMPILER_KIND_CLANG ||
		 result->kind == COMPILER_KIND_CLANG_CL ||
		 result->kind == COMPILER_KIND_GCC);
	end_scratch(scratch);
	return true;
}

b32 compiler_command_add_dependencies(Arena *arena, const Compiler_Command *command,
	String command_line, String dependency_file, String *result)
{
	void *start;

	if (!arena || !command || !result || !command_line.data || !dependency_file.data ||
		command->kind == COMPILER_KIND_UNKNOWN || !command->compiles ||
		command->generates_dependencies) {
		return false;
	}

	start = arena_top(arena);
	arena_append_str(arena, command_line);
	switch (command->kind) {
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
