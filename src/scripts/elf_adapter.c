#include "elf_adapter.h"
#include "elf_batteries.h"
#include "scripts/libs/string.h"

#include "elf.h"
#include "logger.h"
#include "platform/platform.h"
#include "profiler.h"
#include "scripts/libs/filesystem.h"
#include "scripts/libs/path.h"

#include <stdio.h>
#include <string.h>

typedef struct Elf_Script
{
	elf_State *state;
	elf_Ref exports;
}
Elf_Script;

static b32 read_build_table(Script *script, elf_i32 root, Script_Build *result);
static ELF_FUNCTION(l_strings_expand);
static ELF_FUNCTION(l_fs_list);
static ELF_FUNCTION(l_fs_exists);
static ELF_FUNCTION(l_fs_is_file);
static ELF_FUNCTION(l_fs_is_directory);
static ELF_FUNCTION(l_fs_copy_file);
static ELF_FUNCTION(l_fs_move_file);
static ELF_FUNCTION(l_fs_remove);
static ELF_FUNCTION(l_fs_create_directory);
static ELF_FUNCTION(l_fs_write_file);
static ELF_FUNCTION(l_path_join);
static ELF_FUNCTION(l_env_get);
static ELF_FUNCTION(l_env_has);
static ELF_FUNCTION(l_env_set);
static ELF_FUNCTION(l_env_unset);

ELF_FUNCTION(l_bob_build)
{
	(void)nargs;
	(void)nrets;
	Script *script = elf_get_user_data(S);
	Script_Build build = {0};
	if (!read_build_table(script, 1, &build))
	{
		script_set_error(script, "%s", build.error);
		script->failed = true;
		elf_push_int(S, false);
		return 1;
	}

	Script_Options options = script_options_resolve(build.options, script->command_line_options);
	logger_set_verbosity(options.verbosity);
	Profile_Scope scope = profile_scope_begin("builder");
	b32 succeeded = bob_build(build.bob, options.worker_count);
	profile_scope_end(&scope);
	bob_destroy(build.bob);
	if (!succeeded) {
		script_set_error(script, "build failed");
		script->failed = true;
	}
	elf_push_int(S, succeeded);
	return 1;
}

ELF_FUNCTION(l_strings_expand)
{
	(void)nrets;
	Script *script = elf_get_user_data(S);
	if (nargs != 3 || elf_type(S, 1) != ELF_VALUE_TYPE_TABLE || elf_type(S, 2) != ELF_VALUE_TYPE_STRING)
	{
		script_set_error(script, "strings.expand expects a table of strings and a rule string");
		script->failed = true;
		elf_push_nil(S);
		return 1;
	}

	Scratch scratch = begin_different_scratch(script->arena);
	elf_u32 length = 0;
	elf_length(S, 1, &length);
	String_Array strings = { .count = length };
	strings.items = arena_push_zero_aligned(scratch.arena, strings.count * sizeof(String), _Alignof(String));
	for (u32 index = 0; index < strings.count; ++index)
	{
		elf_StrSlice value;
		elf_get_index(S, 1, index);
		if (!elf_to_str(S, -1, &value))
		{
			script_set_error(script, "strings.expand expects a table containing only strings");
			script->failed = true;
			elf_push_nil(S);
			end_scratch(scratch);
			return 1;
		}
		strings.items[index] = string_from_data(value.data, value.size);
		elf_pop(S, 1);
	}

	elf_StrSlice rule_value;
	elf_to_str(S, 2, &rule_value);
	String rule = string_from_data(rule_value.data, rule_value.size);
	String result;
	const char *error;
	if (!script_strings_expand(scratch.arena, strings, rule, &result, &error))
	{
		script_set_error(script, "invalid strings.expand rule: %s", error);
		script->failed = true;
		elf_push_nil(S);
	}
	else {
		elf_push_str(S, result.data, (int)result.size);
	}
	end_scratch(scratch);
	return 1;
}

static b32 binding_error(elf_State *state, Script *script, const char *message)
{
	script_set_error(script, "%s", message);
	script->failed = true;
	elf_push_nil(state);
	return false;
}

static String stack_string(elf_State *state, elf_i32 index)
{
	elf_StrSlice value;
	if (!elf_to_str(state, index, &value)) return (String){0};
	return string_from_data(value.data, value.size);
}

static String stack_string_field(elf_State *state, elf_i32 table, const char *field)
{
	if (!elf_get_field(state, table, field)) return (String){0};
	String result = stack_string(state, -1);
	elf_pop(state, 1);
	return result;
}

static b32 stack_integer_field(elf_State *state, elf_i32 table, const char *field, elf_Integer *value, b32 *present)
{
	if (!elf_get_field(state, table, field)) return false;
	*present = !elf_is_nil(state, -1);
	b32 result = !*present || elf_to_int(state, -1, value);
	elf_pop(state, 1);
	return result;
}

