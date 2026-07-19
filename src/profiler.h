#ifndef PROFILER_H
#define PROFILER_H

#include "base.h"

typedef struct Profile_Scope {
	void *entry;
	u64 start;
	b32 recording;
} Profile_Scope;

void profiler_set_enabled(b32 enabled);
void profiler_reset(void);
Profile_Scope profile_scope_begin(const char *name);
void profile_scope_end(Profile_Scope *scope);
void profiler_print(b32 include_threads);

#endif
