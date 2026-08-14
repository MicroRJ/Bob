#ifndef BOB_H
#define BOB_H

#include "base.h"

#define BOB_VERSION "0.1.0-dev"

typedef struct Bob Bob;
typedef struct Bob_Node Bob_Node;
typedef struct Bob_Node_Context Bob_Node_Context;

typedef enum Bob_Error
{
	BOB_OK,
	BOB_ERROR_OUT_OF_MEMORY,
	BOB_ERROR_INVALID_TASK,
	BOB_ERROR_DUPLICATE_DEPENDENCY,
	BOB_ERROR_SELF_DEPENDENCY,
	BOB_ERROR_ALREADY_PREPARED,
	BOB_ERROR_NOT_PREPARED,
	BOB_ERROR_INVALID_STATE,
	BOB_ERROR_CYCLE,
}
Bob_Error;

#define BOB_ERROR_INVALID_NODE BOB_ERROR_INVALID_TASK

typedef enum Bob_Node_Status
{
	BOB_NODE_PENDING,
	BOB_NODE_READY,
	BOB_NODE_RUNNING,
	BOB_NODE_SUCCEEDED,
	BOB_NODE_FAILED,
	BOB_NODE_BLOCKED,
}
Bob_Node_Status;

/* Output allocated from the node context arena remains valid until bob_destroy. */
typedef struct Bob_Node_Result
{
	void *output;
	b32   succeeded;
	b32   changed;
}
Bob_Node_Result;

typedef Bob_Node_Result Bob_Node_Function(Bob_Node_Context *context, void *user_data);

struct Bob_Node_Context
{
	Bob      *bob;
	Bob_Node *node;
	Arena    *arena;
	void     *execution_data;
};

typedef struct Bob_Node_Desc
{
	String             name;
	Bob_Node_Function *function;
	void              *user_data;
}
Bob_Node_Desc;

typedef enum Bob_Event_Type
{
	BOB_EVENT_STARTED,
	BOB_EVENT_COMPLETED,
}
Bob_Event_Type;

typedef struct Bob_Event
{
	Bob_Event_Type  type;
	Bob_Node       *node;
	/* Meaningful only for BOB_EVENT_COMPLETED. */
	Bob_Node_Result result;
}
Bob_Event;

typedef void Bob_Event_Function(Bob_Event event, void *user_data);

typedef struct Bob_Exec_Params
{
	u32                 worker_count;
	void               *user_data;
	Bob_Event_Function *event;
}
Bob_Exec_Params;

Bob *bob_create(void);
void bob_destroy(Bob *bob);
/* Graph-lifetime storage. Available only before the graph is prepared. */
void *bob_allocate(Bob *bob, u64 size, u64 alignment);
String bob_copy_string(Bob *bob, String string);
Bob_Error bob_add_node(Bob *bob, Bob_Node_Desc description, Bob_Node **node_out);
Bob_Error bob_set_node(Bob *bob, Bob_Node *node, Bob_Node_Desc description);
Bob_Error bob_set_node_action(Bob *bob, Bob_Node *node, Bob_Node_Function *function, void *user_data);
Bob_Error bob_add_dependency(Bob *bob, Bob_Node *node, Bob_Node *dependency);
Bob_Error bob_prepare(Bob *bob);
b32 bob_take_ready(Bob *bob, Bob_Node **node_out);
Bob_Error bob_complete_result(Bob *bob, Bob_Node *node, Bob_Node_Result result);
Bob_Error bob_complete(Bob *bob, Bob_Node *node, b32 succeeded);
b32 bob_execute(Bob *bob, Bob_Exec_Params options);
b32 bob_is_prepared(const Bob *bob);
b32 bob_is_finished(const Bob *bob);
b32 bob_has_failed(const Bob *bob);
u32 bob_node_count(const Bob *bob);
Bob_Node *bob_node_at(const Bob *bob, u32 index);
const char *bob_node_name(const Bob_Node *node);
Bob_Node_Status bob_node_state(const Bob_Node *node);
Bob_Node_Result bob_node_result(const Bob_Node *node);
Bob_Node_Function *bob_node_function(const Bob_Node *node);
void *bob_node_user_data(const Bob_Node *node);
u32 bob_dependency_count(const Bob_Node *node);
Bob_Node *bob_dependency(const Bob_Node *node, u32 index);
const char *bob_error_string(Bob_Error result);

#endif
