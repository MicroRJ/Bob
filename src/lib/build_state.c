#include "build_state_internal.h"

#include <string.h>

#define BUILD_STATE_PATH_INITIAL_CAPACITY 16

static b32 build_state_reserve_paths(Arena *arena, Build_State_Path_Map *map, u32 needed)
{
	if (map->path_capacity >= needed) return true;
	u32 capacity = map->path_capacity ? map->path_capacity : BUILD_STATE_PATH_INITIAL_CAPACITY;
	while (capacity < needed) {
		if (capacity > UINT32_MAX / 2) return false;
		capacity *= 2;
	}
	Bob_Path *paths = arena_push_zero_aligned(arena, (u64)capacity * sizeof(*paths), _Alignof(Bob_Path));
	if (!paths) return false;
	if (map->path_count) memcpy(paths, map->paths, (u64)map->path_count * sizeof(*map->paths));
	map->paths = paths;
	map->path_capacity = capacity;
	return true;
}

static b32 build_state_reserve_atoms(Arena *arena, Build_State_Path_Map *map, u32 atom_id)
{
	if (atom_id < map->atom_capacity) return true;
	u32 capacity = map->atom_capacity ? map->atom_capacity : BUILD_STATE_PATH_INITIAL_CAPACITY;
	while (capacity <= atom_id) {
		if (capacity > UINT32_MAX / 2) return false;
		capacity *= 2;
	}
	u32 *ids = arena_push_zero_aligned(arena, (u64)capacity * sizeof(*ids), _Alignof(u32));
	if (!ids) return false;
	if (map->atom_capacity) memcpy(ids, map->ids_by_atom, (u64)map->atom_capacity * sizeof(*map->ids_by_atom));
	map->ids_by_atom = ids;
	map->atom_capacity = capacity;
	return true;
}

Build_State_Path_Id build_state_path_id(const Build_State *state, Bob_Path path)
{
	u32 atom = path.atom.id;
	if (!state || !bob_path_is_valid(path) || atom >= state->paths.atom_capacity) return BUILD_STATE_PATH_ID_NONE;
	return state->paths.ids_by_atom[atom];
}

Bob_Path build_state_path(const Build_State *state, Build_State_Path_Id id)
{
	if (!state || id == BUILD_STATE_PATH_ID_NONE || id > state->paths.path_count) return (Bob_Path){0};
	return state->paths.paths[id - 1];
}

static Build_State_Path_Id build_state_add_path(Arena *arena, Build_State *state, Bob_Path path)
{
	Build_State_Path_Id existing = build_state_path_id(state, path);
	if (existing != BUILD_STATE_PATH_ID_NONE) return existing;
	if (!arena || !state || !bob_path_is_valid(path) || state->paths.path_count == UINT32_MAX) return BUILD_STATE_PATH_ID_NONE;
	if (!build_state_reserve_paths(arena, &state->paths, state->paths.path_count + 1)) return BUILD_STATE_PATH_ID_NONE;
	if (!build_state_reserve_atoms(arena, &state->paths, path.atom.id)) return BUILD_STATE_PATH_ID_NONE;
	Build_State_Path_Id id = ++state->paths.path_count;
	state->paths.paths[id - 1] = path;
	state->paths.ids_by_atom[path.atom.id] = id;
	return id;
}

b32 build_state_add_replayed_path(Arena *arena, Build_State *state, Bob_Path path)
{
	if (!arena || !state || !bob_path_is_valid(path) || state->paths.path_count == UINT32_MAX) return false;
	if (!build_state_reserve_paths(arena, &state->paths, state->paths.path_count + 1)) return false;
	if (!build_state_reserve_atoms(arena, &state->paths, path.atom.id)) return false;
	Build_State_Path_Id id = ++state->paths.path_count;
	state->paths.paths[id - 1] = path;
	if (state->paths.ids_by_atom[path.atom.id] == BUILD_STATE_PATH_ID_NONE) state->paths.ids_by_atom[path.atom.id] = id;
	return true;
}

b32 build_state_reserve_tasks(Arena *arena, Build_State *state, u32 needed)
{
	if (state->task_capacity >= needed) return true;
	u32 capacity = state->task_capacity ? state->task_capacity : 16;
	while (capacity < needed) {
		if (capacity > UINT32_MAX / 2) return false;
		capacity *= 2;
	}

	Build_State_Task *tasks = arena_push_zero_aligned(arena, (u64)capacity * sizeof(*tasks), _Alignof(Build_State_Task));
	if (!tasks) return false;
	if (state->task_count) memcpy(tasks, state->tasks, (u64)state->task_count * sizeof(*state->tasks));
	state->tasks = tasks;
	state->task_capacity = capacity;
	return true;
}

