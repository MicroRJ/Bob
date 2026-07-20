#include "elf_adapter.h"
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
	elf_Table *exports;
}
Elf_Script;

static b32 read_build_table(Script *script, elf_Table *root, Script_Build *result);
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
	elf_Table *arguments = elf_arg_table(S, 1);
	Script_Build build = {0};
	if (!read_build_table(script, arguments, &build))
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

ELF_FUNCTION(l_bob_version)
{
	(void)nargs;
	(void)nrets;
	elf_push_cstr(S, BOB_VERSION);
	return 1;
}

static const elf_Binding bob_bindings[] =
{
	{ "build",            l_bob_build },
	{ "_version",         l_bob_version },
	{ "_strings_expand",  l_strings_expand },
	{ "_fs_list",         l_fs_list },
	{ "_fs_exists",       l_fs_exists },
	{ "_fs_is_file",      l_fs_is_file },
	{ "_fs_is_directory", l_fs_is_directory },
	{ "_fs_copy_file",    l_fs_copy_file },
	{ "_fs_move_file",    l_fs_move_file },
	{ "_fs_remove",       l_fs_remove },
	{ "_fs_create_directory", l_fs_create_directory },
	{ "_fs_write_file",    l_fs_write_file },
	{ "_path_join",       l_path_join },
	{ "_env_get",         l_env_get },
	{ "_env_has",         l_env_has },
	{ "_env_set",         l_env_set },
	{ "_env_unset",       l_env_unset },
};

