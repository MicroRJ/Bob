#ifndef COMPILER_COMMAND_H
#define COMPILER_COMMAND_H

#include "base.h"

typedef struct Compiler_Command
{
	String_Array include_directories;
}
Compiler_Command;

b32 compiler_command_parse(Arena *arena, String command_line, Compiler_Command *result);

#endif