ELF_FUNCTION(l_fs_list)
{
	(void)nrets;
	Script *script = elf_get_user_data(S);
	if (nargs != 2 || elf_type(S, 1) != ELF_VALUE_TYPE_TABLE) {
		binding_error(S, script, "bob.fs.list expects an options table");
		return 1;
	}

	Script_List_Paths_Options options = {0};
	elf_get_field(S, 1, "root");
	if (!elf_is_nil(S, -1)) {
		options.root = stack_string(S, -1);
		if (!options.root.data) {
			binding_error(S, script, "bob.fs.list option 'root' must be a string");
			return 1;
		}
	}
	elf_pop(S, 1);
	elf_get_field(S, 1, "pattern");
	if (!elf_is_nil(S, -1)) {
		options.pattern = stack_string(S, -1);
		if (!options.pattern.data) {
			binding_error(S, script, "bob.fs.list option 'pattern' must be a string");
			return 1;
		}
	}
	elf_pop(S, 1);
	elf_get_field(S, 1, "recursive");
	if (!elf_is_nil(S, -1)) {
		elf_Integer value;
		if (!elf_to_int(S, -1, &value)) {
			binding_error(S, script, "bob.fs.list option 'recursive' must be a boolean");
			return 1;
		}
		options.recursive = value != 0;
	}
	elf_pop(S, 1);
	elf_get_field(S, 1, "relative");
	if (!elf_is_nil(S, -1)) {
		elf_Integer value;
		if (!elf_to_int(S, -1, &value)) {
			binding_error(S, script, "bob.fs.list option 'relative' must be a boolean");
			return 1;
		}
		options.relative = value != 0;
	}
	elf_pop(S, 1);
	elf_get_field(S, 1, "kind");
	if (!elf_is_nil(S, -1))
	{
		String kind = stack_string(S, -1);
		if (!kind.data) {
			binding_error(S, script, "bob.fs.list option 'kind' must be a string");
			return 1;
		}
		if (string_is(kind, "files")) options.kind = SCRIPT_PATH_FILES;
		else if (string_is(kind, "directories")) options.kind = SCRIPT_PATH_DIRECTORIES;
		else if (string_is(kind, "all")) options.kind = SCRIPT_PATH_ALL;
		else {
			binding_error(S, script, "bob.fs.list option 'kind' must be 'files', 'directories', or 'all'");
			return 1;
		}
	}
	elf_pop(S, 1);

	Scratch scratch = begin_different_scratch(script->arena);
	elf_get_field(S, 1, "patterns");
	if (!elf_is_nil(S, -1))
	{
		if (options.pattern.data) {
			end_scratch(scratch);
			binding_error(S, script, "bob.fs.list accepts either 'pattern' or 'patterns', not both");
			return 1;
		}
		if (elf_type(S, -1) != ELF_VALUE_TYPE_TABLE) {
			end_scratch(scratch);
			binding_error(S, script, "bob.fs.list option 'patterns' must be a table of strings");
			return 1;
		}
		options.has_patterns = true;
		elf_length(S, -1, &options.patterns.count);
		options.patterns.items = arena_push_zero_aligned(scratch.arena, options.patterns.count * sizeof(String), _Alignof(String));
		elf_i32 patterns = elf_abs_index(S, -1);
		for (u32 index = 0; index < options.patterns.count; ++index)
		{
			elf_get_index(S, patterns, index);
			options.patterns.items[index] = stack_string(S, -1);
			if (!options.patterns.items[index].data) {
				end_scratch(scratch);
				binding_error(S, script, "bob.fs.list option 'patterns' must contain only strings");
				return 1;
			}
			elf_pop(S, 1);
		}
	}
	elf_pop(S, 1);
	String_Array paths;
	if (!script_list_paths(scratch.arena, options, &paths))
	{
		end_scratch(scratch);
		binding_error(S, script, "bob.fs.list could not read the root directory");
		return 1;
	}
	elf_new_table(S);
	elf_i32 result = elf_abs_index(S, -1);
	for (u32 index = 0; index < paths.count; ++index) {
		elf_push_str(S, paths.items[index].data, (int)paths.items[index].size);
		elf_append(S, result);
	}
	end_scratch(scratch);
	return 1;
}

static int push_path_kind(elf_State *state, b32 files, b32 directories)
{
	Script *script = elf_get_user_data(state);
	if (elf_type(state, 1) != ELF_VALUE_TYPE_STRING) {
		binding_error(state, script, "filesystem query expects a path string");
		return 1;
	}
	Scratch scratch = begin_different_scratch(script->arena);
	String path = arena_push_string_copy(scratch.arena, stack_string(state, 1));
	Platform_File_Info info;
	b32 found = platform_file_info(path, &info);
	b32 result = found && ((files && !info.is_directory) || (directories && info.is_directory));
	elf_push_int(state, result);
	end_scratch(scratch);
	return 1;
}

