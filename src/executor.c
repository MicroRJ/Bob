#include "executor.h"
#include "c_include_scan.h"
#include "logger.h"
#include "platform.h"
#include "profiler.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Worker
{
   HANDLE thread;
   HANDLE start_event;
   HANDLE done_event;

   Node_Id node;
   const char *command_line;
   Arena output;
   DWORD exit_code;
   DWORD launch_error;
   b32 launched;

   b32 stopping;
   b32 busy;
} Worker;

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

static b32 task_needs_rebuild(const Graph *graph, Node_Id node, const Task *task, const b32 *rebuilt)
{
   u64 oldest_output = UINT64_MAX;
   u64 newest_input = 0;
   u32 i;

   if (task->output_count == 0) return true;

   for (i = 0; i < task->output_count; ++i)
   {
      Platform_File_Info info;
      if (!platform_file_info(task->outputs[i], &info)) return true;
      if (info.write_time < oldest_output) { oldest_output = info.write_time; }
   }
   for (i = 0; i < task->input_count; ++i)
   {
      Platform_File_Info info;
      if (!platform_file_info(task->inputs[i], &info)) return true;
      if (info.write_time > newest_input) { newest_input = info.write_time; }
   }
   {
      C_Include_Scan_Result scan;
      b32 scan_ok;
      Profile_Scope scope = profile_scope_begin("include scanning");
      scan_ok = c_include_scan(
         task->inputs,
         task->input_count,
         task->include_directories,
         task->include_directory_count,
         task->command_line,
         &scan
      );
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
      if (dependency == GRAPH_INVALID_TASK || rebuilt[dependency]) return true;
   }
   return newest_input > oldest_output;
}

static b32 output_append(Arena *output, const void *data, size_t size) {
   if ((u64)size > output->capacity - output->used) return false;
   return arena_push_copy(output, (u64)size, data) != NULL;
}

static void run_command(Worker *worker)
{
   SECURITY_ATTRIBUTES security = { sizeof(security), NULL, TRUE };
   STARTUPINFOA startup = {0};
   PROCESS_INFORMATION process = {0};
   HANDLE output_read = NULL;
   HANDLE output_write = NULL;
   char *command_line = NULL;
   size_t command_size;
   char read_buffer[4096];
   DWORD bytes_read;
   b32 output_ok = true;

   arena_reset(&worker->output);
   worker->exit_code = UINT32_MAX;
   worker->launch_error = ERROR_SUCCESS;
   worker->launched = false;

   if (!CreatePipe(&output_read, &output_write, &security, 0)) {
      worker->launch_error = GetLastError();
      return;
   }
   if (!SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0))
   {
      worker->launch_error = GetLastError();
      CloseHandle(output_read);
      CloseHandle(output_write);
      return;
   }

   command_size = strlen(worker->command_line) + 1;
   command_line = malloc(command_size);
   if (!command_line)
   {
      worker->launch_error = ERROR_NOT_ENOUGH_MEMORY;
      CloseHandle(output_read);
      CloseHandle(output_write);
      return;
   }
   memcpy(command_line, worker->command_line, command_size);

   startup.cb = sizeof(startup);
   startup.dwFlags = STARTF_USESTDHANDLES;
   startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
   startup.hStdOutput = output_write;
   startup.hStdError = output_write;

   if (!CreateProcessA(NULL, command_line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process))
   {
      worker->launch_error = GetLastError();
      free(command_line);
      CloseHandle(output_read);
      CloseHandle(output_write);
      return;
   }
   worker->launched = true;

   free(command_line);
   CloseHandle(output_write);
   output_write = NULL;

   while (ReadFile(output_read, read_buffer, sizeof(read_buffer), &bytes_read, NULL))
   {
      if (bytes_read > 0 && !output_append(&worker->output, read_buffer, bytes_read)) {
         output_ok = false;
      }
   }

   WaitForSingleObject(process.hProcess, INFINITE);
   if (!GetExitCodeProcess(process.hProcess, &worker->exit_code)) {
      worker->launch_error = GetLastError();
   }
   else if (!output_ok) {
      worker->launch_error = ERROR_NOT_ENOUGH_MEMORY;
   }

   CloseHandle(output_read);
   CloseHandle(process.hThread);
   CloseHandle(process.hProcess);
}

