#include "vcvars_cache.h"

#include "logger.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VCVARS_CACHE_HEADER "BOB_VCVARS_CACHE_V1"
#define VCVARS_CAPTURE_MARKER "__BOB_VCVARS_AFTER_4D9F2A71__"

typedef struct Env_Entry {
   String name;
   String value;
} Env_Entry;

typedef struct Env_Table
{
   Env_Entry *items;
   u32 count;
   u32 capacity;
} Env_Table;

static b32 parse_var(String line, Env_Entry *entry)
{
   u64 separator;
   if (line.size && line.data[line.size - 1] == '\r') { --line.size; }
   if (line.size == 0 || line.data[0] == '=') return false;
   for (separator = 0; separator < line.size; ++separator) {
      if (line.data[separator] == '=') { break; }
   }
   if (separator == 0 || separator == line.size) return false;
   entry->name = string_slice(line, 0, separator);
   entry->value = string_slice(line, separator + 1, line.size - separator - 1);
   return true;
}

static b32 parse_capture(Arena *arena, String capture, Env_Table *before, Env_Table *after)
{
   u32 capacity = string_count_lines(capture);
   u64 start = 0;
   b32 found_marker = false;

   before->items = arena_push_zero_aligned(arena, sizeof(*before->items) * capacity, _Alignof(Env_Entry));
   after->items = arena_push_zero_aligned(arena, sizeof(*after->items) * capacity, _Alignof(Env_Entry));
   if (!before->items || !after->items) return false;
   before->capacity = capacity;
   after->capacity = capacity;

   while (start < capture.size)
   {
      u64 end = start;
      while (end < capture.size && capture.data[end] != '\n') ++end;
      String line = string_slice(capture, start, end - start);
      if (line.size && line.data[line.size - 1] == '\r') { --line.size; }
      start = end < capture.size ? end + 1 : end;

      if (string_is(line, VCVARS_CAPTURE_MARKER)) {
         found_marker = true;
         continue;
      }

      Env_Entry entry;
      if (!parse_var(line, &entry)) { continue; }

      Env_Table *destination = found_marker ? after : before;
      if (destination->count >= destination->capacity) return false;
      destination->items[destination->count++] = entry;
   }
   return found_marker;
}

static Env_Entry *find_environment_entry(Env_Table *list, String name)
{
   u32 i;
   for (i = 0; i < list->count; ++i) {
      if (string_equal_insensitive(list->items[i].name, name)) { return &list->items[i]; }
   }
   return NULL;
}

static b32 should_skip_variable(String name)
{
   String preinit = STRING_LITERAL("__VSCMD_PREINIT_");
   if (name.size < preinit.size) return false;
   return string_equal_insensitive(string_slice(name, 0, preinit.size), preinit);
}

static b32 is_path_list_variable(String name)
{
   return string_equal_insensitive(name, STRING_LITERAL("PATH")) ||
   string_equal_insensitive(name, STRING_LITERAL("INCLUDE")) ||
   string_equal_insensitive(name, STRING_LITERAL("EXTERNAL_INCLUDE")) ||
   string_equal_insensitive(name, STRING_LITERAL("LIB")) ||
   string_equal_insensitive(name, STRING_LITERAL("LIBPATH"));
}

static b32 path_list_contains(String list, String item)
{
   u64 start = 0;
   while (start <= list.size)
   {
      u64 end = start;
      while (end < list.size && list.data[end] != ';') ++end;
      if (string_equal_insensitive(string_slice(list, start, end - start), item)) {
         return true;
      }
      if (end == list.size) { break; }
      start = end + 1;
   }
   return false;
}

static String build_path_list_delta(Arena *arena, String before, String after)
{
   u64 result_start = arena_mark(arena);
   u64 start = 0;
   while (start <= after.size)
   {
      u64 end = start;
      String item;
      while (end < after.size && after.data[end] != ';') ++end;
      item = string_slice(after, start, end - start);
      if (item.size && !path_list_contains(before, item)) {
         arena_append_str(arena, item);
         arena_push_char(arena, ';');
      }
      if (end == after.size) { break; }
      start = end + 1;
   }
   return string_from_range((char *)arena->data + result_start, arena_top(arena));
}

static b32 push_cache_rule(Arena *arena, const char *action, String name, String value)
{
   return arena_push_text(arena, action) &&
   arena_push_char(arena, ' ') &&
   arena_append_str(arena, name) &&
   arena_push_char(arena, '=') &&
   arena_append_str(arena, value) &&
   arena_push_char(arena, '\n');
}

static b32 build_cache(Arena *arena, Env_Table *before, Env_Table *after, String *result)
{
   u64 start = arena_mark(arena);
   u32 i;
   if (!arena_push_text(arena, VCVARS_CACHE_HEADER) || !arena_push_char(arena, '\n')) return false;

   for (i = 0; i < after->count; ++i)
   {
      Env_Entry *new_entry = &after->items[i];
      Env_Entry *old_entry;
      String delta;
      if (should_skip_variable(new_entry->name)) { continue; }
      old_entry = find_environment_entry(before, new_entry->name);
      if (!old_entry) {
         if (!push_cache_rule(arena, "set", new_entry->name, new_entry->value)) return false;
         continue;
      }
      if (string_equal(old_entry->value, new_entry->value)) { continue; }
      if (is_path_list_variable(new_entry->name))
      {
         Scratch scratch = get_scratch();
         delta = build_path_list_delta(scratch.arena, old_entry->value, new_entry->value);
         if (delta.size && !push_cache_rule(arena, "prepend", new_entry->name, delta)) {
            end_scratch(scratch);
            return false;
         }
         end_scratch(scratch);
      }
      else if (string_ends_with(new_entry->value, old_entry->value))
      {
         delta = string_slice(new_entry->value, 0, new_entry->value.size - old_entry->value.size);
         if (delta.size && !push_cache_rule(arena, "prepend", new_entry->name, delta)) {
            return false;
         }
      }
      else if (string_starts_with(new_entry->value, old_entry->value))
      {
         delta = string_slice(new_entry->value, old_entry->value.size, new_entry->value.size - old_entry->value.size);
         if (delta.size && !push_cache_rule(arena, "append", new_entry->name, delta)) {
            return false;
         }
      }
      else if (!push_cache_rule(arena, "set", new_entry->name, new_entry->value)) {
         return false;
      }
   }
   *result = string_from_range((char *)arena->data + start, arena_top(arena));
   return true;
}

