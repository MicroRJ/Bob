#ifndef SCRIPT_INTERNAL_H
#define SCRIPT_INTERNAL_H

#include "script.h"

typedef struct Script_Backend
{
	String extension;
	b32 (*load)(Script *script, String path);
	void (*destroy)(Script *script);
	b32 (*invoke)(Script *script, String name);
	b32 (*read_build)(Script *script, Bob_Build *result);
}
Script_Backend;

struct Script
{
	Arena *arena;
	const Script_Backend *backend;
	void *context;
	String_Array functions;
	String error;
	b32 loaded;
};

void script_set_error(Script *script, const char *format, ...);

#endif
