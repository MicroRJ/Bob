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

b32 build_state_stream_encode(Arena *arena, const Bob_Build *build, Build_State *state, String *stream);
Build_State_Stream_Result build_state_stream_replay(Arena *arena, Bob_Build *build, String stream, Build_State *state);
b32 build_state_append_set(Arena *arena, String path, const Bob_Build *build, Build_State *state, Bob_Path output, Bob_Path_Array dependencies, u64 output_stamp, Bob_Fingerprint fingerprint);
b32 build_state_append_remove(String path, Build_State *state, Bob_Path output);
b32 build_state_save(String path, const Bob_Build *build, Build_State *state);
Build_State_Load_Result build_state_load(Arena *arena, Bob_Build *build, String path, Build_State *state);

#endif
