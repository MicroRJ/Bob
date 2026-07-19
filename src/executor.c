#include "executor.h"
#include "c_include_scan.h"
#include "logger.h"
#include "platform/platform.h"
#include "profiler.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <string.h>

typedef struct Worker
{
   HANDLE thread;
   HANDLE start_event;
   HANDLE done_event;

   Node_Id node;
   String command_line;
   Arena output;
   Platform_Process_Result process;

   b32 stopping;
   b32 busy;
} Worker;

typedef struct Task_Runtime {
   b32 rebuilt;
} Task_Runtime;

static String command_executable(Arena *arena, String command_line)
{
   u64 start = 0;
   u64 end;

   while (start < command_line.size && (command_line.data[start] == ' ' || command_line.data[start] == '\t')) {
      ++start;
   }
   if (start == command_line.size) { return (String) {0}; }

   if (command_line.data[start] == '"')
   {
      ++start;
      end = start;
      while (end < command_line.size && command_line.data[end] != '"') {
         ++end;
      }
   }
   else
   {
      end = start;
      while (end < command_line.size && command_line.data[end] != ' ' && command_line.data[end] != '\t') {
         ++end;
      }
   }
   if (end == start) { return (String) {0}; }
   return arena_push_string_copy(arena, string_slice(command_line, start, end - start));
}

static b32 task_needs_rebuild(const Graph *graph, Node_Id node, const Task *task, const Task_Runtime *runtime)
{
   u64 oldest_output = UINT64_MAX;
   u64 newest_input = 0;
   u32 i;

   if (task->outputs.count == 0) return true;

   for (i = 0; i < task->outputs.count; ++i)
   {
      Platform_File_Info info;
		if (!platform_file_info(task->outputs.items[i], &info)) return true;
      if (info.write_time < oldest_output) { oldest_output = info.write_time; }
   }
   for (i = 0; i < task->inputs.count; ++i)
   {
      Platform_File_Info info;
		if (!platform_file_info(task->inputs.items[i], &info)) return true;
      if (info.write_time > newest_input) { newest_input = info.write_time; }
   }
   {
      C_Include_Scan_Result scan;
      b32 scan_ok;
      Profile_Scope scope = profile_scope_begin("include scanning");
      scan_ok = c_include_scan(task->inputs, task->include_directories, task->command_line, &scan);
      profile_scope_end(&scope);
      if (!scan_ok || scan.unresolved_quoted_include) {
         return true;
      }
      if (scan.newest_write_time > newest_input) {
         newest_input = scan.newest_write_time;
      }
   }
   for (i = 0; i < graph_node_dependency_count(graph, node); ++i) {
      Node_Id dependency = graph_node_dependency(graph, node, i);
      if (dependency == GRAPH_INVALID_TASK || runtime[dependency].rebuilt) return true;
   }
   return newest_input > oldest_output;
}

static void run_command(Worker *worker)
{
   arena_reset(&worker->output);
	platform_run_command(worker->command_line, &worker->output, (Platform_Process_Options){ .capture_stderr = true }, &worker->process);
}

static DWORD WINAPI worker_main(void *parameter)
{
   Worker *worker = parameter;

   for (;;)
   {
      WaitForSingleObject(worker->start_event, INFINITE);
      if (worker->stopping) {
		 	destroy_global_scratch();
         return 0;
      }

      {
         Profile_Scope scope = profile_scope_begin("task processes");
         run_command(worker);
         profile_scope_end(&scope);
      }
      SetEvent(worker->done_event);
   }
}

static void stop_workers(Worker *workers, u32 count)
{
   u32 i;

   for (i = 0; i < count; ++i) {
      workers[i].stopping = true;
      SetEvent(workers[i].start_event);
   }
   for (i = 0; i < count; ++i)
   {
      WaitForSingleObject(workers[i].thread, INFINITE);
      CloseHandle(workers[i].thread);
      CloseHandle(workers[i].start_event);
      CloseHandle(workers[i].done_event);
      arena_destroy(&workers[i].output);
   }
}

