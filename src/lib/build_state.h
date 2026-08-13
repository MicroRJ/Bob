#ifndef BUILD_STATE_H
#define BUILD_STATE_H

#include "base.h"

#define BUILD_STATE_STREAM_VERSION     2
#define BUILD_STATE_STREAM_MAGIC       "BOBSTATE"
#define BUILD_STATE_STREAM_MAGIC_SIZE  8
#define BUILD_STATE_STREAM_HEADER_SIZE 16
#define BUILD_STATE_STREAM_RECORD_HEADER_SIZE 8

typedef u32 Build_State_Path_Id;

#define BUILD_STATE_PATH_ID_NONE ((Build_State_Path_Id)0)

typedef struct Build_State_Path
{
	String value;
	// TODO(RJ) could remove this, just makes rehashing a bit more convenient
	u64    hash;
}
Build_State_Path;

typedef struct Build_State_Path_Slot
{
	u64                 hash;
	Build_State_Path_Id id;
}
Build_State_Path_Slot;

// Path IDs are one-based. An ID maps to paths[id - 1], while zero means none.
typedef struct Build_State_Path_Table
{
	Build_State_Path      *paths;
	u32                    path_count;
	u32                    path_capacity;
	Build_State_Path_Slot *slots;
	u32                    slot_count;
	u32                    slot_capacity;
}
Build_State_Path_Table;

u64 build_state_path_hash(String path);
Build_State_Path_Id build_state_path_table_find(const Build_State_Path_Table *table, String path);
Build_State_Path_Id build_state_path_table_intern(Arena *arena, Build_State_Path_Table *table, String path);
String build_state_path_table_get(const Build_State_Path_Table *table, Build_State_Path_Id id);

typedef struct Build_State_Path_Id_Array
{
	Build_State_Path_Id *items;
	u32                  count;
}
Build_State_Path_Id_Array;

typedef struct Build_State_Task
{
	Build_State_Path_Id       output;
	u64                       output_stamp;
	Build_State_Path_Id_Array dependencies;
}
Build_State_Task;

typedef struct Build_State
{
	Build_State_Path_Table paths;
	Build_State_Task      *tasks;
	u32                    task_count;
	u32                    task_capacity;
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

const Build_State_Task *build_state_find(const Build_State *state, String output);
b32 build_state_set(Arena *arena, Build_State *state, String output, String_Array dependencies);
b32 build_state_remove(Build_State *state, String output);
Build_State_Load_Result build_state_load(Arena *arena, String path, Build_State *state);
b32 build_state_save(String path, const Build_State *state);

// A state file is an append-only instruction stream.
// Compaction writes the same instructions into a new file, keeping only live
// paths and the latest value of each task.
typedef enum Build_State_Op
{
	STATE_OP_INTERN = 1,
	STATE_OP_SET,
	STATE_OP_REMOVE,
}
Build_State_Op;

typedef struct Build_State_Stream_Header
{
	u8  magic[BUILD_STATE_STREAM_MAGIC_SIZE];
	u32 version;
	u32 header_size;
}
Build_State_Stream_Header;

// content_size counts the encoded operation and operands following this header.
// The checksum covers those content bytes.
typedef struct Build_State_Record_Header
{
	u32 content_size;
	u32 content_crc32c;
}
Build_State_Record_Header;

// INTERN encodes a byte count followed by the path bytes. Its ID is implicit:
// the first INTERN creates path 1, the second creates path 2, and so on.
typedef struct Build_State_Intern
{
	String path;
}
Build_State_Intern;

// SET creates or replaces the state associated with an output path.
typedef struct Build_State_Set
{
	Build_State_Path_Id       output;
	u64                       output_stamp;
	Build_State_Path_Id_Array dependencies;
}
Build_State_Set;

typedef struct Build_State_Remove
{
	Build_State_Path_Id output;
}
Build_State_Remove;

typedef enum Build_State_Stream_Result
{
	BUILD_STATE_STREAM_OK,
	BUILD_STATE_STREAM_TRUNCATED,
	BUILD_STATE_STREAM_INVALID,
	BUILD_STATE_STREAM_ERROR,
}
Build_State_Stream_Result;

b32 build_state_stream_encode(Arena *arena, const Build_State *state, String *stream);
Build_State_Stream_Result build_state_stream_replay(Arena *arena, String stream, Build_State *state);

#endif
