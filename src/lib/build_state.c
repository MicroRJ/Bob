#include "build_state.h"
#include "build_state_binary.h"
#include "elf.h"
#include "platform_adapter.h"
#include "platform.h"

#include <string.h>

Build_State_Task *build_state_find(Build_State *state, String output)
{
	if (!state) return NULL;
	for (u32 i = 0; i < state->count; ++i) {
		if (string_equal(state->tasks[i].output, output)) return state->tasks + i;
	}
	return NULL;
}

static b32 build_state_reserve(Arena *arena, Build_State *state, u32 capacity)
{
	Build_State_Task *tasks;
	u32 new_capacity;
	if (state->capacity >= capacity) return true;
	new_capacity = state->capacity ? state->capacity : 16;
	while (new_capacity < capacity) {
		if (new_capacity > UINT32_MAX / 2) {
			new_capacity = capacity;
			break;
		}
		new_capacity *= 2;
	}
	tasks = arena_push_zero_aligned(arena,
		(u64)new_capacity * sizeof(*tasks), _Alignof(Build_State_Task));
	if (!tasks) return false;
	if (state->count) memcpy(tasks, state->tasks, state->count * sizeof(*tasks));
	state->tasks = tasks;
	state->capacity = new_capacity;
	return true;
}

b32 build_state_set(Arena *arena, Build_State *state, String output, String_Array dependencies)
{
	u64 mark;
	Build_State_Task copy = {0};
	Build_State_Task *task;

	if (!arena || !state || !output.data || output.size == 0 ||
		(dependencies.count && !dependencies.items)) return false;
	mark = arena_mark(arena);
	copy.output = arena_push_string_copy(arena, output);
	if (!copy.output.data) goto failure;
	if (dependencies.count) {
		copy.dependencies.items = arena_push_zero_aligned(arena,
			(u64)dependencies.count * sizeof(*copy.dependencies.items), _Alignof(String));
		if (!copy.dependencies.items) goto failure;
		for (u32 i = 0; i < dependencies.count; ++i) {
			if (!dependencies.items[i].data || dependencies.items[i].size == 0) goto failure;
			copy.dependencies.items[i] = arena_push_string_copy(arena, dependencies.items[i]);
			if (!copy.dependencies.items[i].data) goto failure;
			++copy.dependencies.count;
		}
	}

	task = build_state_find(state, output);
	if (task) {
		*task = copy;
		return true;
	}
	if (state->count == UINT32_MAX ||
		!build_state_reserve(arena, state, state->count + 1)) goto failure;
	state->tasks[state->count++] = copy;
	return true;

failure:
	arena_restore(arena, mark);
	return false;
}

b32 build_state_remove(Build_State *state, String output)
{
	if (!state) return false;
	for (u32 i = 0; i < state->count; ++i) {
		if (!string_equal(state->tasks[i].output, output)) continue;
		if (i + 1 < state->count) {
			memmove(state->tasks + i, state->tasks + i + 1,
				(state->count - i - 1) * sizeof(*state->tasks));
		}
		--state->count;
		return true;
	}
	return false;
}