ELF_FUNCTION(l_fs_exists)
{
	(void)nrets;
	Script *script = elf_get_user_data(S);
	if (nargs != 2 || elf_type(S, 1) != ELF_VALUE_TYPE_STRING) {
		binding_error(S, script, "bob.fs.exists expects a path string");
		return 1;
	}
	Scratch scratch = begin_different_scratch(script->arena);
	String path = arena_push_string_copy(scratch.arena, stack_string(S, 1));
	Platform_File_Info info;
	elf_push_int(S, platform_file_info(path, &info));
	end_scratch(scratch);
	return 1;
}

ELF_FUNCTION(l_fs_is_file)
{
	(void)nrets;
	if (nargs != 2) {
		Script *script = elf_get_user_data(S);
		binding_error(S, script, "bob.fs.is_file expects a path string");
		return 1;
	}
	return push_path_kind(S, true, false);
}

ELF_FUNCTION(l_fs_is_directory)
{
	(void)nrets;
	if (nargs != 2) {
		Script *script = elf_get_user_data(S);
		binding_error(S, script, "bob.fs.is_directory expects a path string");
		return 1;
	}
	return push_path_kind(S, false, true);
}

ELF_FUNCTION(l_path_join)
{
	(void)nrets;
	Script *script = elf_get_user_data(S);
	if (nargs != 3 || elf_type(S, 1) != ELF_VALUE_TYPE_STRING || elf_type(S, 2) != ELF_VALUE_TYPE_STRING)
	{
		binding_error(S, script, "bob.path.join expects two path strings");
		return 1;
	}
	String left = stack_string(S, 1);
	String right = stack_string(S, 2);
	Scratch scratch = begin_different_scratch(script->arena);
	String result = script_path_join(scratch.arena, left, right);
	elf_push_str(S, result.data, (int)result.size);
	end_scratch(scratch);
	return 1;
}

static int transfer_file(elf_State *state, int nargs, b32 move)
{
	Script *script = elf_get_user_data(state);
	if (nargs < 2) {
		binding_error(state, script, move ? "bob.fs.move_file expects arguments" : "bob.fs.copy_file expects arguments");
		return 1;
	}
	String source = {0};
	String destination = {0};
	b32 overwrite = true;
	if (elf_type(state, 1) == ELF_VALUE_TYPE_TABLE)
	{
		if (nargs != 2) {
			binding_error(state, script, move ? "bob.fs.move_file options must be passed in one table" : "bob.fs.copy_file options must be passed in one table");
			return 1;
		}
		source = stack_string_field(state, 1, "from");
		destination = stack_string_field(state, 1, "to");
		elf_Integer value = 0;
		b32 present = false;
		if (!stack_integer_field(state, 1, "overwrite", &value, &present)) {
			binding_error(state, script, "filesystem option 'overwrite' must be a boolean");
			return 1;
		}
		if (present) overwrite = value != 0;
	}
	else
	{
		if ((nargs != 3 && nargs != 4) || elf_type(state, 1) != ELF_VALUE_TYPE_STRING || elf_type(state, 2) != ELF_VALUE_TYPE_STRING || (nargs == 4 && elf_type(state, 3) != ELF_VALUE_TYPE_INTEGER))
		{
			binding_error(state, script, move ? "bob.fs.move_file expects (from, to, overwrite?)" : "bob.fs.copy_file expects (from, to, overwrite?)");
			return 1;
		}
		source = stack_string(state, 1);
		destination = stack_string(state, 2);
		if (nargs == 4) {
			elf_Integer value;
			elf_to_int(state, 3, &value);
			overwrite = value != 0;
		}
	}
	if (!source.data || !destination.data) {
		binding_error(state, script, move ? "bob.fs.move_file requires 'from' and 'to' strings" : "bob.fs.copy_file requires 'from' and 'to' strings");
		return 1;
	}
	Scratch scratch = begin_different_scratch(script->arena);
	source = arena_push_string_copy(scratch.arena, source);
	destination = arena_push_string_copy(scratch.arena, destination);
	b32 result = move ? platform_move_file(source, destination, overwrite) : platform_copy_file(source, destination, overwrite);
	elf_push_int(state, result);
	end_scratch(scratch);
	return 1;
}

ELF_FUNCTION(l_fs_copy_file)
{
	(void)nrets;
	return transfer_file(S, nargs, false);
}

ELF_FUNCTION(l_fs_move_file)
{
	(void)nrets;
	return transfer_file(S, nargs, true);
}