ELF_FUNCTION(l_strings_expand)
{
	(void)nrets;
	Script *script = elf_get_user_data(S);
	if (nargs != 3 || elf_arg_type(S, 1) != ELF_VALUE_TYPE_TABLE || elf_arg_type(S, 2) != ELF_VALUE_TYPE_STRING)
	{
		script_set_error(script, "strings.expand expects a table of strings and a rule string");
		script->failed = true;
		elf_push_nil(S);
		return 1;
	}

	Scratch scratch = begin_different_scratch(script->arena);
	elf_Table *table = elf_arg_table(S, 1);
	String_Array strings = { .count = elf_table_length(table) };
	strings.items = arena_push_zero_aligned(scratch.arena, strings.count * sizeof(String), _Alignof(String));
	for (u32 index = 0; index < strings.count; ++index)
	{
		elf_ValueView value = elf_get_index(S, table, index);
		if (value.type != ELF_VALUE_TYPE_STRING)
		{
			script_set_error(script, "strings.expand expects a table containing only strings");
			script->failed = true;
			elf_push_nil(S);
			end_scratch(scratch);
			return 1;
		}
		strings.items[index] = string_from_data((void *)elf_str_data(value.as.string), elf_str_size(value.as.string));
	}

	elf_String *rule_value = elf_arg_str(S, 2);
	String rule = string_from_data((void *)elf_str_data(rule_value), elf_str_size(rule_value));
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

static String value_string(elf_ValueView value)
{
	if (value.type != ELF_VALUE_TYPE_STRING) return (String){0};
	return string_from_data((void *)elf_str_data(value.as.string), elf_str_size(value.as.string));
}

ELF_FUNCTION(l_fs_list)
{
	(void)nrets;
	Script *script = elf_get_user_data(S);
	if (nargs != 2 || elf_arg_type(S, 1) != ELF_VALUE_TYPE_TABLE) {
		binding_error(S, script, "bob.fs.list expects an options table");
		return 1;
	}

	elf_Table *table = elf_arg_table(S, 1);
	Script_List_Paths_Options options = {0};
	elf_ValueView value = elf_get_field(S, table, "root");
	if (value.type != ELF_VALUE_TYPE_NIL) {
		options.root = value_string(value);
		if (!options.root.data) {
			binding_error(S, script, "bob.fs.list option 'root' must be a string");
			return 1;
		}
	}
	value = elf_get_field(S, table, "pattern");
	if (value.type != ELF_VALUE_TYPE_NIL) {
		options.pattern = value_string(value);
		if (!options.pattern.data) {
			binding_error(S, script, "bob.fs.list option 'pattern' must be a string");
			return 1;
		}
	}
	value = elf_get_field(S, table, "recursive");
	if (value.type != ELF_VALUE_TYPE_NIL) {
		if (value.type != ELF_VALUE_TYPE_INTEGER) {
			binding_error(S, script, "bob.fs.list option 'recursive' must be a boolean");
			return 1;
		}
		options.recursive = value.as.integer != 0;
	}
	value = elf_get_field(S, table, "relative");
	if (value.type != ELF_VALUE_TYPE_NIL) {
		if (value.type != ELF_VALUE_TYPE_INTEGER) {
			binding_error(S, script, "bob.fs.list option 'relative' must be a boolean");
			return 1;
		}
		options.relative = value.as.integer != 0;
	}
	value = elf_get_field(S, table, "kind");
	if (value.type != ELF_VALUE_TYPE_NIL)
	{
		String kind = value_string(value);
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

	Scratch scratch = begin_different_scratch(script->arena);
	value = elf_get_field(S, table, "patterns");
	if (value.type != ELF_VALUE_TYPE_NIL)
	{
		if (options.pattern.data) {
			end_scratch(scratch);
			binding_error(S, script, "bob.fs.list accepts either 'pattern' or 'patterns', not both");
			return 1;
		}
		if (value.type != ELF_VALUE_TYPE_TABLE) {
			end_scratch(scratch);
			binding_error(S, script, "bob.fs.list option 'patterns' must be a table of strings");
			return 1;
		}
		options.has_patterns = true;
		options.patterns.count = elf_table_length(value.as.table);
		options.patterns.items = arena_push_zero_aligned(scratch.arena, options.patterns.count * sizeof(String), _Alignof(String));
		for (u32 index = 0; index < options.patterns.count; ++index)
		{
			options.patterns.items[index] = value_string(elf_get_index(S, value.as.table, index));
			if (!options.patterns.items[index].data) {
				end_scratch(scratch);
				binding_error(S, script, "bob.fs.list option 'patterns' must contain only strings");
				return 1;
			}
		}
	}
	String_Array paths;
	if (!script_list_paths(scratch.arena, options, &paths))
	{
		end_scratch(scratch);
		binding_error(S, script, "bob.fs.list could not read the root directory");
		return 1;
	}
	void *start = arena_top(scratch.arena);
	for (u32 index = 0; index < paths.count; ++index) {
		if (index) arena_append_char(scratch.arena, '\n');
		arena_append_str(scratch.arena, paths.items[index]);
	}
	String joined = arena_string_from(scratch.arena, start);
	elf_push_str(S, joined.data, (int)joined.size);
	end_scratch(scratch);
	return 1;
}

static int push_path_kind(elf_State *state, b32 files, b32 directories)
{
	Script *script = elf_get_user_data(state);
	if (elf_arg_type(state, 1) != ELF_VALUE_TYPE_STRING) {
		binding_error(state, script, "filesystem query expects a path string");
		return 1;
	}
	elf_String *value = elf_arg_str(state, 1);
	Scratch scratch = begin_different_scratch(script->arena);
	String path = arena_push_string_copy(scratch.arena, string_from_data((void *)elf_str_data(value), elf_str_size(value)));
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
	if (nargs != 2 || elf_arg_type(S, 1) != ELF_VALUE_TYPE_STRING) {
		binding_error(S, script, "bob.fs.exists expects a path string");
		return 1;
	}
	elf_String *value = elf_arg_str(S, 1);
	Scratch scratch = begin_different_scratch(script->arena);
	String path = arena_push_string_copy(scratch.arena, string_from_data((void *)elf_str_data(value), elf_str_size(value)));
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
	if (nargs != 3 || elf_arg_type(S, 1) != ELF_VALUE_TYPE_STRING || elf_arg_type(S, 2) != ELF_VALUE_TYPE_STRING)
	{
		binding_error(S, script, "bob.path.join expects two path strings");
		return 1;
	}
	elf_String *left_value = elf_arg_str(S, 1);
	elf_String *right_value = elf_arg_str(S, 2);
	String left = string_from_data((void *)elf_str_data(left_value), elf_str_size(left_value));
	String right = string_from_data((void *)elf_str_data(right_value), elf_str_size(right_value));
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
	if (elf_arg_type(state, 1) == ELF_VALUE_TYPE_TABLE)
	{
		if (nargs != 2) {
			binding_error(state, script, move ? "bob.fs.move_file options must be passed in one table" : "bob.fs.copy_file options must be passed in one table");
			return 1;
		}
		elf_Table *table = elf_arg_table(state, 1);
		source = value_string(elf_get_field(state, table, "from"));
		destination = value_string(elf_get_field(state, table, "to"));
		elf_ValueView value = elf_get_field(state, table, "overwrite");
		if (value.type != ELF_VALUE_TYPE_NIL) {
			if (value.type != ELF_VALUE_TYPE_INTEGER) {
				binding_error(state, script, "filesystem option 'overwrite' must be a boolean");
				return 1;
			}
			overwrite = value.as.integer != 0;
		}
	}
	else
	{
		if ((nargs != 3 && nargs != 4) || elf_arg_type(state, 1) != ELF_VALUE_TYPE_STRING || elf_arg_type(state, 2) != ELF_VALUE_TYPE_STRING || (nargs == 4 && elf_arg_type(state, 3) != ELF_VALUE_TYPE_INTEGER))
		{
			binding_error(state, script, move ? "bob.fs.move_file expects (from, to, overwrite?)" : "bob.fs.copy_file expects (from, to, overwrite?)");
			return 1;
		}
		elf_String *source_value = elf_arg_str(state, 1);
		elf_String *destination_value = elf_arg_str(state, 2);
		source = string_from_data((void *)elf_str_data(source_value), elf_str_size(source_value));
		destination = string_from_data((void *)elf_str_data(destination_value), elf_str_size(destination_value));
		if (nargs == 4) overwrite = elf_arg_int(state, 3) != 0;
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
	if (elf_arg_type(S, 1) == ELF_VALUE_TYPE_TABLE)
	{
		if (nargs != 2) {
			binding_error(S, script, "bob.fs.remove options must be passed in one table");
			return 1;
		}
		elf_Table *table = elf_arg_table(S, 1);
		path = value_string(elf_get_field(S, table, "path"));
		elf_ValueView value = elf_get_field(S, table, "recursive");
		if (value.type != ELF_VALUE_TYPE_NIL) {
			if (value.type != ELF_VALUE_TYPE_INTEGER) {
				binding_error(S, script, "bob.fs.remove option 'recursive' must be a boolean");
				return 1;
			}
			recursive = value.as.integer != 0;
		}
	}
	else
	{
		if ((nargs != 2 && nargs != 3) || elf_arg_type(S, 1) != ELF_VALUE_TYPE_STRING || (nargs == 3 && elf_arg_type(S, 2) != ELF_VALUE_TYPE_INTEGER))
		{
			binding_error(S, script, "bob.fs.remove expects (path, recursive?)");
			return 1;
		}
		elf_String *path_value = elf_arg_str(S, 1);
		path = string_from_data((void *)elf_str_data(path_value), elf_str_size(path_value));
		if (nargs == 3) recursive = elf_arg_int(S, 2) != 0;
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
	if (elf_arg_type(S, 1) == ELF_VALUE_TYPE_TABLE)
	{
		if (nargs != 2) {
			binding_error(S, script, "bob.fs.create_directory options must be passed in one table");
			return 1;
		}
		elf_Table *table = elf_arg_table(S, 1);
		path = value_string(elf_get_field(S, table, "path"));
		elf_ValueView value = elf_get_field(S, table, "recursive");
		if (value.type != ELF_VALUE_TYPE_NIL) {
			if (value.type != ELF_VALUE_TYPE_INTEGER) {
				binding_error(S, script, "bob.fs.create_directory option 'recursive' must be a boolean");
				return 1;
			}
			recursive = value.as.integer != 0;
		}
	}
	else
	{
		if ((nargs != 2 && nargs != 3) || elf_arg_type(S, 1) != ELF_VALUE_TYPE_STRING || (nargs == 3 && elf_arg_type(S, 2) != ELF_VALUE_TYPE_INTEGER))
		{
			binding_error(S, script, "bob.fs.create_directory expects (path, recursive?)");
			return 1;
		}
		elf_String *path_value = elf_arg_str(S, 1);
		path = string_from_data((void *)elf_str_data(path_value), elf_str_size(path_value));
		if (nargs == 3) recursive = elf_arg_int(S, 2) != 0;
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
	if (elf_arg_type(S, 1) == ELF_VALUE_TYPE_TABLE)
	{
		if (nargs != 2) {
			binding_error(S, script, "bob.fs.write_file options must be passed in one table");
			return 1;
		}
		elf_Table *table = elf_arg_table(S, 1);
		path = value_string(elf_get_field(S, table, "path"));
		contents = value_string(elf_get_field(S, table, "contents"));
	}
	else
	{
		if (nargs != 3 || elf_arg_type(S, 1) != ELF_VALUE_TYPE_STRING || elf_arg_type(S, 2) != ELF_VALUE_TYPE_STRING)
		{
			binding_error(S, script, "bob.fs.write_file expects (path, contents)");
			return 1;
		}
		elf_String *path_value = elf_arg_str(S, 1);
		elf_String *contents_value = elf_arg_str(S, 2);
		path = string_from_data((void *)elf_str_data(path_value), elf_str_size(path_value));
		contents = string_from_data((void *)elf_str_data(contents_value), elf_str_size(contents_value));
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
	if (nargs != 2 || elf_arg_type(state, 1) != ELF_VALUE_TYPE_STRING) {
		binding_error(state, script, has_only ? "bob.env.has expects a variable name" : "bob.env.get expects a variable name");
		return 1;
	}
	elf_String *name_value = elf_arg_str(state, 1);
	Scratch scratch = begin_different_scratch(script->arena);
	String name = arena_push_string_copy(scratch.arena, string_from_data((void *)elf_str_data(name_value), elf_str_size(name_value)));
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
	if (nargs != 3 || elf_arg_type(S, 1) != ELF_VALUE_TYPE_STRING || elf_arg_type(S, 2) != ELF_VALUE_TYPE_STRING) {
		binding_error(S, script, "bob.env.set expects a variable name and value");
		return 1;
	}
	elf_String *name_value = elf_arg_str(S, 1);
	elf_String *environment_value = elf_arg_str(S, 2);
	Scratch scratch = begin_different_scratch(script->arena);
	String name = arena_push_string_copy(scratch.arena, string_from_data((void *)elf_str_data(name_value), elf_str_size(name_value)));
	String value = arena_push_string_copy(scratch.arena, string_from_data((void *)elf_str_data(environment_value), elf_str_size(environment_value)));
	elf_push_int(S, platform_set_environment(name, value));
	end_scratch(scratch);
	return 1;
}

ELF_FUNCTION(l_env_unset)
{
	(void)nrets;
	Script *script = elf_get_user_data(S);
	if (nargs != 2 || elf_arg_type(S, 1) != ELF_VALUE_TYPE_STRING) {
		binding_error(S, script, "bob.env.unset expects a variable name");
		return 1;
	}
	elf_String *name_value = elf_arg_str(S, 1);
	Scratch scratch = begin_different_scratch(script->arena);
	String name = arena_push_string_copy(scratch.arena, string_from_data((void *)elf_str_data(name_value), elf_str_size(name_value)));
	elf_push_int(S, platform_set_environment(name, (String){0}));
	end_scratch(scratch);
	return 1;
}

static const char bob_library_source[] =
	"bob.version = bob._version()\n"
	"bob.strings = { expand = bob._strings_expand }\n"
	"bob.fs = {\n"
	"  list = fun(options) { ret bob._fs_list(options):lines() },\n"
	"  exists = bob._fs_exists,\n"
	"  is_file = bob._fs_is_file,\n"
	"  is_directory = bob._fs_is_directory,\n"
	"  copy_file = bob._fs_copy_file,\n"
	"  move_file = bob._fs_move_file,\n"
	"  remove = bob._fs_remove,\n"
	"  create_directory = bob._fs_create_directory,\n"
	"  write_file = bob._fs_write_file,\n"
	"}\n"
	"bob.path = { join = bob._path_join }\n"
	"bob.env = {\n"
	"  get = bob._env_get,\n"
	"  has = bob._env_has,\n"
	"  set = bob._env_set,\n"
	"  unset = bob._env_unset,\n"
	"}\n";

static b32 is_function(elf_ValueView value)
{
	return value.type == ELF_VALUE_TYPE_CFUNCTION || value.type == ELF_VALUE_TYPE_CLOSURE;
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
	elf_register_library(elf->state, "bob", bob_bindings, ARRAY_COUNT(bob_bindings));
	elf_StrSlice library_source = { (char *)bob_library_source, sizeof(bob_library_source) - 1 };
	if (!elf_push_code_source(elf->state, "bob libraries", library_source)) {
		script_set_error(script, "unable to load Bob script libraries");
		return false;
	}
	elf_push_nil(elf->state);
	elf_call(elf->state, 1, 0);
	if (!elf_push_code_file(elf->state, path.data)) {
		script_set_error(script, "unable to load '%s'", path.data);
		return false;
	}
	elf_push_nil(elf->state);
	elf_call(elf->state, 1, 1);
	if (script->failed) {
		elf_pop_values(elf->state, 1);
		return false;
	}

	elf_ValueView returned = elf_peek_value(elf->state, 0);
	if (returned.type != ELF_VALUE_TYPE_TABLE) {
		script_set_error(script, "script must return a table");
		return false;
	}
	elf->exports = elf_retain_table(returned.as.table);
	elf_pop_values(elf->state, 1);

	elf_u32 cursor = 0;
	elf_ValueView key;
	elf_ValueView value;
	while (elf_table_next(elf->exports, &cursor, &key, &value)) {
		if (key.type == ELF_VALUE_TYPE_STRING && is_function(value)) ++script->functions.count;
	}
	script->functions.items = arena_push_zero_aligned(script->arena, script->functions.count * sizeof(String), _Alignof(String));
	cursor = 0;
	u32 function_index = 0;
	while (elf_table_next(elf->exports, &cursor, &key, &value))
	{
		if (key.type != ELF_VALUE_TYPE_STRING || !is_function(value)) continue;
		String name = string_from_data((void *)elf_str_data(key.as.string), elf_str_size(key.as.string));
		script->functions.items[function_index++] = arena_push_string_copy(script->arena, name);
	}
	return true;
}

void elf_script_destroy(Script *script)
{
	Elf_Script *elf = script->context;
	if (elf->exports) elf_release_table(elf->exports);
	if (elf->state) elf_destroy_state(elf->state);
	elf->exports = NULL;
	elf->state = NULL;
}

b32 elf_script_invoke(Script *script, String name)
{
	Elf_Script *elf = script->context;
	elf_push_field(elf->state, elf->exports, name.data);
	elf_push_nil(elf->state);
	elf_call(elf->state, 1, 0);
	return true;
}

static elf_ValueView field(elf_State *state, elf_Table *table, const char *name) {
   return elf_get_field(state, table, name);
}

static String copy_string_value(Arena *arena, elf_ValueView value)
{
   String string;
   if (value.type != ELF_VALUE_TYPE_STRING) return (String){0};
   string = string_from_data((void *)elf_str_data(value.as.string), elf_str_size(value.as.string));
   return arena_push_string_copy(arena, string);
}

static int copy_string_array(elf_State *state, Arena *arena, elf_ValueView value, const char *field_name, String task_name, String_Array *result, char *error, size_t error_size)
{
   elf_Table *table;
   uint32_t i;

   *result = (String_Array){0};
   if (value.type == ELF_VALUE_TYPE_NIL) return 1;
   if (value.type != ELF_VALUE_TYPE_TABLE) {
      snprintf(error, error_size, "%s for '%s' must be a table", field_name, task_name.data);
      return 0;
   }

   table = value.as.table;
   result->count = elf_table_length(table);
   result->items = arena_push_zero_aligned(arena, result->count * sizeof(*result->items), _Alignof(String));
   if (result->count && !result->items) {
      snprintf(error, error_size, "out of memory");
      return 0;
   }

   for (i = 0; i < result->count; ++i)
   {
      result->items[i] = copy_string_value(arena, elf_get_index(state, table, i));
      if (!result->items[i].data) {
         snprintf(error, error_size, "%s for '%s' must contain strings", field_name, task_name.data);
         return 0;
      }
   }
   return 1;
}

typedef struct Table_List {
   Arena *arena;
   elf_Table **items;
   uint32_t count;
} Table_List;

static uint32_t table_list_find(Table_List *list, elf_Table *table)
{
   uint32_t i;
   for (i = 0; i < list->count; ++i) {
      if (list->items[i] == table) return i;
   }
   return UINT32_MAX;
}

static int table_list_add(Table_List *list, elf_Table *table)
{
   elf_Table **item;
   if (table_list_find(list, table) != UINT32_MAX) return 1;
   item = arena_push_aligned(list->arena, sizeof(*item), _Alignof(elf_Table *));
   if (!item) return 0;
   if (!list->items) list->items = item;
   *item = table;
   ++list->count;
   return 1;
}

static b32 read_build_table(Script *script, elf_Table *root, Script_Build *result)
{
   Scratch scratch;
   elf_ValueView targets_value;
   elf_ValueView options_value;
   Elf_Script *elf;
   elf_State *state;
   Table_List task_tables;
   uint32_t i;
   int success = 0;

   if (!script || !result) return false;
   scratch = begin_scratch();
   task_tables = (Table_List){ .arena = scratch.arena };
   memset(result, 0, sizeof(*result));
   elf = script->context;
   state = elf->state;
   options_value = field(state, root, "options");
   if (options_value.type != ELF_VALUE_TYPE_NIL)
   {
      elf_ValueView workers_value;
      elf_ValueView verbosity_value;
      int64_t integer;

      if (options_value.type != ELF_VALUE_TYPE_TABLE) {
         snprintf(result->error, sizeof(result->error), "returned 'options' field must be a table");
         goto cleanup;
      }
      workers_value = field(state, options_value.as.table, "workers");
      if (workers_value.type != ELF_VALUE_TYPE_NIL)
      {
         if (workers_value.type != ELF_VALUE_TYPE_INTEGER) {
            snprintf(result->error, sizeof(result->error), "options.workers must be a positive integer");
            goto cleanup;
         }
         integer = workers_value.as.integer;
         if (integer < 1 || (uint64_t)integer > UINT32_MAX) {
            snprintf(result->error, sizeof(result->error), "options.workers must be a positive integer");
            goto cleanup;
         }
         result->options.worker_count = (uint32_t)integer;
         result->options.has_worker_count = true;
      }
      verbosity_value = field(state, options_value.as.table, "verbosity");
      if (verbosity_value.type != ELF_VALUE_TYPE_NIL)
      {
         if (verbosity_value.type != ELF_VALUE_TYPE_INTEGER) {
            snprintf(result->error, sizeof(result->error), "options.verbosity must be a non-negative integer");
            goto cleanup;
         }
         integer = verbosity_value.as.integer;
         if (integer < 0 || (uint64_t)integer > INT32_MAX) {
            snprintf(result->error, sizeof(result->error), "options.verbosity must be a non-negative integer");
            goto cleanup;
         }
         result->options.verbosity = (int32_t)integer;
         result->options.has_verbosity = true;
      }
   }
   targets_value = field(state, root, "targets");
   if (targets_value.type != ELF_VALUE_TYPE_TABLE) {
      snprintf(result->error, sizeof(result->error), "returned table requires a 'targets' table");
      goto cleanup;
   }

   for (i = 0; i < elf_table_length(targets_value.as.table); ++i)
   {
      elf_ValueView target = elf_get_index(state, targets_value.as.table, i);
      if (target.type != ELF_VALUE_TYPE_TABLE) {
         snprintf(result->error, sizeof(result->error), "target %u must be a task table", i);
         goto cleanup;
      }
      if (!table_list_add(&task_tables, target.as.table)) {
         snprintf(result->error, sizeof(result->error), "out of memory");
         goto cleanup;
      }
   }

   for (i = 0; i < task_tables.count; ++i)
   {
      elf_ValueView dependencies = field(state, task_tables.items[i], "dependencies");
      uint32_t dependency;
      if (dependencies.type == ELF_VALUE_TYPE_NIL) continue;
      if (dependencies.type != ELF_VALUE_TYPE_TABLE) {
         snprintf(result->error, sizeof(result->error), "dependencies for task %u must be a table", i);
         goto cleanup;
      }
      for (dependency = 0; dependency < elf_table_length(dependencies.as.table); ++dependency)
      {
         elf_ValueView value = elf_get_index(state, dependencies.as.table, dependency);
         if (value.type != ELF_VALUE_TYPE_TABLE) {
            snprintf(result->error, sizeof(result->error), "dependencies for task %u must contain task tables", i);
            goto cleanup;
         }
         if (!table_list_add(&task_tables, value.as.table)) {
            snprintf(result->error, sizeof(result->error), "out of memory");
            goto cleanup;
         }
      }
   }

   result->bob = bob_create();
   if (!result->bob) {
      snprintf(result->error, sizeof(result->error), "out of memory");
      goto cleanup;
   }

   for (i = 0; i < task_tables.count; ++i)
   {
      Bob_Task task = {0};
      elf_Table *description = task_tables.items[i];
      Bob_Node *node;
      Bob_Error bob_error;
      task.name = copy_string_value(scratch.arena, field(state, description, "name"));
      task.command_line = copy_string_value(scratch.arena, field(state, description, "command_line"));
      if (!task.name.data || !task.command_line.data)
      {
         snprintf(result->error, sizeof(result->error), "task %u requires string fields 'name' and 'command_line'", i);
         goto cleanup;
      }
	  elf_ValueView transparent = field(state, description, "transparent");
	  if (transparent.type != ELF_VALUE_TYPE_NIL) {
		 if (transparent.type != ELF_VALUE_TYPE_INTEGER) {
			snprintf(result->error, sizeof(result->error), "transparent for '%s' must be a boolean", task.name.data);
			goto cleanup;
		 }
		 task.transparent = transparent.as.integer != 0;
	  }

      if (!copy_string_array(state, scratch.arena, field(state, description, "inputs"), "inputs", task.name, &task.inputs, result->error, sizeof(result->error))) goto cleanup;
      if (!copy_string_array(state, scratch.arena, field(state, description, "outputs"), "outputs", task.name, &task.outputs, result->error, sizeof(result->error))) goto cleanup;
      if (!copy_string_array(state, scratch.arena, field(state, description, "include_dirs"), "include_dirs", task.name, &task.include_directories, result->error, sizeof(result->error))) goto cleanup;
      bob_error = bob_add_task(result->bob, task, &node);
      if (bob_error != BOB_OK) {
         snprintf(result->error, sizeof(result->error), "unable to add task '%s': %s", task.name.data, bob_error_string(bob_error));
         goto cleanup;
      }
   }

   for (i = 0; i < task_tables.count; ++i)
   {
      elf_ValueView dependencies_value = field(state, task_tables.items[i], "dependencies");
      uint32_t dependency;

      if (dependencies_value.type == ELF_VALUE_TYPE_NIL) { continue; }
      for (dependency = 0; dependency < elf_table_length(dependencies_value.as.table); ++dependency)
      {
         elf_ValueView dependency_value = elf_get_index(state, dependencies_value.as.table, dependency);
         Bob_Error bob_error;
         u32 resolved = table_list_find(&task_tables, dependency_value.as.table);
         if (resolved == UINT32_MAX) goto cleanup;
         Bob_Node *node = bob_node_at(result->bob, i);
         Bob_Node *dependency_node = bob_node_at(result->bob, resolved);
         bob_error = bob_add_dependency(result->bob, node, dependency_node);
         if (bob_error != BOB_OK) {
            snprintf(result->error, sizeof(result->error), "unable to add dependency to '%s': %s", bob_task_name(node), bob_error_string(bob_error));
            goto cleanup;
         }
      }
   }

   success = 1;

   cleanup:
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
	return read_build_table(script, elf->exports, result);
}
