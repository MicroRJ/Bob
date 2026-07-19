#include "graph.h"
#include "executor.h"
#include "elf_adapter.h"
#include "logger.h"
#include "platform/platform.h"
#include "profiler.h"
#include "vcvars_cache.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

static int run_build(const char *path, u32 worker_count, b32 worker_override, i32 verbosity, b32 verbosity_override)
{
   Task_Array_Desc loaded = {0};
   Arena task_arena = {0};
   Graph *graph = NULL;
   u32 task_count;
   u32 i;
   int exit_code = 1;

   {
      Platform_File_Info build_file;
		if (!platform_file_info(string_from_cstring(path), &build_file))
      {
         Scratch scratch = begin_scratch();
         String working_directory;
         if (platform_current_directory(scratch.arena, &working_directory))
         {
            log_error("%s: file not found (working directory: %s)", path, working_directory.data);
         }
         else
         {
            log_error("%s: file not found", path);
         }
         end_scratch(scratch);
         goto cleanup;
      }
   }

   task_arena = arena_create(0);
   if (!task_arena.data)
   {
      log_error("out of memory while loading %s", path);
      goto cleanup;
   }
   {
      b32 loaded_ok;
      Profile_Scope scope = profile_scope_begin("load elf build script");
      loaded_ok = elf_load_task_list(path, &task_arena, &loaded);
      profile_scope_end(&scope);
      if (!loaded_ok)
      {
         log_error("%s: %s", path, loaded.error);
         goto cleanup;
      }
   }
   if (!worker_override && loaded.has_worker_count)
   {
      worker_count = loaded.worker_count;
   }
   if (!verbosity_override && loaded.has_verbosity)
   {
      verbosity = loaded.verbosity;
   }
	logger_set_verbosity(verbosity);

   task_count = loaded.count;
   graph = graph_create();
   if (!graph)
   {
      log_error("out of memory while loading %s", path);
      goto cleanup;
   }

   for (i = 0; i < task_count; ++i)
   {
      Node_Id node;
      Task *task = &loaded.tasks[i];
      Graph_Error error;
      {
         Profile_Scope scope = profile_scope_begin("construct graph");
         error = graph_add_node(graph, loaded.tasks[i].name.data, task, &node);
         profile_scope_end(&scope);
      }
      if (error != GRAPH_OK)
      {
         log_error("%s: unable to create task '%s': %s", path, loaded.tasks[i].name.data, graph_error_str(error));
         goto cleanup;
      }
   }

   for (i = 0; i < task_count; ++i)
   {
      u32 dependency_index;
      for (dependency_index = 0; dependency_index < loaded.tasks[i].dependency_count; ++dependency_index)
      {
         u32 dependency = loaded.tasks[i].dependencies[dependency_index];
         Graph_Error error;

         if (dependency >= task_count)
         {
            log_error("%s: task '%s' has invalid dependency %u", path, loaded.tasks[i].name.data, dependency);
            goto cleanup;
         }
         {
            Profile_Scope scope = profile_scope_begin("construct graph");
            error = graph_add_dependency(graph, (Node_Id)i, (Node_Id)dependency);
            profile_scope_end(&scope);
         }
         if (error != GRAPH_OK)
         {
            log_error("%s: unable to add dependency to '%s': %s", path, loaded.tasks[i].name.data, graph_error_str(error));
            goto cleanup;
         }
      }
   }

   {
      Profile_Scope scope = profile_scope_begin("executor");
      exit_code = executor_run(graph, worker_count) ? 0 : 1;
      profile_scope_end(&scope);
   }

   cleanup:
   graph_destroy(graph);
   arena_destroy(&task_arena);
   return exit_code;
}

int main(int argument_count, char **arguments)
{
   i32 verbosity = 0;
   u32 worker_count = 4;
   const char *build_path = "build.elf";
   b32 has_build_path = false;
   b32 worker_override = false;
   b32 verbosity_override = false;
   b32 cache_vcvars = false;
   b32 profile = false;
   int argument_index;

   logger_init();

   for (argument_index = 1; argument_index < argument_count; ++argument_index)
   {
      if (strcmp(arguments[argument_index], "--verbose") == 0)
      {
         verbosity = 1;
         verbosity_override = true;
         if (argument_index + 1 < argument_count)
         {
            char *end;
            unsigned long parsed = strtoul(arguments[argument_index + 1], &end, 10);
            if (*end == 0)
            {
               if (parsed > INT32_MAX)
               {
                  log_error("invalid verbosity level: %s", arguments[argument_index + 1]);
                  return 2;
               }
               verbosity = (i32)parsed;
               ++argument_index;
            }
         }
      }
      else if (strcmp(arguments[argument_index], "--workers") == 0 && argument_index + 1 < argument_count)
      {
         char *end;
         unsigned long parsed = strtoul(arguments[++argument_index], &end, 10);
         if (*end != 0 || parsed == 0 || parsed > UINT32_MAX)
         {
            log_error("invalid worker count: %s", arguments[argument_index]);
            return 2;
         }
         worker_count = (u32)parsed;
         worker_override = true;
      }
      else if (strcmp(arguments[argument_index], "--cache-vcvars") == 0)
      {
         cache_vcvars = true;
      }
      else if (strcmp(arguments[argument_index], "--profile") == 0)
      {
         profile = true;
      }
      else if (arguments[argument_index][0] != '-' && !has_build_path)
      {
         build_path = arguments[argument_index];
         has_build_path = true;
      }
      else
      {
         log_error("usage: bob [build.elf] [--verbose [N]] [--workers N] [--profile]\n" "       bob --cache-vcvars");
         return 2;
      }
   }
   if (cache_vcvars)
   {
		Scratch scratch;
		String cache_path;
      if (has_build_path || worker_override || verbosity_override || profile)
      {
         log_error("--cache-vcvars cannot be combined with build options");
         return 2;
      }
		scratch = begin_scratch();
		if (!vcvars_cache_refresh(scratch.arena, &cache_path)) {
			end_scratch(scratch);
			return 1;
		}
		log_success("cached vcvars64 environment: %s", cache_path.data);
		end_scratch(scratch);
      return 0;
   }
   profiler_set_enabled(profile);
   profiler_reset();
   {
      Profile_Scope scope = profile_scope_begin("vcvars cache load");
      vcvars_cache_load();
      profile_scope_end(&scope);
   }
   {
      int result;
      Profile_Scope scope = profile_scope_begin("build");
      result = run_build(build_path, worker_count, worker_override, verbosity, verbosity_override);
      profile_scope_end(&scope);
      profiler_print();
      return result;
   }
}
