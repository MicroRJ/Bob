#include "scripts/libs/filesystem.h"

#include "platform_adapter.h"
#include "scripts/libs/path.h"

#include <stdlib.h>

typedef struct Path_Node Path_Node;
struct Path_Node
{
	Path_Node *next;
	String path;
};

typedef struct List_Context
{
	Arena *arena;
	Script_List_Paths_Options options;
	Path_Node *first;
	Path_Node *last;
	u32 count;
} List_Context;

static b32 glob_match(String text, String pattern)
{
	while (pattern.size)
	{
		if (pattern.data[0] == '*')
		{
			pattern = string_slice(pattern, 1, pattern.size - 1);
			if (!pattern.size) return true;
			for (u64 offset = 0; offset <= text.size; ++offset) {
				if (glob_match(string_slice(text, offset, text.size - offset), pattern)) return true;
			}
			return false;
		}
		if (!text.size || (pattern.data[0] != '?' && pattern.data[0] != text.data[0])) return false;
		text = string_slice(text, 1, text.size - 1);
		pattern = string_slice(pattern, 1, pattern.size - 1);
	}
	return text.size == 0;
}

static b32 kind_matches(Script_Path_Kind kind, b32 is_directory)
{
	if (kind == SCRIPT_PATH_ALL) return true;
	if (kind == SCRIPT_PATH_DIRECTORIES) return is_directory;
	return !is_directory;
}

static b32 pattern_matches(Script_List_Paths_Options options, String path)
{
	if (!options.has_patterns) return glob_match(path, options.pattern);
	for (u32 index = 0; index < options.patterns.count; ++index) {
		if (glob_match(path, options.patterns.items[index])) return true;
	}
	return false;
}

static b32 list_directory(List_Context *context, String relative)
{
	String directory = relative.size ? script_path_join(context->arena, context->options.root, relative) : context->options.root;
	Platform_Directory_Entries entries;
	if (!platform_list_directory(context->arena, directory, &entries)) return false;

	for (u32 index = 0; index < entries.count; ++index)
	{
		Platform_Directory_Entry entry = entries.items[index];
		String entry_relative = relative.size ? script_path_join(context->arena, relative, entry.name) : arena_push_string_copy(context->arena, entry.name);
		if (kind_matches(context->options.kind, entry.is_directory) && pattern_matches(context->options, entry_relative))
		{
			Path_Node *node = arena_push_zero_aligned(context->arena, sizeof(*node), _Alignof(Path_Node));
			node->path = context->options.relative ? entry_relative : script_path_join(context->arena, context->options.root, entry_relative);
			if (context->last) context->last->next = node;
			else context->first = node;
			context->last = node;
			++context->count;
		}
		if (entry.is_directory && !entry.is_symbolic_link && context->options.recursive && !list_directory(context, entry_relative)) return false;
	}
	return true;
}

static int compare_paths(const void *left, const void *right)
{
	String a = *(const String *)left;
	String b = *(const String *)right;
	u64 count = a.size < b.size ? a.size : b.size;
	for (u64 index = 0; index < count; ++index) {
		if ((u8)a.data[index] != (u8)b.data[index]) return (u8)a.data[index] < (u8)b.data[index] ? -1 : 1;
	}
	return a.size == b.size ? 0 : (a.size < b.size ? -1 : 1);
}

b32 script_list_paths(Arena *arena, Script_List_Paths_Options options, String_Array *result)
{
	if (!options.root.size) options.root = STRING_LITERAL(".");
	if (!options.has_patterns && !options.pattern.size) options.pattern = STRING_LITERAL("*");
	Scratch scratch = begin_different_scratch(arena);
	List_Context context = { .arena = scratch.arena, .options = options };
	if (!list_directory(&context, (String){0})) {
		end_scratch(scratch);
		return false;
	}

	result->count = context.count;
	result->items = arena_push_zero_aligned(arena, result->count * sizeof(*result->items), _Alignof(String));
	u32 index = 0;
	for (Path_Node *node = context.first; node; node = node->next) result->items[index++] = arena_push_string_copy(arena, node->path);
	qsort(result->items, result->count, sizeof(*result->items), compare_paths);
	end_scratch(scratch);
	return true;
}

b32 script_remove_path(Arena *arena, String path, b32 recursive)
{
	Platform_File_Info info;
	if (!platform_file_info(path, &info)) {
		if (bob_platform_remove_file(path)) return true;
		return bob_platform_remove_directory(path);
	}
	if (!info.is_directory) return bob_platform_remove_file(path);
	if (!recursive || info.is_symbolic_link) return bob_platform_remove_directory(path);

	u64 mark = arena_mark(arena);
	Platform_Directory_Entries entries;
	if (!platform_list_directory(arena, path, &entries)) return false;
	for (u32 index = 0; index < entries.count; ++index)
	{
		String child = script_path_join(arena, path, entries.items[index].name);
		if (!script_remove_path(arena, child, true)) {
			arena_restore(arena, mark);
			return false;
		}
	}
	arena_restore(arena, mark);
	return bob_platform_remove_directory(path);
}
