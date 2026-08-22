#include "build_state_internal.h"

#include <string.h>

b32 build_state_reserve_tasks(Build_State *state, u32 needed)
{
	if (state->task_capacity >= needed) return true;
	u32 capacity = state->task_capacity ? state->task_capacity : 16;
	while (capacity < needed) {
		if (capacity > UINT32_MAX / 2) return false;
		capacity *= 2;
	}

	Build_State_Task *tasks = arena_push_zero_aligned(state->arena, (u64)capacity * sizeof(*tasks), _Alignof(Build_State_Task));
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

b32 build_state_set_unlocked(Build_State *state, Bob_Path output, Bob_Path_Array dependencies, Bob_Fingerprint fingerprint)
{
	if (!state || !state->arena || !bob_path_is_valid(output)) return false;
	if (dependencies.count && !dependencies.items) return false;
	for (u32 i = 0; i < dependencies.count; ++i) {
		if (!bob_path_is_valid(dependencies.items[i])) return false;
	}

	u32 existing = build_state_task_index(state, output);
	if (existing == UINT32_MAX && state->task_count == UINT32_MAX) return false;
	if (existing == UINT32_MAX && !build_state_reserve_tasks(state, state->task_count + 1)) return false;

	Build_State_Task task = {0};
	if (dependencies.count) {
		task.dependencies.items = arena_push_zero_aligned(state->arena, (u64)dependencies.count * sizeof(*task.dependencies.items), _Alignof(Bob_Path));
		if (!task.dependencies.items) return false;
	}

	task.output = output;
	task.fingerprint = fingerprint;
	for (u32 i = 0; i < dependencies.count; ++i) {
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
	state->tasks = replacement->tasks;
	state->task_count = replacement->task_count;
	state->task_capacity = replacement->task_capacity;
}

b32 build_state_init(Build_State *state, Arena *arena)
{
	ASSERT(state);
	ASSERT(arena);
	*state = (Build_State){0};
	state->arena = arena;
	platform_init_mutex(&state->mutex);
	state->initialized = true;
	return true;
}

void build_state_destroy(Build_State *state)
{
	ASSERT(state);
	ASSERT(state->initialized);
	platform_destroy_mutex(&state->mutex);
	state->initialized = false;
}

void build_state_clear(Build_State *state)
{
	ASSERT(state);
	ASSERT(state->initialized);
	platform_lock_mutex(&state->mutex);
	build_state_replace_unlocked(state, &(Build_State){0});
	platform_unlock_mutex(&state->mutex);
}

b32 build_state_get(Build_State *state, Bob_Path output, Build_State_Task *result)
{
	ASSERT(state);
	ASSERT(state->initialized);
	ASSERT(result);
	platform_lock_mutex(&state->mutex);
	const Build_State_Task *task = build_state_find_unlocked(state, output);
	*result = task ? *task : (Build_State_Task){0};
	platform_unlock_mutex(&state->mutex);
	return task != NULL;
}

b32 build_state_set(Build_State *state, Bob_Path output, Bob_Path_Array dependencies, Bob_Fingerprint fingerprint)
{
	ASSERT(state);
	ASSERT(state->initialized);
	platform_lock_mutex(&state->mutex);
	b32 result = build_state_set_unlocked(state, output, dependencies, fingerprint);
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
