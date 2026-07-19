#include "bob.h"
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
   Bob_Build build = {0};
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

   {
      b32 loaded_ok;
      Profile_Scope scope = profile_scope_begin("load elf build script");
      loaded_ok = elf_load_build(path, &build);
      profile_scope_end(&scope);
      if (!loaded_ok)
      {
         log_error("%s: %s", path, build.error);
         goto cleanup;
      }
   }
   if (!worker_override && build.options.has_worker_count)
   {
      worker_count = build.options.worker_count;
   }
   if (!verbosity_override && build.options.has_verbosity)
   {
      verbosity = build.options.verbosity;
   }
	logger_set_verbosity(verbosity);

   {
      Profile_Scope scope = profile_scope_begin("builder");
      exit_code = bob_build(build.bob, worker_count) ? 0 : 1;
      profile_scope_end(&scope);
   }

   cleanup:
   bob_destroy(build.bob);
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
