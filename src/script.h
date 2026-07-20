#ifndef SCRIPT_H
#define SCRIPT_H

#include "bob.h"
#include "cmd_options.h"

typedef struct Script Script;

typedef struct Script_Options
{
	u32 worker_count;
	i32 verbosity;
	b32 has_worker_count;
	b32 has_verbosity;
}
Script_Options;

typedef struct Script_Build
{
	Bob *bob;
	Script_Options options;
	char error[256];
}
Script_Build;

static inline Script_Options script_options_resolve(Script_Options script, Cmd_Options command_line)
{
	Script_Options options = {
		.worker_count = 4,
		.verbosity = 0,
		.has_worker_count = true,
		.has_verbosity = true,
	};
	if (script.has_worker_count) options.worker_count = script.worker_count;
	if (script.has_verbosity) options.verbosity = script.verbosity;
	if (command_line.has_worker_count) options.worker_count = command_line.worker_count;
	if (command_line.has_verbosity) options.verbosity = command_line.verbosity;
	return options;
}

b32 script_supports_path(String path);
Script *script_load(Arena *arena, String path);
void script_destroy(Script *script);

b32 script_is_loaded(Script *script);
String script_error(Script *script);
String_Array script_functions(Script *script);
b32 script_has_function(Script *script, String name);
void script_set_command_line_options(Script *script, Cmd_Options options);
b32 script_invoke(Script *script, String name);

// Compatibility with scripts that return { targets, options }.
b32 script_read_build(Script *script, Script_Build *result);
b32 script_load_build(String path, Script_Build *result);

#endif
