#ifndef BOB_H
#define BOB_H

#include "base.h"

typedef u32 Node_Id;

typedef struct Platform_Mutex Platform_Mutex;
typedef struct Platform_Condition Platform_Condition;

#define BOB_INVALID_TASK UINT32_MAX

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

typedef struct Bob_Id_Array
{
   Node_Id  *items;
   u32       count;
   u32       capacity;
}
Bob_Id_Array;

typedef struct Bob_Node
{
   Bob_Id_Array   dependencies;
   Bob_Id_Array   dependents;
   u32            unfinished_dependencies;
   Bob_Task       task;
   Bob_Task_State state;
}
Bob_Node;

typedef struct Bob_Task_Runtime
{
   b32 rebuilt;
}
Bob_Task_Runtime;

typedef struct Bob
{
   Arena     arena;
   Bob_Node *nodes;
   u32       node_count;
   u32       node_capacity;
   Node_Id  *ready;
   u32       ready_count;
   u32       ready_head;
   u32       terminal_count;
   b32       prepared;
   b32       failed;
   Bob_Task_Runtime *runtime;
   Node_Id          *work;
   u32               work_count;
   u32              *completions;
   u32               completion_count;
   Platform_Mutex   *mutex;
   Platform_Condition *work_available;
   Platform_Condition *completion_available;
   b32               stopping;
}
Bob;

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

b32 bob_take_ready(Bob *bob, Node_Id *node_out);
Bob_Error bob_add_task(Bob *bob, Bob_Task task, Node_Id *node_out);
Bob_Error bob_set_task(Bob *bob, Node_Id node, Bob_Task task);
Bob_Error bob_add_dependency(Bob *bob, Node_Id node, Node_Id dependency);
Bob_Error bob_complete(Bob *bob, Node_Id node, b32 succeeded);
b32 bob_is_finished(const Bob *bob);
b32 bob_has_failed(const Bob *bob);
u32 bob_task_count(const Bob *bob);
const char *bob_task_name(const Bob *bob, Node_Id node);
Bob_Task_State bob_task_state(const Bob *bob, Node_Id node);
const Bob_Task *bob_get_task(const Bob *bob, Node_Id node);
u32 bob_dependency_count(const Bob *bob, Node_Id node);
Node_Id bob_dependency(const Bob *bob, Node_Id node, u32 index);
const char *bob_error_string(Bob_Error result);

#endif
