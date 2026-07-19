#include "script_internal.h"

#include "scripts/elf_adapter.h"

#include <stdarg.h>
#include <stdio.h>

static const Script_Backend backends[] =
{
	{
		.extension = STRING_LITERAL(".elf"),
		.load = elf_script_load,
		.destroy = elf_script_destroy,
		.invoke = elf_script_invoke,
		.read_build = elf_script_read_build,
	},
};

static const Script_Backend *find_backend(String path)
{
	for (u32 i = 0; i < ARRAY_COUNT(backends); ++i) {
		if (string_ends_with_insensitive(path, backends[i].extension)) return &backends[i];
	}
	return NULL;
}

void script_set_error(Script *script, const char *format, ...)
{
	void *start = arena_top(script->arena);
	va_list arguments;
	va_start(arguments, format);
	arena_appendfv(script->arena, format, arguments);
	va_end(arguments);
	script->error = arena_string_from(script->arena, start);
	arena_finalize_string(script->arena, script->error);
}

b32 script_supports_path(String path)
{
	return find_backend(path) != NULL;
}

Script *script_load(Arena *arena, String path)
{
	Script *script = arena_push_zero_aligned(arena, sizeof(*script), _Alignof(Script));
	script->arena = arena;
	path = arena_push_string_copy(arena, path);
	script->backend = find_backend(path);
	if (!script->backend) {
		script_set_error(script, "no script backend supports '%.*s'", (int)path.size, path.data);
		return script;
	}
	script->loaded = script->backend->load(script, path);
	return script;
}

void script_destroy(Script *script)
{
	if (script && script->context && script->backend->destroy) script->backend->destroy(script);
	if (script) {
		script->context = NULL;
		script->loaded = false;
	}
}

b32 script_is_loaded(Script *script)
{
	return script && script->loaded;
}

String script_error(Script *script)
{
	return script ? script->error : (String){0};
}

String_Array script_functions(Script *script)
{
	return script ? script->functions : (String_Array){0};
}

b32 script_has_function(Script *script, String name)
{
	if (!script) return false;
	for (u32 i = 0; i < script->functions.count; ++i) {
		if (string_equal(script->functions.items[i], name)) return true;
	}
	return false;
}

void script_set_command_line_options(Script *script, Cmd_Options options)
{
	if (script) script->command_line_options = options;
}

b32 script_invoke(Script *script, String name)
{
	if (!script_is_loaded(script)) return false;
	if (!script_has_function(script, name)) {
		script_set_error(script, "script has no function '%.*s'", (int)name.size, name.data);
		return false;
	}
	script->failed = false;
	Scratch scratch = begin_different_scratch(script->arena);
	String terminated_name = arena_push_string_copy(scratch.arena, name);
	b32 result = script->backend->invoke(script, terminated_name);
	end_scratch(scratch);
	return result && !script->failed;
}

b32 script_read_build(Script *script, Bob_Build *result)
{
	if (!script_is_loaded(script)) return false;
	return script->backend->read_build(script, result);
}

b32 script_load_build(String path, Bob_Build *result)
{
	*result = (Bob_Build){0};
	Scratch scratch = begin_scratch();
	Script *script = script_load(scratch.arena, path);
	b32 success = false;
	if (script_is_loaded(script)) success = script_read_build(script, result);
	else snprintf(result->error, sizeof(result->error), "%s", script_error(script).data);
	script_destroy(script);
	end_scratch(scratch);
	return success;
}
