#ifndef SCRIPT_LIB_STRING_H
#define SCRIPT_LIB_STRING_H

#include "base.h"

b32 script_strings_expand(Arena *arena, String_Array strings, String rule, String *result, const char **error);

#endif