static b32 build_state_parse_task(elf_State *elf, elf_i32 table, Arena *arena, Build_State *state)
{
	Scratch scratch = begin_different_scratch(arena);
	elf_StrSlice output_slice;
	String output;
	String_Array dependencies = {0};
	elf_u32 dependency_count;
	elf_i32 dependencies_table;
	elf_i32 checkpoint = elf_get_top(elf);

	if (!elf_get_field(elf, table, "output") || !elf_to_str(elf, -1, &output_slice) ||
		output_slice.size == 0) goto failure;
	output = string_from_data(output_slice.data, output_slice.size);
	if (build_state_find(state, output)) goto failure;
	elf_pop(elf, 1);

	if (!elf_get_field(elf, table, "dependencies") ||
		elf_type(elf, -1) != ELF_VALUE_TYPE_TABLE ||
		!elf_length(elf, -1, &dependency_count)) goto failure;
	dependencies_table = elf_abs_index(elf, -1);
	if (dependency_count) {
		dependencies.items = arena_push_zero_aligned(scratch.arena,
			(u64)dependency_count * sizeof(*dependencies.items), _Alignof(String));
		if (!dependencies.items) goto failure;
	}
	for (elf_u32 i = 0; i < dependency_count; ++i) {
		elf_StrSlice dependency;
		if (!elf_get_index(elf, dependencies_table, i) ||
			!elf_to_str(elf, -1, &dependency) || dependency.size == 0) goto failure;
		dependencies.items[dependencies.count++] = string_from_data(dependency.data, dependency.size);
		elf_pop(elf, 1);
	}
	if (!build_state_set(arena, state, output, dependencies)) goto failure;
	elf_set_top(elf, checkpoint);
	end_scratch(scratch);
	return true;

failure:
	elf_set_top(elf, checkpoint);
	end_scratch(scratch);
	return false;
}

b32 build_state_parse(Arena *arena, String source, Build_State *state)
{
	u64 mark;
	Build_State parsed = {0};
	elf_State *elf;
	elf_Integer version;
	elf_u32 task_count;
	elf_i32 root;
	elf_i32 tasks;
	b32 valid = false;

	if (!arena || !state || !source.data || source.size == 0) return false;
	mark = arena_mark(arena);
	elf = elf_create_state();
	if (!elf) return false;
	if (!elf_push_constant_expr(elf, "Bob build state",
		(elf_StrSlice){ source.data, source.size }) ||
		elf_type(elf, -1) != ELF_VALUE_TYPE_TABLE) goto done;
	root = elf_abs_index(elf, -1);

	if (!elf_get_field(elf, root, "version") ||
		!elf_to_int(elf, -1, &version) || version != BUILD_STATE_VERSION) goto done;
	elf_pop(elf, 1);
	if (!elf_get_field(elf, root, "tasks") ||
		elf_type(elf, -1) != ELF_VALUE_TYPE_TABLE ||
		!elf_length(elf, -1, &task_count)) goto done;
	tasks = elf_abs_index(elf, -1);
	for (elf_u32 i = 0; i < task_count; ++i) {
		elf_i32 task;
		if (!elf_get_index(elf, tasks, i) || elf_type(elf, -1) != ELF_VALUE_TYPE_TABLE) goto done;
		task = elf_abs_index(elf, -1);
		if (!build_state_parse_task(elf, task, arena, &parsed)) goto done;
		elf_pop(elf, 1);
	}
	valid = true;

done:
	elf_destroy_state(elf);
	if (!valid) {
		arena_restore(arena, mark);
		return false;
	}
	*state = parsed;
	return true;
}

static void build_state_write_string(Arena *arena, String string)
{
	static const char hex[] = "0123456789ABCDEF";
	arena_append_char(arena, '"');
	for (u64 i = 0; i < string.size; ++i) {
		u8 byte = (u8)string.data[i];
		switch (byte) {
		case '\\': arena_append_text(arena, "\\\\"); break;
		case '"':  arena_append_text(arena, "\\\""); break;
		case '\0': arena_append_text(arena, "\\0");  break;
		case '\a': arena_append_text(arena, "\\a");  break;
		case '\b': arena_append_text(arena, "\\b");  break;
		case '\f': arena_append_text(arena, "\\f");  break;
		case '\n': arena_append_text(arena, "\\n");  break;
		case '\r': arena_append_text(arena, "\\r");  break;
		case '\t': arena_append_text(arena, "\\t");  break;
		case '\v': arena_append_text(arena, "\\v");  break;
		default:
			if (byte < 0x20 || byte == 0x7F) {
				arena_append_text(arena, "\\x");
				arena_append_char(arena, hex[byte >> 4]);
				arena_append_char(arena, hex[byte & 0xF]);
			}
			else arena_append_char(arena, (char)byte);
			break;
		}
	}
	arena_append_char(arena, '"');
}

