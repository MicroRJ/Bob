#include "bob.h"
#include "c_include_scan.h"
#include "logger.h"
#include "platform/platform.h"
#include "profiler.h"

typedef struct Builder Builder;

typedef struct Worker
{
	Builder                 *builder;
	Platform_Thread         *thread;
	Bob_Node                *node;
	String                   command_line;
	Arena                    output;
	Platform_Process_Result  process;
	b32                      rebuilt;
	b32                      awaiting_acknowledgement;
}
Worker;

struct Builder
{
	Bob                *bob;
	Worker             *workers;
	u32                 worker_count;
	u32                 thread_count;
	u32                 arena_count;
	u32                 running;
	Bob_Node          **work;
	u32                 work_count;
	Worker            **completions;
	u32                 completion_count;
	Platform_Mutex     *mutex;
	Platform_Condition *work_available;
	Platform_Condition *completion_available;
	b32                 stopping;
};

static b32 task_needs_rebuild(const Bob_Node *node, const Bob_Task *task)
{
	u64 oldest_output = UINT64_MAX;
	u64 newest_input = 0;
	u32 i;

	if (task->outputs.count == 0) return true;

	for (i = 0; i < task->outputs.count; ++i) {
		Platform_File_Info info;
		if (!platform_file_info(task->outputs.items[i], &info)) return true;
		if (info.write_time < oldest_output) oldest_output = info.write_time;
	}
	for (i = 0; i < task->inputs.count; ++i) {
		Platform_File_Info info;
		if (!platform_file_info(task->inputs.items[i], &info)) return true;
		if (info.write_time > newest_input) newest_input = info.write_time;
	}

	{
		Profile_Scope scope = profile_scope_begin("include scanning");
		C_Include_Scan_Result scan;
		b32 scan_ok = c_include_scan(task->inputs, task->include_directories, task->command_line, &scan);
		profile_scope_end(&scope);
		if (!scan_ok || scan.unresolved_quoted_include) return true;
		if (scan.newest_write_time > newest_input) newest_input = scan.newest_write_time;
	}

	for (i = 0; i < bob_dependency_count(node); ++i) {
		Bob_Node *dependency = bob_dependency(node, i);
		if (!dependency || dependency->rebuilt) return true;
	}

	return newest_input > oldest_output;
}

static String command_executable(Arena *arena, String command_line)
{
	u64 start = 0;
	u64 end;
	while (start < command_line.size && (command_line.data[start] == ' ' || command_line.data[start] == '\t')) ++start;
	if (start == command_line.size) return (String){0};
	if (command_line.data[start] == '"') {
		++start;
		end = start;
		while (end < command_line.size && command_line.data[end] != '"') ++end;
	} else {
		end = start;
		while (end < command_line.size && command_line.data[end] != ' ' && command_line.data[end] != '\t') ++end;
	}
	if (end == start) return (String){0};
	return arena_push_string_copy(arena, string_slice(command_line, start, end - start));
}

static void request_stop_locked(Builder *builder)
{
	builder->stopping = true;
	if (builder->work_available) platform_condition_broadcast(builder->work_available);
	if (builder->completion_available) platform_condition_broadcast(builder->completion_available);
}

static void run_command(Worker *worker)
{
	arena_reset(&worker->output);
	platform_run_command(worker->command_line, &worker->output, (Platform_Process_Options){ .capture_stderr = true }, &worker->process);
}

