#include "bob.h"

#include <stdlib.h>
#include <string.h>

static void *bob_push(Bob *bob, u64 size, u64 alignment) {
   return arena_push_zero_aligned(&bob->arena, size, alignment);
}

static b32 copy_string_array(Arena *arena, String_Array source, String_Array *result)
{
   u32 i;
   *result = (String_Array){0};
   if (!source.count) return true;
   result->items = arena_push_zero_aligned(arena, source.count * sizeof(*result->items), _Alignof(String));
   if (!result->items) return false;
   result->count = source.count;
   for (i = 0; i < source.count; ++i) {
      result->items[i] = arena_push_string_copy(arena, source.items[i]);
      if (!result->items[i].data) return false;
   }
   return true;
}

static b32 copy_task(Arena *arena, Bob_Task source, Bob_Task *result)
{
   u64 mark = arena_mark(arena);
   *result = (Bob_Task){0};
   result->name = arena_push_string_copy(arena, source.name);
   result->command_line = arena_push_string_copy(arena, source.command_line);
   if (!result->name.data || !result->command_line.data || !copy_string_array(arena, source.inputs, &result->inputs) || !copy_string_array(arena, source.outputs, &result->outputs) || !copy_string_array(arena, source.include_directories, &result->include_directories)) {
      arena_restore(arena, mark);
      *result = (Bob_Task){0};
      return false;
   }
   return true;
}

static b32 reserve(Bob *bob, void **memory, u32 element_size, u32 count, u32 *capacity, u32 needed, u64 alignment)
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

   new_memory = bob_push(bob, new_capacity * element_size, alignment);
   if (!new_memory) {
      return false;
   }
   if (*memory && count) { memcpy(new_memory, *memory, count * element_size); }

   *memory = new_memory;
   *capacity = new_capacity;
   return true;
}

static b32 node_array_push(Bob *bob, Bob_Id_Array *array, Node_Id node)
{
   if (!reserve(bob, (void **)&array->items, sizeof(*array->items), array->count,
   &array->capacity, array->count + 1, _Alignof(Node_Id))) {
      return false;
   }
   array->items[array->count++] = node;
   return true;
}

static b32 valid_node(const Bob *bob, Node_Id node) {
   return bob && (u32)node < bob->node_count;
}

static b32 enqueue_ready(Bob *bob, Node_Id node)
{
   /* A node is enqueued at most once, so node_count slots are sufficient. */
   bob->ready[bob->ready_count++] = node;
   bob->nodes[node].state = BOB_TASK_READY;
   return true;
}

Bob *bob_create(void)
{
   Bob *bob = calloc(1, sizeof(*bob));
   if (!bob) return NULL;

   bob->arena = arena_create(0);
   if (!bob->arena.data) {
      free(bob);
      return NULL;
   }
   return bob;
}

void bob_destroy(Bob *bob)
{
   if (!bob) { return; }
   arena_destroy(&bob->arena);
   free(bob);
}

Bob_Error bob_add_task(Bob *bob, Bob_Task task, Node_Id *node_out)
{
   Bob_Node *node;

   if (!bob || !task.name.data || !node_out) {
      return BOB_ERROR_INVALID_TASK;
   }
   if (bob->prepared) {
      return BOB_ERROR_ALREADY_PREPARED;
   }
   if (bob->node_count >= UINT32_MAX) {
      return BOB_ERROR_OUT_OF_MEMORY;
   }
   if (!reserve(bob, (void **)&bob->nodes, sizeof(*bob->nodes),
   bob->node_count, &bob->node_capacity, bob->node_count + 1,
   _Alignof(Bob_Node))) {
      return BOB_ERROR_OUT_OF_MEMORY;
   }

   node = &bob->nodes[bob->node_count];
   memset(node, 0, sizeof(*node));
   if (!copy_task(&bob->arena, task, &node->task)) return BOB_ERROR_OUT_OF_MEMORY;
   node->state = BOB_TASK_PENDING;

   *node_out = (Node_Id)bob->node_count;
   ++bob->node_count;
   return BOB_OK;
}

