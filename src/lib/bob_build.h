#ifndef BOB_BUILD_H
#define BOB_BUILD_H

#include "bob.h"

#define BOB_FINGERPRINT_SIZE 32

typedef struct Bob_Fingerprint
{
	u8 bytes[BOB_FINGERPRINT_SIZE];
}
Bob_Fingerprint;

// User fed task descriptor; the internal runtime representation is normalized.
typedef struct Bob_Task_Desc
{
	String       name;
	String       command_line;
	String       working_directory;
	String_Array inputs;
	String_Array outputs;
	String_Array include_directories;
	b32          transparent;
}
Bob_Task_Desc;

typedef struct Bob_Build_Params
{
	u32 worker_count;
	b32 explain;
}
Bob_Build_Params;

b32 bob_build(Bob *bob, Bob_Build_Params options);

Bob_Error bob_add_task(Bob *bob, Bob_Task_Desc task, Bob_Node **node_out);
Bob_Error bob_set_task(Bob *bob, Bob_Node *node, Bob_Task_Desc task);

u32 bob_task_count(const Bob *bob);
const char *bob_task_name(const Bob_Node *node);
Bob_Node_Status bob_task_state(const Bob_Node *node);

const Bob_Task_Desc *bob_get_task_desc(const Bob_Node *node);

#endif
