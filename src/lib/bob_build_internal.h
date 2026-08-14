#ifndef BOB_BUILD_INTERNAL_H
#define BOB_BUILD_INTERNAL_H

#include "bob_build.h"
#include "bob_atom.h"

typedef struct Bob_Path
{
	Bob_Atom atom;
}
Bob_Path;

typedef struct Bob_Path_Array
{
	Bob_Path *items;
	u32       count;
}
Bob_Path_Array;

b32 bob_path_resolve(Bob_Build *build, Bob_Path directory, String source, Bob_Path *result);
String bob_path_string(const Bob_Build *build, Bob_Path path);
b32 bob_path_is_valid(Bob_Path path);
Bob_Path bob_build_root(const Bob_Build *build);

#endif