ELF_FUNCTION(l_fs_remove)
{
	(void)nrets;
	Script *script = elf_get_user_data(S);
	String path = {0};
	b32 recursive = false;
	if (nargs < 2) {
		binding_error(S, script, "bob.fs.remove expects arguments");
		return 1;
	}
	if (elf_type(S, 1) == ELF_VALUE_TYPE_TABLE)
	{
		if (nargs != 2) {
			binding_error(S, script, "bob.fs.remove options must be passed in one table");
			return 1;
		}
		path = stack_string_field(S, 1, "path");
		elf_Integer value = 0;
		b32 present = false;
		if (!stack_integer_field(S, 1, "recursive", &value, &present)) {
			binding_error(S, script, "bob.fs.remove option 'recursive' must be a boolean");
			return 1;
		}
		if (present) recursive = value != 0;
	}
	else
	{
		if ((nargs != 2 && nargs != 3) || elf_type(S, 1) != ELF_VALUE_TYPE_STRING || (nargs == 3 && elf_type(S, 2) != ELF_VALUE_TYPE_INTEGER))
		{
			binding_error(S, script, "bob.fs.remove expects (path, recursive?)");
			return 1;
		}
		path = stack_string(S, 1);
		if (nargs == 3) {
			elf_Integer value;
			elf_to_int(S, 2, &value);
			recursive = value != 0;
		}
	}
	if (!path.data) {
		binding_error(S, script, "bob.fs.remove requires a path string");
		return 1;
	}
	Scratch scratch = begin_different_scratch(script->arena);
	path = arena_push_string_copy(scratch.arena, path);
	elf_push_int(S, script_remove_path(scratch.arena, path, recursive));
	end_scratch(scratch);
	return 1;
}

ELF_FUNCTION(l_fs_create_directory)
{
	(void)nrets;
	Script *script = elf_get_user_data(S);
	String path = {0};
	b32 recursive = false;
	if (nargs < 2) {
		binding_error(S, script, "bob.fs.create_directory expects arguments");
		return 1;
	}
	if (elf_type(S, 1) == ELF_VALUE_TYPE_TABLE)
	{
		if (nargs != 2) {
			binding_error(S, script, "bob.fs.create_directory options must be passed in one table");
			return 1;
		}
		path = stack_string_field(S, 1, "path");
		elf_Integer value = 0;
		b32 present = false;
		if (!stack_integer_field(S, 1, "recursive", &value, &present)) {
			binding_error(S, script, "bob.fs.create_directory option 'recursive' must be a boolean");
			return 1;
		}
		if (present) recursive = value != 0;
	}
	else
	{
		if ((nargs != 2 && nargs != 3) || elf_type(S, 1) != ELF_VALUE_TYPE_STRING || (nargs == 3 && elf_type(S, 2) != ELF_VALUE_TYPE_INTEGER))
		{
			binding_error(S, script, "bob.fs.create_directory expects (path, recursive?)");
			return 1;
		}
		path = stack_string(S, 1);
		if (nargs == 3) {
			elf_Integer value;
			elf_to_int(S, 2, &value);
			recursive = value != 0;
		}
	}
	if (!path.data) {
		binding_error(S, script, "bob.fs.create_directory requires a path string");
		return 1;
	}
	Scratch scratch = begin_different_scratch(script->arena);
	path = arena_push_string_copy(scratch.arena, path);
	b32 result = recursive ? platform_create_directories(path) : platform_create_directory(path);
	elf_push_int(S, result);
	end_scratch(scratch);
	return 1;
}

ELF_FUNCTION(l_fs_write_file)
{
	(void)nrets;
	Script *script = elf_get_user_data(S);
	String path = {0};
	String contents = {0};
	if (nargs < 2) {
		binding_error(S, script, "bob.fs.write_file expects arguments");
		return 1;
	}
	if (elf_type(S, 1) == ELF_VALUE_TYPE_TABLE)
	{
		if (nargs != 2) {
			binding_error(S, script, "bob.fs.write_file options must be passed in one table");
			return 1;
		}
		path = stack_string_field(S, 1, "path");
		contents = stack_string_field(S, 1, "contents");
	}
	else
	{
		if (nargs != 3 || elf_type(S, 1) != ELF_VALUE_TYPE_STRING || elf_type(S, 2) != ELF_VALUE_TYPE_STRING)
		{
			binding_error(S, script, "bob.fs.write_file expects (path, contents)");
			return 1;
		}
		path = stack_string(S, 1);
		contents = stack_string(S, 2);
	}
	if (!path.data || !contents.data) {
		binding_error(S, script, "bob.fs.write_file requires string fields 'path' and 'contents'");
		return 1;
	}
	Scratch scratch = begin_different_scratch(script->arena);
	path = arena_push_string_copy(scratch.arena, path);
	elf_push_int(S, platform_write_entire_file(path, contents.data, (size_t)contents.size));
	end_scratch(scratch);
	return 1;
}

