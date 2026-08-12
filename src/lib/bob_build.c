#include "bob_build.h"
#include "build_state.h"
#include "compiler_command.h"
#include "logger.h"
#include "make_depfile.h"
#include "platform_adapter.h"
#include "platform.h"
#include "profiler.h"

#define BOB_BUILD_STATE_PATH ".bob/state.elf"

typedef enum Bob_Rebuild_Reason
{
	BOB_REBUILD_UP_TO_DATE,
	BOB_REBUILD_NO_OUTPUTS,
	BOB_REBUILD_OUTPUT_MISSING,
	BOB_REBUILD_INPUT_MISSING,
	BOB_REBUILD_STATE_MISSING,
	BOB_REBUILD_DEPENDENCY_MISSING,
	BOB_REBUILD_DEPENDENCY_CHANGED,
	BOB_REBUILD_INPUT_NEWER,
}
Bob_Rebuild_Reason;

typedef struct Bob_Rebuild_Decision
{
	Bob_Rebuild_Reason reason;
	String             path;
	String             reference;
	const Bob_Node    *dependency;
	b32                rebuild;
}
Bob_Rebuild_Decision;

typedef struct Bob_Build_Task
{
	Bob_Task         task;
	Compiler_Command compiler;
	String_Array     inputs;
	String_Array     outputs;
	String           execution_command_line;
	String           execution_directory;
	String           dependency_file;
	b32              tracks_dependencies;
}
Bob_Build_Task;

typedef struct Bob_Build_Completion
{
	Bob_Node                   *node;
	Bob_Build_Task             *task;
	Bob_Platform_Process_Result process;
	String_Array                dependencies;
	Bob_Rebuild_Decision        decision;
	b32                         dependency_tracking;
	b32                         dependency_state_valid;
}
Bob_Build_Completion;

typedef struct Bob_Builder
{
	Build_State state;
	Build_State updates;
	Build_State removals;
	Arena       state_arena;
	Arena       update_arena;
	b32         dependency_tracking;
	b32         explain;
	b32         state_changed;
	b32         internal_error;
}
Bob_Builder;

static Bob_Node_Result build_task_action(Bob_Node_Context *context, void *user_data);

static Bob_Rebuild_Decision task_rebuild_decision(const Bob_Builder *builder,
	const Bob_Node *node,
	const Bob_Build_Task *build_task)
{
	const String_Array *inputs = &build_task->inputs;
	const String_Array *outputs = &build_task->outputs;
	u64 oldest_output = UINT64_MAX;
	u64 newest_input = 0;
	String oldest_output_path = {0};
	String newest_input_path = {0};

	if (outputs->count == 0) return (Bob_Rebuild_Decision){
		.reason = BOB_REBUILD_NO_OUTPUTS,
		.rebuild = true,
	};
	for (u32 i = 0; i < outputs->count; ++i) {
		Bob_Platform_File_Info info;
		if (!bob_platform_file_info(outputs->items[i], &info)) {
			return (Bob_Rebuild_Decision){
				.reason = BOB_REBUILD_OUTPUT_MISSING,
				.path = outputs->items[i],
				.rebuild = true,
			};
		}
		if ((u64)info.modified_unix_ms < oldest_output) {
			oldest_output = (u64)info.modified_unix_ms;
			oldest_output_path = outputs->items[i];
		}
	}
	for (u32 i = 0; i < inputs->count; ++i) {
		Bob_Platform_File_Info info;
		if (!bob_platform_file_info(inputs->items[i], &info)) {
			return (Bob_Rebuild_Decision){
				.reason = BOB_REBUILD_INPUT_MISSING,
				.path = inputs->items[i],
				.rebuild = true,
			};
		}
		if ((u64)info.modified_unix_ms > newest_input) {
			newest_input = (u64)info.modified_unix_ms;
			newest_input_path = inputs->items[i];
		}
	}

	if (build_task->tracks_dependencies) {
		Build_State_Task *state_task = build_state_find(
			(Build_State *)&builder->state, outputs->items[0]);
		if (!state_task) return (Bob_Rebuild_Decision){
			.reason = BOB_REBUILD_STATE_MISSING,
			.path = outputs->items[0],
			.rebuild = true,
		};
		for (u32 i = 0; i < state_task->dependencies.count; ++i) {
			Bob_Platform_File_Info info;
			if (!bob_platform_file_info(state_task->dependencies.items[i], &info)) {
				return (Bob_Rebuild_Decision){
					.reason = BOB_REBUILD_DEPENDENCY_MISSING,
					.path = state_task->dependencies.items[i],
					.rebuild = true,
				};
			}
			if ((u64)info.modified_unix_ms > newest_input) {
				newest_input = (u64)info.modified_unix_ms;
				newest_input_path = state_task->dependencies.items[i];
			}
		}
	}

	for (u32 i = 0; i < bob_dependency_count(node); ++i) {
		Bob_Node *dependency = bob_dependency(node, i);
		if (!dependency || bob_node_result(dependency).changed) {
			return (Bob_Rebuild_Decision){
				.reason = BOB_REBUILD_DEPENDENCY_CHANGED,
				.dependency = dependency,
				.rebuild = true,
			};
		}
	}
	if (newest_input > oldest_output) return (Bob_Rebuild_Decision){
		.reason = BOB_REBUILD_INPUT_NEWER,
		.path = newest_input_path,
		.reference = oldest_output_path,
		.rebuild = true,
	};
	return (Bob_Rebuild_Decision){
		.reason = BOB_REBUILD_UP_TO_DATE,
		.path = newest_input_path,
		.reference = oldest_output_path,
	};
}

