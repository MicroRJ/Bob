#include "elf_adapter.h"

#include "elf.h"

#include <stdio.h>
#include <string.h>

static elf_ValueView field(elf_State *state, elf_Table *table, const char *name) {
   return elf_get_field(state, table, name);
}

static String copy_string_value(Arena *arena, elf_ValueView value)
{
   String string;
   if (value.type != ELF_VALUE_TYPE_STRING) return (String){0};
   string = string_from_data((void *)elf_str_data(value.as.string), elf_str_size(value.as.string));
   return arena_push_string_copy(arena, string);
}

static int copy_string_array(elf_State *state, Arena *arena, elf_ValueView value, const char *field_name, String task_name, String_Array *result, char *error, size_t error_size)
{
   elf_Table *table;
   uint32_t i;

   *result = (String_Array){0};
   if (value.type == ELF_VALUE_TYPE_NIL) return 1;
   if (value.type != ELF_VALUE_TYPE_TABLE) {
      snprintf(error, error_size, "%s for '%s' must be a table", field_name, task_name.data);
      return 0;
   }

   table = value.as.table;
   result->count = elf_table_length(table);
   result->items = arena_push_zero_aligned(arena, result->count * sizeof(*result->items), _Alignof(String));
   if (result->count && !result->items) {
      snprintf(error, error_size, "out of memory");
      return 0;
   }

   for (i = 0; i < result->count; ++i)
   {
      result->items[i] = copy_string_value(arena, elf_get_index(state, table, i));
      if (!result->items[i].data) {
         snprintf(error, error_size, "%s for '%s' must contain strings", field_name, task_name.data);
         return 0;
      }
   }
   return 1;
}

typedef struct Table_List {
   Arena *arena;
   elf_Table **items;
   uint32_t count;
} Table_List;

static uint32_t table_list_find(Table_List *list, elf_Table *table)
{
   uint32_t i;
   for (i = 0; i < list->count; ++i) {
      if (list->items[i] == table) return i;
   }
   return UINT32_MAX;
}

static int table_list_add(Table_List *list, elf_Table *table)
{
   elf_Table **item;
   if (table_list_find(list, table) != UINT32_MAX) return 1;
   item = arena_push_aligned(list->arena, sizeof(*item), _Alignof(elf_Table *));
   if (!item) return 0;
   if (!list->items) list->items = item;
   *item = table;
   ++list->count;
   return 1;
}

