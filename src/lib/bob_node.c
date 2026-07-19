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
   result->transparent = source.transparent;
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

static b32 node_array_push(Bob *bob, Bob_Node_Array *array, Bob_Node *node)
{
   if (!reserve(bob, (void **)&array->items, sizeof(*array->items), array->count,
   &array->capacity, array->count + 1, _Alignof(Bob_Node *))) {
      return false;
   }
   array->items[array->count++] = node;
   return true;
}

static b32 valid_node(const Bob *bob, const Bob_Node *node)
{
   u32 i;
   if (!bob || !node) return false;
   for (i = 0; i < bob->node_count; ++i) {
      if (bob->nodes[i] == node) return true;
   }
   return false;
}

static b32 enqueue_ready(Bob *bob, Bob_Node *node)
{
   /* A node is enqueued at most once, so node_count slots are sufficient. */
   bob->ready[bob->ready_count++] = node;
   node->state = BOB_TASK_READY;
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

Bob_Error bob_add_task(Bob *bob, Bob_Task task, Bob_Node **node_out)
{
   Bob_Node *node;

   if (!bob || !task.name.data || !node_out) {
      return BOB_ERROR_INVALID_TASK;
   }
   if (bob->prepared) {
      return BOB_ERROR_ALREADY_PREPARED;
   }
   if (!reserve(bob, (void **)&bob->nodes, sizeof(*bob->nodes),
   bob->node_count, &bob->node_capacity, bob->node_count + 1,
   _Alignof(Bob_Node *))) {
      return BOB_ERROR_OUT_OF_MEMORY;
   }

   node = bob_push(bob, sizeof(*node), _Alignof(Bob_Node));
   if (!node) return BOB_ERROR_OUT_OF_MEMORY;
   if (!copy_task(&bob->arena, task, &node->task)) return BOB_ERROR_OUT_OF_MEMORY;
   node->state = BOB_TASK_PENDING;

   bob->nodes[bob->node_count] = node;
   *node_out = node;
   ++bob->node_count;
   return BOB_OK;
}

Bob_Error bob_add_dependency(Bob *bob, Bob_Node *node, Bob_Node *dependency)
{
   u32 i;

   if (!valid_node(bob, node) || !valid_node(bob, dependency)) {
      return BOB_ERROR_INVALID_TASK;
   }
   if (bob->prepared) {
      return BOB_ERROR_ALREADY_PREPARED;
   }
   if (node == dependency) {
      return BOB_ERROR_SELF_DEPENDENCY;
   }

   for (i = 0; i < node->dependencies.count; ++i)
   {
      if (node->dependencies.items[i] == dependency) {
         return BOB_ERROR_DUPLICATE_DEPENDENCY;
      }
   }

   if (!node_array_push(bob, &node->dependencies, dependency)) {
      return BOB_ERROR_OUT_OF_MEMORY;
   }
   if (!node_array_push(bob, &dependency->dependents, node)) {
      --node->dependencies.count;
      return BOB_ERROR_OUT_OF_MEMORY;
   }
   return BOB_OK;
}

static Bob_Error validate_acyclic(Bob *bob)
{
   u64 arena_start;
   Bob_Node **queue;
   u32 head = 0;
   u32 count = 0;
   u32 visited = 0;
   u32 i;

   if (bob->node_count == 0) {
      return BOB_OK;
   }

   arena_start = arena_mark(&bob->arena);
   queue = bob_push(bob, bob->node_count * sizeof(*queue), _Alignof(Bob_Node *));
   if (!queue) {
      arena_restore(&bob->arena, arena_start);
      return BOB_ERROR_OUT_OF_MEMORY;
   }

   for (i = 0; i < bob->node_count; ++i)
   {
      Bob_Node *node = bob->nodes[i];
      node->unfinished_dependencies = node->dependencies.count;
      if (node->unfinished_dependencies == 0) {
         queue[count++] = node;
      }
   }

   while (head < count)
   {
      Bob_Node *node = queue[head++];
      const Bob_Node_Array *dependents = &node->dependents;
      ++visited;

      for (i = 0; i < dependents->count; ++i)
      {
         Bob_Node *dependent = dependents->items[i];
         --dependent->unfinished_dependencies;
         if (dependent->unfinished_dependencies == 0) {
            queue[count++] = dependent;
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
      bob->ready = bob_push(bob, bob->node_count * sizeof(*bob->ready), _Alignof(Bob_Node *));
      if (!bob->ready) {
         return BOB_ERROR_OUT_OF_MEMORY;
      }
   }

   bob->prepared = true;
   for (i = 0; i < bob->node_count; ++i)
   {
      Bob_Node *node = bob->nodes[i];
      node->unfinished_dependencies = node->dependencies.count;
      if (node->unfinished_dependencies == 0) {
         enqueue_ready(bob, node);
      }
   }
   return BOB_OK;
}

b32 bob_take_ready(Bob *bob, Bob_Node **node_out)
{
   Bob_Node *node;

   if (!bob || !bob->prepared || !node_out || bob->ready_head == bob->ready_count) {
      return false;
   }

   node = bob->ready[bob->ready_head++];
   node->state = BOB_TASK_RUNNING;
   *node_out = node;
   return true;
}

static void block_node_and_dependents(Bob *bob, Bob_Node *node)
{
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

Bob_Error bob_complete(Bob *bob, Bob_Node *node, b32 succeeded)
{
   u32 i;

   if (!bob || !bob->prepared) {
      return BOB_ERROR_NOT_PREPARED;
   }
   if (!valid_node(bob, node)) {
      return BOB_ERROR_INVALID_TASK;
   }

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
      Bob_Node *dependent = node->dependents.items[i];

      if (dependent->state != BOB_TASK_PENDING) {
         continue;
      }
      --dependent->unfinished_dependencies;
      if (dependent->unfinished_dependencies == 0) {
         enqueue_ready(bob, dependent);
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

Bob_Node *bob_node_at(const Bob *bob, u32 index) {
   return bob && index < bob->node_count ? bob->nodes[index] : NULL;
}

const char *bob_task_name(const Bob_Node *node) {
   return node ? node->task.name.data : NULL;
}

Bob_Task_State bob_task_state(const Bob_Node *node) {
   return node ? node->state : BOB_TASK_BLOCKED;
}

const Bob_Task *bob_get_task(const Bob_Node *node) {
   return node ? &node->task : NULL;
}

Bob_Error bob_set_task(Bob *bob, Bob_Node *node, Bob_Task task)
{
   if (!valid_node(bob, node)) return BOB_ERROR_INVALID_TASK;
   if (!task.name.data) task.name = node->task.name;
   if (!copy_task(&bob->arena, task, &node->task)) return BOB_ERROR_OUT_OF_MEMORY;
   return BOB_OK;
}

u32 bob_dependency_count(const Bob_Node *node) {
   return node ? node->dependencies.count : 0;
}

Bob_Node *bob_dependency(const Bob_Node *node, u32 index) {
   if (!node || index >= node->dependencies.count) return NULL;
   return node->dependencies.items[index];
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