static u32 worker_main(void *data)
{
	Worker *worker = data;
	Builder *builder = worker->builder;
	for (;;)
	{
		b32 stopping;

		platform_mutex_lock(builder->mutex);
		while (!builder->stopping && builder->work_count == 0) {
			if (!platform_condition_wait(builder->work_available, builder->mutex)) {
				log_fatal("failed waiting for worker queue");
				request_stop_locked(builder);
			}
		}
		if (builder->stopping) {
			platform_mutex_unlock(builder->mutex);
			break;
		}

		Bob_Node *node = builder->work[--builder->work_count];
		platform_mutex_unlock(builder->mutex);

		const Bob_Task *task = bob_get_task(node);

		Profile_Scope incremental_check_scope = profile_scope_begin("incremental checks");
		b32 needs_rebuild = task_needs_rebuild(node, task);
		profile_scope_end(&incremental_check_scope);
		worker->rebuilt = needs_rebuild;
		worker->node = node;
		worker->command_line = task->command_line;

		if (needs_rebuild)
		{
			Profile_Scope scope = profile_scope_begin("task processes");
			run_command(worker);
			profile_scope_end(&scope);
		}

		platform_mutex_lock(builder->mutex);
		worker->awaiting_acknowledgement = true;
		builder->completions[builder->completion_count++] = worker;
		platform_condition_signal(builder->completion_available);
		while (!builder->stopping && worker->awaiting_acknowledgement) {
			if (!platform_condition_wait(builder->work_available, builder->mutex)) {
				log_fatal("failed waiting for task completion acknowledgement");
				request_stop_locked(builder);
			}
		}
		stopping = builder->stopping;
		platform_mutex_unlock(builder->mutex);
		if (stopping) break;
	}
	destroy_global_scratch();
	return 0;
}

static void stop_workers(Builder *builder)
{
	if (builder->mutex) {
		platform_mutex_lock(builder->mutex);
		request_stop_locked(builder);
		platform_mutex_unlock(builder->mutex);
	}
	for (u32 i = 0; i < builder->thread_count; ++i) {
		platform_thread_join(builder->workers[i].thread);
		platform_thread_destroy(builder->workers[i].thread);
	}
	for (u32 i = 0; i < builder->arena_count; ++i) arena_destroy(&builder->workers[i].output);
}

static b32 report_completion(Worker *worker)
{
	ASSERT(worker);
	ASSERT(worker->node);

	worker->node->rebuilt = worker->rebuilt;
	if (!worker->rebuilt) {
		logger_log_at(0, LOG_LEVEL_INFO, "up-to-date", "%s", bob_task_name(worker->node));
		logger_log_at(1, LOG_LEVEL_TRACE, "command", "%s", bob_get_task(worker->node)->command_line.data);
		return true;
	}
	b32 succeeded = worker->process.error_code == 0 && worker->process.exit_code == 0;
	if (worker->process.output.size > 0) logger_log_string_at(2, LOG_LEVEL_INFO, bob_task_name(worker->node), worker->process.output);
	logger_log_at(0, succeeded ? LOG_LEVEL_SUCCESS : LOG_LEVEL_ERROR, succeeded ? "succeeded" : "failed", "%s", bob_task_name(worker->node));
	if (succeeded) {
		logger_log_at(1, LOG_LEVEL_TRACE, "command", "%s", worker->command_line.data);
		logger_log_at(1, LOG_LEVEL_TRACE, "exit-code", "0");
	}
	if (worker->process.error_code != 0) {
		Scratch scratch = begin_scratch();
		String executable;
		String working_directory;
		logger_log(LOG_LEVEL_ERROR, bob_task_name(worker->node), "%s", worker->process.launched ? "process error" : "failed to start process");
		logger_log(LOG_LEVEL_ERROR, "command", "%s", worker->command_line.data);
		{
			String message;
			if (platform_error_message(worker->process.error_code, scratch.arena, &message)) logger_log(LOG_LEVEL_ERROR, "os", "error %u: %s", worker->process.error_code, message.data);
			else logger_log(LOG_LEVEL_ERROR, "os", "error %u", worker->process.error_code);
		}
		executable = command_executable(scratch.arena, worker->command_line);
		if (executable.data) logger_log(LOG_LEVEL_ERROR, "executable", "%s (%s)", executable.data, platform_executable_resolves(executable) ? "found" : "not found in current directory or PATH");
		else logger_log(LOG_LEVEL_ERROR, "executable", "unable to parse from command");
		if (platform_current_directory(scratch.arena, &working_directory)) logger_log(LOG_LEVEL_ERROR, "working-directory", "%s", working_directory.data);
		end_scratch(scratch);
	} else if (worker->process.exit_code != 0) {
		logger_log(LOG_LEVEL_ERROR, bob_task_name(worker->node), "process exited with code %u", worker->process.exit_code);
		logger_log(LOG_LEVEL_ERROR, "command", "%s", worker->command_line.data);
	}
	return succeeded;
}