static int push_environment(elf_State *state, int nargs, b32 has_only)
{
	Script *script = elf_get_user_data(state);
	if (nargs != 2 || elf_type(state, 1) != ELF_VALUE_TYPE_STRING) {
		binding_error(state, script, has_only ? "bob.env.has expects a variable name" : "bob.env.get expects a variable name");
		return 1;
	}
	Scratch scratch = begin_different_scratch(script->arena);
	String name = arena_push_string_copy(scratch.arena, stack_string(state, 1));
	String value;
	b32 read = platform_get_environment(name, scratch.arena, &value);
	if (!read) {
		end_scratch(scratch);
		binding_error(state, script, "unable to read environment variable");
		return 1;
	}
	if (has_only) elf_push_int(state, value.data != NULL);
	else if (value.data) elf_push_str(state, value.data, (int)value.size);
	else elf_push_nil(state);
	end_scratch(scratch);
	return 1;
}

ELF_FUNCTION(l_env_get)
{
	(void)nrets;
	return push_environment(S, nargs, false);
}

ELF_FUNCTION(l_env_has)
{
	(void)nrets;
	return push_environment(S, nargs, true);
}

ELF_FUNCTION(l_env_set)
{
	(void)nrets;
	Script *script = elf_get_user_data(S);
	if (nargs != 3 || elf_type(S, 1) != ELF_VALUE_TYPE_STRING || elf_type(S, 2) != ELF_VALUE_TYPE_STRING) {
		binding_error(S, script, "bob.env.set expects a variable name and value");
		return 1;
	}
	Scratch scratch = begin_different_scratch(script->arena);
	String name = arena_push_string_copy(scratch.arena, stack_string(S, 1));
	String value = arena_push_string_copy(scratch.arena, stack_string(S, 2));
	elf_push_int(S, platform_set_environment(name, value));
	end_scratch(scratch);
	return 1;
}

ELF_FUNCTION(l_env_unset)
{
	(void)nrets;
	Script *script = elf_get_user_data(S);
	if (nargs != 2 || elf_type(S, 1) != ELF_VALUE_TYPE_STRING) {
		binding_error(S, script, "bob.env.unset expects a variable name");
		return 1;
	}
	Scratch scratch = begin_different_scratch(script->arena);
	String name = arena_push_string_copy(scratch.arena, stack_string(S, 1));
	elf_push_int(S, platform_set_environment(name, (String){0}));
	end_scratch(scratch);
	return 1;
}

static b32 set_function(elf_State *state, elf_i32 table, const char *name, elf_Function function)
{
	elf_push_fun(state, function);
	return elf_set_field(state, table, name);
}

static b32 register_bob_library(elf_State *state)
{
	elf_i32 checkpoint = elf_get_top(state);
	elf_new_table(state);
	elf_i32 bob = elf_abs_index(state, -1);

	if (!set_function(state, bob, "build", l_bob_build)) goto error;
	elf_push_cstr(state, BOB_VERSION);
	if (!elf_set_field(state, bob, "version")) goto error;

	elf_new_table(state);
	elf_i32 strings = elf_abs_index(state, -1);
	if (!set_function(state, strings, "expand", l_strings_expand)) goto error;
	if (!elf_set_field(state, bob, "strings")) goto error;

	elf_new_table(state);
	elf_i32 fs = elf_abs_index(state, -1);
	if (!set_function(state, fs, "list", l_fs_list)) goto error;
	if (!set_function(state, fs, "exists", l_fs_exists)) goto error;
	if (!set_function(state, fs, "is_file", l_fs_is_file)) goto error;
	if (!set_function(state, fs, "is_directory", l_fs_is_directory)) goto error;
	if (!set_function(state, fs, "copy_file", l_fs_copy_file)) goto error;
	if (!set_function(state, fs, "move_file", l_fs_move_file)) goto error;
	if (!set_function(state, fs, "remove", l_fs_remove)) goto error;
	if (!set_function(state, fs, "create_directory", l_fs_create_directory)) goto error;
	if (!set_function(state, fs, "write_file", l_fs_write_file)) goto error;
	if (!elf_set_field(state, bob, "fs")) goto error;

	elf_new_table(state);
	elf_i32 path = elf_abs_index(state, -1);
	if (!set_function(state, path, "join", l_path_join)) goto error;
	if (!elf_set_field(state, bob, "path")) goto error;

	elf_new_table(state);
	elf_i32 env = elf_abs_index(state, -1);
	if (!set_function(state, env, "get", l_env_get)) goto error;
	if (!set_function(state, env, "has", l_env_has)) goto error;
	if (!set_function(state, env, "set", l_env_set)) goto error;
	if (!set_function(state, env, "unset", l_env_unset)) goto error;
	if (!elf_set_field(state, bob, "env")) goto error;

	if (!elf_set_global(state, "bob")) goto error;
	return true;

error:
	elf_set_top(state, checkpoint);
	return false;
}

