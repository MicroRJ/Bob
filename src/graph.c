#include "graph.h"

#include <stdlib.h>
#include <string.h>

typedef struct Node_Id_Array
{
   Node_Id *items;
   u32 count;
   u32 capacity;
} Node_Id_Array;

typedef struct Node
{
   char *name;
   const void *data;
   Node_Id_Array dependencies;
   Node_Id_Array dependents;
   u32 unfinished_dependencies;
   Task_State state;
} Node;

// NOTE(RJ) we just want another arena for the nodes!
struct Graph
{
   Arena arena;
   Node *nodes;
   u32 node_count;
   u32 node_capacity;

   Node_Id *ready;
   u32 ready_count;
   u32 ready_head;

   u32 terminal_count;
   b32 prepared;
   b32 failed;
};

static void *graph_push(Graph *graph, u64 size, u64 alignment) {
   return arena_push_zero_aligned(&graph->arena, size, alignment);
}

static b32 reserve(Graph *graph, void **memory, u32 element_size, u32 count, u32 *capacity, u32 needed, u64 alignment)
{
   size_t new_capacity;
   void *new_memory;

   if (*capacity >= needed) {
      return true;
   }

   new_capacity = *capacity ? *capacity : 8;
   while (new_capacity < needed)
   {
      if (new_capacity > SIZE_MAX / 2) {
         return false;
      }
      new_capacity *= 2;
   }

   if (new_capacity > SIZE_MAX / element_size) {
      return false;
   }

   new_memory = graph_push(graph, new_capacity * element_size, alignment);
   if (!new_memory) {
      return false;
   }
   if (*memory && count) { memcpy(new_memory, *memory, count * element_size); }

   *memory = new_memory;
   *capacity = new_capacity;
   return true;
}

static b32 node_array_push(Graph *graph, Node_Id_Array *array, Node_Id node)
{
   if (!reserve(graph, (void **)&array->items, sizeof(*array->items), array->count,
   &array->capacity, array->count + 1, _Alignof(Node_Id))) {
      return false;
   }
   array->items[array->count++] = node;
   return true;
}

static b32 valid_node(const Graph *graph, Node_Id node) {
   return graph && (u32)node < graph->node_count;
}

static b32 enqueue_ready(Graph *graph, Node_Id node)
{
   /* A node is enqueued at most once, so node_count slots are sufficient. */
   graph->ready[graph->ready_count++] = node;
   graph->nodes[node].state = GRAPH_TASK_READY;
   return true;
}

Graph *graph_create(void)
{
   Graph *graph = calloc(1, sizeof(*graph));
   if (!graph) return NULL;

   graph->arena = arena_create(0);
   if (!graph->arena.data) {
      free(graph);
      return NULL;
   }
   return graph;
}

void graph_destroy(Graph *graph)
{
   if (!graph) { return; }
   arena_destroy(&graph->arena);
   free(graph);
}

Graph_Error graph_add_node(Graph *graph, const char *name, const void *data, Node_Id *node_out)
{
   Node *node;
   u32 name_length;

   if (!graph || !name || !node_out) {
      return GRAPH_ERROR_INVALID_TASK;
   }
   if (graph->prepared) {
      return GRAPH_ERROR_ALREADY_PREPARED;
   }
   if (graph->node_count >= UINT32_MAX) {
      return GRAPH_ERROR_OUT_OF_MEMORY;
   }
   if (!reserve(graph, (void **)&graph->nodes, sizeof(*graph->nodes),
   graph->node_count, &graph->node_capacity, graph->node_count + 1,
   _Alignof(Node))) {
      return GRAPH_ERROR_OUT_OF_MEMORY;
   }

   node = &graph->nodes[graph->node_count];
   memset(node, 0, sizeof(*node));

   name_length = strlen(name);
   node->name = arena_push_copy(&graph->arena, name_length + 1, name);
   if (!node->name) {
      return GRAPH_ERROR_OUT_OF_MEMORY;
   }
   memcpy(node->name, name, name_length + 1);
   node->data = data;
   node->state = GRAPH_TASK_PENDING;

   *node_out = (Node_Id)graph->node_count;
   ++graph->node_count;
   return GRAPH_OK;
}