b32 build_state_write(Arena *arena, const Build_State *state, String *source)
{
	u64 mark;
	void *start;
	if (!arena || !state || !source || (state->count && !state->tasks)) return false;
	mark = arena_mark(arena);
	start = arena_top(arena);
	arena_append_text(arena, "{\n\tversion = ");
	arena_appendf(arena, "%u", BUILD_STATE_VERSION);
	arena_append_text(arena, ",\n\ttasks = {");
	for (u32 i = 0; i < state->count; ++i) {
		const Build_State_Task *task = state->tasks + i;
		if (!task->output.data || task->output.size == 0 ||
			(task->dependencies.count && !task->dependencies.items)) goto failure;
		arena_append_text(arena, "\n\t\t{\n\t\t\toutput = ");
		build_state_write_string(arena, task->output);
		arena_append_text(arena, ",\n\t\t\tdependencies = {");
		for (u32 dependency = 0; dependency < task->dependencies.count; ++dependency) {
			String path = task->dependencies.items[dependency];
			if (!path.data || path.size == 0) goto failure;
			arena_append_text(arena, "\n\t\t\t\t");
			build_state_write_string(arena, path);
			arena_append_char(arena, ',');
		}
		if (task->dependencies.count) arena_append_text(arena, "\n\t\t\t");
		arena_append_text(arena, "},\n\t\t},");
	}
	if (state->count) arena_append_text(arena, "\n\t");
	arena_append_text(arena, "},\n}");
	*source = arena_string_from(arena, start);
	arena_finalize_string(arena, *source);
	return true;

failure:
	arena_restore(arena, mark);
	*source = (String){0};
	return false;
}

Build_State_Load_Result build_state_load(Arena *arena, String path, Build_State *state)
{
	Bob_Platform_File_Info info;
	Scratch scratch;
	String source;
	b32 parsed;

	if (!arena || !state || !string_is_terminated(path) || path.size == 0) {
		return BUILD_STATE_LOAD_ERROR;
	}
	*state = (Build_State){0};
	if (!bob_platform_file_info(path, &info)) return BUILD_STATE_LOAD_MISSING;
	scratch = begin_different_scratch(arena);
	if (!bob_platform_read_entire_file(scratch.arena, path, &source)) {
		end_scratch(scratch);
		return BUILD_STATE_LOAD_ERROR;
	}
	parsed = build_state_parse(arena, source, state);
	end_scratch(scratch);
	return parsed ? BUILD_STATE_LOAD_OK : BUILD_STATE_LOAD_INVALID;
}

static String build_state_parent_directory(String path)
{
	for (u64 i = path.size; i > 0; --i) {
		u64 separator = i - 1;
		if (path.data[separator] != '/' && path.data[separator] != '\\') continue;
		if (separator == 0 || (separator == 2 && path.data[1] == ':')) ++separator;
		return string_slice(path, 0, separator);
	}
	return (String){0};
}

b32 build_state_save(Arena *arena, String path, const Build_State *state)
{
	u64 mark;
	String parent;
	String temporary = {0};
	String source;
	b32 result = false;
	void *start;

	if (!arena || !state || !string_is_terminated(path) || path.size == 0) return false;
	mark = arena_mark(arena);
	parent = build_state_parent_directory(path);
	if (parent.size) {
		parent = arena_push_string_copy(arena, parent);
		if (!parent.data || !platform_create_directories(parent.data)) goto done;
	}
	start = arena_top(arena);
	arena_append_str(arena, path);
	arena_append_text(arena, ".tmp");
	temporary = arena_string_from(arena, start);
	arena_finalize_string(arena, temporary);
	if (!build_state_write(arena, state, &source) || source.size > SIZE_MAX ||
		!bob_platform_write_entire_file(temporary, source.data, (size_t)source.size)) goto done;
	if (!platform_move_file(temporary.data, path.data, true)) goto done;
	result = true;

done:
	if (!result && temporary.data) platform_remove_file(temporary.data);
	arena_restore(arena, mark);
	return result;
}
