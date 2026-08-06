#include "make_depfile.h"

static b32 depfile_whitespace(char character)
{
	return character == ' ' || character == '\t' ||
		character == '\v' || character == '\f';
}

static b32 depfile_drive_colon(String token, String contents, u64 cursor)
{
	char letter;
	char separator;
	if (token.size != 1 || cursor + 1 >= contents.size) return false;
	letter = token.data[0];
	separator = contents.data[cursor + 1];
	return ((letter >= 'a' && letter <= 'z') ||
		(letter >= 'A' && letter <= 'Z')) &&
		(separator == '/' || separator == '\\');
}

static b32 depfile_add_dependency(String_Array *dependencies, String dependency)
{
	for (u32 i = 0; i < dependencies->count; ++i) {
		if (string_equal(dependencies->items[i], dependency)) return false;
	}
	dependencies->items[dependencies->count++] = dependency;
	return true;
}

b32 make_depfile_parse(Arena *arena, String contents, String_Array *dependencies)
{
	u64 result_mark;
	Scratch scratch;
	String_Array parsed = {0};
	u64 cursor = 0;
	u64 token_mark;
	u32 target_count = 0;
	b32 reading_dependencies = false;
	b32 valid = true;

	if (!arena || !dependencies || (!contents.data && contents.size)) return false;
	*dependencies = (String_Array){0};
	result_mark = arena_mark(arena);
	scratch = begin_different_scratch(arena);

	if (contents.size > (UINT32_MAX - 1)) {
		valid = false;
		goto done;
	}
	parsed.items = arena_push_zero_aligned(scratch.arena,
		(contents.size + 1) * sizeof(*parsed.items), _Alignof(String));
	if (!parsed.items) {
		valid = false;
		goto done;
	}
	token_mark = arena_mark(scratch.arena);

	while (cursor <= contents.size) {
		b32 end = cursor == contents.size;
		char character = end ? '\n' : contents.data[cursor];

		if (!end && character == '\\') {
			if (cursor + 1 < contents.size && contents.data[cursor + 1] == '\n') {
				character = ' ';
				cursor += 1;
			}
			else if (cursor + 2 < contents.size && contents.data[cursor + 1] == '\r' &&
				contents.data[cursor + 2] == '\n') {
				character = ' ';
				cursor += 2;
			}
			else if (cursor + 1 < contents.size) {
				char escaped = contents.data[cursor + 1];
				if (depfile_whitespace(escaped) || escaped == '#' || escaped == ':' ||
					escaped == '$' || escaped == '\\') {
					arena_append_char(scratch.arena, escaped);
					cursor += 2;
					continue;
				}
				arena_append_char(scratch.arena, character);
				++cursor;
				continue;
			}
			else {
				arena_append_char(scratch.arena, character);
				++cursor;
				continue;
			}
		}

		if (!end && character == '$' && cursor + 1 < contents.size &&
			contents.data[cursor + 1] == '$') {
			arena_append_char(scratch.arena, '$');
			cursor += 2;
			continue;
		}

		if (!end && character == ':' && !reading_dependencies) {
			String token = arena_string_from(scratch.arena, scratch.arena->data + token_mark);
			if (depfile_drive_colon(token, contents, cursor)) {
				arena_append_char(scratch.arena, character);
				++cursor;
				continue;
			}
			if (token.size) {
				++target_count;
				arena_restore(scratch.arena, token_mark);
			}
			if (!target_count) {
				valid = false;
				break;
			}
			reading_dependencies = true;
			++cursor;
			continue;
		}

		if (end || character == '\n' || character == '\r' ||
			depfile_whitespace(character) || character == '#') {
			String token = arena_string_from(scratch.arena, scratch.arena->data + token_mark);
			if (token.size) {
				if (reading_dependencies) {
					arena_finalize_string(scratch.arena, token);
					if (!depfile_add_dependency(&parsed, token)) {
						arena_restore(scratch.arena, token_mark);
					}
				}
				else {
					++target_count;
					arena_restore(scratch.arena, token_mark);
				}
			}
			token_mark = arena_mark(scratch.arena);

			if (!end && character == '#') {
				while (cursor < contents.size && contents.data[cursor] != '\n' &&
					contents.data[cursor] != '\r') ++cursor;
				continue;
			}
			if (end || character == '\n' || character == '\r') {
				if (target_count && !reading_dependencies) {
					valid = false;
					break;
				}
				target_count = 0;
				reading_dependencies = false;
				if (end) break;
				if (character == '\r' && cursor + 1 < contents.size &&
					contents.data[cursor + 1] == '\n') ++cursor;
			}
			++cursor;
			continue;
		}

		arena_append_char(scratch.arena, character);
		++cursor;
	}

	if (valid && parsed.count) {
		dependencies->items = arena_push_zero_aligned(arena,
			parsed.count * sizeof(*dependencies->items), _Alignof(String));
		if (!dependencies->items) {
			valid = false;
			goto done;
		}
		for (u32 i = 0; i < parsed.count; ++i) {
			dependencies->items[i] = arena_push_string_copy(arena, parsed.items[i]);
			if (!dependencies->items[i].data) {
				valid = false;
				goto done;
			}
			++dependencies->count;
		}
	}

done:
	end_scratch(scratch);
	if (!valid) {
		arena_restore(arena, result_mark);
		*dependencies = (String_Array){0};
	}
	return valid;
}
