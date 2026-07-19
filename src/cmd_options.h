#ifndef CMD_OPTIONS_H
#define CMD_OPTIONS_H

#include "base.h"

typedef struct Cmd_Options
{
	u32 worker_count;
	i32 verbosity;
	b32 has_worker_count;
	b32 has_verbosity;
}
Cmd_Options;

#endif
