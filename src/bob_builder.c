#include "bob.h"
#include "c_include_scan.h"
#include "logger.h"
#include "platform/platform.h"
#include "profiler.h"

#include <stdlib.h>

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

static void request_stop_locked(Bob *bob)
{
	bob->stopping = true;
	if (bob->work_available) platform_condition_broadcast(bob->work_available);
	if (bob->completion_available) platform_condition_broadcast(bob->completion_available);
}

static void run_command(Bob_Worker *worker)
{
	arena_reset(&worker->output);
	platform_run_command(worker->command_line, &worker->output, (Platform_Process_Options){ .capture_stderr = true }, &worker->process);
}

static u32 worker_main(void *data)
{
	Bob_Worker *worker = data;
	Bob *bob = worker->bob;
	for (;;)
	{
		b32 stopping;

		platform_mutex_lock(bob->mutex);
		while (!bob->stopping && bob->work_count == 0) {
			if (!platform_condition_wait(bob->work_available, bob->mutex)) {
				log_fatal("failed waiting for worker queue");
				request_stop_locked(bob);
			}
		}
		if (bob->stopping) {
			platform_mutex_unlock(bob->mutex);
			break;
		}

		Bob_Node *node = bob->work[--bob->work_count];
		platform_mutex_unlock(bob->mutex);

		const Bob_Task *task = bob_get_task(node);
		worker->node = node;
		worker->command_line = task->command_line;

		Profile_Scope scope = profile_scope_begin("task processes");
		run_command(worker);
		profile_scope_end(&scope);

		platform_mutex_lock(bob->mutex);
		worker->awaiting_acknowledgement = true;
		bob->completions[bob->completion_count++] = worker;
		platform_condition_signal(bob->completion_available);
		while (!bob->stopping && worker->awaiting_acknowledgement) {
			if (!platform_condition_wait(bob->work_available, bob->mutex)) {
				log_fatal("failed waiting for task completion acknowledgement");
				request_stop_locked(bob);
			}
		}
		stopping = bob->stopping;
		platform_mutex_unlock(bob->mutex);
		if (stopping) break;
	}
	destroy_global_scratch();
	return 0;
}

static void stop_workers(Bob *bob, Bob_Worker *workers, u32 thread_count, u32 arena_count)
{
	u32 i;
	if (bob->mutex) {
		platform_mutex_lock(bob->mutex);
		request_stop_locked(bob);
		platform_mutex_unlock(bob->mutex);
	}
	for (i = 0; i < thread_count; ++i) {
		platform_thread_join(workers[i].thread);
		platform_thread_destroy(workers[i].thread);
	}
	for (i = 0; i < arena_count; ++i) arena_destroy(&workers[i].output);
}

static b32 report_completion(Bob_Worker *worker)
{
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
	Bob_Worker *workers = NULL;
	Bob_Error prepare_result;
	u32 thread_count = 0;
	u32 arena_count = 0;
	u32 running = 0;
	u32 task_count;
	b32 internal_error = false;
	u32 i;

	if (!bob || worker_count == 0) return false;
	task_count = bob_task_count(bob);
	for (i = 0; i < task_count; ++i) {
		const Bob_Task *task = bob_get_task(bob_node_at(bob, i));
		if (!task || !task->command_line.data) return false;
		if ((task->inputs.count && !task->inputs.items) || (task->outputs.count && !task->outputs.items) || (task->include_directories.count && !task->include_directories.items)) return false;
	}
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

	workers = calloc(worker_count, sizeof(*workers));
	bob->work = calloc(worker_count, sizeof(*bob->work));
	bob->completions = calloc(worker_count, sizeof(*bob->completions));
	bob->mutex = platform_mutex_create();
	bob->work_available = platform_condition_create();
	bob->completion_available = platform_condition_create();
	if (!workers || !bob->work || !bob->completions || !bob->mutex || !bob->work_available || !bob->completion_available) {
		internal_error = true;
		goto cleanup;
	}

	for (i = 0; i < worker_count; ++i) {
		workers[i].bob = bob;
		workers[i].node = NULL;
		workers[i].output = arena_create(MEGABYTES(256));
		if (!workers[i].output.data) {
			internal_error = true;
			break;
		}
		++arena_count;
		workers[i].thread = platform_thread_create(worker_main, &workers[i]);
		if (!workers[i].thread) {
			internal_error = true;
			break;
		}
		++thread_count;
	}
	if (internal_error) goto cleanup;

	while (!bob_is_finished(bob))
	{
		Bob_Node *node;
		while (running < worker_count && bob_take_ready(bob, &node))
		{
			const Bob_Task *task = bob_get_task(node);

			Profile_Scope scope = profile_scope_begin("incremental checks");
			b32 needs_rebuild = task_needs_rebuild(node, task);
			profile_scope_end(&scope);

			if (!needs_rebuild) {
				logger_log_at(0, LOG_LEVEL_INFO, "up-to-date", "%s", bob_task_name(node));
				logger_log_at(1, LOG_LEVEL_TRACE, "command", "%s", task->command_line.data);
				if (bob_complete(bob, node, true) != BOB_OK) {
					internal_error = true;
					break;
				}
				continue;
			}

			node->rebuilt = true;

			platform_mutex_lock(bob->mutex);
			bob->work[bob->work_count++] = node;
			platform_condition_broadcast(bob->work_available);
			platform_mutex_unlock(bob->mutex);

			++running;
		}

		if (internal_error || bob_is_finished(bob)) break;

		if (running == 0) {
			internal_error = true;
			break;
		}

		{
			Bob_Worker *worker = NULL;
			b32 succeeded;
			platform_mutex_lock(bob->mutex);
			while (!bob->stopping && bob->completion_count == 0) {
				if (!platform_condition_wait(bob->completion_available, bob->mutex)) {
					log_fatal("failed waiting for worker completion");
					request_stop_locked(bob);
				}
			}
			if (bob->completion_count > 0) {
				worker = bob->completions[--bob->completion_count];
			}
			platform_mutex_unlock(bob->mutex);
			if (!worker) {
				internal_error = true;
				break;
			}

			succeeded = report_completion(worker);
			if (bob_complete(bob, worker->node, succeeded) != BOB_OK) internal_error = true;
			platform_mutex_lock(bob->mutex);
			worker->node = NULL;
			worker->awaiting_acknowledgement = false;
			platform_condition_broadcast(bob->work_available);
			platform_mutex_unlock(bob->mutex);
			--running;
			if (internal_error) break;
		}
	}

cleanup:
	stop_workers(bob, workers, thread_count, arena_count);
	platform_condition_destroy(bob->completion_available);
	platform_condition_destroy(bob->work_available);
	platform_mutex_destroy(bob->mutex);
	free(bob->completions);
	free(bob->work);
	free(workers);
	bob->completion_available = NULL;
	bob->work_available = NULL;
	bob->mutex = NULL;
	bob->completions = NULL;
	bob->work = NULL;
	bob->completion_count = 0;
	bob->work_count = 0;
	bob->stopping = false;
	return !internal_error && !bob_has_failed(bob);
}
