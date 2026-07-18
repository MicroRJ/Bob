#ifndef VCVARS_CACHE_H
#define VCVARS_CACHE_H

#include "base.h"

b32 vcvars_cache_refresh(char *path, u32 path_size);
b32 vcvars_cache_load(void);
b32 vcvars_cache_apply(String cache);

#endif
