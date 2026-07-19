#include "base.h"
#include "platform.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

THREAD_LOCAL Arena global_scratch_arena;

Arena arena_create(u64 capacity)
{
	Arena arena = {0};
	if (capacity == 0) capacity = MEGABYTES(64);
	arena.data = platform_virtual_reserve(capacity);
	if (arena.data) arena.capacity = capacity;
	return arena;
}

void arena_destroy(Arena *arena)
{
	if (!arena) return;
	platform_virtual_free(arena->data);
	memset(arena, 0, sizeof(*arena));
}

void arena_reset(Arena *arena)
{
	if (arena) arena->used = 0;
}

u64 arena_mark(Arena *arena)
{
	return arena ? arena->used : 0;
}

void arena_restore(Arena *arena, u64 mark)
{
	assert(arena && mark <= arena->used);
	if (arena && mark <= arena->used) arena->used = mark;
}

void *arena_top(Arena *arena)
{
	return arena && arena->data ? arena->data + arena->used : NULL;
}

void *arena_reserve_aligned(Arena *arena, u64 size, u64 alignment)
{
	uintptr_t top;
	u64 padding;
	u64 needed;

	if (!arena || !arena->data || alignment == 0) {
		assert(!"invalid arena or alignment");
		return NULL;
	}
	top = (uintptr_t)(arena->data + arena->used);
	padding = (alignment - top % alignment) % alignment;
	if (padding > arena->capacity - arena->used ||
	size > arena->capacity - arena->used - padding) {
		assert(!"arena capacity exceeded");
	return NULL;
}
needed = arena->used + padding + size;
if (needed > arena->committed) {
	u64 commit_granularity = KILOBYTES(64);
	u64 committed = (needed + commit_granularity - 1) &
	~(commit_granularity - 1);
	if (committed > arena->capacity) committed = arena->capacity;
	if (!platform_virtual_commit(arena->data + arena->committed,
	committed - arena->committed)) {
		assert(!"unable to commit arena memory");
	return NULL;
}
arena->committed = committed;
}
return arena->data + arena->used + padding;
}

void *arena_reserve(Arena *arena, u64 size)
{
	return arena_reserve_aligned(arena, size, 1);
}

void *arena_push_aligned(Arena *arena, u64 size, u64 alignment)
{
	char *result = arena_reserve_aligned(arena, size, alignment);
	if (!result) return NULL;
	arena->used = (u64)((u8 *)result - arena->data) + size;
	return result;
}

void *arena_push(Arena *arena, u64 size)
{
	return arena_push_aligned(arena, size, 1);
}

void *arena_push_zero_aligned(Arena *arena, u64 size, u64 alignment)
{
	void *result = arena_push_aligned(arena, size, alignment);
	if (result) memset(result, 0, (size_t)size);
	return result;
}

void *arena_push_zero(Arena *arena, u64 size)
{
	return arena_push_zero_aligned(arena, size, 1);
}

void *arena_push_copy_aligned(Arena *arena, u64 size, u64 alignment, const void *data)
{
	void *result = arena_push_aligned(arena, size, alignment);
	if (result && size) memcpy(result, data, (size_t)size);
	return result;
}

void *arena_push_copy(Arena *arena, u64 size, const void *data)
{
	return arena_push_copy_aligned(arena, size, 1, data);
}

char *arena_push_data(Arena *arena, const void *data, u64 size)
{
	return arena_push_copy(arena, size, data);
}

static char *arena_append_data(Arena *arena, const void *data, u64 size)
{
	char *result = arena_reserve(arena, size + 1);
	assert(result);
	if (size) memmove(result, data, (size_t)size);
	result[size] = 0;
	arena->used += size;
	return result;
}

char *arena_append_text(Arena *arena, const char *text)
{
	return arena_append_data(arena, text, text ? (u64)strlen(text) : 0);
}

char *arena_append_str(Arena *arena, String string)
{
	return arena_append_data(arena, string.data, string.size);
}

char *arena_append_char(Arena *arena, char character)
{
	char *result = arena_reserve(arena, 2);
	assert(result);
	arena->used += 1;
	*result = character;
	*(result + 1) = 0;
	return result;
}

void arena_push_repeat(Arena *arena, char character, u64 count)
{
	void *result = arena_push(arena, count);
	if (result && count) memset(result, character, (size_t)count);
}

char *arena_appendfv(Arena *arena, const char *format, va_list arguments)
{
	va_list copy;
	int length;
	char *data;

	va_copy(copy, arguments);
	length = vsnprintf(NULL, 0, format, copy);
	va_end(copy);
	assert(length >= 0);
	if (length < 0) return NULL;
	data = arena_reserve(arena, (u64)length + 1);
	assert(data);
	if (!data) return NULL;
	int written = vsnprintf(data, (size_t)length + 1, format, arguments);
	assert(written == length);
	if (written != length) {
		return NULL;
	}
	arena->used += (u64)length;
	return data;
}

