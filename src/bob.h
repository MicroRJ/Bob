#ifndef BOB_H
#define BOB_H

#include "base.h"
#include "platform\platform.h"

typedef struct Platform_Mutex Platform_Mutex;
typedef struct Platform_Condition Platform_Condition;
typedef struct Bob Bob;
typedef struct Bob_Node Bob_Node;

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

typedef enum Bob_Task_State
{
   BOB_TASK_PENDING,
   BOB_TASK_READY,
   BOB_TASK_RUNNING,
   BOB_TASK_SUCCEEDED,
   BOB_TASK_FAILED,
   BOB_TASK_BLOCKED,
}
Bob_Task_State;

typedef struct Bob_Task
{
   String       name;
   String       command_line;
   String_Array inputs;
   String_Array outputs;
   String_Array include_directories;
}
Bob_Task;

typedef struct Bob_Node_Array
{
   Bob_Node **items;
   u32        count;
   u32        capacity;
}
Bob_Node_Array;

struct Bob_Node
{
   Bob_Node_Array dependencies;
   Bob_Node_Array dependents;
   u32            unfinished_dependencies;
   Bob_Task       task;
   Bob_Task_State state;
   b32            rebuilt;
};

typedef struct Bob_Worker
{
	Bob                    *bob;
	Platform_Thread        *thread;
	Bob_Node               *node;
	String                  command_line;
	Arena                   output;
	Platform_Process_Result process;
	b32                     awaiting_acknowledgement;
}
Bob_Worker;

struct Bob
{
   Arena               arena;
   Bob_Node          **nodes;
   u32                 node_count;
   u32                 node_capacity;
   Bob_Node          **ready;
   u32                 ready_count;
   u32                 ready_head;
   u32                 terminal_count;
   b32                 prepared;
   b32                 failed;
   Bob_Node          **work;
   u32                 work_count;
   Bob_Worker        **completions;
   u32                 completion_count;
   Platform_Mutex     *mutex;
   Platform_Condition *work_available;
   Platform_Condition *completion_available;
   b32                 stopping;
};

typedef struct Bob_Options
{
   u32 worker_count;
   i32 verbosity;
   b32 has_worker_count;
   b32 has_verbosity;
}
Bob_Options;

typedef struct Bob_Build
{
   Bob          *bob;
   Bob_Options   options;
   char          error[256];
}
Bob_Build;

Bob *bob_create(void);
void bob_destroy(Bob *bob);
Bob_Error bob_prepare(Bob *bob);
b32 bob_build(Bob *bob, u32 worker_count);

b32 bob_take_ready(Bob *bob, Bob_Node **node_out);
Bob_Error bob_add_task(Bob *bob, Bob_Task task, Bob_Node **node_out);
Bob_Error bob_set_task(Bob *bob, Bob_Node *node, Bob_Task task);
Bob_Error bob_add_dependency(Bob *bob, Bob_Node *node, Bob_Node *dependency);
Bob_Error bob_complete(Bob *bob, Bob_Node *node, b32 succeeded);
b32 bob_is_finished(const Bob *bob);
b32 bob_has_failed(const Bob *bob);
u32 bob_task_count(const Bob *bob);
Bob_Node *bob_node_at(const Bob *bob, u32 index);
const char *bob_task_name(const Bob_Node *node);
Bob_Task_State bob_task_state(const Bob_Node *node);
const Bob_Task *bob_get_task(const Bob_Node *node);
u32 bob_dependency_count(const Bob_Node *node);
Bob_Node *bob_dependency(const Bob_Node *node, u32 index);
const char *bob_error_string(Bob_Error result);

#endif
