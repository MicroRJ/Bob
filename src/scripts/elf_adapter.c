#include "elf_adapter.h"
#include "elf_batteries.h"

#include "bob_build.h"
#include "elf.h"
#include "logger.h"
#include "platform_adapter.h"
#include "profiler.h"

#include <stdio.h>
#include <string.h>

typedef struct Elf_Script
{
	elf_State *state;
	elf_Ref exports;
}
Elf_Script;

static b32 read_build_table(Script *script, elf_i32 root, Script_Build *result);
ELF_FUNCTION(l_bob_build)
{
	(void)nargs;
	(void)nrets;
	Script *script = elf_get_user_data(S);
	Script_Build build = {0};
	if (!read_build_table(script, 1, &build))
	{
		script_set_error(script, "%s", build.error);
		script->failed = true;
		elf_push_int(S, false);
		return 1;
	}

	Script_Options options = script_options_resolve(build.options, script->command_line_options);
	logger_set_verbosity(options.verbosity);
	Profile_Scope scope = profile_scope_begin("builder");
	b32 succeeded = bob_build(build.bob, (Bob_Build_Params){
		.worker_count = options.worker_count,
		.explain = script->command_line_options.explain,
	});
	profile_scope_end(&scope);
	bob_destroy(build.bob);
	if (!succeeded) {
		script_set_error(script, "build failed");
		script->failed = true;
	}
	elf_push_int(S, succeeded);
	return 1;
}

static String stack_string(elf_State *state, elf_i32 index)
{
	elf_StrSlice value;
	if (!elf_to_str(state, index, &value)) return (String){0};
	return string_from_data(value.data, value.size);
}

static b32 stack_integer_field(elf_State *state, elf_i32 table, const char *field, elf_Integer *value, b32 *present)
{
	if (!elf_get_field(state, table, field)) return false;
	*present = !elf_is_nil(state, -1);
	b32 result = !*present || elf_to_int(state, -1, value);
	elf_pop(state, 1);
	return result;
}

static b32 set_function(elf_State *state, elf_i32 table, const char *name, elf_Function function)
{
	elf_push_fun(state, function);
	return elf_set_field(state, table, name);
}

static b32 register_bob_library(elf_State *state)
{
	elf_i32 checkpoint = elf_get_top(state);
	elf_new_table(state);
	elf_i32 bob = elf_abs_index(state, -1);

	if (!set_function(state, bob, "build", l_bob_build)) goto error;
	elf_push_cstr(state, BOB_VERSION);
	if (!elf_set_field(state, bob, "version")) goto error;

	if (!elf_set_global(state, "bob")) goto error;
	return true;

error:
	elf_set_top(state, checkpoint);
	return false;
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
	elf_open_batteries(elf->state);
	if (!register_bob_library(elf->state)) {
		script_set_error(script, "unable to register Bob script libraries");
		return false;
	}
	String source;
	if (!bob_platform_read_entire_file(script->arena, path, &source)) {
		script_set_error(script, "unable to read '%s'", path.data);
		return false;
	}
	if (source.size > UINT32_MAX) {
		script_set_error(script, "script is too large: '%s'", path.data);
		return false;
	}
	if (!elf_push_code_source(elf->state, path.data,
	                          (elf_StrSlice){ source.data, (elf_u32)source.size })) {
		script_set_error(script, "unable to load '%s'", path.data);
		return false;
	}
	elf_push_nil(elf->state);
	elf_call(elf->state, 1, 1);
	if (script->failed) {
		elf_pop(elf->state, 1);
		return false;
	}

	if (elf_type(elf->state, -1) != ELF_VALUE_TYPE_TABLE) {
		script_set_error(script, "script must return a table");
		return false;
	}
	elf->exports = elf_create_ref(elf->state, -1);
	elf_pop(elf->state, 1);
	if (elf->exports == ELF_NO_REF) {
		script_set_error(script, "unable to retain script exports");
		return false;
	}

	elf_i32 checkpoint = elf_get_top(elf->state);
	elf_push_ref(elf->state, elf->exports);
	elf_i32 exports = elf_abs_index(elf->state, -1);
	elf_u32 cursor = 0;
	while (elf_next(elf->state, exports, &cursor)) {
		elf_StrSlice name;
		if (elf_to_str(elf->state, -2, &name) && elf_is_callable(elf->state, -1)) ++script->functions.count;
		elf_pop(elf->state, 2);
	}
	script->functions.items = arena_push_zero_aligned(script->arena, script->functions.count * sizeof(String), _Alignof(String));
	cursor = 0;
	u32 function_index = 0;
	while (elf_next(elf->state, exports, &cursor))
	{
		elf_StrSlice slice;
		if (elf_to_str(elf->state, -2, &slice) && elf_is_callable(elf->state, -1)) {
			String name = string_from_data(slice.data, slice.size);
			script->functions.items[function_index++] = arena_push_string_copy(script->arena, name);
		}
		elf_pop(elf->state, 2);
	}
	elf_set_top(elf->state, checkpoint);
	return true;
}