u32 build_state_task_index(const Build_State *state, Bob_Path output)
{
	if (!state || !bob_path_is_valid(output)) return UINT32_MAX;
	for (u32 i = 0; i < state->task_count; ++i) {
		if (state->tasks[i].output.atom.id == output.atom.id) return i;
	}
	return UINT32_MAX;
}

const Build_State_Task *build_state_find_unlocked(const Build_State *state, Bob_Path output)
{
	u32 index = build_state_task_index(state, output);
	return index == UINT32_MAX ? NULL : state->tasks + index;
}

b32 build_state_set_unlocked(Arena *arena, Build_State *state, Bob_Path output, Bob_Path_Array dependencies, Bob_Fingerprint fingerprint)
{
	if (!arena || !state || !bob_path_is_valid(output)) return false;
	if (dependencies.count && !dependencies.items) return false;
	for (u32 i = 0; i < dependencies.count; ++i) {
		if (!bob_path_is_valid(dependencies.items[i])) return false;
	}

	u32 existing = build_state_task_index(state, output);
	if (existing == UINT32_MAX && state->task_count == UINT32_MAX) return false;
	if (existing == UINT32_MAX && !build_state_reserve_tasks(arena, state, state->task_count + 1)) return false;

	Build_State_Task task = {0};
	if (dependencies.count) {
		task.dependencies.items = arena_push_zero_aligned(arena, (u64)dependencies.count * sizeof(*task.dependencies.items), _Alignof(Bob_Path));
		if (!task.dependencies.items) return false;
	}

	task.output = output;
	task.fingerprint = fingerprint;
	if (build_state_add_path(arena, state, output) == BUILD_STATE_PATH_ID_NONE) return false;
	for (u32 i = 0; i < dependencies.count; ++i) {
		if (build_state_add_path(arena, state, dependencies.items[i]) == BUILD_STATE_PATH_ID_NONE) return false;
		task.dependencies.items[task.dependencies.count++] = dependencies.items[i];
	}

	if (existing != UINT32_MAX) state->tasks[existing] = task;
	else state->tasks[state->task_count++] = task;
	return true;
}

b32 build_state_remove_unlocked(Build_State *state, Bob_Path output)
{
	u32 index = build_state_task_index(state, output);
	if (index == UINT32_MAX) return false;
	if (index + 1 < state->task_count) memmove(state->tasks + index, state->tasks + index + 1, (u64)(state->task_count - index - 1) * sizeof(*state->tasks));
	--state->task_count;
	return true;
}

void build_state_replace_unlocked(Build_State *state, const Build_State *replacement)
{
	state->paths = replacement->paths;
	state->tasks = replacement->tasks;
	state->task_count = replacement->task_count;
	state->task_capacity = replacement->task_capacity;
}

b32 build_state_init(Build_State *state)
{
	if (!state) return false;
	*state = (Build_State){0};
	platform_init_mutex(&state->mutex);
	state->initialized = true;
	return true;
}

void build_state_destroy(Build_State *state)
{
	if (!state || !state->initialized) return;
	platform_destroy_mutex(&state->mutex);
	state->initialized = false;
}

void build_state_clear(Build_State *state)
{
	if (!state || !state->initialized) return;
	platform_lock_mutex(&state->mutex);
	build_state_replace_unlocked(state, &(Build_State){0});
	platform_unlock_mutex(&state->mutex);
}

b32 build_state_get_task(Build_State *state, Bob_Path output, Build_State_Task_Snapshot *result)
{
	if (!state || !state->initialized || !result) return false;
	*result = (Build_State_Task_Snapshot){0};
	platform_lock_mutex(&state->mutex);
	const Build_State_Task *task = build_state_find_unlocked(state, output);
	if (task) {
		result->output = task->output;
		result->output_stamp = task->output_stamp;
		result->fingerprint = task->fingerprint;
		result->dependencies = task->dependencies;
	}
	platform_unlock_mutex(&state->mutex);
	return task != NULL;
}

b32 build_state_set(Arena *arena, Build_State *state, Bob_Path output, Bob_Path_Array dependencies, Bob_Fingerprint fingerprint)
{
	if (!state || !state->initialized) return false;
	platform_lock_mutex(&state->mutex);
	b32 result = build_state_set_unlocked(arena, state, output, dependencies, fingerprint);
	platform_unlock_mutex(&state->mutex);
	return result;
}

b32 build_state_remove(Build_State *state, Bob_Path output)
{
	if (!state || !state->initialized) return false;
	platform_lock_mutex(&state->mutex);
	b32 result = build_state_remove_unlocked(state, output);
	platform_unlock_mutex(&state->mutex);
	return result;
}