b32 elf_load_build(const char *path, Bob_Build *result)
{
   Scratch scratch;
   elf_State *state;
   elf_ValueView returned;
   elf_ValueView targets_value;
   elf_ValueView options_value;
   elf_Table *root = NULL;
   Table_List task_tables;
   uint32_t i;
   int success = 0;

   if (!path || !result) return false;
   scratch = begin_scratch();
   task_tables = (Table_List){ .arena = scratch.arena };
   memset(result, 0, sizeof(*result));

   state = elf_create_state();
   if (!state) {
      snprintf(result->error, sizeof(result->error), "unable to create elf state");
      goto cleanup;
   }

   if (!elf_push_code_file(state, path)) {
      snprintf(result->error, sizeof(result->error), "unable to load '%s'", path);
      goto cleanup;
   }
   elf_push_nil(state);
   elf_call(state, 1, 1);

   returned = elf_peek_value(state, 0);
   if (returned.type != ELF_VALUE_TYPE_TABLE) {
      snprintf(result->error, sizeof(result->error), "script must return a table");
      goto cleanup;
   }
   root = elf_retain_table(returned.as.table);
   elf_pop_values(state, 1);
   options_value = field(state, root, "options");
   if (options_value.type != ELF_VALUE_TYPE_NIL)
   {
      elf_ValueView workers_value;
      elf_ValueView verbosity_value;
      int64_t integer;

      if (options_value.type != ELF_VALUE_TYPE_TABLE) {
         snprintf(result->error, sizeof(result->error), "returned 'options' field must be a table");
         goto cleanup;
      }
      workers_value = field(state, options_value.as.table, "workers");
      if (workers_value.type != ELF_VALUE_TYPE_NIL)
      {
         if (workers_value.type != ELF_VALUE_TYPE_INTEGER) {
            snprintf(result->error, sizeof(result->error), "options.workers must be a positive integer");
            goto cleanup;
         }
         integer = workers_value.as.integer;
         if (integer < 1 || (uint64_t)integer > UINT32_MAX) {
            snprintf(result->error, sizeof(result->error), "options.workers must be a positive integer");
            goto cleanup;
         }
         result->options.worker_count = (uint32_t)integer;
         result->options.has_worker_count = true;
      }
      verbosity_value = field(state, options_value.as.table, "verbosity");
      if (verbosity_value.type != ELF_VALUE_TYPE_NIL)
      {
         if (verbosity_value.type != ELF_VALUE_TYPE_INTEGER) {
            snprintf(result->error, sizeof(result->error), "options.verbosity must be a non-negative integer");
            goto cleanup;
         }
         integer = verbosity_value.as.integer;
         if (integer < 0 || (uint64_t)integer > INT32_MAX) {
            snprintf(result->error, sizeof(result->error), "options.verbosity must be a non-negative integer");
            goto cleanup;
         }
         result->options.verbosity = (int32_t)integer;
         result->options.has_verbosity = true;
      }
   }
   targets_value = field(state, root, "targets");
   if (targets_value.type != ELF_VALUE_TYPE_TABLE) {
      snprintf(result->error, sizeof(result->error), "returned table requires a 'targets' table");
      goto cleanup;
   }

   for (i = 0; i < elf_table_length(targets_value.as.table); ++i)
   {
      elf_ValueView target = elf_get_index(state, targets_value.as.table, i);
      if (target.type != ELF_VALUE_TYPE_TABLE) {
         snprintf(result->error, sizeof(result->error), "target %u must be a task table", i);
         goto cleanup;
      }
      if (!table_list_add(&task_tables, target.as.table)) {
         snprintf(result->error, sizeof(result->error), "out of memory");
         goto cleanup;
      }
   }

   for (i = 0; i < task_tables.count; ++i)
   {
      elf_ValueView dependencies = field(state, task_tables.items[i], "dependencies");
      uint32_t dependency;
      if (dependencies.type == ELF_VALUE_TYPE_NIL) continue;
      if (dependencies.type != ELF_VALUE_TYPE_TABLE) {
         snprintf(result->error, sizeof(result->error), "dependencies for task %u must be a table", i);
         goto cleanup;
      }
      for (dependency = 0; dependency < elf_table_length(dependencies.as.table); ++dependency)
      {
         elf_ValueView value = elf_get_index(state, dependencies.as.table, dependency);
         if (value.type != ELF_VALUE_TYPE_TABLE) {
            snprintf(result->error, sizeof(result->error), "dependencies for task %u must contain task tables", i);
            goto cleanup;
         }
         if (!table_list_add(&task_tables, value.as.table)) {
            snprintf(result->error, sizeof(result->error), "out of memory");
            goto cleanup;
         }
      }
   }

   result->bob = bob_create();
   if (!result->bob) {
      snprintf(result->error, sizeof(result->error), "out of memory");
      goto cleanup;
   }

   for (i = 0; i < task_tables.count; ++i)
   {
      Bob_Task task = {0};
      elf_Table *description = task_tables.items[i];
      Bob_Node *node;
      Bob_Error bob_error;
      task.name = copy_string_value(scratch.arena, field(state, description, "name"));
      task.command_line = copy_string_value(scratch.arena, field(state, description, "command_line"));
      if (!task.name.data || !task.command_line.data)
      {
         snprintf(result->error, sizeof(result->error), "task %u requires string fields 'name' and 'command_line'", i);
         goto cleanup;
      }

      if (!copy_string_array(state, scratch.arena, field(state, description, "inputs"), "inputs", task.name, &task.inputs, result->error, sizeof(result->error))) goto cleanup;
      if (!copy_string_array(state, scratch.arena, field(state, description, "outputs"), "outputs", task.name, &task.outputs, result->error, sizeof(result->error))) goto cleanup;
      if (!copy_string_array(state, scratch.arena, field(state, description, "include_dirs"), "include_dirs", task.name, &task.include_directories, result->error, sizeof(result->error))) goto cleanup;
      bob_error = bob_add_task(result->bob, task, &node);
      if (bob_error != BOB_OK) {
         snprintf(result->error, sizeof(result->error), "unable to add task '%s': %s", task.name.data, bob_error_string(bob_error));
         goto cleanup;
      }
   }

   for (i = 0; i < task_tables.count; ++i)
   {
      elf_ValueView dependencies_value = field(state, task_tables.items[i], "dependencies");
      uint32_t dependency;

      if (dependencies_value.type == ELF_VALUE_TYPE_NIL) { continue; }
      for (dependency = 0; dependency < elf_table_length(dependencies_value.as.table); ++dependency)
      {
         elf_ValueView dependency_value = elf_get_index(state, dependencies_value.as.table, dependency);
         uint32_t resolved;
         Bob_Error bob_error;
         resolved = table_list_find(&task_tables, dependency_value.as.table);
         if (resolved == UINT32_MAX) goto cleanup;
         Bob_Node *node = bob_node_at(result->bob, i);
         Bob_Node *dependency_node = bob_node_at(result->bob, resolved);
         bob_error = bob_add_dependency(result->bob, node, dependency_node);
         if (bob_error != BOB_OK) {
            snprintf(result->error, sizeof(result->error), "unable to add dependency to '%s': %s", bob_task_name(node), bob_error_string(bob_error));
            goto cleanup;
         }
      }
   }

   success = 1;

   cleanup:
   end_scratch(scratch);
   elf_release_table(root);
   elf_destroy_state(state);
   if (!success)
   {
      char error[sizeof(result->error)];
      memcpy(error, result->error, sizeof(error));
      bob_destroy(result->bob);
      memset(result, 0, sizeof(*result));
      memcpy(result->error, error, sizeof(error));
   }
   return success;
}