b32 elf_script_load(Script *script, String path)
{
	Elf_Script *elf = arena_push_zero_aligned(script->arena, sizeof(*elf), _Alignof(Elf_Script));
	elf->state = elf_create_state();
	script->context = elf;
	if (!elf->state) {
		script_set_error(script, "unable to create elf state");
		return false;
	}
	elf_set_user_data(elf->state, script);
	elf_open_batteries(elf->state);
	if (!register_bob_library(elf->state)) {
		script_set_error(script, "unable to register Bob script libraries");
		return false;
	}
	String source;
	if (!platform_read_entire_file(script->arena, path, &source)) {
		script_set_error(script, "unable to read '%s'", path.data);
		return false;
	}
	if (source.size > UINT32_MAX) {
		script_set_error(script, "script is too large: '%s'", path.data);
		return false;
	}
	if (!elf_push_code_source(elf->state, path.data,
	                          (elf_StrSlice){ source.data, (elf_u32)source.size })) {
		script_set_error(script, "unable to load '%s'", path.data);
		return false;
	}
	elf_push_nil(elf->state);
	elf_call(elf->state, 1, 1);
	if (script->failed) {
		elf_pop(elf->state, 1);
		return false;
	}

	if (elf_type(elf->state, -1) != ELF_VALUE_TYPE_TABLE) {
		script_set_error(script, "script must return a table");
		return false;
	}
	elf->exports = elf_create_ref(elf->state, -1);
	elf_pop(elf->state, 1);
	if (elf->exports == ELF_NO_REF) {
		script_set_error(script, "unable to retain script exports");
		return false;
	}

	elf_i32 checkpoint = elf_get_top(elf->state);
	elf_push_ref(elf->state, elf->exports);
	elf_i32 exports = elf_abs_index(elf->state, -1);
	elf_u32 cursor = 0;
	while (elf_next(elf->state, exports, &cursor)) {
		elf_StrSlice name;
		if (elf_to_str(elf->state, -2, &name) && elf_is_callable(elf->state, -1)) ++script->functions.count;
		elf_pop(elf->state, 2);
	}
	script->functions.items = arena_push_zero_aligned(script->arena, script->functions.count * sizeof(String), _Alignof(String));
	cursor = 0;
	u32 function_index = 0;
	while (elf_next(elf->state, exports, &cursor))
	{
		elf_StrSlice slice;
		if (elf_to_str(elf->state, -2, &slice) && elf_is_callable(elf->state, -1)) {
			String name = string_from_data(slice.data, slice.size);
			script->functions.items[function_index++] = arena_push_string_copy(script->arena, name);
		}
		elf_pop(elf->state, 2);
	}
	elf_set_top(elf->state, checkpoint);
	return true;
}

void elf_script_destroy(Script *script)
{
	Elf_Script *elf = script->context;
	if (elf->exports != ELF_NO_REF) elf_release_ref(elf->state, elf->exports);
	if (elf->state) elf_destroy_state(elf->state);
	elf->exports = ELF_NO_REF;
	elf->state = NULL;
}

b32 elf_script_invoke(Script *script, String name)
{
	Elf_Script *elf = script->context;
	elf_i32 checkpoint = elf_get_top(elf->state);
	if (!elf_push_ref(elf->state, elf->exports)) return false;
	elf_i32 exports = elf_abs_index(elf->state, -1);
	if (!elf_get_field(elf->state, exports, name.data) || !elf_is_callable(elf->state, -1)) {
		elf_set_top(elf->state, checkpoint);
		return false;
	}
	elf_push_nil(elf->state);
	elf_call(elf->state, 1, 0);
	elf_set_top(elf->state, checkpoint);
	return true;
}

static String copy_stack_string(Arena *arena, elf_State *state, elf_i32 index)
{
	String string = stack_string(state, index);
	return string.data ? arena_push_string_copy(arena, string) : (String){0};
}

static b32 copy_string_array_field(elf_State *state, Arena *arena, elf_i32 table, const char *field_name, String task_name, String_Array *result, char *error, size_t error_size)
{
	elf_i32 checkpoint = elf_get_top(state);
	*result = (String_Array){0};
	if (!elf_get_field(state, table, field_name)) return false;
	if (elf_is_nil(state, -1)) {
		elf_set_top(state, checkpoint);
		return true;
	}
	elf_i32 array = elf_abs_index(state, -1);
	elf_u32 count = 0;
	if (!elf_length(state, array, &count)) {
		snprintf(error, error_size, "%s for '%s' must be a table", field_name, task_name.data);
		elf_set_top(state, checkpoint);
		return false;
	}
	result->count = count;
	result->items = arena_push_zero_aligned(arena, count * sizeof(*result->items), _Alignof(String));
	for (elf_u32 i = 0; i < count; ++i)
	{
		elf_get_index(state, array, i);
		result->items[i] = copy_stack_string(arena, state, -1);
		elf_pop(state, 1);
		if (!result->items[i].data) {
			snprintf(error, error_size, "%s for '%s' must contain strings", field_name, task_name.data);
			elf_set_top(state, checkpoint);
			return false;
		}
	}
	elf_set_top(state, checkpoint);
	return true;
}

