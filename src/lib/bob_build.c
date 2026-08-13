#include "bob_build.h"
#include "build_state.h"
#include "build_state_stream.h"
#include "compiler_command.h"
#include "logger.h"
#include "make_depfile.h"
#include "platform_adapter.h"
#include "platform.h"
#include "profiler.h"
#include "blake3.h"

#include <string.h>

#define BOB_BUILD_STATE_PATH ".bob/state"

typedef enum Bob_Rebuild_Reason
{
	BOB_REBUILD_UP_TO_DATE,
	BOB_REBUILD_NO_OUTPUTS,
	BOB_REBUILD_OUTPUT_MISSING,
	BOB_REBUILD_INPUT_MISSING,
	BOB_REBUILD_STATE_MISSING,
	BOB_REBUILD_STATE_CHANGED,
	BOB_REBUILD_FINGERPRINT_CHANGED,
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
	Bob_Task_Desc    task;
	Compiler_Command compiler;
	Bob_Path_Array   inputs;
	Bob_Path_Array   outputs;
	Bob_Path_Array   include_directories;
	String           execution_command_line;
	Bob_Path         execution_directory;
	Bob_Path         dependency_file;
	Bob_Fingerprint  fingerprint;
	b32              tracks_dependencies;
}
Bob_Build_Task;

typedef struct Bob_Build_Completion
{
	// TODO(RJ) remove bob from here!
	Bob                        *bob;
	Bob_Node                   *node;
	Bob_Build_Task             *task;
	Bob_Platform_Process_Result process;
	String_Array                dependencies;
	Bob_Rebuild_Decision        decision;
	b32                         dependency_state_valid;
}
Bob_Build_Completion;

typedef struct Bob_Builder
{
	Bob           *bob;
	Bob_Path       state_path;
	Build_State    state;
	Arena          state_arena;
	b32            state_tracking;
	b32            state_changed;
	b32            explain;
	b32            internal_error;
}
Bob_Builder;

static Bob_Node_Result build_task_action(Bob_Node_Context *context, void *user_data);

