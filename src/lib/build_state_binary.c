#include "build_state_binary.h"

#include <string.h>

#define BUILD_STATE_PATH_INITIAL_CAPACITY 16
#define BUILD_STATE_PATH_LOAD_NUMERATOR   3
#define BUILD_STATE_PATH_LOAD_DENOMINATOR 4

u64 build_state_path_hash(String path)
{
	u64 hash = 14695981039346656037ULL;
	for (u64 i = 0; i < path.size; ++i) {
		hash ^= (u8)path.data[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

static Build_State_Path_Slot *build_state_path_slot(Build_State_Path_Slot *slots, u32 capacity, u64 hash)
{
	u32 mask = capacity - 1;
	u32 index = (u32)hash & mask;
	while (slots[index].id != BUILD_STATE_PATH_ID_NONE) {
		index = (index + 1) & mask;
	}
	return slots + index;
}

static b32 build_state_path_table_reserve_paths(Arena *arena, Build_State_Path_Table *table, u32 needed)
{
	if (table->path_capacity >= needed) return true;
	u32 capacity = table->path_capacity ? table->path_capacity : BUILD_STATE_PATH_INITIAL_CAPACITY;
	while (capacity < needed) {
		if (capacity > UINT32_MAX / 2) return false;
		capacity *= 2;
	}

	Build_State_Path *paths = arena_push_zero_aligned(arena, (u64)capacity * sizeof(*paths), _Alignof(Build_State_Path));
	if (!paths) return false;

	if (table->path_count) {
		memcpy(paths, table->paths, (u64)table->path_count * sizeof(*table->paths));
	}

	table->paths = paths;
	table->path_capacity = capacity;
	return true;
}

static b32 build_state_path_table_rehash(Arena *arena, Build_State_Path_Table *table, u32 capacity)
{

	if (capacity < BUILD_STATE_PATH_INITIAL_CAPACITY || (capacity & (capacity - 1)) != 0) return false;

	Build_State_Path_Slot *slots = arena_push_zero_aligned(arena, (u64)capacity * sizeof(*slots), _Alignof(Build_State_Path_Slot));
	if (!slots) return false;
	for (u32 i = 0; i < table->path_count; ++i) {
		Build_State_Path_Id id = i + 1;
		Build_State_Path_Slot *slot = build_state_path_slot(slots, capacity, table->paths[i].hash);
		slot->hash = table->paths[i].hash;
		slot->id = id;
	}
	table->slots = slots;
	table->slot_count = table->path_count;
	table->slot_capacity = capacity;
	return true;
}

static b32 build_state_path_table_reserve_slot(Arena *arena, Build_State_Path_Table *table)
{
	u32 capacity = table->slot_capacity;
	if (capacity == 0) {
		return build_state_path_table_rehash(arena, table, BUILD_STATE_PATH_INITIAL_CAPACITY);
	}
	if ((u64)(table->slot_count + 1) * BUILD_STATE_PATH_LOAD_DENOMINATOR <= (u64)capacity * BUILD_STATE_PATH_LOAD_NUMERATOR) return true;
	if (capacity > UINT32_MAX / 2) return false;
	return build_state_path_table_rehash(arena, table, capacity * 2);
}

Build_State_Path_Id build_state_path_table_find(const Build_State_Path_Table *table, String path)
{
	if (!table || !path.data || path.size == 0 || !table->slots || table->slot_capacity == 0) return BUILD_STATE_PATH_ID_NONE;
	u64 hash = build_state_path_hash(path);
	u32 mask = table->slot_capacity - 1;
	u32 index = (u32) hash & mask;
	for (u32 probe = 0; probe < table->slot_capacity; ++probe) {
		const Build_State_Path_Slot *slot = table->slots + index;
		if (slot->id == BUILD_STATE_PATH_ID_NONE) return BUILD_STATE_PATH_ID_NONE;
		if (slot->hash == hash && slot->id <= table->path_count) {
			String stored = table->paths[slot->id - 1].value;
			if (string_equal(stored, path)) return slot->id;
		}
		index = (index + 1) & mask;
	}
	return BUILD_STATE_PATH_ID_NONE;
}

Build_State_Path_Id build_state_path_table_intern(Arena *arena, Build_State_Path_Table *table, String path)
{

	if (!arena || !table || !path.data || path.size == 0 || table->path_count == UINT32_MAX) return BUILD_STATE_PATH_ID_NONE;
	Build_State_Path_Id id = build_state_path_table_find(table, path);
	if (id != BUILD_STATE_PATH_ID_NONE) return id;

	Build_State_Path_Table original = *table;
	u64 mark = arena_mark(arena);
	String copy = arena_push_string_copy(arena, path);
	if (!copy.data || !build_state_path_table_reserve_paths(arena, table, table->path_count + 1) || !build_state_path_table_reserve_slot(arena, table)) goto failure;

	u64 hash = build_state_path_hash(path);
	id = table->path_count + 1;
	table->paths[table->path_count++] = (Build_State_Path){ copy, hash };
	Build_State_Path_Slot *slot = build_state_path_slot(table->slots, table->slot_capacity, hash);
	slot->hash = hash;
	slot->id = id;
	++table->slot_count;
	return id;

failure:
	*table = original;
	arena_restore(arena, mark);
	return BUILD_STATE_PATH_ID_NONE;
}

String build_state_path_table_get(
	const Build_State_Path_Table *table, Build_State_Path_Id id)
{
	if (!table || id == BUILD_STATE_PATH_ID_NONE || id > table->path_count) {
		return (String){0};
	}
	return table->paths[id - 1].value;
}

static b32 build_state_binary_reserve_tasks(Arena *arena, Build_State_Binary *state, u32 needed)
{
	if (state->task_capacity >= needed) return true;
	u32 capacity = state->task_capacity ? state->task_capacity : 16;
	while (capacity < needed) {
		if (capacity > UINT32_MAX / 2) return false;
		capacity *= 2;
	}

	Build_State_Binary_Task *tasks = arena_push_zero_aligned(arena,
		(u64)capacity * sizeof(*tasks), _Alignof(Build_State_Binary_Task));
	if (!tasks) return false;
	if (state->task_count) {
		memcpy(tasks, state->tasks, (u64)state->task_count * sizeof(*state->tasks));
	}
	state->tasks = tasks;
	state->task_capacity = capacity;
	return true;
}

Build_State_Binary_Task *build_state_binary_find(Build_State_Binary *state, String output)
{
	if (!state) return NULL;
	Build_State_Path_Id output_id = build_state_path_table_find(&state->paths, output);
	if (output_id == BUILD_STATE_PATH_ID_NONE) return NULL;
	for (u32 i = 0; i < state->task_count; ++i) {
		if (state->tasks[i].output == output_id) return state->tasks + i;
	}
	return NULL;
}

b32 build_state_binary_set(Arena *arena, Build_State_Binary *state,
	String output, String_Array dependencies)
{
	if (!arena || !state || !output.data || output.size == 0 ||
		(dependencies.count && !dependencies.items)) return false;
	for (u32 i = 0; i < dependencies.count; ++i) {
		if (!dependencies.items[i].data || dependencies.items[i].size == 0) return false;
	}

	Build_State_Binary_Task *existing = build_state_binary_find(state, output);
	if (!existing && (state->task_count == UINT32_MAX ||
		!build_state_binary_reserve_tasks(arena, state, state->task_count + 1))) return false;

	Build_State_Binary_Task task = {0};
	if (dependencies.count) {
		task.dependencies.items = arena_push_zero_aligned(arena,
			(u64)dependencies.count * sizeof(*task.dependencies.items),
			_Alignof(Build_State_Path_Id));
		if (!task.dependencies.items) return false;
	}

	task.output = build_state_path_table_intern(arena, &state->paths, output);
	if (task.output == BUILD_STATE_PATH_ID_NONE) return false;
	for (u32 i = 0; i < dependencies.count; ++i) {
		Build_State_Path_Id id = build_state_path_table_intern(
			arena, &state->paths, dependencies.items[i]);
		if (id == BUILD_STATE_PATH_ID_NONE) return false;
		task.dependencies.items[task.dependencies.count++] = id;
	}

	if (existing) *existing = task;
	else state->tasks[state->task_count++] = task;
	return true;
}

b32 build_state_binary_remove(Build_State_Binary *state, String output)
{
	Build_State_Binary_Task *task = build_state_binary_find(state, output);
	if (!task) return false;
	u32 index = (u32)(task - state->tasks);
	if (index + 1 < state->task_count) {
		memmove(state->tasks + index, state->tasks + index + 1,
			(u64)(state->task_count - index - 1) * sizeof(*state->tasks));
	}
	--state->task_count;
	return true;
}
