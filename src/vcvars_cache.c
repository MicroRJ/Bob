#include "vcvars_cache.h"

#include "logger.h"
#include "platform.h"

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

static b32 parse_environment_lines(Arena *arena, String capture, Env_Table *table)
{
	u32 capacity = string_count_lines(capture);
	u64 start = 0;
	table->items = arena_push_zero_aligned(arena, sizeof(*table->items) * capacity, _Alignof(Env_Entry));
	if (!table->items) return false;
	table->capacity = capacity;

	while (start < capture.size)
	{
		u64 end = start;
		while (end < capture.size && capture.data[end] != '\n') ++end;
		String line = string_slice(capture, start, end - start);
		if (line.size && line.data[line.size - 1] == '\r') { --line.size; }
		start = end < capture.size ? end + 1 : end;

		Env_Entry entry;
		if (!parse_var(line, &entry)) { continue; }
		if (table->count >= table->capacity) return false;
		table->items[table->count++] = entry;
	}
	return true;
}

static b32 parse_environment_block(Arena *arena, String block, Env_Table *table)
{
	u32 capacity = 0;
	u64 start = 0;
	while (start < block.size) {
		++capacity;
		while (start < block.size && block.data[start]) ++start;
		++start;
	}
	table->items = arena_push_zero_aligned(arena, sizeof(*table->items) * capacity, _Alignof(Env_Entry));
	if (!table->items) return false;
	table->capacity = capacity;
	start = 0;
	while (start < block.size)
	{
		u64 end = start;
		Env_Entry entry;
		while (end < block.size && block.data[end]) ++end;
		if (parse_var(string_slice(block, start, end - start), &entry)) {
			table->items[table->count++] = entry;
		}
		start = end + 1;
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
	String_Array before_items = string_split(arena, before, ';');
	String_Array after_items = string_split(arena, after, ';');
	if (!before_items.items || !after_items.items) return (String){0};

	u64 result_start = arena_mark(arena);
	for (u32 i = 0; i < after_items.count; ++i) {
		String item = after_items.items[i];
		if (item.size && !path_list_contains(before_items, item)) {
			arena_append_str(arena, item);
			arena_append_char(arena, ';');
		}
	}
	return string_from_range((char *)arena->data + result_start, arena_top(arena));
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
	arena_push_text(arena, VCVARS_CACHE_HEADER);
	arena_append_char(arena, '\n');
	for (u32 i = 0; i < diff->count; ++i)
	{
		Env_Diff entry = diff->items[i];
		const char *type = entry.type == ENV_DIFF_PREPEND ? "prepend" : entry.type == ENV_DIFF_APPEND ? "append" : "set";
		arena_push_text(arena, type);
		arena_append_char(arena, ' ');
		arena_append_str(arena, entry.name);
		arena_append_char(arena, '=');
		arena_append_str(arena, entry.value);
		arena_append_char(arena, '\n');
	}
	*result = string_from_range((char *)arena->data + start, arena_top(arena));
	return true;
}

static b32 cache_paths(Arena *arena, String *parent, String *path)
{
	String local_app_data;
	char *start;
	if (!arena || !parent || !path ||
		!platform_local_app_data(arena, &local_app_data)) return false;

	start = arena_top(arena);
	if (!arena_append_str(arena, local_app_data) ||
		!arena_push_text(arena, "\\bob") ||
		!arena_append_char(arena, 0)) return false;
	*parent = string_from_range(start, (char *)arena_top(arena) - 1);

	start = arena_top(arena);
	if (!arena_append_str(arena, *parent) ||
		!arena_push_text(arena, "\\vcvars64.env") ||
		!arena_append_char(arena, 0)) return false;
	*path = string_from_range(start, (char *)arena_top(arena) - 1);
	return true;
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

	if (!platform_get_environment(string_from_cstring(name_text), scratch.arena, &current)) {
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
	success = platform_set_environment(string_from_cstring(name_text), string_from_cstring(value_text));
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

	static const char command[] = "cmd.exe /d /s /c \"call vcvars64 >nul&&set\"";

	Env_Table before = {0};
	Env_Table after = {0};
	Env_Diff_Table diff = {0};
	String before_block;
	String after_capture;
	String cache;
	u32 exit_code;
	b32 success = false;

	String directory;
	String path;
	u64 mark;
	if (!arena || !result_path) return false;
	mark = arena_mark(arena);
	*result_path = (String){0};

	if (!cache_paths(arena, &directory, &path))
	{
		log_error("unable to get cache paths");
		goto cleanup;
	}

	if (!platform_get_environment_block(arena, &before_block))
	{
		log_error("unable to capture the current environment");
		goto cleanup;
	}
	if (!parse_environment_block(arena, before_block, &before))
	{
		log_error("unable to parse the current environment");
		goto cleanup;
	}
	if (!platform_capture_stdout(command, arena, &after_capture, &exit_code) || exit_code != 0) {
		log_error("vcvars64 failed with exit code %u", exit_code);
		goto cleanup;
	}
	if (!parse_environment_lines(arena, after_capture, &after))
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
	if (!platform_create_directory(directory))
	{
		log_error("unable to write vcvars cache: %s", path);
		goto cleanup;
	}
	if (!platform_write_entire_file(path, cache.data, cache.size))
	{
		log_error("unable to write vcvars cache: %s", path);
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
	Scratch scratch = get_scratch();
	String directory;
	String path;
	String data = {0};
	b32 success;
	if (!cache_paths(scratch.arena, &directory, &path)) {
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
