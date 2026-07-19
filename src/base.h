#ifndef BASE_H
#define BASE_H

#include <stdint.h>
#include <stdarg.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef float  f32;
typedef double f64;

typedef i32 b32;

#if defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL _Thread_local
#endif

#ifndef true
#define true 1
#endif

#ifndef false
#define false 0
#endif

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define KILOBYTES(value) ((u64)(value) << 10)
#define MEGABYTES(value) ((u64)(value) << 20)
#define GIGABYTES(value) ((u64)(value) << 30)

typedef struct Arena {
	u64 capacity;
	u64 committed;
	u64 used;
	u8 *data;
} Arena;

typedef struct Scratch {
	Arena *arena;
	u64 restore_used;
} Scratch;

typedef struct String {
	union {
		char *data;
		char *text;
	};
	u64 size;
} String;

typedef struct String_Array {
	String *items;
	u32 count;
} String_Array;

#define STRING_LITERAL(text) ((String){ .data = (char *)(text), .size = sizeof(text) - 1 })

extern THREAD_LOCAL Arena global_scratch_arena;

Arena arena_create(u64 capacity);
void arena_destroy(Arena *arena);
void arena_reset(Arena *arena);
u64 arena_mark(Arena *arena);
void arena_restore(Arena *arena, u64 mark);
void *arena_top(Arena *arena);
void *arena_reserve(Arena *arena, u64 size);
void *arena_reserve_aligned(Arena *arena, u64 size, u64 alignment);
void *arena_push(Arena *arena, u64 size);
void *arena_push_aligned(Arena *arena, u64 size, u64 alignment);
void *arena_push_zero(Arena *arena, u64 size);
void *arena_push_zero_aligned(Arena *arena, u64 size, u64 alignment);
void *arena_push_copy(Arena *arena, u64 size, const void *data);
void *arena_push_copy_aligned(Arena *arena, u64 size, u64 alignment, const void *data);
char *arena_push_data(Arena *arena, const void *data, u64 size);
char *arena_append_text(Arena *arena, const char *text);
char *arena_append_str(Arena *arena, String string);
char *arena_append_char(Arena *arena, char character);
void arena_push_repeat(Arena *arena, char character, u64 count);
char *arena_appendfv(Arena *arena, const char *format, va_list arguments);
char *arena_appendf(Arena *arena, const char *format, ...);
String arena_string_from(Arena *arena, void *start);
void arena_finalize_string(Arena *arena, String string);

Scratch get_scratch(void);
void end_scratch(Scratch scratch);
void destroy_global_scratch(void);

String string_from_data(void *data, u64 size);
String string_from_range(void *start, void *end);
String string_from_cstring(const char *text);
b32 string_is_terminated(String string);
b32 string_equal(String a, String b);
String string_slice(String string, u64 offset, u64 size);
String_Array string_split(Arena *arena, String string, char separator);
String_Array string_split_lines(Arena *arena, String string);
String_Array string_split_block(Arena *arena, String string);
b32 string_split_first(String string, char separator, String *left, String *right);
String string_trim_whitespace(String string);
String arena_push_string_copy(Arena *arena, String string);
String arena_push_cstring(Arena *arena, const char *text);
b32 string_equal_insensitive(String left, String right);
b32 string_starts_with(String text, String prefix);
b32 string_ends_with(String text, String suffix);
b32 string_is(String text, const char *literal);
u32 string_count_lines(String str);



#endif