typedef struct Ref_List
{
	Arena *arena;
	elf_Ref *items;
	u32 count;
}
Ref_List;

static u32 ref_list_find(Ref_List *list, elf_State *state, elf_i32 value)
{
	elf_i32 absolute = elf_abs_index(state, value);
	for (u32 i = 0; i < list->count; ++i)
	{
		if (!elf_push_ref(state, list->items[i])) continue;
		b32 equal = elf_equal(state, absolute, -1);
		elf_pop(state, 1);
		if (equal) return i;
	}
	return UINT32_MAX;
}

static b32 ref_list_add(Ref_List *list, elf_State *state, elf_i32 value)
{
	if (ref_list_find(list, state, value) != UINT32_MAX) return true;
	elf_Ref reference = elf_create_ref(state, value);
	if (reference == ELF_NO_REF) return false;
	elf_Ref *item = arena_push_aligned(list->arena, sizeof(*item), _Alignof(elf_Ref));
	if (!list->items) list->items = item;
	*item = reference;
	++list->count;
	return true;
}

static b32 read_build_table(Script *script, elf_i32 root, Script_Build *result)
{
	if (!script || !result) return false;
	Elf_Script *elf = script->context;
	elf_State *state = elf->state;
	elf_i32 checkpoint = elf_get_top(state);
	root = elf_abs_index(state, root);
	Scratch scratch = begin_scratch();
	Ref_List task_tables = { .arena = scratch.arena };
	b32 success = false;
	memset(result, 0, sizeof(*result));

	elf_get_field(state, root, "options");
	if (!elf_is_nil(state, -1))
	{
		elf_i32 options = elf_abs_index(state, -1);
		if (elf_type(state, options) != ELF_VALUE_TYPE_TABLE) {
			snprintf(result->error, sizeof(result->error), "returned 'options' field must be a table");
			goto cleanup;
		}
		elf_Integer integer = 0;
		b32 present = false;
		if (!stack_integer_field(state, options, "workers", &integer, &present) || (present && (integer < 1 || (u64)integer > UINT32_MAX))) {
			snprintf(result->error, sizeof(result->error), "options.workers must be a positive integer");
			goto cleanup;
		}
		if (present) {
			result->options.worker_count = (u32)integer;
			result->options.has_worker_count = true;
		}
		if (!stack_integer_field(state, options, "verbosity", &integer, &present) || (present && (integer < 0 || (u64)integer > INT32_MAX))) {
			snprintf(result->error, sizeof(result->error), "options.verbosity must be a non-negative integer");
			goto cleanup;
		}
		if (present) {
			result->options.verbosity = (i32)integer;
			result->options.has_verbosity = true;
		}
	}
	elf_pop(state, 1);

	elf_get_field(state, root, "targets");
	elf_i32 targets = elf_abs_index(state, -1);
	elf_u32 target_count = 0;
	if (!elf_length(state, targets, &target_count)) {
		snprintf(result->error, sizeof(result->error), "returned table requires a 'targets' table");
		goto cleanup;
	}
	for (elf_u32 i = 0; i < target_count; ++i)
	{
		elf_get_index(state, targets, i);
		if (elf_type(state, -1) != ELF_VALUE_TYPE_TABLE) {
			snprintf(result->error, sizeof(result->error), "target %u must be a task table", i);
			goto cleanup;
		}
		if (!ref_list_add(&task_tables, state, -1)) {
			snprintf(result->error, sizeof(result->error), "unable to retain target %u", i);
			goto cleanup;
		}
		elf_pop(state, 1);
	}
	elf_pop(state, 1);

	for (u32 i = 0; i < task_tables.count; ++i)
	{
		elf_i32 task_checkpoint = elf_get_top(state);
		elf_push_ref(state, task_tables.items[i]);
		elf_i32 task = elf_abs_index(state, -1);
		elf_get_field(state, task, "dependencies");
		if (elf_is_nil(state, -1)) {
			elf_set_top(state, task_checkpoint);
			continue;
		}
		elf_i32 dependencies = elf_abs_index(state, -1);
		elf_u32 dependency_count = 0;
		if (!elf_length(state, dependencies, &dependency_count)) {
			snprintf(result->error, sizeof(result->error), "dependencies for task %u must be a table", i);
			goto cleanup;
		}
		for (elf_u32 dependency = 0; dependency < dependency_count; ++dependency)
		{
			elf_get_index(state, dependencies, dependency);
			if (elf_type(state, -1) != ELF_VALUE_TYPE_TABLE) {
				snprintf(result->error, sizeof(result->error), "dependencies for task %u must contain task tables", i);
				goto cleanup;
			}
			if (!ref_list_add(&task_tables, state, -1)) {
				snprintf(result->error, sizeof(result->error), "unable to retain dependency for task %u", i);
				goto cleanup;
			}
			elf_pop(state, 1);
		}
		elf_set_top(state, task_checkpoint);
	}

	result->bob = bob_create();
	if (!result->bob) {
		snprintf(result->error, sizeof(result->error), "out of memory");
		goto cleanup;
	}

	for (u32 i = 0; i < task_tables.count; ++i)
	{
		elf_i32 task_checkpoint = elf_get_top(state);
		elf_push_ref(state, task_tables.items[i]);
		elf_i32 description = elf_abs_index(state, -1);
		Bob_Task task = {0};
		elf_get_field(state, description, "name");
		task.name = copy_stack_string(scratch.arena, state, -1);
		elf_pop(state, 1);
		elf_get_field(state, description, "command_line");
		task.command_line = copy_stack_string(scratch.arena, state, -1);
		elf_pop(state, 1);
		if (!task.name.data || !task.command_line.data) {
			snprintf(result->error, sizeof(result->error), "task %u requires string fields 'name' and 'command_line'", i);
			goto cleanup;
		}
		elf_Integer transparent = 0;
		b32 present = false;
		if (!stack_integer_field(state, description, "transparent", &transparent, &present)) {
			snprintf(result->error, sizeof(result->error), "transparent for '%s' must be a boolean", task.name.data);
			goto cleanup;
		}
		if (present) task.transparent = transparent != 0;
		if (!copy_string_array_field(state, scratch.arena, description, "inputs", task.name, &task.inputs, result->error, sizeof(result->error))) goto cleanup;
		if (!copy_string_array_field(state, scratch.arena, description, "outputs", task.name, &task.outputs, result->error, sizeof(result->error))) goto cleanup;
		if (!copy_string_array_field(state, scratch.arena, description, "include_dirs", task.name, &task.include_directories, result->error, sizeof(result->error))) goto cleanup;
		Bob_Node *node;
		Bob_Error bob_error = bob_add_task(result->bob, task, &node);
		if (bob_error != BOB_OK) {
			snprintf(result->error, sizeof(result->error), "unable to add task '%s': %s", task.name.data, bob_error_string(bob_error));
			goto cleanup;
		}
		elf_set_top(state, task_checkpoint);
	}

	for (u32 i = 0; i < task_tables.count; ++i)
	{
		elf_i32 task_checkpoint = elf_get_top(state);
		elf_push_ref(state, task_tables.items[i]);
		elf_i32 description = elf_abs_index(state, -1);
		elf_get_field(state, description, "dependencies");
		if (elf_is_nil(state, -1)) {
			elf_set_top(state, task_checkpoint);
			continue;
		}
		elf_i32 dependencies = elf_abs_index(state, -1);
		elf_u32 dependency_count = 0;
		elf_length(state, dependencies, &dependency_count);
		for (elf_u32 dependency = 0; dependency < dependency_count; ++dependency)
		{
			elf_get_index(state, dependencies, dependency);
			u32 resolved = ref_list_find(&task_tables, state, -1);
			elf_pop(state, 1);
			if (resolved == UINT32_MAX) {
				snprintf(result->error, sizeof(result->error), "unable to resolve dependency for task %u", i);
				goto cleanup;
			}
			Bob_Node *node = bob_node_at(result->bob, i);
			Bob_Node *dependency_node = bob_node_at(result->bob, resolved);
			Bob_Error bob_error = bob_add_dependency(result->bob, node, dependency_node);
			if (bob_error != BOB_OK) {
				snprintf(result->error, sizeof(result->error), "unable to add dependency to '%s': %s", bob_task_name(node), bob_error_string(bob_error));
				goto cleanup;
			}
		}
		elf_set_top(state, task_checkpoint);
	}
	success = true;

cleanup:
	elf_set_top(state, checkpoint);
	for (u32 i = 0; i < task_tables.count; ++i) elf_release_ref(state, task_tables.items[i]);
	end_scratch(scratch);
	if (!success)
	{
		char error[sizeof(result->error)];
		memcpy(error, result->error, sizeof(error));
		bob_destroy(result->bob);
		memset(result, 0, sizeof(*result));
		memcpy(result->error, error, sizeof(error));
	}
	return success;
}

b32 elf_script_read_build(Script *script, Script_Build *result)
{
	Elf_Script *elf = script->context;
	elf_i32 checkpoint = elf_get_top(elf->state);
	if (!elf_push_ref(elf->state, elf->exports)) return false;
	elf_i32 root = elf_abs_index(elf->state, -1);
	b32 success = read_build_table(script, root, result);
	elf_set_top(elf->state, checkpoint);
	return success;
}
