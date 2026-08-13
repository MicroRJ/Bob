#ifndef BUILD_STATE_INTERNAL_H
#define BUILD_STATE_INTERNAL_H

#include "build_state.h"

Build_State_Path_Id build_state_path_id(const Build_State *state, Bob_Path path);
Bob_Path build_state_path(const Build_State *state, Build_State_Path_Id id);
b32 build_state_add_replayed_path(Arena *arena, Build_State *state, Bob_Path path);
b32 build_state_reserve_tasks(Arena *arena, Build_State *state, u32 needed);
u32 build_state_task_index(const Build_State *state, Bob_Path output);
const Build_State_Task *build_state_find_unlocked(const Build_State *state, Bob_Path output);
b32 build_state_set_unlocked(Arena *arena, Build_State *state, Bob_Path output, Bob_Path_Array dependencies, Bob_Fingerprint fingerprint);
b32 build_state_remove_unlocked(Build_State *state, Bob_Path output);
void build_state_replace_unlocked(Build_State *state, const Build_State *replacement);

#endif
