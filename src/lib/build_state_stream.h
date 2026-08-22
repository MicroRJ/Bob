#ifndef BUILD_STATE_STREAM_H
#define BUILD_STATE_STREAM_H

#include "build_state.h"

#define BUILD_STATE_STREAM_VERSION            3
#define BUILD_STATE_STREAM_MAGIC              "BOBSTATE"
#define BUILD_STATE_STREAM_MAGIC_SIZE         8
#define BUILD_STATE_STREAM_HEADER_SIZE        16
#define BUILD_STATE_STREAM_RECORD_HEADER_SIZE 8

typedef enum Build_State_Load_Result
{
	BUILD_STATE_LOAD_OK,
	BUILD_STATE_LOAD_RECOVERED,
	BUILD_STATE_LOAD_MISSING,
	BUILD_STATE_LOAD_INVALID,
	BUILD_STATE_LOAD_ERROR,
}
Build_State_Load_Result;

// A state file is an append-only instruction stream. Compaction writes the
// same instructions into a new file, keeping only live paths and tasks.
typedef enum Build_State_Op
{
	STATE_OP_INTERN = 1,
	STATE_OP_SET,
	STATE_OP_REMOVE,
}
Build_State_Op;

// SET stores an output ID, output timestamp, task fingerprint, and dependency IDs.
typedef enum Build_State_Stream_Result
{
	BUILD_STATE_STREAM_OK,
	BUILD_STATE_STREAM_TRUNCATED,
	BUILD_STATE_STREAM_INVALID,
	BUILD_STATE_STREAM_ERROR,
}
Build_State_Stream_Result;

// Runtime index for the path IDs in one state stream.
typedef struct Build_State_Stream
{
	Bob_Build   *build;
	Build_State *state;

	Bob_Path *paths;
	u32       path_count;
	u32       path_capacity;
	u32      *ids_by_atom;
	u32       atom_capacity;
}
Build_State_Stream;

void build_state_stream_init(Build_State_Stream *state_stream, Bob_Build *build, Build_State *state);
b32 build_state_stream_encode(Build_State_Stream *state_stream, Arena *arena, String *stream);
Build_State_Stream_Result build_state_stream_replay(Build_State_Stream *state_stream, String stream);
b32 build_state_stream_append_set(Build_State_Stream *state_stream, String path, Bob_Path output, Bob_Path_Array dependencies, u64 output_stamp, Bob_Fingerprint fingerprint);
b32 build_state_stream_append_remove(Build_State_Stream *state_stream, String path, Bob_Path output);
b32 build_state_stream_save(Build_State_Stream *state_stream, String path);
Build_State_Load_Result build_state_stream_load(Build_State_Stream *state_stream, String path);

#endif