static b32 cache_paths(char *directory, u32 directory_size, char *path, u32 path_size)
{
   Scratch scratch = get_scratch();
   String local_app_data;
   int length;
   if (!platform_local_app_data(scratch.arena, &local_app_data)) {
      end_scratch(scratch);
      return false;
   }
   length = snprintf(directory, directory_size, "%s\\bob", local_app_data.data);
   if (length < 0 || (u32)length >= directory_size) {
      end_scratch(scratch);
      return false;
   }
   length = snprintf(path, path_size, "%s\\vcvars64.env", directory);
   end_scratch(scratch);
   return length >= 0 && (u32)length < path_size;
}

static b32 apply_rule(String action, String name, String value)
{
   Scratch scratch = get_scratch();
   char *name_text = arena_append_str(scratch.arena, name);
   char *value_text;
   String current = {0};
   b32 success;
   if (!name_text || !arena_push_zero(scratch.arena, 1)) {
      end_scratch(scratch);
      return false;
   }

   if (!platform_get_environment(name_text, scratch.arena, &current)) {
      end_scratch(scratch);
      return false;
   }
   if (string_is(action, "prepend")) {
      value_text = arena_append_str(scratch.arena, value);
      arena_append_str(scratch.arena, current);
   }
   else if (string_is(action, "append"))
   {
      value_text = arena_top(scratch.arena);
      arena_append_str(scratch.arena, current);
      arena_append_str(scratch.arena, value);
   }
   else if (string_is(action, "set")) {
      value_text = arena_append_str(scratch.arena, value);
   }
   else {
      end_scratch(scratch);
      return false;
   }
   if (!arena_push_zero(scratch.arena, 1)) {
      end_scratch(scratch);
      return false;
   }
   success = platform_set_environment(name_text, value_text);
   end_scratch(scratch);
   return success;
}

b32 vcvars_cache_apply(String cache)
{
   u64 start = 0;
   b32 saw_header = false;
   while (start < cache.size)
   {
      u64 end = start;
      u64 space;
      u64 separator;
      String line;
      while (end < cache.size && cache.data[end] != '\n') ++end;
      line = string_slice(cache, start, end - start);
      if (line.size && line.data[line.size - 1] == '\r') { --line.size; }
      start = end < cache.size ? end + 1 : end;
      if (!saw_header)
      {
         if (!string_is(line, VCVARS_CACHE_HEADER)) return false;
         saw_header = true;
         continue;
      }
      if (line.size == 0) { continue; }
      for (space = 0; space < line.size && line.data[space] != ' '; ++space) {}
      for (separator = space + 1; separator < line.size && line.data[separator] != '='; ++separator) {}
      if (space == 0 || separator >= line.size) return false;
      if (!apply_rule(
            string_slice(line, 0, space),
            string_slice(line, space + 1, separator - space - 1),
            string_slice(line, separator + 1, line.size - separator - 1)
         )) {
         return false;
      }
   }
   return saw_header;
}

b32 vcvars_cache_refresh(char *path, u32 path_size)
{

   static const char command[] = "cmd.exe /d /s /c \"set&echo " VCVARS_CAPTURE_MARKER "&call vcvars64 >nul&&set\"";

   Arena arena = arena_create(0);
   Env_Table before = {0};
   Env_Table after = {0};
   String capture;
   String cache;
   char directory[KILOBYTES(32)];
   u32 exit_code;
   b32 success = false;

   if (!arena.data || !cache_paths(directory, sizeof(directory), path, path_size)) { goto escape; }

   if (!platform_capture_stdout(command, &arena, &capture, &exit_code) || exit_code != 0) {
      log_error("vcvars64 failed with exit code %u", exit_code);
      goto escape;
   }
   if (!parse_capture(&arena, capture, &before, &after) || !build_cache(&arena, &before, &after, &cache)) {
      log_error("unable to parse the environment produced by vcvars64");
      goto escape;
   }
   if (!platform_create_directory(directory)
   ||  !platform_write_entire_file(path, cache.data, (size_t)cache.size)) {
      log_error("unable to write vcvars cache: %s", path);
      goto escape;
   }
   success = true;

   escape:
   arena_destroy(&arena);
   return success;
}

b32 vcvars_cache_load(void)
{
   char directory[KILOBYTES(32)];
   char path[KILOBYTES(32)];
   Arena arena = arena_create(0);
   String data = {0};
   b32 success;
   if (!arena.data || !cache_paths(directory, sizeof(directory), path, sizeof(path))) {
      arena_destroy(&arena);
      return false;
   }
   if (!platform_read_entire_file(&arena, path, &data)) {
      arena_destroy(&arena);
      return false;
   }
   success = vcvars_cache_apply(data);
   if (!success) { log_warning("ignoring invalid vcvars cache: %s", path); }
   arena_destroy(&arena);
   return success;
}