b32 bob_build(Bob *bob, u32 worker_count)
{
	if (!bob || worker_count == 0) return false;

	u32 task_count = bob_task_count(bob);
	for (u32 i = 0; i < task_count; ++i) {
		const Bob_Task *task = bob_get_task(bob_node_at(bob, i));
		if (!task || !task->command_line.data) return false;
		if ((task->inputs.count && !task->inputs.items) || (task->outputs.count && !task->outputs.items) || (task->include_directories.count && !task->include_directories.items)) return false;
	}

	Bob_Error prepare_result;
	{
		Profile_Scope scope = profile_scope_begin("prepare Bob");
		prepare_result = bob_prepare(bob);
		profile_scope_end(&scope);
	}
	if (prepare_result != BOB_OK) {
		log_error("unable to prepare Bob: %s", bob_error_string(prepare_result));
		return false;
	}
	if (bob_is_finished(bob)) return true;
	if (worker_count > task_count) worker_count = task_count;

	Builder builder = { .bob = bob, .worker_count = worker_count };
	Scratch scratch = begin_scratch();
	b32 internal_error = false;
	builder.workers = arena_push_zero_aligned(scratch.arena, worker_count * sizeof(*builder.workers), _Alignof(Worker));
	builder.work = arena_push_zero_aligned(scratch.arena, worker_count * sizeof(*builder.work), _Alignof(Bob_Node *));
	builder.completions = arena_push_zero_aligned(scratch.arena, worker_count * sizeof(*builder.completions), _Alignof(Worker *));
	builder.mutex = platform_mutex_create();
	builder.work_available = platform_condition_create();
	builder.completion_available = platform_condition_create();
	if (!builder.workers || !builder.work || !builder.completions || !builder.mutex || !builder.work_available || !builder.completion_available) {
		internal_error = true;
		goto cleanup;
	}

	for (u32 i = 0; i < worker_count; ++i) {
		Worker *worker = &builder.workers[i];
		worker->builder = &builder;
		worker->output = arena_create(MEGABYTES(256));
		if (!worker->output.data) {
			internal_error = true;
			break;
		}
		++builder.arena_count;
		worker->thread = platform_thread_create(worker_main, worker);
		if (!worker->thread) {
			internal_error = true;
			break;
		}
		++builder.thread_count;
	}
	if (internal_error) goto cleanup;

	while (!bob_is_finished(bob))
	{
		platform_mutex_lock(builder.mutex);
		Bob_Node *node;
		while (builder.running < builder.worker_count && bob_take_ready(bob, &node))
		{
			builder.work[builder.work_count++] = node;
			++builder.running;
		}
		platform_condition_broadcast(builder.work_available);
		platform_mutex_unlock(builder.mutex);

		if (internal_error || bob_is_finished(bob)) break;

		if (builder.running == 0) {
			internal_error = true;
			break;
		}

		platform_mutex_lock(builder.mutex);
		while (!builder.stopping && builder.completion_count == 0) {
			if (!platform_condition_wait(builder.completion_available, builder.mutex)) {
				log_fatal("failed waiting for worker completion");
				request_stop_locked(&builder);
			}
		}

		Worker *worker = NULL;
		if (builder.completion_count > 0) {
			worker = builder.completions[--builder.completion_count];
		}
		platform_mutex_unlock(builder.mutex);

		if (!worker) {
			internal_error = true;
			break;
		}

		Profile_Scope report_completion_scope = profile_scope_begin("report_completion_scope");
		b32 succeeded = report_completion(worker);
		if (bob_complete(bob, worker->node, succeeded) != BOB_OK) {
			internal_error = true;
		}
		profile_scope_end(&report_completion_scope);

		platform_mutex_lock(builder.mutex);
		worker->node = NULL;
		worker->awaiting_acknowledgement = false;
		platform_condition_broadcast(builder.work_available);
		platform_mutex_unlock(builder.mutex);

		--builder.running;
		if (internal_error) break;
	}

cleanup:
	stop_workers(&builder);
	platform_condition_destroy(builder.completion_available);
	platform_condition_destroy(builder.work_available);
	platform_mutex_destroy(builder.mutex);
	end_scratch(scratch);
	return !internal_error && !bob_has_failed(bob);
}