char *arena_appendf(Arena *arena, const char *format, ...)
{
	va_list arguments;
	char *result;
	va_start(arguments, format);
	result = arena_appendfv(arena, format, arguments);
	va_end(arguments);
	return result;
}

String arena_string_from(Arena *arena, void *start)
{
	assert(arena && start && (u8 *)start >= arena->data &&
		(u8 *)start <= arena->data + arena->used);
	if (!arena || !start || (u8 *)start < arena->data ||
		(u8 *)start > arena->data + arena->used) return (String){0};
	return string_from_range(start, arena_top(arena));
}

void arena_finalize_string(Arena *arena, String string)
{
	assert(arena && string.data && string.data + string.size == (char *)arena_top(arena));
	assert(*(char *)arena_top(arena) == 0);
	if (!arena || !string.data || string.data + string.size != (char *)arena_top(arena)) return;
	arena_push_zero(arena, 1);
}

Scratch get_scratch(void)
{
	Scratch scratch;
	if (!global_scratch_arena.data) {
		global_scratch_arena = arena_create(0);
	}
	scratch.arena = &global_scratch_arena;
	scratch.restore_used = global_scratch_arena.used;
	return scratch;
}

void end_scratch(Scratch scratch)
{
	arena_restore(scratch.arena, scratch.restore_used);
}

void destroy_global_scratch(void)
{
	arena_destroy(&global_scratch_arena);
}

String string_from_data(void *data, u64 size)
{
	String string;
	string.data = data;
	string.size = size;
	return string;
}

String string_from_range(void *start, void *end)
{
	assert((u8 *)end >= (u8 *)start);
	if ((u8 *)end < (u8 *)start) return (String){0};
	return string_from_data(start, (u64)((u8 *)end - (u8 *)start));
}

String string_from_cstring(const char *text)
{
	return string_from_data((char *)text, text ? (u64)strlen(text) : 0);
}

b32 string_is_terminated(String string)
{
	return string.data && string.data[string.size] == 0;
}

String string_slice(String string, u64 offset, u64 size)
{
	assert(offset <= string.size && size <= string.size - offset);
	if (offset > string.size || size > string.size - offset) {
		return (String){0};
	}
	return string_from_data(string.data + offset, size);
}

String_Array string_split(Arena *arena, String string, char separator)
{
	String_Array result = {0};
	if (!arena) return result;

	u32 count = 1;
	for (u64 i = 0; i < string.size; ++i) {
		if (string.data[i] == separator) {
			if (count == UINT32_MAX) return result;
			++count;
		}
	}

	result.items = arena_push_zero_aligned(arena, sizeof(*result.items) * count, _Alignof(String));
	if (!result.items) return (String_Array){0};

	u64 start = 0;

	for (u64 i = 0; i <= string.size; ++i) {
		if (i == string.size || string.data[i] == separator) {
			result.items[result.count++] = string_slice(string, start, i - start);
			start = i + 1;
		}
	}
	return result;
}

String_Array string_split_lines(Arena *arena, String string)
{
	String_Array result = string_split(arena, string, '\n');
	for (u32 i = 0; i < result.count; ++i) {
		String *line = result.items + i;
		if (line->size && line->data[line->size - 1] == '\r') --line->size;
	}
	return result;
}

String_Array string_split_block(Arena *arena, String string)
{
	String_Array result = string_split(arena, string, 0);
	while (result.count && result.items[result.count - 1].size == 0) {
		--result.count;
	}
	return result;
}

String arena_push_string_copy(Arena *arena, String string)
{
	char *data = arena_push(arena, string.size + 1);
	if (!data) return (String){0};
	if (string.size) memcpy(data, string.data, (size_t)string.size);
	data[string.size] = 0;
	return string_from_data(data, string.size);
}

String arena_push_cstring(Arena *arena, const char *text)
{
	return arena_push_string_copy(arena, string_from_cstring(text));
}

b32 string_equal(String a, String b)
{
	return a.size == b.size && (a.size == 0 || memcmp(a.data, b.data, (size_t)a.size) == 0);
}

static b32 ascii_lower(u32 c)
{
	return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

b32 string_equal_insensitive(String left, String right)
{
	u64 i;
	if (left.size != right.size) return false;
	for (i = 0; i < left.size; ++i) {
		if (ascii_lower(left.data[i]) != ascii_lower(right.data[i])) {
			return false;
		}
	}
	return true;
}

b32 string_starts_with(String text, String prefix)
{
	return prefix.size <= text.size &&
	memcmp(text.data, prefix.data, (size_t)prefix.size) == 0;
}

b32 string_ends_with(String text, String suffix)
{
	return suffix.size <= text.size &&
	memcmp(text.data + text.size - suffix.size, suffix.data,
	(size_t)suffix.size) == 0;
}

b32 string_is(String text, const char *literal)
{
	return string_equal(text, string_from_cstring(literal));
}

u32 string_count_lines(String str)
{
	u64 i;
	u32 count = str.size ? 1 : 0;
	for (i = 0; i < str.size; ++i) {
		if (str.data[i] == '\n') ++count;
	}
	return count;
}