static b32 path_is_absolute(String path)
{
	return path.size > 0 && (path.data[0] == '/' || path.data[0] == '\\' ||
		(path.size > 1 && path.data[1] == ':'));
}

static b32 resolve_task_path(Arena *arena, String directory, String path,
	String *result)
{
	Scratch scratch;
	void *start;
	String joined;
	b32 succeeded;
	if (!directory.data) {
		*result = path;
		return true;
	}
	if (path_is_absolute(path)) {
		return bob_platform_absolute_path(arena, path, result);
	}
	scratch = begin_different_scratch(arena);
	start = arena_top(scratch.arena);
	arena_append_str(scratch.arena, directory);
	if (directory.size && directory.data[directory.size - 1] != '/' &&
		directory.data[directory.size - 1] != '\\') {
		arena_append_char(scratch.arena, '/');
	}
	arena_append_str(scratch.arena, path);
	joined = arena_string_from(scratch.arena, start);
	arena_finalize_string(scratch.arena, joined);
	succeeded = bob_platform_absolute_path(arena, joined, result);
	end_scratch(scratch);
	return succeeded;
}

static b32 resolve_dependency_paths(Arena *arena, String directory,
	String_Array *dependencies)
{
	String_Array resolved = {0};
	if (!directory.data || dependencies->count == 0) return true;
	resolved.items = arena_push_zero_aligned(arena,
		(u64)dependencies->count * sizeof(*resolved.items), _Alignof(String));
	if (!resolved.items) return false;
	for (u32 i = 0; i < dependencies->count; ++i) {
		if (!resolve_task_path(arena, directory, dependencies->items[i],
			&resolved.items[i])) return false;
		++resolved.count;
	}
	*dependencies = resolved;
	return true;
}

static void run_command(Bob_Node_Context *context, const Bob_Build_Task *task,
	Bob_Build_Completion *completion)
{
	Scratch scratch = begin_different_scratch(context->arena);
	completion->dependency_tracking = task->tracks_dependencies;
	if (completion->dependency_tracking) {
		platform_remove_file(task->dependency_file.data);
	}

	bob_platform_run_command(task->execution_command_line, context->arena,
		(Bob_Platform_Process_Options){
			.working_directory = task->execution_directory,
			.capture_stderr = true,
		}, &completion->process);
	if (completion->dependency_tracking) {
		String contents;
		b32 process_succeeded = completion->process.error_code == 0 &&
			completion->process.exit_code == 0;
		completion->dependency_state_valid = process_succeeded &&
			bob_platform_read_entire_file(scratch.arena, task->dependency_file, &contents) &&
			make_depfile_parse(context->arena, contents, &completion->dependencies) &&
			resolve_dependency_paths(context->arena, task->execution_directory,
				&completion->dependencies);
		platform_remove_file(task->dependency_file.data);
	}
	end_scratch(scratch);
}