Graph_Error graph_add_dependency(Graph *graph, Node_Id node_id, Node_Id dependency_id)
{
   Node *node;
   Node *dependency;
   u32 i;

   if (!valid_node(graph, node_id) || !valid_node(graph, dependency_id)) {
      return GRAPH_ERROR_INVALID_TASK;
   }
   if (graph->prepared) {
      return GRAPH_ERROR_ALREADY_PREPARED;
   }
   if (node_id == dependency_id) {
      return GRAPH_ERROR_SELF_DEPENDENCY;
   }

   node = &graph->nodes[node_id];
   dependency = &graph->nodes[dependency_id];
   for (i = 0; i < node->dependencies.count; ++i)
   {
      if (node->dependencies.items[i] == dependency_id) {
         return GRAPH_ERROR_DUPLICATE_DEPENDENCY;
      }
   }

   if (!node_array_push(graph, &node->dependencies, dependency_id)) {
      return GRAPH_ERROR_OUT_OF_MEMORY;
   }
   if (!node_array_push(graph, &dependency->dependents, node_id)) {
      --node->dependencies.count;
      return GRAPH_ERROR_OUT_OF_MEMORY;
   }
   return GRAPH_OK;
}

static Graph_Error validate_acyclic(Graph *graph)
{
   u64 arena_start;
   u32 *remaining;
   Node_Id *queue;
   u32 head = 0;
   u32 count = 0;
   u32 visited = 0;
   u32 i;

   if (graph->node_count == 0) {
      return GRAPH_OK;
   }

   arena_start = arena_mark(&graph->arena);
   remaining = graph_push(graph, graph->node_count * sizeof(*remaining), _Alignof(u32));
   queue = graph_push(graph, graph->node_count * sizeof(*queue), _Alignof(Node_Id));
   if (!remaining || !queue) {
      arena_restore(&graph->arena, arena_start);
      return GRAPH_ERROR_OUT_OF_MEMORY;
   }

   for (i = 0; i < graph->node_count; ++i)
   {
      remaining[i] = graph->nodes[i].dependencies.count;
      if (remaining[i] == 0) {
         queue[count++] = (Node_Id)i;
      }
   }

   while (head < count)
   {
      Node_Id node_id = queue[head++];
      const Node_Id_Array *dependents = &graph->nodes[node_id].dependents;
      ++visited;

      for (i = 0; i < dependents->count; ++i)
      {
         Node_Id dependent_id = dependents->items[i];
         --remaining[dependent_id];
         if (remaining[dependent_id] == 0) {
            queue[count++] = dependent_id;
         }
      }
   }

   arena_restore(&graph->arena, arena_start);
   return visited == graph->node_count ? GRAPH_OK : GRAPH_ERROR_CYCLE;
}

Graph_Error graph_prepare(Graph *graph)
{
   Graph_Error result;
   u32 i;

   if (!graph) {
      return GRAPH_ERROR_INVALID_TASK;
   }
   if (graph->prepared) {
      return GRAPH_ERROR_ALREADY_PREPARED;
   }

   result = validate_acyclic(graph);
   if (result != GRAPH_OK) {
      return result;
   }

   if (graph->node_count > 0)
   {
      graph->ready = graph_push(graph, graph->node_count * sizeof(*graph->ready), _Alignof(Node_Id));
      if (!graph->ready) {
         return GRAPH_ERROR_OUT_OF_MEMORY;
      }
   }

   graph->prepared = true;
   for (i = 0; i < graph->node_count; ++i)
   {
      Node *node = &graph->nodes[i];
      node->unfinished_dependencies = node->dependencies.count;
      if (node->unfinished_dependencies == 0) {
         enqueue_ready(graph, (Node_Id)i);
      }
   }
   return GRAPH_OK;
}

b32 graph_take_ready(Graph *graph, Node_Id *node_out)
{
   Node_Id node;

   if (!graph || !graph->prepared || !node_out || graph->ready_head == graph->ready_count) {
      return false;
   }

   node = graph->ready[graph->ready_head++];
   graph->nodes[node].state = GRAPH_TASK_RUNNING;
   *node_out = node;
   return true;
}

