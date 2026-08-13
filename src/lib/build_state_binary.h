#ifndef BUILD_STATE_BINARY_H
#define BUILD_STATE_BINARY_H

#include "base.h"

#define BUILD_STATE_BINARY_VERSION 1

#define BUILD_STATE_SNAPSHOT_MAGIC      "BOBSTATE"
#define BUILD_STATE_SNAPSHOT_MAGIC_SIZE 8
#define BUILD_STATE_SNAPSHOT_HEADER_SIZE 48

#define BUILD_STATE_JOURNAL_MAGIC      "BOBJRNL"
#define BUILD_STATE_JOURNAL_MAGIC_SIZE 8

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

typedef struct Build_State_Binary_Task
{
	Build_State_Path_Id       output;
	Build_State_Path_Id_Array dependencies;
}
Build_State_Binary_Task;

typedef struct Build_State_Binary
{
	u64                        generation;
	Build_State_Path_Table     paths;
	Build_State_Binary_Task   *tasks;
	u32                        task_count;
	u32                        task_capacity;
}
Build_State_Binary;

typedef enum Build_State_Load_Result
{
	BUILD_STATE_LOAD_OK,
	BUILD_STATE_LOAD_MISSING,
	BUILD_STATE_LOAD_INVALID,
	BUILD_STATE_LOAD_ERROR,
}
Build_State_Load_Result;

const Build_State_Binary_Task *build_state_binary_find(const Build_State_Binary *state, String output);
b32 build_state_binary_set(Arena *arena, Build_State_Binary *state, String output, String_Array dependencies);
b32 build_state_binary_remove(Build_State_Binary *state, String output);
Build_State_Load_Result build_state_binary_load(
	Arena *arena, String path, Build_State_Binary *state);
b32 build_state_binary_save(String path, const Build_State_Binary *state);

// These structures describe decoded fields. Files are encoded and decoded
// field by field in little-endian order; native C structure layouts are never
// written directly.
typedef struct Build_State_Snapshot_Header
{
	u8  magic[BUILD_STATE_SNAPSHOT_MAGIC_SIZE];
	u32 version;
	u32 header_size;
	u64 generation;
	u64 payload_size;
	u32 path_count;
	u32 task_count;
	u32 payload_crc32c;
	u32 reserved;
}
Build_State_Snapshot_Header;

typedef struct Build_State_Snapshot_Path_Header
{
	u32 size;
}
Build_State_Snapshot_Path_Header;

// record_size counts the encoded bytes following record_size.
typedef struct Build_State_Snapshot_Task_Header
{
	u32                 record_size;
	Build_State_Path_Id output;
	u32                 dependency_count;
}
Build_State_Snapshot_Task_Header;

typedef enum Build_State_Journal_Operation
{
	BUILD_STATE_JOURNAL_SET = 1,
	BUILD_STATE_JOURNAL_REMOVE,
}
Build_State_Journal_Operation;

typedef struct Build_State_Journal_Header
{
	u8  magic[BUILD_STATE_JOURNAL_MAGIC_SIZE];
	u32 version;
	u32 header_size;
	u64 base_generation;
	u64 reserved;
}
Build_State_Journal_Header;

// content_size counts the encoded operation and payload following this
// header. The checksum covers those content bytes.
typedef struct Build_State_Journal_Record_Header
{
	u32 content_size;
	u32 content_crc32c;
}
Build_State_Journal_Record_Header;

// Journal records keep paths inline. Replaying a record interns these strings
// into the in-memory path table; path IDs never need to remain stable across
// snapshot compaction.
typedef struct Build_State_Journal_Record
{
	Build_State_Journal_Operation operation;
	String                        output;
	String_Array                  dependencies;
}
Build_State_Journal_Record;

#endif
