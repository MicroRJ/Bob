#include "bob_build_internal.h"
#include "build_state.h"
#include "build_state_stream.h"
#include "compiler_command.h"
#include "logger.h"
#include "make_depfile.h"
#include "platform_adapter.h"
#include "platform.h"
#include "profiler.h"
#include "blake3.h"

#include <stdio.h>
#include <string.h>

#define BOB_BUILD_STATE_PATH ".bob/state"

struct Bob_Build
{
	Arena         arena;
	Bob          *graph;
	Bob_Interner *interner;
	Bob_Path      root;
};

static b32 bob_path_is_absolute(String path)
{
	return path.size > 0 && (path.data[0] == '/' || path.data[0] == '\\' ||
		(path.size >= 3 && path.data[1] == ':' && (path.data[2] == '/' || path.data[2] == '\\')));
}

static b32 bob_path_platform_absolute(Arena *arena, String path, String *result)
{
	String terminated = path;
	if (!string_is_terminated(terminated)) {
		terminated = arena_push_string_copy(arena, path);
		if (!terminated.data) return false;
	}
	Platform_String_Result query = platform_get_absolute_path(terminated.data, NULL, 0);
	if (query.error || query.required_capacity == 0) return false;
	char *data = arena_reserve(arena, query.required_capacity);
	if (!data) return false;
	Platform_String_Result filled = platform_get_absolute_path(terminated.data, data, query.required_capacity);
	if (filled.error || !arena_push(arena, query.required_capacity)) return false;
	*result = string_from_data(data, filled.size);
	return true;
}

static String bob_path_normalize_separators(String path)
{
	u64 write = 0;
	for (u64 read = 0; read < path.size; ++read) {
		char character = path.data[read] == '\\' ? '/' : path.data[read];
		b32 preserve_unc_prefix = write < 2 && read < 2 && character == '/';
		if (character == '/' && write > 0 && path.data[write - 1] == '/' && !preserve_unc_prefix) continue;
		path.data[write++] = character;
	}
	if (write >= 2 && path.data[1] == ':' && path.data[0] >= 'a' && path.data[0] <= 'z') path.data[0] -= 'a' - 'A';
	b32 root = write == 1 && path.data[0] == '/';
	b32 drive_root = write == 3 && path.data[1] == ':' && path.data[2] == '/';
	if (write > 0 && path.data[write - 1] == '/' && !root && !drive_root) --write;
	path.data[write] = 0;
	path.size = write;
	return path;
}

b32 bob_path_resolve(Bob_Build *build, Bob_Path directory, String source, Bob_Path *result)
{
	if (!build || !result || !source.data || source.size == 0) return false;
	Scratch scratch = begin_scratch();
	String candidate = source;
	if (!bob_path_is_absolute(source)) {
		String base = bob_path_string(build, directory);
		if (!base.data) goto failure;
		void *start = arena_top(scratch.arena);
		arena_append_str(scratch.arena, base);
		if (base.size && base.data[base.size - 1] != '/') arena_append_char(scratch.arena, '/');
		arena_append_str(scratch.arena, source);
		candidate = arena_string_from(scratch.arena, start);
		arena_finalize_string(scratch.arena, candidate);
	}
	String absolute;
	if (!bob_path_platform_absolute(scratch.arena, candidate, &absolute)) goto failure;
	absolute = bob_path_normalize_separators(absolute);
	if (!absolute.data || absolute.size == 0) goto failure;
	Bob_Atom atom = bob_interner_intern(build->interner, absolute);
	if (!atom.id) goto failure;
	*result = (Bob_Path){ atom };
	end_scratch(scratch);
	return true;

failure:
	end_scratch(scratch);
	return false;
}

String bob_path_string(const Bob_Build *build, Bob_Path path)
{
	return build ? bob_interner_string(build->interner, path.atom) : (String){0};
}

b32 bob_path_is_valid(Bob_Path path)
{
	return bob_atom_is_valid(path.atom);
}

Bob_Path bob_build_root(const Bob_Build *build)
{
	return build ? build->root : (Bob_Path){0};
}

