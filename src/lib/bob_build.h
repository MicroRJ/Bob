#ifndef BOB_BUILD_H
#define BOB_BUILD_H

#include "bob.h"

typedef struct Bob_Task
{
	String       name;
	String       command_line;
	String_Array inputs;
	String_Array outputs;
	String_Array include_directories;
	b32          transparent;
}
Bob_Task;

typedef struct Bob_Build_Options
{
	u32 worker_count;
	b32 explain;
}
Bob_Build_Options;

b32 bob_build(Bob *bob, Bob_Build_Options options);
Bob_Error bob_add_task(Bob *bob, Bob_Task task, Bob_Node **node_out);
Bob_Error bob_set_task(Bob *bob, Bob_Node *node, Bob_Task task);
u32 bob_task_count(const Bob *bob);
const char *bob_task_name(const Bob_Node *node);
Bob_Task_State bob_task_state(const Bob_Node *node);
const Bob_Task *bob_get_task(const Bob_Node *node);

#endif