Bob_Error bob_add_dependency(Bob *bob, Node_Id node_id, Node_Id dependency_id)
{
   Bob_Node *node;
   Bob_Node *dependency;
   u32 i;

   if (!valid_node(bob, node_id) || !valid_node(bob, dependency_id)) {
      return BOB_ERROR_INVALID_TASK;
   }
   if (bob->prepared) {
      return BOB_ERROR_ALREADY_PREPARED;
   }
   if (node_id == dependency_id) {
      return BOB_ERROR_SELF_DEPENDENCY;
   }

   node = &bob->nodes[node_id];
   dependency = &bob->nodes[dependency_id];
   for (i = 0; i < node->dependencies.count; ++i)
   {
      if (node->dependencies.items[i] == dependency_id) {
         return BOB_ERROR_DUPLICATE_DEPENDENCY;
      }
   }

   if (!node_array_push(bob, &node->dependencies, dependency_id)) {
      return BOB_ERROR_OUT_OF_MEMORY;
   }
   if (!node_array_push(bob, &dependency->dependents, node_id)) {
      --node->dependencies.count;
      return BOB_ERROR_OUT_OF_MEMORY;
   }
   return BOB_OK;
}

static Bob_Error validate_acyclic(Bob *bob)
{
   u64 arena_start;
   u32 *remaining;
   Node_Id *queue;
   u32 head = 0;
   u32 count = 0;
   u32 visited = 0;
   u32 i;

   if (bob->node_count == 0) {
      return BOB_OK;
   }

   arena_start = arena_mark(&bob->arena);
   remaining = bob_push(bob, bob->node_count * sizeof(*remaining), _Alignof(u32));
   queue = bob_push(bob, bob->node_count * sizeof(*queue), _Alignof(Node_Id));
   if (!remaining || !queue) {
      arena_restore(&bob->arena, arena_start);
      return BOB_ERROR_OUT_OF_MEMORY;
   }

   for (i = 0; i < bob->node_count; ++i)
   {
      remaining[i] = bob->nodes[i].dependencies.count;
      if (remaining[i] == 0) {
         queue[count++] = (Node_Id)i;
      }
   }

   while (head < count)
   {
      Node_Id node_id = queue[head++];
      const Bob_Id_Array *dependents = &bob->nodes[node_id].dependents;
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

   arena_restore(&bob->arena, arena_start);
   return visited == bob->node_count ? BOB_OK : BOB_ERROR_CYCLE;
}

Bob_Error bob_prepare(Bob *bob)
{
   Bob_Error result;
   u32 i;

   if (!bob) {
      return BOB_ERROR_INVALID_TASK;
   }
   if (bob->prepared) {
      return BOB_ERROR_ALREADY_PREPARED;
   }

   result = validate_acyclic(bob);
   if (result != BOB_OK) {
      return result;
   }

   if (bob->node_count > 0)
   {
      bob->ready = bob_push(bob, bob->node_count * sizeof(*bob->ready), _Alignof(Node_Id));
      if (!bob->ready) {
         return BOB_ERROR_OUT_OF_MEMORY;
      }
   }

   bob->prepared = true;
   for (i = 0; i < bob->node_count; ++i)
   {
      Bob_Node *node = &bob->nodes[i];
      node->unfinished_dependencies = node->dependencies.count;
      if (node->unfinished_dependencies == 0) {
         enqueue_ready(bob, (Node_Id)i);
      }
   }
   return BOB_OK;
}

b32 bob_take_ready(Bob *bob, Node_Id *node_out)
{
   Node_Id node;

   if (!bob || !bob->prepared || !node_out || bob->ready_head == bob->ready_count) {
      return false;
   }

   node = bob->ready[bob->ready_head++];
   bob->nodes[node].state = BOB_TASK_RUNNING;
   *node_out = node;
   return true;
}

static void block_node_and_dependents(Bob *bob, Node_Id node_id)
{
   Bob_Node *node = &bob->nodes[node_id];
   u32 i;

   if (node->state == BOB_TASK_BLOCKED || node->state == BOB_TASK_SUCCEEDED ||
   node->state == BOB_TASK_FAILED) {
      return;
   }

   node->state = BOB_TASK_BLOCKED;
   ++bob->terminal_count;
   for (i = 0; i < node->dependents.count; ++i) {
      block_node_and_dependents(bob, node->dependents.items[i]);
   }
}

Bob_Error bob_complete(Bob *bob, Node_Id node_id, b32 succeeded)
{
   Bob_Node *node;
   u32 i;

   if (!bob || !bob->prepared) {
      return BOB_ERROR_NOT_PREPARED;
   }
   if (!valid_node(bob, node_id)) {
      return BOB_ERROR_INVALID_TASK;
   }

   node = &bob->nodes[node_id];
   if (node->state != BOB_TASK_RUNNING) {
      return BOB_ERROR_INVALID_STATE;
   }

   node->state = succeeded ? BOB_TASK_SUCCEEDED : BOB_TASK_FAILED;
   ++bob->terminal_count;

   if (!succeeded)
   {
      bob->failed = true;
      for (i = 0; i < node->dependents.count; ++i) {
         block_node_and_dependents(bob, node->dependents.items[i]);
      }
      return BOB_OK;
   }

   for (i = 0; i < node->dependents.count; ++i)
   {
      Node_Id dependent_id = node->dependents.items[i];
      Bob_Node *dependent = &bob->nodes[dependent_id];

      if (dependent->state != BOB_TASK_PENDING) {
         continue;
      }
      --dependent->unfinished_dependencies;
      if (dependent->unfinished_dependencies == 0) {
         enqueue_ready(bob, dependent_id);
      }
   }
   return BOB_OK;
}

b32 bob_is_finished(const Bob *bob) {
   return bob && bob->prepared && bob->terminal_count == bob->node_count;
}

b32 bob_has_failed(const Bob *bob) {
   return bob && bob->failed;
}

u32 bob_task_count(const Bob *bob) {
   return bob ? bob->node_count : 0;
}

const char *bob_task_name(const Bob *bob, Node_Id node) {
   return valid_node(bob, node) ? bob->nodes[node].task.name.data : NULL;
}

Bob_Task_State bob_task_state(const Bob *bob, Node_Id node) {
   return valid_node(bob, node) ? bob->nodes[node].state : BOB_TASK_BLOCKED;
}

const Bob_Task *bob_get_task(const Bob *bob, Node_Id node) {
   return valid_node(bob, node) ? &bob->nodes[node].task : NULL;
}

Bob_Error bob_set_task(Bob *bob, Node_Id node, Bob_Task task)
{
   if (!valid_node(bob, node)) return BOB_ERROR_INVALID_TASK;
   if (!task.name.data) task.name = bob->nodes[node].task.name;
   if (!copy_task(&bob->arena, task, &bob->nodes[node].task)) return BOB_ERROR_OUT_OF_MEMORY;
   return BOB_OK;
}

u32 bob_dependency_count(const Bob *bob, Node_Id node) {
   return valid_node(bob, node) ? bob->nodes[node].dependencies.count : 0;
}

Node_Id bob_dependency(const Bob *bob, Node_Id node, u32 index) {
   if (!valid_node(bob, node) || index >= bob->nodes[node].dependencies.count) return BOB_INVALID_TASK;
   return bob->nodes[node].dependencies.items[index];
}

const char *bob_error_string(Bob_Error result)
{
   switch (result)
   {
      case BOB_OK: return "ok";
      case BOB_ERROR_OUT_OF_MEMORY: return "out of memory";
      case BOB_ERROR_INVALID_TASK: return "invalid node";
      case BOB_ERROR_DUPLICATE_DEPENDENCY: return "duplicate dependency";
      case BOB_ERROR_SELF_DEPENDENCY: return "self dependency";
      case BOB_ERROR_ALREADY_PREPARED: return "Bob already prepared";
      case BOB_ERROR_NOT_PREPARED: return "Bob not prepared";
      case BOB_ERROR_INVALID_STATE: return "invalid node state";
      case BOB_ERROR_CYCLE: return "dependency cycle";
   }
   return "unknown Bob result";
}