static Bob_Node_Result build_task_action(Bob_Node_Context *context, void *user_data)
{
	Bob_Builder *builder = context->execution_data;
	Bob_Build_Task *task = user_data;
	Bob_Build_Completion *completion;
	b32 succeeded;

	completion = arena_push_zero_aligned(context->arena, sizeof(*completion),
		_Alignof(Bob_Build_Completion));
	if (!completion || !builder || !task) return (Bob_Node_Result){0};
	completion->node = context->node;
	completion->task = task;
	{
		Profile_Scope scope = profile_scope_begin("incremental checks");
		completion->decision = task_rebuild_decision(builder, context->node, task);
		profile_scope_end(&scope);
	}
	if (completion->decision.rebuild) {
		Profile_Scope scope = profile_scope_begin("task processes");
		run_command(context, task, completion);
		profile_scope_end(&scope);
	}
	succeeded = !completion->decision.rebuild ||
		(completion->process.error_code == 0 && completion->process.exit_code == 0);
	return (Bob_Node_Result){
		.output = completion,
		.succeeded = succeeded,
		.changed = succeeded && completion->decision.rebuild && !task->task.transparent,
	};
}

static void report_explanation(const Bob_Build_Completion *completion)
{
	const Bob_Rebuild_Decision *decision = &completion->decision;
	const char *name = bob_task_name(completion->node);
	switch (decision->reason) {
	case BOB_REBUILD_UP_TO_DATE:
		logger_log(LOG_LEVEL_INFO, "explain", "%s: inputs are not newer than outputs", name);
		break;
	case BOB_REBUILD_NO_OUTPUTS:
		logger_log(LOG_LEVEL_INFO, "explain", "%s: rebuilding because no outputs are declared", name);
		break;
	case BOB_REBUILD_OUTPUT_MISSING:
		logger_log(LOG_LEVEL_INFO, "explain", "%s: rebuilding because output is missing: %s", name, decision->path.data);
		break;
	case BOB_REBUILD_INPUT_MISSING:
		logger_log(LOG_LEVEL_INFO, "explain", "%s: rebuilding because input is missing: %s", name, decision->path.data);
		break;
	case BOB_REBUILD_STATE_MISSING:
		logger_log(LOG_LEVEL_INFO, "explain", "%s: rebuilding because dependency state is missing: %s", name, decision->path.data);
		break;
	case BOB_REBUILD_DEPENDENCY_MISSING:
		logger_log(LOG_LEVEL_INFO, "explain", "%s: rebuilding because recorded dependency is missing: %s", name, decision->path.data);
		break;
	case BOB_REBUILD_DEPENDENCY_CHANGED:
		logger_log(LOG_LEVEL_INFO, "explain", "%s: rebuilding because dependency changed: %s", name,
			decision->dependency ? bob_task_name(decision->dependency) : "unknown");
		break;
	case BOB_REBUILD_INPUT_NEWER:
		logger_log(LOG_LEVEL_INFO, "explain", "%s: rebuilding because %s is newer than %s", name,
			decision->path.data, decision->reference.data);
		break;
	}
}

