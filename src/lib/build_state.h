#ifndef BUILD_STATE_H
#define BUILD_STATE_H

#include "base.h"

#define BUILD_STATE_VERSION 1

typedef struct Build_State_Task
{
	String       output;
	String_Array dependencies;
}
Build_State_Task;

typedef struct Build_State
{
	Build_State_Task *tasks;
	u32               count;
	u32               capacity;
}
Build_State;

Build_State_Task *build_state_find(Build_State *state, String output);
b32 build_state_set(Arena *arena, Build_State *state, String output, String_Array dependencies);
b32 build_state_parse(Arena *arena, String source, Build_State *state);
b32 build_state_write(Arena *arena, const Build_State *state, String *source);

#endif
