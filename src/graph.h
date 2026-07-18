#ifndef GRAPH_H
#define GRAPH_H

#include "base.h"

#include <stddef.h>

typedef u32 Node_Id;

#define GRAPH_INVALID_TASK UINT32_MAX

typedef enum Task_State {
    GRAPH_TASK_PENDING,
    GRAPH_TASK_READY,
    GRAPH_TASK_RUNNING,
    GRAPH_TASK_SUCCEEDED,
    GRAPH_TASK_FAILED,
    GRAPH_TASK_BLOCKED,
} Task_State;

typedef enum Graph_Error {
    GRAPH_OK,
    GRAPH_ERROR_OUT_OF_MEMORY,
    GRAPH_ERROR_INVALID_TASK,
    GRAPH_ERROR_DUPLICATE_DEPENDENCY,
    GRAPH_ERROR_SELF_DEPENDENCY,
    GRAPH_ERROR_ALREADY_PREPARED,
    GRAPH_ERROR_NOT_PREPARED,
    GRAPH_ERROR_INVALID_STATE,
    GRAPH_ERROR_CYCLE,
} Graph_Error;

typedef struct Graph Graph;

Graph *graph_create(void);
void graph_destroy(Graph *graph);

Graph_Error graph_add_node(Graph *graph, const char *name, const void *data,
                           Node_Id *node_out);
Graph_Error graph_add_dependency(Graph *graph, Node_Id node, Node_Id dependency);

/* Finalizes the graph and places all initially runnable nodes in the ready queue. */
Graph_Error graph_prepare(Graph *graph);

/* Takes one runnable node and marks it running. Returns false when none are ready. */
b32 graph_take_ready(Graph *graph, Node_Id *node_out);

/* Completes a running node. Failure blocks every node that transitively depends on it. */
Graph_Error graph_complete(Graph *graph, Node_Id node, b32 succeeded);

b32 graph_is_finished(const Graph *graph);
b32 graph_has_failed(const Graph *graph);

u32 graph_node_count(const Graph *graph);
const char *graph_node_name(const Graph *graph, Node_Id node);
Task_State graph_node_state(const Graph *graph, Node_Id node);
const void *graph_node_data(const Graph *graph, Node_Id node);
Graph_Error graph_set_node_data(Graph *graph, Node_Id node, const void *data);
u32 graph_node_dependency_count(const Graph *graph, Node_Id node);
Node_Id graph_node_dependency(const Graph *graph, Node_Id node, u32 index);
const char *graph_error_str(Graph_Error result);

#endif