static void report_completion(const Bob_Build_Completion *completion)
{
	const Bob_Build_Task *build_task = completion->task;
	const Bob_Task *task = &build_task->task;
	String command_line = task->command_line;
	b32 succeeded = !completion->decision.rebuild ||
		(completion->process.error_code == 0 && completion->process.exit_code == 0);

	if (!completion->decision.rebuild) {
		logger_log_at(0, LOG_LEVEL_INFO, "up-to-date", "%s",
			bob_task_name(completion->node));
		logger_log_at(1, LOG_LEVEL_TRACE, "command", "%s", command_line.data);
		return;
	}
	if (completion->process.output.size > 0) {
		if (succeeded) logger_log_string_at(0, LOG_LEVEL_INFO,
			bob_task_name(completion->node), completion->process.output);
		else logger_log_string(LOG_LEVEL_ERROR, bob_task_name(completion->node),
			completion->process.output);
	}
	logger_log_at(0, succeeded ? LOG_LEVEL_SUCCESS : LOG_LEVEL_ERROR,
		succeeded ? "succeeded" : "failed", "%s", bob_task_name(completion->node));
	if (succeeded) {
		logger_log_at(1, LOG_LEVEL_TRACE, "command", "%s", command_line.data);
		logger_log_at(1, LOG_LEVEL_TRACE, "exit-code", "0");
	}
	if (completion->process.error_code != 0) {
		Scratch scratch = begin_scratch();
		String working_directory;
		logger_log(LOG_LEVEL_ERROR, bob_task_name(completion->node), "%s",
			completion->process.launched ? "process error" : "failed to start process");
		logger_log_at(1, LOG_LEVEL_ERROR, "command", "%s", command_line.data);
		{
			String message;
			if (bob_platform_error_message(completion->process.error_code,
				scratch.arena, &message)) {
				logger_log(LOG_LEVEL_ERROR, "os", "error %u: %s",
					completion->process.error_code, message.data);
			}
			else logger_log(LOG_LEVEL_ERROR, "os", "error %u",
				completion->process.error_code);
		}
		if (build_task->compiler.executable.data) {
			logger_log(LOG_LEVEL_ERROR, "executable", "%s (%s)",
				build_task->compiler.executable.data,
				bob_platform_executable_resolves(build_task->compiler.executable) ?
				"found" : "not found in current directory or PATH");
		}
		else logger_log(LOG_LEVEL_ERROR, "executable", "unable to parse from command");
		if (build_task->execution_directory.data) {
			logger_log(LOG_LEVEL_ERROR, "working-directory", "%s",
				build_task->execution_directory.data);
		}
		else if (bob_platform_current_directory(scratch.arena, &working_directory)) {
			logger_log(LOG_LEVEL_ERROR, "working-directory", "%s", working_directory.data);
		}
		end_scratch(scratch);
	}
	else if (completion->process.exit_code != 0) {
		logger_log(LOG_LEVEL_ERROR, bob_task_name(completion->node),
			"process exited with code %u", completion->process.exit_code);
		logger_log_at(1, LOG_LEVEL_ERROR, "command", "%s", command_line.data);
	}
}

static void collect_dependency_state(Bob_Builder *builder, const Bob_Build_Completion *completion, b32 succeeded)
{
	String output;
	if (!completion->dependency_tracking) return;
	output = completion->task->outputs.items[0];
	builder->state_changed = true;
	if (!succeeded || !completion->dependency_state_valid) {
		if (!build_state_set(&builder->update_arena, &builder->removals,
			output, (String_Array){0})) builder->internal_error = true;
		if (succeeded && !completion->dependency_state_valid) {
			log_warning("could not read compiler dependencies for %s",
				bob_task_name(completion->node));
		}
		return;
	}
	if (!build_state_set(&builder->update_arena, &builder->updates,
		output, completion->dependencies)) builder->internal_error = true;
}

static void build_task_completed(Bob_Node *node, Bob_Node_Result result, void *user_data)
{
	Bob_Builder *builder = user_data;
	Bob_Build_Completion *completion;
	if (!node || bob_node_function(node) != build_task_action) return;
	completion = result.output;
	if (!completion) {
		builder->internal_error = true;
		return;
	}
	{
		Profile_Scope scope = profile_scope_begin("report completion");
		if (builder->explain) report_explanation(completion);
		report_completion(completion);
		profile_scope_end(&scope);
	}
	collect_dependency_state(builder, completion, result.succeeded);
}

static b32 merge_dependency_state(Bob_Builder *builder)
{
	for (u32 i = 0; i < builder->removals.count; ++i) {
		build_state_remove(&builder->state, builder->removals.tasks[i].output);
	}
	for (u32 i = 0; i < builder->updates.count; ++i) {
		Build_State_Task *update = builder->updates.tasks + i;
		if (!build_state_set(&builder->state_arena, &builder->state,
			update->output, update->dependencies)) return false;
	}
	return true;
}

static b32 valid_task(const Bob_Task *task)
{
	return task && task->command_line.data &&
		(!task->inputs.count || task->inputs.items) &&
		(!task->outputs.count || task->outputs.items) &&
		(!task->include_directories.count || task->include_directories.items);
}

