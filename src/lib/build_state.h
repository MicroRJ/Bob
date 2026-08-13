#ifndef BUILD_STATE_H
#define BUILD_STATE_H

#include "bob.h"

#define BUILD_STATE_STREAM_VERSION     2
#define BUILD_STATE_STREAM_MAGIC       "BOBSTATE"
#define BUILD_STATE_STREAM_MAGIC_SIZE  8
#define BUILD_STATE_STREAM_HEADER_SIZE 16
#define BUILD_STATE_STREAM_RECORD_HEADER_SIZE 8

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
	Bob_Path       output;
	u64            output_stamp;
	Bob_Path_Array dependencies;
}
Build_State_Task;

typedef struct Build_State
{
	Build_State_Path_Map paths;
	Build_State_Task    *tasks;
	u32                  task_count;
	u32                  task_capacity;
}
Build_State;

typedef enum Build_State_Load_Result
{
	BUILD_STATE_LOAD_OK,
	BUILD_STATE_LOAD_RECOVERED,
	BUILD_STATE_LOAD_MISSING,
	BUILD_STATE_LOAD_INVALID,
	BUILD_STATE_LOAD_ERROR,
}
Build_State_Load_Result;

const Build_State_Task *build_state_find(const Build_State *state, Bob_Path output);
b32 build_state_set(Arena *arena, Build_State *state, Bob_Path output, Bob_Path_Array dependencies);
b32 build_state_remove(Build_State *state, Bob_Path output);
Build_State_Load_Result build_state_load(Arena *arena, Bob *bob, String path, Build_State *state);
b32 build_state_save(String path, const Bob *bob, const Build_State *state);

// A state file is an append-only instruction stream. Compaction writes the
// same instructions into a new file, keeping only live paths and tasks.
typedef enum Build_State_Op
{
	STATE_OP_INTERN = 1,
	STATE_OP_SET,
	STATE_OP_REMOVE,
}
Build_State_Op;

typedef enum Build_State_Stream_Result
{
	BUILD_STATE_STREAM_OK,
	BUILD_STATE_STREAM_TRUNCATED,
	BUILD_STATE_STREAM_INVALID,
	BUILD_STATE_STREAM_ERROR,
}
Build_State_Stream_Result;

b32 build_state_stream_encode(Arena *arena, const Bob *bob, const Build_State *state, String *stream);
Build_State_Stream_Result build_state_stream_replay(Arena *arena, Bob *bob, String stream, Build_State *state);
b32 build_state_append_set(Arena *arena, String path, const Bob *bob, Build_State *state, Bob_Path output, Bob_Path_Array dependencies, u64 output_stamp);
b32 build_state_append_remove(String path, Build_State *state, Bob_Path output);

#endif