Bob_Build *bob_build_create_at(String root)
{
	Arena arena = arena_create(0);
	if (!arena.data) return NULL;
	arena_set_name(&arena, "Bob build");

	Bob_Build *build = arena_push_zero_aligned(&arena, sizeof(*build), _Alignof(Bob_Build));
	if (!build) {
		logger_log_string(LOG_LEVEL_ERROR, "build", LIT("cannot initiate build, could not allocate memory"));
		arena_destroy(&arena);
		return NULL;
	}

	build->arena = arena;

	// NOTE(RJ) interner expects a stable arena pointer
	build->interner = bob_interner_create(&build->arena);
	build->graph = bob_create();

	if (!build->interner || !build->graph) {
		logger_log_string(LOG_LEVEL_ERROR, "build", LIT("cannot initiate build, could not allocate memory"));
		goto failure;
	}

	String absolute;
	Scratch scratch = begin_scratch();
	if (!root.data || root.size == 0 || !bob_path_platform_absolute(scratch.arena, root, &absolute)) {
		end_scratch(scratch);
		goto failure;
	}

	absolute = bob_path_normalize_separators(absolute);
	build->root.atom = bob_interner_intern(build->interner, absolute);
	end_scratch(scratch);
	if (!bob_path_is_valid(build->root)) goto failure;
	return build;

failure:
	bob_destroy(build->graph);
	bob_interner_destroy(build->interner);
	arena_destroy(&arena);
	return NULL;
}

Bob_Build *bob_build_create(void)
{
	Scratch scratch = begin_scratch();

	Bob_Build *build = NULL;

	String directory;
	if (bob_platform_current_directory(scratch.arena, &directory)) {
		build = bob_build_create_at(directory);
	}
	else {
		logger_log_string(LOG_LEVEL_ERROR, "build", LIT("cannot initiate build, platform api function 'bob_platform_current_directory' failed"));
	}

	end_scratch(scratch);
	return build;
}

void bob_build_destroy(Bob_Build *build)
{
	Arena arena;
	if (!build) return;
	bob_destroy(build->graph);
	bob_interner_destroy(build->interner);
	arena = build->arena;
	arena_destroy(&arena);
}

Bob *bob_build_graph(Bob_Build *build)
{
	return build ? build->graph : NULL;
}

const Bob *bob_build_graph_const(const Bob_Build *build)
{
	return build ? build->graph : NULL;
}

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


typedef struct Build_Task
{
	// Copy of the user's command line.
	String           command_line;
	// The processed command line, once other options are injected.
	String           execution_command_line;
	// Derived compiler metadata from the command line.
	Compiler_Command compiler;

	Bob_Path_Array   inputs;
	Bob_Path_Array   outputs;
	Bob_Path_Array   include_directories;
	Bob_Path         execution_directory;
	Bob_Path         dependency_file;

	Bob_Fingerprint  fingerprint;

	// Whether the compiler supports deps files and the task has any outputs.
	b32              tracks_dependencies;

	// If the task is transparent it is never marked as changed
	b32              transparent;
}
Build_Task;

typedef struct Bob_Build_Completion
{
	Bob_Build                  *build;
	Bob_Node                   *node;
	Build_Task             *task;
	Bob_Platform_Process_Result process;
	Bob_Path_Array              dependencies;
	Bob_Rebuild_Decision        decision;
	b32                         dependency_state_valid;
}
Bob_Build_Completion;

typedef struct Bob_Builder
{
	Bob_Build          *build;
	Bob                *bob;
	Bob_Path            state_path;
	Build_State         state;
	Arena               state_arena;
	void               *event_user_data;
	Bob_Event_Function *event;
	u32                 task_count;
	u32                 completed_task_count;
	b32                 state_tracking;
	b32                 state_changed;
	b32                 explain;
	b32                 internal_error;
}
Bob_Builder;

static Bob_Node_Result build_task_action(Bob_Node_Context *context, void *user_data);

