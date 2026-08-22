#ifndef BUILD_STATE_INTERNAL_H
#define BUILD_STATE_INTERNAL_H

#include "build_state.h"

b32 build_state_reserve_tasks(Build_State *state, u32 needed);
u32 build_state_task_index(const Build_State *state, Bob_Path output);
const Build_State_Task *build_state_find_unlocked(const Build_State *state, Bob_Path output);
b32 build_state_set_unlocked(Build_State *state, Bob_Path output, Bob_Path_Array dependencies, Bob_Fingerprint fingerprint);
b32 build_state_remove_unlocked(Build_State *state, Bob_Path output);
void build_state_replace_unlocked(Build_State *state, const Build_State *replacement);

#endif