static void block_node_and_dependents(Graph *graph, Node_Id node_id)
{
   Node *node = &graph->nodes[node_id];
   u32 i;

   if (node->state == GRAPH_TASK_BLOCKED || node->state == GRAPH_TASK_SUCCEEDED ||
   node->state == GRAPH_TASK_FAILED) {
      return;
   }

   node->state = GRAPH_TASK_BLOCKED;
   ++graph->terminal_count;
   for (i = 0; i < node->dependents.count; ++i) {
      block_node_and_dependents(graph, node->dependents.items[i]);
   }
}

Graph_Error graph_complete(Graph *graph, Node_Id node_id, b32 succeeded)
{
   Node *node;
   u32 i;

   if (!graph || !graph->prepared) {
      return GRAPH_ERROR_NOT_PREPARED;
   }
   if (!valid_node(graph, node_id)) {
      return GRAPH_ERROR_INVALID_TASK;
   }

   node = &graph->nodes[node_id];
   if (node->state != GRAPH_TASK_RUNNING) {
      return GRAPH_ERROR_INVALID_STATE;
   }

   node->state = succeeded ? GRAPH_TASK_SUCCEEDED : GRAPH_TASK_FAILED;
   ++graph->terminal_count;

   if (!succeeded)
   {
      graph->failed = true;
      for (i = 0; i < node->dependents.count; ++i) {
         block_node_and_dependents(graph, node->dependents.items[i]);
      }
      return GRAPH_OK;
   }

   for (i = 0; i < node->dependents.count; ++i)
   {
      Node_Id dependent_id = node->dependents.items[i];
      Node *dependent = &graph->nodes[dependent_id];

      if (dependent->state != GRAPH_TASK_PENDING) {
         continue;
      }
      --dependent->unfinished_dependencies;
      if (dependent->unfinished_dependencies == 0) {
         enqueue_ready(graph, dependent_id);
      }
   }
   return GRAPH_OK;
}

b32 graph_is_finished(const Graph *graph) {
   return graph && graph->prepared && graph->terminal_count == graph->node_count;
}

b32 graph_has_failed(const Graph *graph) {
   return graph && graph->failed;
}

u32 graph_node_count(const Graph *graph) {
   return graph ? graph->node_count : 0;
}

const char *graph_node_name(const Graph *graph, Node_Id node) {
   return valid_node(graph, node) ? graph->nodes[node].name : NULL;
}

Task_State graph_node_state(const Graph *graph, Node_Id node) {
   return valid_node(graph, node) ? graph->nodes[node].state : GRAPH_TASK_BLOCKED;
}

const void *graph_node_data(const Graph *graph, Node_Id node) {
   return valid_node(graph, node) ? graph->nodes[node].data : NULL;
}

Graph_Error graph_set_node_data(Graph *graph, Node_Id node, const void *data)
{
   if (!valid_node(graph, node)) return GRAPH_ERROR_INVALID_TASK;
   graph->nodes[node].data = data;
   return GRAPH_OK;
}

u32 graph_node_dependency_count(const Graph *graph, Node_Id node) {
   return valid_node(graph, node) ? graph->nodes[node].dependencies.count : 0;
}

Node_Id graph_node_dependency(const Graph *graph, Node_Id node, u32 index) {
   if (!valid_node(graph, node) || index >= graph->nodes[node].dependencies.count) return GRAPH_INVALID_TASK;
   return graph->nodes[node].dependencies.items[index];
}

const char *graph_error_str(Graph_Error result)
{
   switch (result)
   {
      case GRAPH_OK: return "ok";
      case GRAPH_ERROR_OUT_OF_MEMORY: return "out of memory";
      case GRAPH_ERROR_INVALID_TASK: return "invalid node";
      case GRAPH_ERROR_DUPLICATE_DEPENDENCY: return "duplicate dependency";
      case GRAPH_ERROR_SELF_DEPENDENCY: return "self dependency";
      case GRAPH_ERROR_ALREADY_PREPARED: return "graph already prepared";
      case GRAPH_ERROR_NOT_PREPARED: return "graph not prepared";
      case GRAPH_ERROR_INVALID_STATE: return "invalid node state";
      case GRAPH_ERROR_CYCLE: return "dependency cycle";
   }
   return "unknown graph result";
}