b32 bob_build(Bob *bob, Bob_Build_Options options)
{
	Bob_Builder builder = {0};
	Build_State_Load_Result load_result = BUILD_STATE_LOAD_MISSING;
	b32 result;

	if (!bob || options.worker_count == 0) return false;
	builder.explain = options.explain;
	for (u32 i = 0; i < bob_node_count(bob); ++i) {
		Bob_Node *node = bob_node_at(bob, i);
		const Bob_Build_Task *task;
		if (bob_node_function(node) != build_task_action) continue;
		task = bob_node_user_data(node);
		if (!task || !valid_task(&task->task)) return false;
		if (task->tracks_dependencies) {
			builder.dependency_tracking = true;
		}
	}
	builder.state_arena = arena_create(MEGABYTES(64));
	builder.update_arena = arena_create(MEGABYTES(64));
	if (!builder.state_arena.data || !builder.update_arena.data) {
		result = false;
		goto cleanup;
	}
	if (builder.dependency_tracking) {
		load_result = build_state_load(&builder.state_arena,
			STRING_LITERAL(BOB_BUILD_STATE_PATH), &builder.state);
		if (load_result == BUILD_STATE_LOAD_ERROR) {
			log_warning("could not load Bob build state");
		}
		else if (load_result == BUILD_STATE_LOAD_INVALID) {
			log_warning("ignoring invalid Bob build state");
			builder.state = (Build_State){0};
			builder.state_changed = true;
		}
	}

	result = bob_execute(bob, (Bob_Execute_Options){
		.worker_count = options.worker_count,
		.user_data = &builder,
		.completed = build_task_completed,
	});
	if (builder.internal_error || !merge_dependency_state(&builder)) result = false;
	if (builder.dependency_tracking && builder.state_changed &&
		!build_state_save(&builder.update_arena, STRING_LITERAL(BOB_BUILD_STATE_PATH),
			&builder.state)) {
		platform_remove_file(BOB_BUILD_STATE_PATH);
		log_warning("could not save Bob build state");
		result = false;
	}

cleanup:
	arena_destroy(&builder.update_arena);
	arena_destroy(&builder.state_arena);
	return result;
}

static b32 copy_optional_string(Bob *bob, String source, String *result)
{
	if (!source.data) return source.size == 0;
	*result = bob_copy_string(bob, source);
	return result->data != NULL;
}

static b32 copy_string_array(Bob *bob, String_Array source, String_Array *result)
{
	if (source.count == 0) return true;
	if (!source.items) return false;
	result->items = bob_allocate(bob,
		(u64)source.count * sizeof(*result->items), _Alignof(String));
	if (!result->items) return false;
	for (u32 i = 0; i < source.count; ++i) {
		if (!source.items[i].data) return false;
		result->items[i] = bob_copy_string(bob, source.items[i]);
		if (!result->items[i].data) return false;
		++result->count;
	}
	return true;
}

static b32 resolve_task_paths(Bob *bob, Arena *scratch, String directory,
	String_Array source, String_Array *result)
{
	if (!directory.data) {
		*result = source;
		return true;
	}
	*result = (String_Array){0};
	if (source.count == 0) return true;
	result->items = bob_allocate(bob,
		(u64)source.count * sizeof(*result->items), _Alignof(String));
	if (!result->items) return false;
	for (u32 i = 0; i < source.count; ++i) {
		String resolved;
		if (!resolve_task_path(scratch, directory, source.items[i], &resolved)) {
			return false;
		}
		result->items[i] = bob_copy_string(bob, resolved);
		if (!result->items[i].data) return false;
		++result->count;
	}
	return true;
}