b32 executor_run(Graph *graph, u32 worker_count)
{
   Worker *workers;
   HANDLE *done_events;
   Task_Runtime *runtime;
   Graph_Error prepare_result;
   u32 created = 0;
   u32 running = 0;
   u32 task_count;
   b32 internal_error = false;
   u32 i;

   if (!graph || worker_count == 0) {
      return false;
   }
   task_count = graph_node_count(graph);
   for (i = 0; i < task_count; ++i)
   {
      const Task *task = graph_node_data(graph, (Node_Id)i);
      if (!task || !task->command_line.data) {
         return false;
      }
      if ((task->inputs.count && !task->inputs.items) || (task->outputs.count && !task->outputs.items) || (task->include_directories.count && !task->include_directories.items)) {
         return false;
      }
   }

   {
      Profile_Scope scope = profile_scope_begin("prepare graph");
      prepare_result = graph_prepare(graph);
      profile_scope_end(&scope);
   }
   if (prepare_result != GRAPH_OK) {
      log_error("unable to prepare graph: %s", graph_error_str(prepare_result));
      return false;
   }
   if (graph_is_finished(graph)) {
      return true;
   }

   if (worker_count > task_count) {
      worker_count = task_count;
   }
   if (worker_count > MAXIMUM_WAIT_OBJECTS) {
      worker_count = MAXIMUM_WAIT_OBJECTS;
   }

   workers = calloc(worker_count, sizeof(*workers));
   done_events = malloc(worker_count * sizeof(*done_events));
   runtime = calloc(task_count, sizeof(*runtime));
   if (!workers || !done_events || !runtime)
   {
      free(workers);
      free(done_events);
      free(runtime);
      return false;
   }

   for (i = 0; i < worker_count; ++i)
   {
      Worker *worker = &workers[i];
      worker->node = GRAPH_INVALID_TASK;
      worker->output = arena_create(MEGABYTES(256));
      worker->start_event = CreateEventA(NULL, FALSE, FALSE, NULL);
      worker->done_event = CreateEventA(NULL, FALSE, FALSE, NULL);
      if (!worker->output.data || !worker->start_event || !worker->done_event) {
         internal_error = true;
         break;
      }
      worker->thread = CreateThread(NULL, 0, worker_main, worker, 0, NULL);
      if (!worker->thread) {
         internal_error = true;
         break;
      }
      done_events[i] = worker->done_event;
      ++created;
   }

   if (internal_error)
   {
      if (i < worker_count)
      {
         if (workers[i].start_event) { CloseHandle(workers[i].start_event); }
         if (workers[i].done_event) { CloseHandle(workers[i].done_event); }
         arena_destroy(&workers[i].output);
      }
      stop_workers(workers, created);
      free(done_events);
      free(workers);
      free(runtime);
      return false;
   }

   while (!graph_is_finished(graph))
   {
      for (i = 0; i < worker_count; ++i)
      {
         Node_Id node;
         Worker *worker = &workers[i];

         while (!worker->busy && graph_take_ready(graph, &node))
         {
            const Task *task = graph_node_data(graph, node);
            b32 needs_rebuild;
            Profile_Scope scope = profile_scope_begin("incremental checks");
            needs_rebuild = task_needs_rebuild(graph, node, task, runtime);
            profile_scope_end(&scope);
            if (!needs_rebuild)
            {
				logger_log_at(0, LOG_LEVEL_INFO, "up-to-date", "%s", graph_node_name(graph, node));
				logger_log_at(1, LOG_LEVEL_TRACE, "command", "%s", task->command_line.data);
               if (graph_complete(graph, node, true) != GRAPH_OK) {
                  internal_error = true;
                  break;
               }
               continue;
            }

            worker->node = node;
            worker->command_line = task->command_line;
            worker->busy = true;
            runtime[node].rebuilt = true;
            ++running;
            SetEvent(worker->start_event);
         }
      }

      if (internal_error) { break; }
      if (graph_is_finished(graph)) { break; }

      if (running == 0) {
         internal_error = true;
         break;
      }

      {
         DWORD wait_result = WaitForMultipleObjects(worker_count, done_events, FALSE, INFINITE);
         u32 worker_index;
         Worker *worker;
         b32 succeeded;

         if (wait_result < WAIT_OBJECT_0 || wait_result >= WAIT_OBJECT_0 + worker_count) {
            internal_error = true;
            break;
         }

         worker_index = wait_result - WAIT_OBJECT_0;
         worker = &workers[worker_index];

         succeeded = worker->process.error_code == 0 && worker->process.exit_code == 0;
		 if (worker->process.output.size > 0) {
			logger_log_string_at(2, LOG_LEVEL_INFO, graph_node_name(graph, worker->node),
				worker->process.output);
		 }
		 logger_log_at(0,
               succeeded ? LOG_LEVEL_SUCCESS : LOG_LEVEL_ERROR,
               succeeded ? "succeeded" : "failed",
               "%s",
               graph_node_name(graph, worker->node)
		 );
		 if (succeeded) {
			logger_log_at(1, LOG_LEVEL_TRACE, "command", "%s", worker->command_line.data);
			logger_log_at(1, LOG_LEVEL_TRACE, "exit-code", "0");
		 }
         if (worker->process.error_code != 0)
         {
            Scratch scratch = begin_scratch();

			logger_log(LOG_LEVEL_ERROR, graph_node_name(graph, worker->node), "%s",
				worker->process.launched ? "process error" : "failed to start process");
			logger_log(LOG_LEVEL_ERROR, "command", "%s", worker->command_line.data);

            {
				String message;
				if (platform_error_message(worker->process.error_code, scratch.arena, &message)) {
					logger_log(LOG_LEVEL_ERROR, "os", "error %u: %s",
						worker->process.error_code, message.data);
				} else {
					logger_log(LOG_LEVEL_ERROR, "os", "error %u", worker->process.error_code);
				}
			}

            String executable = command_executable(scratch.arena, worker->command_line);
            if (executable.data)
            {
               b32 executable_resolves = platform_executable_resolves(executable);
				logger_log(LOG_LEVEL_ERROR, "executable", "%s (%s)", executable.data,
					executable_resolves ? "found" : "not found in current directory or PATH");
            }
            else {
				logger_log(LOG_LEVEL_ERROR, "executable", "unable to parse from command");
            }

            String working_directory;
            b32 has_working_directory = platform_current_directory(scratch.arena, &working_directory);

            if (has_working_directory) {
				logger_log(LOG_LEVEL_ERROR, "working-directory", "%s", working_directory.data);
            }
            end_scratch(scratch);
         }
         else if (worker->process.exit_code != 0)
         {
			logger_log(LOG_LEVEL_ERROR, graph_node_name(graph, worker->node),
				"process exited with code %u", worker->process.exit_code);
			logger_log(LOG_LEVEL_ERROR, "command", "%s", worker->command_line.data);
         }

         if (graph_complete(graph, worker->node, succeeded) != GRAPH_OK) {
            internal_error = true;
            break;
         }

         worker->busy = false;
         worker->node = GRAPH_INVALID_TASK;
         --running;
      }
   }

   stop_workers(workers, worker_count);
   free(done_events);
   free(workers);
   free(runtime);
   return !internal_error && !graph_has_failed(graph);
}
