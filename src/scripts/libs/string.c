#include "scripts/libs/string.h"

typedef struct Expand_Parser
{
	Arena *arena;
	String string;
	char *cur;
	char *eof;
	b32 last;
	const char *error;
}
Expand_Parser;

static b32 expand_sequence(Expand_Parser *parser, b32 group)
{
	while (parser->cur < parser->eof && *parser->cur != ')')
	{
		u64 mark = arena_mark(parser->arena);
		if (*parser->cur == '\'')
		{
			char *start = ++parser->cur;
			while (parser->cur < parser->eof && *parser->cur != '\'') ++parser->cur;
			if (parser->cur == parser->eof) {
				parser->error = "unterminated literal";
				return false;
			}
			arena_append_str(parser->arena, string_from_range(start, parser->cur));
			++parser->cur;
		}
		else if (*parser->cur == '.')
		{
			arena_append_str(parser->arena, parser->string);
			++parser->cur;
		}
		else if (*parser->cur == '(')
		{
			++parser->cur;
			if (!expand_sequence(parser, true)) return false;
		}
		else
		{
			parser->error = "expected a literal, '.', or group";
			return false;
		}

		if (parser->cur < parser->eof && *parser->cur == '*') {
			++parser->cur;
			if (parser->last) arena_restore(parser->arena, mark);
		}
	}

	if (!group) return true;
	if (parser->cur == parser->eof) {
		parser->error = "unterminated group";
		return false;
	}
	++parser->cur;
	return true;
}

b32 script_strings_expand(Arena *arena, String_Array strings, String rule, String *result, const char **error)
{
	u64 mark = arena_mark(arena);
	void *start = arena_top(arena);
	arena_append_str(arena, (String){0});

	for (u32 index = 0; index < strings.count; ++index)
	{
		Expand_Parser parser = {
			.arena = arena,
			.string = strings.items[index],
			.cur = rule.data,
			.eof = rule.data + rule.size,
			.last = index + 1 == strings.count,
		};
		if (!expand_sequence(&parser, false) || parser.cur != parser.eof)
		{
			arena_restore(arena, mark);
			*result = (String){0};
			if (error) *error = parser.error ? parser.error : "unexpected ')'";
			return false;
		}
	}

	arena_append_str(arena, (String){0});
	*result = arena_string_from(arena, start);
	arena_finalize_string(arena, *result);
	if (error) *error = NULL;
	return true;
}