void elf_script_destroy(Script *script)
{
	Elf_Script *elf = script->context;
	if (elf->exports != ELF_NO_REF) elf_release_ref(elf->state, elf->exports);
	if (elf->state) elf_destroy_state(elf->state);
	elf->exports = ELF_NO_REF;
	elf->state = NULL;
}

b32 elf_script_invoke(Script *script, String name)
{
	Elf_Script *elf = script->context;
	elf_i32 checkpoint = elf_get_top(elf->state);
	if (!elf_push_ref(elf->state, elf->exports)) return false;
	elf_i32 exports = elf_abs_index(elf->state, -1);
	if (!elf_get_field(elf->state, exports, name.data) || !elf_is_callable(elf->state, -1)) {
		elf_set_top(elf->state, checkpoint);
		return false;
	}
	elf_push_nil(elf->state);
	elf_call(elf->state, 1, 0);
	elf_set_top(elf->state, checkpoint);
	return true;
}

static String copy_stack_string(Arena *arena, elf_State *state, elf_i32 index)
{
	String string = stack_string(state, index);
	return string.data ? arena_push_string_copy(arena, string) : (String){0};
}

static b32 copy_string_array_field(elf_State *state, Arena *arena, elf_i32 table, const char *field_name, String task_name, String_Array *result, char *error, size_t error_size)
{
	elf_i32 checkpoint = elf_get_top(state);
	*result = (String_Array){0};
	if (!elf_get_field(state, table, field_name)) return false;
	if (elf_is_nil(state, -1)) {
		elf_set_top(state, checkpoint);
		return true;
	}
	elf_i32 array = elf_abs_index(state, -1);
	elf_u32 count = 0;
	if (!elf_length(state, array, &count)) {
		snprintf(error, error_size, "%s for '%s' must be a table", field_name, task_name.data);
		elf_set_top(state, checkpoint);
		return false;
	}
	result->count = count;
	result->items = arena_push_zero_aligned(arena, count * sizeof(*result->items), _Alignof(String));
	for (elf_u32 i = 0; i < count; ++i)
	{
		elf_get_index(state, array, i);
		result->items[i] = copy_stack_string(arena, state, -1);
		elf_pop(state, 1);
		if (!result->items[i].data) {
			snprintf(error, error_size, "%s for '%s' must contain strings", field_name, task_name.data);
			elf_set_top(state, checkpoint);
			return false;
		}
	}
	elf_set_top(state, checkpoint);
	return true;
}

typedef struct Ref_List
{
	Arena *arena;
	elf_Ref *items;
	u32 count;
}
Ref_List;

static u32 ref_list_find(Ref_List *list, elf_State *state, elf_i32 value)
{
	elf_i32 absolute = elf_abs_index(state, value);
	for (u32 i = 0; i < list->count; ++i)
	{
		if (!elf_push_ref(state, list->items[i])) continue;
		b32 equal = elf_equal(state, absolute, -1);
		elf_pop(state, 1);
		if (equal) return i;
	}
	return UINT32_MAX;
}

static b32 ref_list_add(Ref_List *list, elf_State *state, elf_i32 value)
{
	if (ref_list_find(list, state, value) != UINT32_MAX) return true;
	elf_Ref reference = elf_create_ref(state, value);
	if (reference == ELF_NO_REF) return false;
	elf_Ref *item = arena_push_aligned(list->arena, sizeof(*item), _Alignof(elf_Ref));
	if (!list->items) list->items = item;
	*item = reference;
	++list->count;
	return true;
}

