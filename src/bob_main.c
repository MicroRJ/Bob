
#include "bob.h"
#include "logger.h"
#include "platform_adapter.h"
#include "profiler.h"
#include "script.h"
#include "vcvars_cache.h"
#include "elf.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>

static int run_build(Script *script, Cmd_Options command_line_options)
{
   Script_Build build = {0};
   int exit_code = 1;

	if (!script_read_build(script, &build)) {
		log_error("%s", build.error);
		goto cleanup;
	}
	Script_Options options = script_options_resolve(build.options, command_line_options);
	logger_set_verbosity(options.verbosity);

   {
      Profile_Scope scope = profile_scope_begin("builder");
      exit_code = bob_build(build.bob, options.worker_count) ? 0 : 1;
      profile_scope_end(&scope);
   }

   cleanup:
   bob_destroy(build.bob);
   return exit_code;
}

static int run_script(String path, String function_name, Cmd_Options command_line_options)
{
	Platform_File_Info build_file;
	if (!platform_file_info(path, &build_file))
	{
		Scratch scratch = begin_scratch();
		String working_directory;
		if (platform_current_directory(scratch.arena, &working_directory)) log_error("%s: file not found (working directory: %s)", path.data, working_directory.data);
		else log_error("%s: file not found", path.data);
		end_scratch(scratch);
		return 1;
	}

	Scratch scratch = begin_scratch();
	Profile_Scope load_scope = profile_scope_begin("load build script");
	Script *script = script_load(scratch.arena, path);
	profile_scope_end(&load_scope);
	if (!script_is_loaded(script)) {
		log_error("%s: %s", path.data, script_error(script).data);
		script_destroy(script);
		end_scratch(scratch);
		return 1;
	}
	script_set_command_line_options(script, command_line_options);

	int exit_code = 1;
	if (script_has_function(script, function_name)) {
		exit_code = script_invoke(script, function_name) ? 0 : 1;
		if (exit_code) log_error("%s: %s", path.data, script_error(script).data);
	}
	else if (string_is(function_name, "build")) {
		exit_code = run_build(script, command_line_options);
	}
	else
	{
		String_Array functions = script_functions(script);
		if (functions.count)
		{
			void *start = arena_top(scratch.arena);
			for (u32 i = 0; i < functions.count; ++i) {
				if (i) arena_append_text(scratch.arena, ", ");
				arena_append_str(scratch.arena, functions.items[i]);
			}
			String available = arena_string_from(scratch.arena, start);
			arena_finalize_string(scratch.arena, available);
			log_error("script has no function '%.*s' (available: %s)", (int)function_name.size, function_name.data, available.data);
		}
		else log_error("script has no function '%.*s'", (int)function_name.size, function_name.data);
	}

	script_destroy(script);
	end_scratch(scratch);
	return exit_code;
}

int main(int argument_count, char **arguments)
{
   Cmd_Options command_line_options = {0};
   String build_path = STRING_LITERAL("build.elf");
   String function_name = STRING_LITERAL("build");
   b32 has_build_path = false;
   b32 has_function = false;
   b32 cache_vcvars = false;
   b32 profile = false;
   b32 profile_threads = false;
   int argument_index;

   logger_init();

   for (argument_index = 1; argument_index < argument_count; ++argument_index)
   {
      if (strcmp(arguments[argument_index], "--verbose") == 0)
      {
         command_line_options.verbosity = 1;
         command_line_options.has_verbosity = true;
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
               command_line_options.verbosity = (i32)parsed;
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
         command_line_options.worker_count = (u32)parsed;
         command_line_options.has_worker_count = true;
      }
      else if (strcmp(arguments[argument_index], "--cache-vcvars") == 0)
      {
         cache_vcvars = true;
      }
      else if (strcmp(arguments[argument_index], "--profile") == 0)
      {
         profile = true;
      }
      else if (strcmp(arguments[argument_index], "--profile-threads") == 0)
      {
         profile = true;
         profile_threads = true;
      }
      else if (strcmp(arguments[argument_index], "--version") == 0)
      {
         printf("bob %s\n", BOB_VERSION);
			printf("elf %s\n", elf_version());
         return 0;
      }
      else if (arguments[argument_index][0] != '-')
      {
         String argument = string_from_cstring(arguments[argument_index]);
         if (script_supports_path(argument)) {
            if (has_build_path) {
               log_error("multiple build files specified: %s", argument.data);
               return 2;
            }
            build_path = argument;
            has_build_path = true;
         }
         else if (!has_function) {
            function_name = argument;
            has_function = true;
         }
         else {
            log_error("unexpected positional argument: %s", argument.data);
            return 2;
         }
      }
      else
      {
         log_error("usage: bob [build-file] [function] [--verbose [N]] [--workers N] [--profile | --profile-threads]\n" "       bob --cache-vcvars\n" "       bob --version");
         return 2;
      }
   }
   if (cache_vcvars)
   {
		Scratch scratch;
		String cache_path;
      if (has_build_path || has_function || command_line_options.has_worker_count || command_line_options.has_verbosity || profile)
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
      result = run_script(build_path, function_name, command_line_options);
      profile_scope_end(&scope);
      profiler_print(profile_threads);
      return result;
   }
}