static Bob_Rebuild_Decision task_rebuild_decision(Bob_Builder *builder, const Bob_Node *node, const Build_Task *task)
{
	const Bob_Path_Array *inputs = &task->inputs;
	const Bob_Path_Array *outputs = &task->outputs;
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
		String path = bob_path_string(builder->build, outputs->items[i]);
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
		String path = bob_path_string(builder->build, inputs->items[i]);
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
		String output = bob_path_string(builder->build, output_path);
		Build_State_Task_Snapshot state_task;
		if (!build_state_get_task(&builder->state, output_path, &state_task)) return (Bob_Rebuild_Decision){ .reason = BOB_REBUILD_STATE_MISSING, .path = output, .rebuild = true };
		if (state_task.output_stamp != primary_output_stamp) return (Bob_Rebuild_Decision){ .reason = BOB_REBUILD_STATE_CHANGED, .path = output, .rebuild = true };
		if (memcmp(state_task.fingerprint.bytes, task->fingerprint.bytes, BOB_FINGERPRINT_SIZE) != 0) return (Bob_Rebuild_Decision){ .reason = BOB_REBUILD_FINGERPRINT_CHANGED, .path = output, .rebuild = true };
		for (u32 i = 0; i < state_task.dependencies.count; ++i) {
			Bob_Platform_File_Info info;
			String dependency = bob_path_string(builder->build, state_task.dependencies.items[i]);
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

static void run_command(Bob_Node_Context *context, Bob_Build *build, const Build_Task *task, Bob_Build_Completion *completion)
{
	Scratch scratch = begin_different_scratch(context->arena);
	String dependency_file = bob_path_string(build, task->dependency_file);
	String execution_directory = bob_path_string(build, task->execution_directory);
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
		String_Array dependencies = {0};
		b32 process_succeeded = completion->process.error_code == 0 &&
			completion->process.exit_code == 0;
		completion->dependency_state_valid = process_succeeded &&
			bob_platform_read_entire_file(scratch.arena, dependency_file, &contents) &&
			make_depfile_parse(scratch.arena, contents, &dependencies);
		if (completion->dependency_state_valid && dependencies.count > 0) {
			completion->dependencies.items = arena_push_zero_aligned(context->arena,
				(u64)dependencies.count * sizeof(*completion->dependencies.items),
				_Alignof(Bob_Path));
			if (!completion->dependencies.items) completion->dependency_state_valid = false;
			for (u32 i = 0; completion->dependency_state_valid && i < dependencies.count; ++i) {
				if (!bob_path_resolve(build, task->execution_directory,
					dependencies.items[i], completion->dependencies.items + i)) {
					completion->dependency_state_valid = false;
				}
				else ++completion->dependencies.count;
			}
		}
		platform_remove_file(dependency_file.data);
	}
	end_scratch(scratch);
}

static Bob_Node_Result build_task_action(Bob_Node_Context *context, void *user_data)
{
	Bob_Builder *builder = context->execution_data;
	Build_Task *task = user_data;
	Bob_Build_Completion *completion;
	b32 succeeded;

	completion = arena_push_zero_aligned(context->arena, sizeof(*completion), _Alignof(Bob_Build_Completion));
	if (!completion || !builder || !task) return (Bob_Node_Result){0};
	completion->build = builder->build;
	completion->node = context->node;
	completion->task = task;
	{
		Profile_Scope scope = profile_scope_begin("incremental checks");
		completion->decision = task_rebuild_decision(builder, context->node, task);
		profile_scope_end(&scope);
	}
	if (completion->decision.rebuild) {
		Profile_Scope scope = profile_scope_begin("task processes");
		run_command(context, builder->build, task, completion);
		profile_scope_end(&scope);
	}
	succeeded = !completion->decision.rebuild ||
		(completion->process.error_code == 0 && completion->process.exit_code == 0);
	return (Bob_Node_Result){
		.output = completion,
		.succeeded = succeeded,
		.changed = succeeded && completion->decision.rebuild && !task->transparent,
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

static void report_completion(const Bob_Build_Completion *completion, u32 completed, u32 total)
{
	const Build_Task *build_task = completion->task;
	String command_line = build_task->command_line;
	b32 succeeded = !completion->decision.rebuild ||
		(completion->process.error_code == 0 && completion->process.exit_code == 0);
	char tag[64];

	if (!completion->decision.rebuild) {
		snprintf(tag, sizeof(tag), "%u/%u up-to-date", completed, total);
		logger_log_at(0, LOG_LEVEL_INFO, tag, "%s", bob_task_name(completion->node));
		logger_log_at(1, LOG_LEVEL_TRACE, "command", "%s", command_line.data);
		return;
	}
	if (completion->process.output.size > 0) {
		if (succeeded) logger_log_string_at(0, LOG_LEVEL_INFO,
			bob_task_name(completion->node), completion->process.output);
		else logger_log_string(LOG_LEVEL_ERROR, bob_task_name(completion->node),
			completion->process.output);
	}
	snprintf(tag, sizeof(tag), "%u/%u %s", completed, total, succeeded ? "succeeded" : "failed");
	logger_log_at(0, succeeded ? LOG_LEVEL_SUCCESS : LOG_LEVEL_ERROR, tag, "%s", bob_task_name(completion->node));
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
		String execution_directory = bob_path_string(completion->build, build_task->execution_directory);
		logger_log(LOG_LEVEL_ERROR, "working-directory", "%s", execution_directory.data);
		end_scratch(scratch);
	}
	else if (completion->process.exit_code != 0) {
		logger_log(LOG_LEVEL_ERROR, bob_task_name(completion->node),
			"process exited with code %u", completion->process.exit_code);
		logger_log_at(1, LOG_LEVEL_ERROR, "command", "%s", command_line.data);
	}
}

static void record_task_completion_state(Bob_Builder *builder, const Bob_Build_Completion *completion, b32 succeeded)
{
	if (!completion->decision.rebuild || completion->task->outputs.count == 0 || builder->internal_error) return;
	Bob_Path output_path = completion->task->outputs.items[0];
	String state_path = bob_path_string(builder->build, builder->state_path);

	if (!succeeded || (completion->task->tracks_dependencies && !completion->dependency_state_valid)) {
		if (!build_state_append_remove(state_path, &builder->state, output_path)) builder->internal_error = true;
		else builder->state_changed = true;

		if (succeeded && completion->task->tracks_dependencies && !completion->dependency_state_valid) {
			log_warning("could not read compiler dependencies for %s", bob_task_name(completion->node));
		}
		return;
	}
	Bob_Platform_File_Info info;
	String output = bob_path_string(builder->build, output_path);
	u64 output_stamp = bob_platform_file_info(output, &info) && !info.is_directory ? (u64)info.modified_unix_ms : 0;
	if (!build_state_append_set(&builder->state_arena, state_path, builder->build, &builder->state, output_path, completion->dependencies, output_stamp, completion->task->fingerprint)) builder->internal_error = true;
	else builder->state_changed = true;
}

static void build_task_event(Bob_Event event, void *user_data)
{
	Bob_Builder *builder = user_data;

	if (event.type == BOB_EVENT_COMPLETED && event.node && bob_node_function(event.node) == build_task_action)
	{
		Bob_Build_Completion *completion = event.result.output;
		++builder->completed_task_count;
		if (!completion) builder->internal_error = true;
		else
		{
			Profile_Scope scope = profile_scope_begin("report completion");
			if (builder->explain) report_explanation(completion);
			report_completion(completion, builder->completed_task_count, builder->task_count);
			profile_scope_end(&scope);
			record_task_completion_state(builder, completion, event.result.succeeded);
		}
	}
	if (builder->event) builder->event(event, builder->event_user_data);
}

static b32 valid_task(const Build_Task *task)
{
	return task && task->command_line.data && task->execution_command_line.data &&
		bob_path_is_valid(task->execution_directory) &&
		(!task->inputs.count || task->inputs.items) &&
		(!task->outputs.count || task->outputs.items) &&
		(!task->include_directories.count || task->include_directories.items);
}

b32 bob_build(Bob_Build *build, Bob_Build_Params options)
{
	Bob_Builder builder = {0};

	if (!build || !build->graph || options.worker_count == 0) return false;
	Bob *bob = build->graph;

	builder.build = build;
	builder.bob   = bob;
	builder.event = options.event;
	builder.event_user_data = options.user_data;

	if (!bob_path_resolve(build, bob_build_root(build), LIT(BOB_BUILD_STATE_PATH), &builder.state_path)) return false;
	builder.explain = options.explain;
	for (u32 i = 0; i < bob_node_count(bob); ++i) {
		Bob_Node *node = bob_node_at(bob, i);
		if (bob_node_function(node) != build_task_action) continue;
		const Build_Task *task = bob_node_user_data(node);
		if (!valid_task(task)) return false;
		++builder.task_count;
		if (task->outputs.count > 0) builder.state_tracking = true;
	}

	builder.state_arena = arena_create(MEGABYTES(64));
	arena_set_name(&builder.state_arena, "build state");

	b32 result = true;

	if (!builder.state_arena.data) {
		result = false;
		goto cleanup;
	}
	if (!build_state_init(&builder.state)) {
		result = false;
		goto cleanup;
	}
	if (builder.state_tracking) {
		String state_path = bob_path_string(build, builder.state_path);
		Build_State_Load_Result load_result = build_state_load(&builder.state_arena, build, state_path, &builder.state);
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
			if (!build_state_save(state_path, build, &builder.state)) {
				log_warning("could not prepare Bob build state");
				result = false;
				goto cleanup;
			}
		}
	}

	result = bob_execute(bob, (Bob_Exec_Params){
		.worker_count = options.worker_count,
		.user_data = &builder,
		.event = build_task_event,
	});
	if (builder.internal_error) result = false;
	if (result && builder.state_tracking && builder.state_changed) {
		if (!build_state_save(bob_path_string(build, builder.state_path), build, &builder.state)) {
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
		(u8)(value >>  0),
		(u8)(value >>  8),
		(u8)(value >> 16),
		(u8)(value >> 24),
	};
	blake3_hasher_update(hasher, bytes, sizeof(bytes));
}

static void fingerprint_u64(blake3_hasher *hasher, u64 value)
{
	u8 bytes[8] = {
		(u8)(value >>  0),
		(u8)(value >>  8),
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

static b32 fingerprint_paths(const Bob_Build *build, blake3_hasher *hasher, Bob_Path_Array paths)
{
	if (!build || !hasher || (paths.count && !paths.items)) return false;
	fingerprint_u32(hasher, paths.count);
	for (u32 i = 0; i < paths.count; ++i) {
		String path = bob_path_string(build, paths.items[i]);
		if (!path.data || !fingerprint_string(hasher, path)) return false;
	}
	return true;
}

static b32 build_task_fingerprint(const Bob_Build *build, const Build_Task *task, Bob_Fingerprint *result)
{
	static const char domain[] = "bob.task.fingerprint";
	blake3_hasher hasher;
	if (!build || !task || !result) return false;
	blake3_hasher_init(&hasher);
	blake3_hasher_update(&hasher, domain, sizeof(domain) - 1);
	fingerprint_u32(&hasher, 1);
	if (!fingerprint_string(&hasher, task->command_line)) return false;
	if (!fingerprint_string(&hasher, bob_path_string(build, task->execution_directory))) return false;
	if (!fingerprint_paths(build, &hasher, task->inputs)) return false;
	if (!fingerprint_paths(build, &hasher, task->outputs)) return false;
	if (!fingerprint_paths(build, &hasher, task->include_directories)) return false;
	fingerprint_u32(&hasher, task->transparent ? 1 : 0);
	blake3_hasher_finalize(&hasher, result->bytes, sizeof(result->bytes));
	return true;
}

static b32 copy_optional_string(Bob *bob, String source, String *result)
{
	if (!source.data) return source.size == 0;
	*result = bob_copy_string(bob, source);
	return result->data != NULL;
}

static b32 resolve_task_paths(Bob_Build *build, Bob_Path directory, String_Array source, Bob_Path_Array *result)
{
	Bob *bob = build ? build->graph : NULL;
	*result = (Bob_Path_Array){0};
	if (source.count == 0) return true;
	result->items = bob_allocate(bob, (u64)source.count * sizeof(*result->items), _Alignof(Bob_Path));
	if (!result->items) return false;
	for (u32 i = 0; i < source.count; ++i) {
		if (!bob_path_resolve(build, directory, source.items[i], result->items + i)) return false;
		++result->count;
	}
	return true;
}

static Build_Task *create_build_task(Bob_Build *build, Bob_Task_Desc desc)
{
	Bob *bob = build ? build->graph : NULL;
	Build_Task *task = bob_allocate(bob, sizeof(*task), _Alignof(Build_Task));
	Scratch scratch;
	Compiler_Command compiler;
	b32 valid = false;
	if (!task || !desc.command_line.data) return NULL;
	if (desc.working_directory.data && desc.working_directory.size == 0) return NULL;
	task->command_line = bob_copy_string(bob, desc.command_line);
	task->transparent = desc.transparent;
	if (!task->command_line.data) return NULL;

	scratch = begin_scratch();
	task->execution_directory = bob_build_root(build);
	if (desc.working_directory.data && !bob_path_resolve(build, task->execution_directory, desc.working_directory, &task->execution_directory)) goto done;
	if (!resolve_task_paths(build, task->execution_directory, desc.inputs, &task->inputs) ||
		!resolve_task_paths(build, task->execution_directory, desc.outputs, &task->outputs) ||
		!resolve_task_paths(build, task->execution_directory, desc.include_directories, &task->include_directories)) goto done;
	if (!compiler_command_parse(scratch.arena, task->command_line, &compiler)) {
		goto done;
	}
	task->compiler = compiler;
	task->compiler.executable = (String){0};
	if (!copy_optional_string(bob, compiler.executable, &task->compiler.executable)) {
		goto done;
	}
	task->execution_command_line = task->command_line;
	task->tracks_dependencies = task->outputs.count > 0 && task->compiler.can_add_make_dependencies;
	if (task->tracks_dependencies) {
		String augmented;
		void *start = arena_top(scratch.arena);
		arena_append_str(scratch.arena, bob_path_string(build, task->outputs.items[0]));
		arena_append_text(scratch.arena, ".d.tmp");
		String dependency_file = arena_string_from(scratch.arena, start);
		if (!bob_path_resolve(build, task->execution_directory, dependency_file, &task->dependency_file) ||
			!compiler_command_add_dependencies(scratch.arena, &task->compiler,
				task->command_line, bob_path_string(build, task->dependency_file), &augmented)) goto done;
		task->execution_command_line = bob_copy_string(bob, augmented);
		if (!task->execution_command_line.data) goto done;
	}
	if (!build_task_fingerprint(build, task, &task->fingerprint)) goto done;
	valid = true;

done:
	end_scratch(scratch);
	return valid ? task : NULL;
}

Bob_Error bob_add_task(Bob_Build *build, Bob_Task_Desc desc, Bob_Node **node_out)
{
	Bob *bob = build ? build->graph : NULL;
	Build_Task *task;
	if (!bob || !desc.name.data || !node_out) return BOB_ERROR_INVALID_TASK;
	if (bob_is_prepared(bob)) return BOB_ERROR_ALREADY_PREPARED;
	task = create_build_task(build, desc);
	if (!task) return BOB_ERROR_OUT_OF_MEMORY;
	return bob_add_node(bob, (Bob_Node_Desc){
		.name = desc.name,
		.function = build_task_action,
		.user_data = task,
	}, node_out);
}

Bob_Error bob_set_task(Bob_Build *build, Bob_Node *node, Bob_Task_Desc task)
{
	Bob *bob = build ? build->graph : NULL;
	Build_Task *copy;
	Bob_Error result;
	if (!bob || !node) return BOB_ERROR_INVALID_TASK;
	if (bob_is_prepared(bob)) return BOB_ERROR_ALREADY_PREPARED;
	for (u32 i = 0; i < bob_node_count(bob); ++i) {
		if (bob_node_at(bob, i) == node) goto found;
	}
	return BOB_ERROR_INVALID_TASK;

found:
	copy = create_build_task(build, task);
	if (!copy) return BOB_ERROR_OUT_OF_MEMORY;
	result = bob_set_node(bob, node, (Bob_Node_Desc){
		.name = task.name,
		.function = build_task_action,
		.user_data = copy,
	});
	return result;
}

u32 bob_task_count(const Bob_Build *build)
{
	return bob_node_count(bob_build_graph_const(build));
}

const char *bob_task_name(const Bob_Node *node)
{
	return bob_node_name(node);
}

Bob_Node_Status bob_task_state(const Bob_Node *node)
{
	return bob_node_state(node);
}
