#ifndef MAKE_DEPFILE_H
#define MAKE_DEPFILE_H

#include "base.h"

b32 make_depfile_parse(Arena *arena, String contents, String_Array *dependencies);

#endif
