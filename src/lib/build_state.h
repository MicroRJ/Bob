#ifndef BUILD_STATE_H
#define BUILD_STATE_H

#include "bob_build.h"
#include "platform.h"

typedef u32 Build_State_Path_Id;

#define BUILD_STATE_PATH_ID_NONE ((Build_State_Path_Id)0)

// Path IDs belong to one state stream. Runtime code uses Bob_Path instead.
typedef struct Build_State_Path_Map
{
	Bob_Path *paths;
	u32       path_count;
	u32       path_capacity;
	u32      *ids_by_atom;
	u32       atom_capacity;
}
Build_State_Path_Map;

typedef struct Build_State_Task
{
	Bob_Path        output;
	u64             output_stamp;
	Bob_Fingerprint fingerprint;
	Bob_Path_Array  dependencies;
}
Build_State_Task;

// Dependency storage is immutable and remains valid until the state arena is destroyed.
typedef struct Build_State_Task_Snapshot
{
	Bob_Path        output;
	u64             output_stamp;
	Bob_Fingerprint fingerprint;
	Bob_Path_Array  dependencies;
}
Build_State_Task_Snapshot;

typedef struct Build_State
{
	Platform_Mutex       mutex;
	Build_State_Path_Map paths;
	Build_State_Task    *tasks;
	u32                  task_count;
	u32                  task_capacity;
	b32                  initialized;
}
Build_State;

b32 build_state_init(Build_State *state);
void build_state_destroy(Build_State *state);
void build_state_clear(Build_State *state);
b32 build_state_get_task(Build_State *state, Bob_Path output, Build_State_Task_Snapshot *result);
b32 build_state_set(Arena *arena, Build_State *state, Bob_Path output, Bob_Path_Array dependencies, Bob_Fingerprint fingerprint);
b32 build_state_remove(Build_State *state, Bob_Path output);

#endif