static Bob_Build_Task *build_task_create(Bob *bob, Bob_Task task, String fallback_name)
{
	Bob_Build_Task *copy = bob_allocate(bob, sizeof(*copy), _Alignof(Bob_Build_Task));
	Bob_Task *description;
	Scratch scratch;
	Compiler_Command compiler;
	b32 valid = false;
	if (!copy) return NULL;
	description = &copy->task;
	if (!task.name.data) task.name = fallback_name;
	if (task.working_directory.data && task.working_directory.size == 0) return NULL;
	description->transparent = task.transparent;
	if (!task.name.data || !copy_optional_string(bob, task.name, &description->name) ||
		!copy_optional_string(bob, task.command_line, &description->command_line) ||
		!copy_optional_string(bob, task.working_directory,
			&description->working_directory) ||
		!copy_string_array(bob, task.inputs, &description->inputs) ||
		!copy_string_array(bob, task.outputs, &description->outputs) ||
		!copy_string_array(bob, task.include_directories,
			&description->include_directories)) return NULL;

	scratch = begin_scratch();
	copy->inputs = description->inputs;
	copy->outputs = description->outputs;
	if (description->working_directory.data) {
		String absolute_directory;
		if (!bob_platform_absolute_path(scratch.arena,
			description->working_directory, &absolute_directory)) goto done;
		copy->execution_directory = bob_copy_string(bob, absolute_directory);
		if (!copy->execution_directory.data ||
			!resolve_task_paths(bob, scratch.arena, copy->execution_directory,
				description->inputs, &copy->inputs) ||
			!resolve_task_paths(bob, scratch.arena, copy->execution_directory,
				description->outputs, &copy->outputs)) goto done;
	}
	if (!compiler_command_parse(scratch.arena, description->command_line, &compiler)) {
		goto done;
	}
	copy->compiler = compiler;
	copy->compiler.executable = (String){0};
	if (!copy_optional_string(bob, compiler.executable, &copy->compiler.executable)) {
		goto done;
	}
	copy->execution_command_line = description->command_line;
	copy->tracks_dependencies = copy->outputs.count > 0 &&
		compiler_command_can_add_make_dependencies(&copy->compiler);
	if (copy->tracks_dependencies) {
		String augmented;
		void *start = arena_top(scratch.arena);
		arena_append_str(scratch.arena, copy->outputs.items[0]);
		arena_append_text(scratch.arena, ".d.tmp");
		copy->dependency_file = bob_copy_string(bob,
			arena_string_from(scratch.arena, start));
		if (!copy->dependency_file.data ||
			!compiler_command_add_dependencies(scratch.arena, &copy->compiler,
				description->command_line, copy->dependency_file, &augmented)) goto done;
		copy->execution_command_line = bob_copy_string(bob, augmented);
		if (!copy->execution_command_line.data) goto done;
	}
	valid = true;

done:
	end_scratch(scratch);
	return valid ? copy : NULL;
}

Bob_Error bob_add_task(Bob *bob, Bob_Task task, Bob_Node **node_out)
{
	Bob_Build_Task *copy;
	if (!bob || !task.name.data || !node_out) return BOB_ERROR_INVALID_TASK;
	if (bob_is_prepared(bob)) return BOB_ERROR_ALREADY_PREPARED;
	copy = build_task_create(bob, task, (String){0});
	if (!copy) return BOB_ERROR_OUT_OF_MEMORY;
	return bob_add_node(bob, (Bob_Node_Description){
		.name = copy->task.name,
		.function = build_task_action,
		.user_data = copy,
	}, node_out);
}

Bob_Error bob_set_task(Bob *bob, Bob_Node *node, Bob_Task task)
{
	Bob_Build_Task *copy;
	Bob_Error result;
	String fallback_name;
	if (!bob || !node) return BOB_ERROR_INVALID_TASK;
	if (bob_is_prepared(bob)) return BOB_ERROR_ALREADY_PREPARED;
	for (u32 i = 0; i < bob_node_count(bob); ++i) {
		if (bob_node_at(bob, i) == node) goto found;
	}
	return BOB_ERROR_INVALID_TASK;

found:
	fallback_name = string_from_cstring(bob_node_name(node));
	copy = build_task_create(bob, task, fallback_name);
	if (!copy) return BOB_ERROR_OUT_OF_MEMORY;
	result = bob_set_node(bob, node, (Bob_Node_Description){
		.name = copy->task.name,
		.function = build_task_action,
		.user_data = copy,
	});
	return result;
}

u32 bob_task_count(const Bob *bob)
{
	return bob_node_count(bob);
}

const char *bob_task_name(const Bob_Node *node)
{
	return bob_node_name(node);
}

Bob_Task_State bob_task_state(const Bob_Node *node)
{
	return bob_node_state(node);
}

const Bob_Task *bob_get_task(const Bob_Node *node)
{
	const Bob_Build_Task *task;
	if (!node || bob_node_function(node) != build_task_action) return NULL;
	task = bob_node_user_data(node);
	return task ? &task->task : NULL;
}
