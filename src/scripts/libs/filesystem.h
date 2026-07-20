#ifndef SCRIPT_LIB_FILESYSTEM_H
#define SCRIPT_LIB_FILESYSTEM_H

#include "base.h"

typedef enum Script_Path_Kind
{
	SCRIPT_PATH_FILES,
	SCRIPT_PATH_DIRECTORIES,
	SCRIPT_PATH_ALL,
} Script_Path_Kind;

typedef struct Script_List_Paths_Options
{
	String root;
	String pattern;
	Script_Path_Kind kind;
	b32 recursive;
	b32 relative;
} Script_List_Paths_Options;

b32 script_list_paths(Arena *arena, Script_List_Paths_Options options, String_Array *result);

#endif
