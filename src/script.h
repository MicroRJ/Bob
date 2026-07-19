#ifndef SCRIPT_H
#define SCRIPT_H

#include "bob.h"

typedef struct Script Script;

b32 script_supports_path(String path);
Script *script_load(Arena *arena, String path);
void script_destroy(Script *script);

b32 script_is_loaded(Script *script);
String script_error(Script *script);
String_Array script_functions(Script *script);
b32 script_has_function(Script *script, String name);
void script_set_build_overrides(Script *script, Bob_Options options);
b32 script_invoke(Script *script, String name);

// Compatibility with scripts that return { targets, options }.
b32 script_read_build(Script *script, Bob_Build *result);
b32 script_load_build(String path, Bob_Build *result);

#endif
