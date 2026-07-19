#ifndef VCVARS_CACHE_H
#define VCVARS_CACHE_H

#include "base.h"

b32 vcvars_cache_refresh(Arena *arena, String *path);
b32 vcvars_cache_load(void);
b32 vcvars_cache_apply(String cache);

#endif