static b32 read_build_table(Script *script, elf_i32 root, Script_Build *result)
{
	if (!script || !result) return false;
	Elf_Script *elf = script->context;
	elf_State *state = elf->state;
	elf_i32 checkpoint = elf_get_top(state);
	root = elf_abs_index(state, root);
	Scratch scratch = begin_scratch();
	Ref_List task_tables = { .arena = scratch.arena };
	b32 success = false;
	memset(result, 0, sizeof(*result));

	elf_get_field(state, root, "options");
	if (!elf_is_nil(state, -1))
	{
		elf_i32 options = elf_abs_index(state, -1);
		if (elf_type(state, options) != ELF_VALUE_TYPE_TABLE) {
			snprintf(result->error, sizeof(result->error), "returned 'options' field must be a table");
			goto cleanup;
		}
		elf_Integer integer = 0;
		b32 present = false;
		if (!stack_integer_field(state, options, "workers", &integer, &present) || (present && (integer < 1 || (u64)integer > UINT32_MAX))) {
			snprintf(result->error, sizeof(result->error), "options.workers must be a positive integer");
			goto cleanup;
		}
		if (present) {
			result->options.worker_count = (u32)integer;
			result->options.has_worker_count = true;
		}
		if (!stack_integer_field(state, options, "verbosity", &integer, &present) || (present && (integer < 0 || (u64)integer > INT32_MAX))) {
			snprintf(result->error, sizeof(result->error), "options.verbosity must be a non-negative integer");
			goto cleanup;
		}
		if (present) {
			result->options.verbosity = (i32)integer;
			result->options.has_verbosity = true;
		}
	}
	elf_pop(state, 1);

	elf_get_field(state, root, "targets");
	elf_i32 targets = elf_abs_index(state, -1);
	elf_u32 target_count = 0;
	if (!elf_length(state, targets, &target_count)) {
		snprintf(result->error, sizeof(result->error), "returned table requires a 'targets' table");
		goto cleanup;
	}
	for (elf_u32 i = 0; i < target_count; ++i)
	{
		elf_get_index(state, targets, i);
		if (elf_type(state, -1) != ELF_VALUE_TYPE_TABLE) {
			snprintf(result->error, sizeof(result->error), "target %u must be a task table", i);
			goto cleanup;
		}
		if (!ref_list_add(&task_tables, state, -1)) {
			snprintf(result->error, sizeof(result->error), "unable to retain target %u", i);
			goto cleanup;
		}
		elf_pop(state, 1);
	}
	elf_pop(state, 1);

	for (u32 i = 0; i < task_tables.count; ++i)
	{
		elf_i32 task_checkpoint = elf_get_top(state);
		elf_push_ref(state, task_tables.items[i]);
		elf_i32 task = elf_abs_index(state, -1);
		elf_get_field(state, task, "dependencies");
		if (elf_is_nil(state, -1)) {
			elf_set_top(state, task_checkpoint);
			continue;
		}
		elf_i32 dependencies = elf_abs_index(state, -1);
		elf_u32 dependency_count = 0;
		if (!elf_length(state, dependencies, &dependency_count)) {
			snprintf(result->error, sizeof(result->error), "dependencies for task %u must be a table", i);
			goto cleanup;
		}
		for (elf_u32 dependency = 0; dependency < dependency_count; ++dependency)
		{
			elf_get_index(state, dependencies, dependency);
			if (elf_type(state, -1) != ELF_VALUE_TYPE_TABLE) {
				snprintf(result->error, sizeof(result->error), "dependencies for task %u must contain task tables", i);
				goto cleanup;
			}
			if (!ref_list_add(&task_tables, state, -1)) {
				snprintf(result->error, sizeof(result->error), "unable to retain dependency for task %u", i);
				goto cleanup;
			}
			elf_pop(state, 1);
		}
		elf_set_top(state, task_checkpoint);
	}

	result->bob = bob_create();
	if (!result->bob) {
		snprintf(result->error, sizeof(result->error), "out of memory");
		goto cleanup;
	}

	for (u32 i = 0; i < task_tables.count; ++i)
	{
		elf_i32 task_checkpoint = elf_get_top(state);
		elf_push_ref(state, task_tables.items[i]);
		elf_i32 description = elf_abs_index(state, -1);
		Bob_Task_Desc task = {0};
		elf_get_field(state, description, "name");
		task.name = copy_stack_string(scratch.arena, state, -1);
		elf_pop(state, 1);
		elf_get_field(state, description, "command_line");
		task.command_line = copy_stack_string(scratch.arena, state, -1);
		elf_pop(state, 1);
		if (!task.name.data || !task.command_line.data) {
			snprintf(result->error, sizeof(result->error), "task %u requires string fields 'name' and 'command_line'", i);
			goto cleanup;
		}
		elf_get_field(state, description, "working_directory");
		if (!elf_is_nil(state, -1)) {
			task.working_directory = copy_stack_string(scratch.arena, state, -1);
			if (!task.working_directory.data || task.working_directory.size == 0) {
				snprintf(result->error, sizeof(result->error),
					"working_directory for '%s' must be a non-empty string",
					task.name.data);
				goto cleanup;
			}
		}
		elf_pop(state, 1);
		elf_Integer transparent = 0;
		b32 present = false;
		if (!stack_integer_field(state, description, "transparent", &transparent, &present)) {
			snprintf(result->error, sizeof(result->error), "transparent for '%s' must be a boolean", task.name.data);
			goto cleanup;
		}
		if (present) task.transparent = transparent != 0;
		if (!copy_string_array_field(state, scratch.arena, description, "inputs", task.name, &task.inputs, result->error, sizeof(result->error))) goto cleanup;
		if (!copy_string_array_field(state, scratch.arena, description, "outputs", task.name, &task.outputs, result->error, sizeof(result->error))) goto cleanup;
		if (!copy_string_array_field(state, scratch.arena, description, "include_dirs", task.name, &task.include_directories, result->error, sizeof(result->error))) goto cleanup;
		Bob_Node *node;
		Bob_Error bob_error = bob_add_task(result->bob, task, &node);
		if (bob_error != BOB_OK) {
			snprintf(result->error, sizeof(result->error), "unable to add task '%s': %s", task.name.data, bob_error_string(bob_error));
			goto cleanup;
		}
		elf_set_top(state, task_checkpoint);
	}

	for (u32 i = 0; i < task_tables.count; ++i)
	{
		elf_i32 task_checkpoint = elf_get_top(state);
		elf_push_ref(state, task_tables.items[i]);
		elf_i32 description = elf_abs_index(state, -1);
		elf_get_field(state, description, "dependencies");
		if (elf_is_nil(state, -1)) {
			elf_set_top(state, task_checkpoint);
			continue;
		}
		elf_i32 dependencies = elf_abs_index(state, -1);
		elf_u32 dependency_count = 0;
		elf_length(state, dependencies, &dependency_count);
		for (elf_u32 dependency = 0; dependency < dependency_count; ++dependency)
		{
			elf_get_index(state, dependencies, dependency);
			u32 resolved = ref_list_find(&task_tables, state, -1);
			elf_pop(state, 1);
			if (resolved == UINT32_MAX) {
				snprintf(result->error, sizeof(result->error), "unable to resolve dependency for task %u", i);
				goto cleanup;
			}
			Bob_Node *node = bob_node_at(result->bob, i);
			Bob_Node *dependency_node = bob_node_at(result->bob, resolved);
			Bob_Error bob_error = bob_add_dependency(result->bob, node, dependency_node);
			if (bob_error != BOB_OK) {
				snprintf(result->error, sizeof(result->error), "unable to add dependency to '%s': %s", bob_task_name(node), bob_error_string(bob_error));
				goto cleanup;
			}
		}
		elf_set_top(state, task_checkpoint);
	}
	success = true;

cleanup:
	elf_set_top(state, checkpoint);
	for (u32 i = 0; i < task_tables.count; ++i) elf_release_ref(state, task_tables.items[i]);
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
	elf_i32 checkpoint = elf_get_top(elf->state);
	if (!elf_push_ref(elf->state, elf->exports)) return false;
	elf_i32 root = elf_abs_index(elf->state, -1);
	b32 success = read_build_table(script, root, result);
	elf_set_top(elf->state, checkpoint);
	return success;
}
