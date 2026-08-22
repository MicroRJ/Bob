#ifndef BUILD_STATE_H
#define BUILD_STATE_H

#include "bob_build_internal.h"
#include "platform.h"

typedef struct Build_State_Task
{
	Bob_Path        output;
	u64             output_stamp;
	Bob_Fingerprint fingerprint;
	// Dependency storage is immutable and remains valid until the state arena is destroyed.
	Bob_Path_Array  dependencies;
}
Build_State_Task;

typedef struct Build_State
{
	// NOTE(RJ): state access is thread safe
	Platform_Mutex       mutex;
	// Backing storage must outlive the state.
	Arena               *arena;

	Build_State_Task    *tasks;
	u32                  task_count;
	u32                  task_capacity;
	b32                  initialized;
}
Build_State;

b32 build_state_init(Build_State *state, Arena *arena);
void build_state_destroy(Build_State *state);
void build_state_clear(Build_State *state);

// NOTE(RJ): the result is a copy!
b32 build_state_get(Build_State *state, Bob_Path output, Build_State_Task *result);

// NOTE(RJ): these are only used for tests!
b32 build_state_set(Build_State *state, Bob_Path output, Bob_Path_Array dependencies, Bob_Fingerprint fingerprint);
b32 build_state_remove(Build_State *state, Bob_Path output);

#endif