static void print_windows_error(DWORD error)
{
   char message[512];
   DWORD length = FormatMessageA(
      FORMAT_MESSAGE_FROM_SYSTEM |
      FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL,
      error,
      0,
      message,
      sizeof(message),
      NULL
   );

   while (length > 0
      &&  (message[length - 1] == '\r' || message[length - 1] == '\n' || message[length - 1] == ' ')) {
      --length;
   }
   if (length > 0) {
      message[length] = 0;
      fprintf(stderr, "error %lu: %s\n", error, message);
   }
   else {
      fprintf(stderr, "error %lu\n", error);
   }
}

static DWORD WINAPI worker_main(void *parameter)
{
   Worker *worker = parameter;

   for (;;)
   {
      WaitForSingleObject(worker->start_event, INFINITE);
      if (worker->stopping) {
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

b32 executor_run_with_options(Graph *graph, u32 worker_count, i32 verbosity)
{
   Worker *workers;
   HANDLE *done_events;
   b32 *rebuilt;
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
      if (!task || !task->command_line) {
         return false;
      }
      if ((task->input_count && !task->inputs)
         ||  (task->output_count && !task->outputs)
         ||  (task->include_directory_count && !task->include_directories)) {
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
   rebuilt = calloc(task_count, sizeof(*rebuilt));
   if (!workers || !done_events || !rebuilt)
   {
      free(workers);
      free(done_events);
      free(rebuilt);
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
      free(rebuilt);
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
            needs_rebuild = task_needs_rebuild(graph, node, task, rebuilt);
            profile_scope_end(&scope);
            if (!needs_rebuild)
            {
               if (verbosity >= 0)
               {
                  logger_log(LOG_LEVEL_INFO, "up-to-date", "%s", graph_node_name(graph, node));
                  if (verbosity >= 1) {
                     printf("  command: %s\n", task->command_line);
                  }
               }
               if (graph_complete(graph, node, true) != GRAPH_OK) {
                  internal_error = true;
                  break;
               }
               continue;
            }

            worker->node = node;
            worker->command_line = task->command_line;
            worker->busy = true;
            rebuilt[node] = true;
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

         if (verbosity >= 2 && worker->output.used > 0)
         {
            printf("[%s]\n", graph_node_name(graph, worker->node));
            fwrite(worker->output.data, 1, (size_t)worker->output.used, stdout);
            if (worker->output.data[worker->output.used - 1] != '\n') {
               putchar('\n');
            }
         }
         succeeded = worker->launch_error == ERROR_SUCCESS && worker->exit_code == 0;
         if (verbosity >= 0)
         {
            logger_log(
               succeeded ? LOG_LEVEL_SUCCESS : LOG_LEVEL_ERROR,
               succeeded ? "succeeded" : "failed",
               "%s",
               graph_node_name(graph, worker->node)
            );
            if (verbosity >= 1 && succeeded) {
               printf("  command: %s\n  exit code: 0\n", worker->command_line);
            }
         }
         if (worker->launch_error != ERROR_SUCCESS)
         {
            Scratch scratch = get_scratch();

            fprintf(
               stderr,
               "[%s] %s\n  command: %s\n  "
               ,
               graph_node_name(graph, worker->node),
               worker->launched ? "process error" : "failed to start process",
               worker->command_line
            );

            print_windows_error(worker->launch_error);

            String executable = command_executable(scratch.arena, string_from_cstring(worker->command_line));
            if (executable.data)
            {
               b32 executable_resolves = platform_executable_resolves(executable);
               fprintf(
                  stderr,
                  "  executable: %s (%s)\n",
                  executable.data
                  ,
                  executable_resolves ? "found" : "not found in current directory or PATH"
               );
            }
            else {
               fprintf(stderr, "  executable: unable to parse from command\n");
            }

            String working_directory;
            b32 has_working_directory = platform_current_directory(scratch.arena, &working_directory);

            if (has_working_directory) {
               fprintf(stderr, "  working directory: %s\n", working_directory.data);
            }
            end_scratch(scratch);
         }
         else if (worker->exit_code != 0)
         {
            fprintf(
               stderr,
               "[%s] process exited with code %lu\n  command: %s\n"
               ,
               graph_node_name(graph, worker->node),
               worker->exit_code,
               worker->command_line
            );
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
   free(rebuilt);
   return !internal_error && !graph_has_failed(graph);
}

b32 executor_run(Graph *graph, u32 worker_count) {
   return executor_run_with_options(graph, worker_count, -1);
}
