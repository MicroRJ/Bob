#include "elf_adapter.h"
#include "scripts/libs/string.h"

#include "elf.h"
#include "logger.h"
#include "profiler.h"

#include <stdio.h>
#include <string.h>

typedef struct Elf_Script
{
	elf_State *state;
	elf_Table *exports;
}
Elf_Script;

static b32 read_build_table(Script *script, elf_Table *root, Script_Build *result);

ELF_FUNCTION(l_bob_build)
{
	(void)nargs;
	(void)nrets;
	Script *script = elf_get_user_data(S);
	elf_Table *arguments = elf_arg_table(S, 1);
	Script_Build build = {0};
	if (!read_build_table(script, arguments, &build))
	{
		script_set_error(script, "%s", build.error);
		script->failed = true;
		elf_push_int(S, false);
		return 1;
	}

	Script_Options options = script_options_resolve(build.options, script->command_line_options);
	logger_set_verbosity(options.verbosity);
	Profile_Scope scope = profile_scope_begin("builder");
	b32 succeeded = bob_build(build.bob, options.worker_count);
	profile_scope_end(&scope);
	bob_destroy(build.bob);
	if (!succeeded) {
		script_set_error(script, "build failed");
		script->failed = true;
	}
	elf_push_int(S, succeeded);
	return 1;
}

static const elf_Binding bob_bindings[] =
{
	{ "build", l_bob_build },
};

ELF_FUNCTION(l_strings_expand)
{
	(void)nrets;
	Script *script = elf_get_user_data(S);
	if (nargs != 3 || elf_arg_type(S, 1) != ELF_VALUE_TYPE_TABLE || elf_arg_type(S, 2) != ELF_VALUE_TYPE_STRING)
	{
		script_set_error(script, "strings.expand expects a table of strings and a rule string");
		script->failed = true;
		elf_push_nil(S);
		return 1;
	}

	Scratch scratch = begin_different_scratch(script->arena);
	elf_Table *table = elf_arg_table(S, 1);
	String_Array strings = { .count = elf_table_length(table) };
	strings.items = arena_push_zero_aligned(scratch.arena, strings.count * sizeof(String), _Alignof(String));
	for (u32 index = 0; index < strings.count; ++index)
	{
		elf_ValueView value = elf_get_index(S, table, index);
		if (value.type != ELF_VALUE_TYPE_STRING)
		{
			script_set_error(script, "strings.expand expects a table containing only strings");
			script->failed = true;
			elf_push_nil(S);
			end_scratch(scratch);
			return 1;
		}
		strings.items[index] = string_from_data((void *)elf_str_data(value.as.string), elf_str_size(value.as.string));
	}

	elf_String *rule_value = elf_arg_str(S, 2);
	String rule = string_from_data((void *)elf_str_data(rule_value), elf_str_size(rule_value));
	String result;
	const char *error;
	if (!script_strings_expand(scratch.arena, strings, rule, &result, &error))
	{
		script_set_error(script, "invalid strings.expand rule: %s", error);
		script->failed = true;
		elf_push_nil(S);
	}
	else {
		elf_push_str(S, result.data, (int)result.size);
	}
	end_scratch(scratch);
	return 1;
}

static const elf_Binding string_bindings[] =
{
	{ "expand", l_strings_expand },
};

static b32 is_function(elf_ValueView value)
{
	return value.type == ELF_VALUE_TYPE_CFUNCTION || value.type == ELF_VALUE_TYPE_CLOSURE;
}

b32 elf_script_load(Script *script, String path)
{
	Elf_Script *elf = arena_push_zero_aligned(script->arena, sizeof(*elf), _Alignof(Elf_Script));
	elf->state = elf_create_state();
	script->context = elf;
	if (!elf->state) {
		script_set_error(script, "unable to create elf state");
		return false;
	}
	elf_set_user_data(elf->state, script);
	elf_register_library(elf->state, "bob", bob_bindings, ARRAY_COUNT(bob_bindings));
	elf_register_library(elf->state, "strings", string_bindings, ARRAY_COUNT(string_bindings));
	if (!elf_push_code_file(elf->state, path.data)) {
		script_set_error(script, "unable to load '%s'", path.data);
		return false;
	}
	elf_push_nil(elf->state);
	elf_call(elf->state, 1, 1);
	if (script->failed) {
		elf_pop_values(elf->state, 1);
		return false;
	}

	elf_ValueView returned = elf_peek_value(elf->state, 0);
	if (returned.type != ELF_VALUE_TYPE_TABLE) {
		script_set_error(script, "script must return a table");
		return false;
	}
	elf->exports = elf_retain_table(returned.as.table);
	elf_pop_values(elf->state, 1);

	elf_u32 cursor = 0;
	elf_ValueView key;
	elf_ValueView value;
	while (elf_table_next(elf->exports, &cursor, &key, &value)) {
		if (key.type == ELF_VALUE_TYPE_STRING && is_function(value)) ++script->functions.count;
	}
	script->functions.items = arena_push_zero_aligned(script->arena, script->functions.count * sizeof(String), _Alignof(String));
	cursor = 0;
	u32 function_index = 0;
	while (elf_table_next(elf->exports, &cursor, &key, &value))
	{
		if (key.type != ELF_VALUE_TYPE_STRING || !is_function(value)) continue;
		String name = string_from_data((void *)elf_str_data(key.as.string), elf_str_size(key.as.string));
		script->functions.items[function_index++] = arena_push_string_copy(script->arena, name);
	}
	return true;
}

void elf_script_destroy(Script *script)
{
	Elf_Script *elf = script->context;
	if (elf->exports) elf_release_table(elf->exports);
	if (elf->state) elf_destroy_state(elf->state);
	elf->exports = NULL;
	elf->state = NULL;
}

b32 elf_script_invoke(Script *script, String name)
{
	Elf_Script *elf = script->context;
	elf_push_field(elf->state, elf->exports, name.data);
	elf_push_nil(elf->state);
	elf_call(elf->state, 1, 0);
	return true;
}

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

static b32 read_build_table(Script *script, elf_Table *root, Script_Build *result)
{
   Scratch scratch;
   elf_ValueView targets_value;
   elf_ValueView options_value;
   Elf_Script *elf;
   elf_State *state;
   Table_List task_tables;
   uint32_t i;
   int success = 0;

   if (!script || !result) return false;
   scratch = begin_scratch();
   task_tables = (Table_List){ .arena = scratch.arena };
   memset(result, 0, sizeof(*result));
   elf = script->context;
   state = elf->state;
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
	  elf_ValueView transparent = field(state, description, "transparent");
	  if (transparent.type != ELF_VALUE_TYPE_NIL) {
		 if (transparent.type != ELF_VALUE_TYPE_INTEGER) {
			snprintf(result->error, sizeof(result->error), "transparent for '%s' must be a boolean", task.name.data);
			goto cleanup;
		 }
		 task.transparent = transparent.as.integer != 0;
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
         Bob_Error bob_error;
         u32 resolved = table_list_find(&task_tables, dependency_value.as.table);
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

b32 elf_script_read_build(Script *script, Script_Build *result)
{
	Elf_Script *elf = script->context;
	return read_build_table(script, elf->exports, result);
}
