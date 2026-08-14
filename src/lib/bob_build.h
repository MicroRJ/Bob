#ifndef BOB_BUILD_H
#define BOB_BUILD_H

#include "bob.h"

#define BOB_FINGERPRINT_SIZE 32

typedef struct Bob_Build Bob_Build;

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
	u32                 worker_count;
	b32                 explain;
	/* Events are delivered on the thread calling bob_build. */
	void               *user_data;
	Bob_Event_Function *event;
}
Bob_Build_Params;

Bob_Build *bob_build_create(void);
Bob_Build *bob_build_create_at(String root);
void bob_build_destroy(Bob_Build *build);
Bob *bob_build_graph(Bob_Build *build);
const Bob *bob_build_graph_const(const Bob_Build *build);

b32 bob_build(Bob_Build *build, Bob_Build_Params options);

Bob_Error bob_add_task(Bob_Build *build, Bob_Task_Desc task, Bob_Node **node_out);
Bob_Error bob_set_task(Bob_Build *build, Bob_Node *node, Bob_Task_Desc task);

u32 bob_task_count(const Bob_Build *build);
const char *bob_task_name(const Bob_Node *node);
Bob_Node_Status bob_task_state(const Bob_Node *node);

#endif
