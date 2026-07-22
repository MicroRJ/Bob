#include "vcvars_cache.h"

#include "logger.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VCVARS_CACHE_HEADER "BOB_VCVARS_CACHE_V1"

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

typedef enum Env_Diff_Type
{
	ENV_DIFF_SET,
	ENV_DIFF_PREPEND,
	ENV_DIFF_APPEND,
} Env_Diff_Type;

typedef struct Env_Diff
{
	Env_Diff_Type type;
	String name;
	String value;
} Env_Diff;

typedef struct Env_Diff_Table
{
	Env_Diff *items;
	u32 count;
	u32 capacity;
} Env_Diff_Table;

static b32 parse_var(String line, Env_Entry *entry)
{
	String name;
	String value;
	if (line.size == 0 || line.data[0] == '=') return false;
	if (!string_split_first(line, '=', &name, &value) || name.size == 0) return false;
	entry->name = name;
	entry->value = value;
	return true;
}

static b32 parse_environment_table(Arena *arena, String_Array lines, Env_Table *table)
{
	table->items = arena_push_zero_aligned(arena, sizeof(*table->items) * lines.count, _Alignof(Env_Entry));
	if (!table->items) return false;
	table->capacity = lines.count;
	for (u32 i = 0; i < lines.count; ++i) {
		Env_Entry entry;
		if (!parse_var(lines.items[i], &entry)) continue;
		if (table->count >= table->capacity) return false;
		table->items[table->count++] = entry;
	}
	return true;
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

static b32 path_list_contains(String_Array list, String item)
{
	for (u32 i = 0; i < list.count; ++i) {
		if (string_equal_insensitive(list.items[i], item)) return true;
	}
	return false;
}

static String build_path_list_delta(Arena *arena, String before, String after)
{
	Scratch scratch = begin_different_scratch(arena);

	String_Array before_items = string_split(scratch.arena, before, ';');
	String_Array after_items = string_split(scratch.arena, after, ';');
	if (!before_items.items || !after_items.items) {
		end_scratch(scratch);
		return (String){0};
	}

	char *result_start = arena_top(arena);
	for (u32 i = 0; i < after_items.count; ++i) {
		String item = after_items.items[i];
		if (item.size && !path_list_contains(before_items, item)) {
			arena_append_str(arena, item);
			arena_append_char(arena, ';');
		}
	}
	String result = arena_string_from(arena, result_start);
	arena_finalize_string(arena, result);

	end_scratch(scratch);
	return result;
}

static b32 push_environment_diff(Env_Diff_Table *table, Env_Diff_Type type,
String name, String value)
{
	if (table->count >= table->capacity) return false;
	table->items[table->count++] = (Env_Diff){ type, name, value };
	return true;
}

static b32 build_environment_diff(Arena *arena, Env_Table *before, Env_Table *after, Env_Diff_Table *diff)
{

	diff->capacity = after->count;
	diff->items = arena_push_zero_aligned(arena, sizeof(*diff->items) * after->count, _Alignof(Env_Diff));
	if (!diff->items) return false;

	for (u32 i = 0; i < after->count; ++i)
	{
		Env_Entry *new_entry = &after->items[i];
		if (should_skip_variable(new_entry->name)) { continue; }

		Env_Entry *old_entry = find_environment_entry(before, new_entry->name);
		if (!old_entry) {
			if (!push_environment_diff(diff, ENV_DIFF_SET, new_entry->name, new_entry->value)) return false;
			continue;
		}

		if (string_equal(old_entry->value, new_entry->value)) { continue; }

		String delta;
		if (is_path_list_variable(new_entry->name))
		{
			delta = build_path_list_delta(arena, old_entry->value, new_entry->value);
			if (delta.size && !push_environment_diff(diff, ENV_DIFF_PREPEND, new_entry->name, delta)) return false;
		}
		else if (string_ends_with(new_entry->value, old_entry->value))
		{
			delta = string_slice(new_entry->value, 0, new_entry->value.size - old_entry->value.size);
			if (delta.size && !push_environment_diff(diff, ENV_DIFF_PREPEND, new_entry->name, delta)) {
				return false;
			}
		}
		else if (string_starts_with(new_entry->value, old_entry->value))
		{
			delta = string_slice(new_entry->value, old_entry->value.size, new_entry->value.size - old_entry->value.size);
			if (delta.size && !push_environment_diff(diff, ENV_DIFF_APPEND, new_entry->name, delta)) {
				return false;
			}
		}
		else if (!push_environment_diff(diff, ENV_DIFF_SET, new_entry->name, new_entry->value)) {
			return false;
		}
	}
	return true;
}

static b32 serialize_environment_diff(Arena *arena, Env_Diff_Table *diff, String *result)
{
	u64 start = arena_mark(arena);
	arena_append_text(arena, VCVARS_CACHE_HEADER);
	arena_append_char(arena, '\n');
	for (u32 i = 0; i < diff->count; ++i)
	{
		Env_Diff entry = diff->items[i];
		const char *type = entry.type == ENV_DIFF_PREPEND ? "prepend" : entry.type == ENV_DIFF_APPEND ? "append" : "set";
		arena_append_text(arena, type);
		arena_append_char(arena, ' ');
		arena_append_str(arena, entry.name);
		arena_append_char(arena, '=');
		arena_append_str(arena, entry.value);
		arena_append_char(arena, '\n');
	}
	*result = arena_string_from(arena, (char *)arena->data + start);
	arena_finalize_string(arena, *result);
	return true;
}

static b32 ensure_cache_path(Arena *arena, String *path)
{
	if (!arena || !path) return false;

	u64 mark = arena_mark(arena);
	*path = (String){0};

	String local_app_data;
	if (!bob_platform_local_app_data(arena, &local_app_data)) goto failure;
	if (!string_is_terminated(local_app_data)) goto failure;
	char *start = local_app_data.data;
	// Reuse the returned terminator as the next append position.
	arena->used -= 1;
	arena_append_text(arena, "\\bob");
	b32 directory_ready = bob_platform_create_directory(string_from_range(start, (char *)arena_top(arena)));
	if (!directory_ready) goto failure;
	arena_append_text(arena, "\\vcvars64.env");
	*path = arena_string_from(arena, start);
	arena_finalize_string(arena, *path);
	return true;

	failure:
	arena_restore(arena, mark);
	return false;
}

static b32 apply_rule(String action, String name, String value)
{
	Scratch scratch = begin_scratch();
	char *name_text = arena_append_str(scratch.arena, name);
	char *value_text;
	String current = {0};
	b32 success;
	if (!name_text || !arena_push_zero(scratch.arena, 1)) {
		end_scratch(scratch);
		return false;
	}

	if (!bob_platform_get_environment(string_from_cstring(name_text), scratch.arena, &current)) {
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
	success = bob_platform_set_environment(string_from_cstring(name_text), string_from_cstring(value_text));
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

b32 vcvars_cache_refresh(Arena *arena, String *result_path)
{

	static const String command = STRING_LITERAL("cmd.exe /d /s /c \"call vcvars64 >nul&&set\"");

	Env_Table before = {0};
	Env_Table after = {0};
	Env_Diff_Table diff = {0};
	String before_block;
	String after_capture;
	String cache;
	Platform_Process_Result process;
	b32 success = false;

	String path;
	u64 mark;
	if (!arena || !result_path) return false;
	mark = arena_mark(arena);
	*result_path = (String){0};

	if (!ensure_cache_path(arena, &path))
	{
		log_error("unable to get cache paths");
		goto cleanup;
	}

	if (!bob_platform_get_environment_block(arena, &before_block))
	{
		log_error("unable to capture the current environment");
		goto cleanup;
	}
	if (!parse_environment_table(arena, string_split_block(arena, before_block), &before))
	{
		log_error("unable to parse the current environment");
		goto cleanup;
	}
	if (!platform_run_command(command, arena, (Platform_Process_Options){ .hide_window = true }, &process)) {
		log_error("unable to run vcvars64 (error %u)", process.error_code);
		goto cleanup;
	}
	if (process.exit_code != 0) {
		log_error("vcvars64 failed with exit code %u", process.exit_code);
		goto cleanup;
	}
	after_capture = process.output;
	if (!parse_environment_table(arena, string_split_lines(arena, after_capture), &after))
	{
		log_error("unable to parse the environment produced by vcvars64");
		goto cleanup;
	}
	if (!build_environment_diff(arena, &before, &after, &diff))
	{
		log_error("unable to create environment diff");
		goto cleanup;
	}
	if (!serialize_environment_diff(arena, &diff, &cache))
	{
		log_error("unable to serialize environment diff");
		goto cleanup;
	}
	if (!platform_write_entire_file(path, cache.data, cache.size))
	{
		log_error("unable to write vcvars cache: %s", path.data);
		goto cleanup;
	}
	success = true;
	*result_path = path;

	cleanup:
	if (!success) arena_restore(arena, mark);
	return success;
}

b32 vcvars_cache_load(void)
{
	Scratch scratch = begin_scratch();
	String path;
	String data = {0};
	b32 success;
	if (!ensure_cache_path(scratch.arena, &path)) {
		end_scratch(scratch);
		return false;
	}
	if (!platform_read_entire_file(scratch.arena, path, &data)) {
		end_scratch(scratch);
		return false;
	}
	success = vcvars_cache_apply(data);
	if (!success) { log_warning("ignoring invalid vcvars cache: %s", path.data); }
	end_scratch(scratch);
	return success;
}