static Bob_Rebuild_Decision task_rebuild_decision(Bob_Builder *builder,
	const Bob_Node *node,
	const Bob_Build_Task *build_task)
{
	const Bob_Path_Array *inputs = &build_task->inputs;
	const Bob_Path_Array *outputs = &build_task->outputs;
	u64 oldest_output = UINT64_MAX;
	u64 newest_input = 0;
	u64 primary_output_stamp = 0;
	String oldest_output_path = {0};
	String newest_input_path = {0};

	if (outputs->count == 0) return (Bob_Rebuild_Decision){
		.reason = BOB_REBUILD_NO_OUTPUTS,
		.rebuild = true,
	};
	for (u32 i = 0; i < outputs->count; ++i) {
		Bob_Platform_File_Info info;
		String path = bob_path_string(builder->bob, outputs->items[i]);
		if (!bob_platform_file_info(path, &info)) {
			return (Bob_Rebuild_Decision){
				.reason = BOB_REBUILD_OUTPUT_MISSING,
				.path = path,
				.rebuild = true,
			};
		}
		if (!info.is_directory && (u64)info.modified_unix_ms < oldest_output) {
			oldest_output = (u64)info.modified_unix_ms;
			oldest_output_path = path;
		}
		if (i == 0 && !info.is_directory) primary_output_stamp = (u64)info.modified_unix_ms;
	}
	for (u32 i = 0; i < inputs->count; ++i) {
		Bob_Platform_File_Info info;
		String path = bob_path_string(builder->bob, inputs->items[i]);
		if (!bob_platform_file_info(path, &info)) {
			return (Bob_Rebuild_Decision){
				.reason = BOB_REBUILD_INPUT_MISSING,
				.path = path,
				.rebuild = true,
			};
		}
		if ((u64)info.modified_unix_ms > newest_input) {
			newest_input = (u64)info.modified_unix_ms;
			newest_input_path = path;
		}
	}

	if (outputs->count > 0) {
		Bob_Path output_path = outputs->items[0];
		String output = bob_path_string(builder->bob, output_path);
		Build_State_Task_Snapshot state_task;
		if (!build_state_get_task(&builder->state, output_path, &state_task)) return (Bob_Rebuild_Decision){ .reason = BOB_REBUILD_STATE_MISSING, .path = output, .rebuild = true };
		if (state_task.output_stamp != primary_output_stamp) return (Bob_Rebuild_Decision){ .reason = BOB_REBUILD_STATE_CHANGED, .path = output, .rebuild = true };
		if (memcmp(state_task.fingerprint.bytes, build_task->fingerprint.bytes, BOB_FINGERPRINT_SIZE) != 0) return (Bob_Rebuild_Decision){ .reason = BOB_REBUILD_FINGERPRINT_CHANGED, .path = output, .rebuild = true };
		for (u32 i = 0; i < state_task.dependencies.count; ++i) {
			Bob_Platform_File_Info info;
			String dependency = bob_path_string(builder->bob, state_task.dependencies.items[i]);
			if (!dependency.data || !bob_platform_file_info(dependency, &info)) return (Bob_Rebuild_Decision){ .reason = BOB_REBUILD_DEPENDENCY_MISSING, .path = dependency, .rebuild = true };
			if ((u64)info.modified_unix_ms > newest_input) {
				newest_input = (u64)info.modified_unix_ms;
				newest_input_path = dependency;
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

static void run_command(Bob_Node_Context *context, const Bob_Build_Task *task, Bob_Build_Completion *completion)
{
	Scratch scratch = begin_different_scratch(context->arena);
	String dependency_file = bob_path_string(context->bob, task->dependency_file);
	String execution_directory = bob_path_string(context->bob, task->execution_directory);
	if (task->tracks_dependencies) {
		platform_remove_file(dependency_file.data);
	}

	bob_platform_run_command(task->execution_command_line, context->arena,
		(Bob_Platform_Process_Options){
			.working_directory = execution_directory,
			.capture_stderr = true,
		}, &completion->process);
	if (task->tracks_dependencies) {
		String contents;
		b32 process_succeeded = completion->process.error_code == 0 &&
			completion->process.exit_code == 0;
		completion->dependency_state_valid = process_succeeded &&
			bob_platform_read_entire_file(scratch.arena, dependency_file, &contents) &&
			make_depfile_parse(context->arena, contents, &completion->dependencies);
		platform_remove_file(dependency_file.data);
	}
	end_scratch(scratch);
}

static Bob_Node_Result build_task_action(Bob_Node_Context *context, void *user_data)
{
	Bob_Builder *builder = context->execution_data;
	Bob_Build_Task *task = user_data;
	Bob_Build_Completion *completion;
	b32 succeeded;

	completion = arena_push_zero_aligned(context->arena, sizeof(*completion), _Alignof(Bob_Build_Completion));
	if (!completion || !builder || !task) return (Bob_Node_Result){0};
	completion->bob = context->bob;
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
	case BOB_REBUILD_STATE_CHANGED:
		logger_log(LOG_LEVEL_INFO, "explain", "%s: rebuilding because recorded state does not match the output: %s", name, decision->path.data);
		break;
	case BOB_REBUILD_FINGERPRINT_CHANGED:
		logger_log(LOG_LEVEL_INFO, "explain", "%s: rebuilding because task configuration changed: %s", name, decision->path.data);
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
	const Bob_Task_Desc *task = &build_task->task;
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
		String execution_directory = bob_path_string(completion->bob, build_task->execution_directory);
		logger_log(LOG_LEVEL_ERROR, "working-directory", "%s", execution_directory.data);
		end_scratch(scratch);
	}
	else if (completion->process.exit_code != 0) {
		logger_log(LOG_LEVEL_ERROR, bob_task_name(completion->node),
			"process exited with code %u", completion->process.exit_code);
		logger_log_at(1, LOG_LEVEL_ERROR, "command", "%s", command_line.data);
	}
}

static b32 resolve_dependency_paths(Bob *bob, Arena *arena, Bob_Path directory, String_Array source, Bob_Path_Array *result)
{
	*result = (Bob_Path_Array){0};
	if (source.count == 0) return true;
	result->items = arena_push_zero_aligned(arena, (u64)source.count * sizeof(*result->items), _Alignof(Bob_Path));
	if (!result->items) return false;
	for (u32 i = 0; i < source.count; ++i) {
		if (!bob_path_resolve(bob, directory, source.items[i], result->items + result->count)) return false;
		++result->count;
	}
	return true;
}

static void collect_build_state(Bob_Builder *builder, const Bob_Build_Completion *completion, b32 succeeded)
{
	Bob_Path output_path;
	String state_path;
	if (!completion->decision.rebuild || completion->task->outputs.count == 0 || builder->internal_error) return;
	output_path = completion->task->outputs.items[0];
	state_path = bob_path_string(builder->bob, builder->state_path);
	if (!succeeded || (completion->task->tracks_dependencies && !completion->dependency_state_valid)) {
		if (!build_state_append_remove(state_path, &builder->state, output_path)) builder->internal_error = true;
		else builder->state_changed = true;
		if (succeeded && completion->task->tracks_dependencies && !completion->dependency_state_valid) {
			log_warning("could not read compiler dependencies for %s",
				bob_task_name(completion->node));
		}
		return;
	}
	Scratch scratch = begin_scratch();
	Bob_Path_Array dependencies = {0};
	if (completion->task->tracks_dependencies && !resolve_dependency_paths(builder->bob, scratch.arena, completion->task->execution_directory, completion->dependencies, &dependencies)) {
		builder->internal_error = true;
		end_scratch(scratch);
		return;
	}
	Bob_Platform_File_Info info;
	String output = bob_path_string(builder->bob, output_path);
	u64 output_stamp = bob_platform_file_info(output, &info) && !info.is_directory ? (u64)info.modified_unix_ms : 0;
	if (!build_state_append_set(&builder->state_arena, state_path, builder->bob, &builder->state, output_path, dependencies, output_stamp, completion->task->fingerprint)) builder->internal_error = true;
	else builder->state_changed = true;
	end_scratch(scratch);
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
	collect_build_state(builder, completion, result.succeeded);
}

static b32 valid_task(const Bob_Task_Desc *task)
{
	return task && task->command_line.data &&
		(!task->inputs.count || task->inputs.items) &&
		(!task->outputs.count || task->outputs.items) &&
		(!task->include_directories.count || task->include_directories.items);
}

b32 bob_build(Bob *bob, Bob_Build_Params options)
{
	Bob_Builder builder = {0};
	Build_State_Load_Result load_result = BUILD_STATE_LOAD_MISSING;
	b32 result;

	if (!bob || options.worker_count == 0) return false;
	builder.bob = bob;
	if (!bob_path_resolve(bob, bob_build_root(bob), STRING_LITERAL(BOB_BUILD_STATE_PATH), &builder.state_path)) return false;
	builder.explain = options.explain;
	for (u32 i = 0; i < bob_node_count(bob); ++i) {
		Bob_Node *node = bob_node_at(bob, i);
		const Bob_Build_Task *task;
		if (bob_node_function(node) != build_task_action) continue;
		task = bob_node_user_data(node);
		if (!task || !valid_task(&task->task)) return false;
		if (task->outputs.count > 0) builder.state_tracking = true;
	}
	builder.state_arena = arena_create(MEGABYTES(64));
	arena_set_name(&builder.state_arena, "build state");
	if (!builder.state_arena.data) {
		result = false;
		goto cleanup;
	}
	if (!build_state_init(&builder.state)) {
		result = false;
		goto cleanup;
	}
	if (builder.state_tracking) {
		String state_path = bob_path_string(bob, builder.state_path);
		load_result = build_state_load(&builder.state_arena, bob, state_path, &builder.state);
		if (load_result == BUILD_STATE_LOAD_ERROR) {
			log_warning("could not load Bob build state");
			result = false;
			goto cleanup;
		}
		else if (load_result == BUILD_STATE_LOAD_INVALID) {
			log_warning("ignoring invalid Bob build state");
			build_state_clear(&builder.state);
		}
		if (load_result != BUILD_STATE_LOAD_OK) {
			if (!build_state_save(state_path, bob, &builder.state)) {
				log_warning("could not prepare Bob build state");
				result = false;
				goto cleanup;
			}
		}
	}

	result = bob_execute(bob, (Bob_Exec_Params){
		.worker_count = options.worker_count,
		.user_data = &builder,
		.completed = build_task_completed,
	});
	if (builder.internal_error) result = false;
	if (result && builder.state_tracking && builder.state_changed) {
		if (!build_state_save(bob_path_string(bob, builder.state_path), bob, &builder.state)) {
			log_warning("could not compact Bob build state");
			result = false;
		}
	}

cleanup:
	build_state_destroy(&builder.state);
	arena_destroy(&builder.state_arena);
	return result;
}

static void fingerprint_u32(blake3_hasher *hasher, u32 value)
{
	u8 bytes[4] = {
		(u8)value,
		(u8)(value >> 8),
		(u8)(value >> 16),
		(u8)(value >> 24),
	};
	blake3_hasher_update(hasher, bytes, sizeof(bytes));
}

static void fingerprint_u64(blake3_hasher *hasher, u64 value)
{
	u8 bytes[8] = {
		(u8)value,
		(u8)(value >> 8),
		(u8)(value >> 16),
		(u8)(value >> 24),
		(u8)(value >> 32),
		(u8)(value >> 40),
		(u8)(value >> 48),
		(u8)(value >> 56),
	};
	blake3_hasher_update(hasher, bytes, sizeof(bytes));
}

static b32 fingerprint_string(blake3_hasher *hasher, String value)
{
	if (!hasher || (!value.data && value.size) || value.size > SIZE_MAX) return false;
	fingerprint_u64(hasher, value.size);
	if (value.size) blake3_hasher_update(hasher, value.data, (size_t)value.size);
	return true;
}

static b32 fingerprint_paths(const Bob *bob, blake3_hasher *hasher, Bob_Path_Array paths)
{
	if (!bob || !hasher || (paths.count && !paths.items)) return false;
	fingerprint_u32(hasher, paths.count);
	for (u32 i = 0; i < paths.count; ++i) {
		String path = bob_path_string(bob, paths.items[i]);
		if (!path.data || !fingerprint_string(hasher, path)) return false;
	}
	return true;
}

static b32 build_task_fingerprint(const Bob *bob, const Bob_Build_Task *task, Bob_Fingerprint *result)
{
	static const char domain[] = "bob.task.fingerprint";
	blake3_hasher hasher;
	if (!bob || !task || !result) return false;
	blake3_hasher_init(&hasher);
	blake3_hasher_update(&hasher, domain, sizeof(domain) - 1);
	fingerprint_u32(&hasher, 1);
	if (!fingerprint_string(&hasher, task->task.command_line)) return false;
	if (!fingerprint_string(&hasher, bob_path_string(bob, task->execution_directory))) return false;
	if (!fingerprint_paths(bob, &hasher, task->inputs)) return false;
	if (!fingerprint_paths(bob, &hasher, task->outputs)) return false;
	if (!fingerprint_paths(bob, &hasher, task->include_directories)) return false;
	fingerprint_u32(&hasher, task->task.transparent ? 1 : 0);
	blake3_hasher_finalize(&hasher, result->bytes, sizeof(result->bytes));
	return true;
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
	result->items = bob_allocate(bob, (u64)source.count * sizeof(*result->items), _Alignof(String));
	if (!result->items) return false;
	for (u32 i = 0; i < source.count; ++i) {
		if (!source.items[i].data) return false;
		result->items[i] = bob_copy_string(bob, source.items[i]);
		if (!result->items[i].data) return false;
		++result->count;
	}
	return true;
}

static b32 resolve_task_paths(Bob *bob, Bob_Path directory, String_Array source, Bob_Path_Array *result)
{
	*result = (Bob_Path_Array){0};
	if (source.count == 0) return true;
	result->items = bob_allocate(bob, (u64)source.count * sizeof(*result->items), _Alignof(Bob_Path));
	if (!result->items) return false;
	for (u32 i = 0; i < source.count; ++i) {
		if (!bob_path_resolve(bob, directory, source.items[i], result->items + i)) return false;
		++result->count;
	}
	return true;
}

static Bob_Build_Task *build_task_create(Bob *bob, Bob_Task_Desc task, String fallback_name)
{
	Bob_Build_Task *copy = bob_allocate(bob, sizeof(*copy), _Alignof(Bob_Build_Task));
	Bob_Task_Desc *description;
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
	copy->execution_directory = bob_build_root(bob);
	if (description->working_directory.data && !bob_path_resolve(bob, copy->execution_directory, description->working_directory, &copy->execution_directory)) goto done;
	if (!resolve_task_paths(bob, copy->execution_directory, description->inputs, &copy->inputs) ||
		!resolve_task_paths(bob, copy->execution_directory, description->outputs, &copy->outputs) ||
		!resolve_task_paths(bob, copy->execution_directory, description->include_directories, &copy->include_directories)) goto done;
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
		arena_append_str(scratch.arena, bob_path_string(bob, copy->outputs.items[0]));
		arena_append_text(scratch.arena, ".d.tmp");
		String dependency_file = arena_string_from(scratch.arena, start);
		if (!bob_path_resolve(bob, copy->execution_directory, dependency_file, &copy->dependency_file) ||
			!compiler_command_add_dependencies(scratch.arena, &copy->compiler,
				description->command_line, bob_path_string(bob, copy->dependency_file), &augmented)) goto done;
		copy->execution_command_line = bob_copy_string(bob, augmented);
		if (!copy->execution_command_line.data) goto done;
	}
	if (!build_task_fingerprint(bob, copy, &copy->fingerprint)) goto done;
	valid = true;

done:
	end_scratch(scratch);
	return valid ? copy : NULL;
}

Bob_Error bob_add_task(Bob *bob, Bob_Task_Desc desc, Bob_Node **node_out)
{
	Bob_Build_Task *task;
	if (!bob || !desc.name.data || !node_out) return BOB_ERROR_INVALID_TASK;
	if (bob_is_prepared(bob)) return BOB_ERROR_ALREADY_PREPARED;
	task = build_task_create(bob, desc, (String){0});
	if (!task) return BOB_ERROR_OUT_OF_MEMORY;
	return bob_add_node(bob, (Bob_Node_Desc){
		.name = task->task.name,
		.function = build_task_action,
		.user_data = task,
	}, node_out);
}

Bob_Error bob_set_task(Bob *bob, Bob_Node *node, Bob_Task_Desc task)
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
	result = bob_set_node(bob, node, (Bob_Node_Desc){
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

Bob_Node_Status bob_task_state(const Bob_Node *node)
{
	return bob_node_state(node);
}

const Bob_Task_Desc *bob_get_task_desc(const Bob_Node *node)
{
	const Bob_Build_Task *task;
	if (!node || bob_node_function(node) != build_task_action) return NULL;
	task = bob_node_user_data(node);
	return task ? &task->task : NULL;
}
