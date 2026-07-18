#include "c_include_scan.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Scan_Context {
    const char **include_directories;
    u32 include_directory_count;
    String_Array command_include_directories;
    char **visited;
    u32 visited_count;
    u32 visited_capacity;
    C_Include_Scan_Result *result;
} Scan_Context;

typedef struct Parsed_Include {
    String path;
    b32 quoted;
} Parsed_Include;

typedef struct Parsed_Include_Array {
    Parsed_Include *items;
    u32 count;
} Parsed_Include_Array;

static char *copy_string(const char *string);

static b32 next_command_argument(const char **cursor_in_out, char *argument,
                                 size_t argument_size)
{
    const char *cursor = *cursor_in_out;
    size_t count = 0;
    b32 quoted = false;

    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (!*cursor) {
        *cursor_in_out = cursor;
        return false;
    }
    while (*cursor && (quoted || (*cursor != ' ' && *cursor != '\t'))) {
        if (*cursor == '"') {
            quoted = !quoted;
            ++cursor;
            continue;
        }
        if (count + 1 >= argument_size) return false;
        argument[count++] = *cursor++;
    }
    argument[count] = 0;
    *cursor_in_out = cursor;
    return true;
}

static String_Array parse_command_include_directories(Arena *arena,
                                                      const char *command_line)
{
    String_Array result = {0};
    const char *cursor = command_line;
    char argument[32768];
    size_t maximum_count;

    if (!command_line) return result;
    maximum_count = strlen(command_line) / 2 + 1;
    if (maximum_count > UINT32_MAX) return result;
    result.items = arena_push_zero(arena, maximum_count * sizeof(*result.items));
    if (!result.items) return (String_Array){0};

    while (next_command_argument(&cursor, argument, sizeof(argument))) {
        const char *directory = NULL;
        if (strcmp(argument, "/I") == 0 || strcmp(argument, "-I") == 0 ||
            strcmp(argument, "-isystem") == 0) {
            if (!next_command_argument(&cursor, argument, sizeof(argument))) break;
            directory = argument;
        } else if ((argument[0] == '/' || argument[0] == '-') &&
                   argument[1] == 'I' && argument[2]) {
            directory = argument + 2;
        }
        if (directory && *directory) {
            result.items[result.count] = arena_push_cstring(arena, directory);
            if (!result.items[result.count].data) return (String_Array){0};
            ++result.count;
        }
    }
    return result;
}

static b32 ascii_equal_ignore_case(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return *a == *b;
}

static b32 is_c_file(const char *path)
{
    const char *extension = strrchr(path, '.');
    return extension &&
        (ascii_equal_ignore_case(extension, ".c") ||
         ascii_equal_ignore_case(extension, ".h") ||
         ascii_equal_ignore_case(extension, ".hh") ||
         ascii_equal_ignore_case(extension, ".hpp") ||
         ascii_equal_ignore_case(extension, ".inc") ||
         ascii_equal_ignore_case(extension, ".inl"));
}

static char *copy_string(const char *string)
{
    size_t size = strlen(string) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, string, size);
    return copy;
}

static b32 remember_path(Scan_Context *context, const char *path, b32 *was_seen)
{
    u32 i;

    for (i = 0; i < context->visited_count; ++i) {
        if (strcmp(context->visited[i], path) == 0) {
            *was_seen = true;
            return true;
        }
    }
    *was_seen = false;
    if (context->visited_count == context->visited_capacity) {
        u32 capacity = context->visited_capacity ? context->visited_capacity * 2 : 16;
        char **visited = realloc(context->visited, capacity * sizeof(*visited));
        if (!visited) return false;
        context->visited = visited;
        context->visited_capacity = capacity;
    }
    context->visited[context->visited_count] = copy_string(path);
    if (!context->visited[context->visited_count]) return false;
    ++context->visited_count;
    return true;
}

typedef struct Include_Scanner {
    char *cur;
    char *eof;
    b32 can_start_directive;
} Include_Scanner;

static b32 is_identifier_continue(char character)
{
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') || character == '_' ||
           (character >= '0' && character <= '9');
}

static b32 is_horizontal_space(char character)
{
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\v' || character == '\f';
}

static void skip_trivia(Include_Scanner *scanner)
{
    char *cur = scanner->cur;

    for (;;) {
        if (cur < scanner->eof && is_horizontal_space(*cur)) {
            ++cur;
        } else if (cur < scanner->eof && *cur == '\n') {
            ++cur;
            scanner->can_start_directive = true;
        } else if (cur < scanner->eof && cur + 1 < scanner->eof &&
                   cur[0] == '/' && cur[1] == '/') {
            cur += 2;
            while (cur < scanner->eof && *cur != '\n') ++cur;
        } else if (cur < scanner->eof && cur + 1 < scanner->eof &&
                   cur[0] == '/' && cur[1] == '*') {
            cur += 2;
            while (cur < scanner->eof) {
                if (*cur == '\n') scanner->can_start_directive = true;
                if (*cur == '*' && cur + 1 < scanner->eof && cur[1] == '/') {
                    cur += 2;
                    break;
                }
                ++cur;
            }
        } else {
            break;
        }
    }
    scanner->cur = cur;
}

static b32 skip_directive_trivia(Include_Scanner *scanner)
{
    char *cur = scanner->cur;

    for (;;) {
        while (cur < scanner->eof && is_horizontal_space(*cur)) ++cur;
        if (cur < scanner->eof && cur + 1 < scanner->eof &&
            cur[0] == '/' && cur[1] == '*') {
            cur += 2;
            while (cur < scanner->eof) {
                if (*cur == '\n') return false;
                if (*cur == '*' && cur + 1 < scanner->eof && cur[1] == '/') {
                    cur += 2;
                    break;
                }
                ++cur;
            }
            if (cur == scanner->eof) return false;
        } else {
            scanner->cur = cur;
            return cur < scanner->eof && *cur != '\n';
        }
    }
}

static void skip_quoted_region(Include_Scanner *scanner)
{
    char closing = *scanner->cur++;

    while (scanner->cur < scanner->eof) {
        char character = *scanner->cur++;
        if (character == '\\' && scanner->cur < scanner->eof) {
            ++scanner->cur;
        } else if (character == closing) {
            break;
        }
    }
}

static b32 parse_include_directive(Include_Scanner *scanner,
                                   Parsed_Include *include)
{
    static const char name[] = "include";
    char *start;
    char closing;

    if (!skip_directive_trivia(scanner) ||
        scanner->eof - scanner->cur < (ptrdiff_t)(sizeof(name) - 1) ||
        memcmp(scanner->cur, name, sizeof(name) - 1) != 0) return false;
    scanner->cur += sizeof(name) - 1;
    if (scanner->cur < scanner->eof &&
        is_identifier_continue(*scanner->cur)) return false;

    if (!skip_directive_trivia(scanner) ||
        (*scanner->cur != '"' && *scanner->cur != '<')) return false;
    include->quoted = *scanner->cur == '"';
    closing = include->quoted ? '"' : '>';
    start = ++scanner->cur;
    while (scanner->cur < scanner->eof && *scanner->cur != closing &&
           *scanner->cur != '\n') ++scanner->cur;
    if (scanner->cur == scanner->eof || *scanner->cur != closing ||
        scanner->cur == start) return false;

    include->path = string_from_range(start, scanner->cur++);
    return true;
}

static b32 next_file_include(Include_Scanner *scanner, Parsed_Include *include)
{
    while (scanner->cur < scanner->eof) {
        skip_trivia(scanner);
        if (scanner->cur == scanner->eof) break;

        if (scanner->can_start_directive && *scanner->cur == '#') {
            ++scanner->cur;
            scanner->can_start_directive = false;
            if (parse_include_directive(scanner, include)) return true;
            continue;
        }

        scanner->can_start_directive = false;
        if (*scanner->cur == '"' || *scanner->cur == '\'') {
            skip_quoted_region(scanner);
        } else {
            ++scanner->cur;
        }
    }
    return false;
}

static Parsed_Include_Array parse_file_includes(Arena *arena, String source)
{
    Parsed_Include_Array result = {0};
    u32 count = 0;
    u32 pass;

    for (pass = 0; pass < 2; ++pass) {
        Include_Scanner scanner = {
            source.data, source.data + source.size, true
        };
        u32 index = 0;

        if (pass == 1) {
            if (count == 0) break;
            result.items = arena_push_zero(arena, sizeof(*result.items) * count);
            if (!result.items) return (Parsed_Include_Array){0};
            result.count = count;
        }

        for (;;) {
            Parsed_Include include;
            if (!next_file_include(&scanner, &include)) break;
            if (pass == 0) {
                ++count;
            } else {
                result.items[index] = include;
                result.items[index].path =
                    arena_push_string_copy(arena, include.path);
                if (!result.items[index].path.data) {
                    return (Parsed_Include_Array){0};
                }
                ++index;
            }
        }
    }
    return result;
}

static b32 join_path(char *output, size_t output_size, const char *directory,
                     const char *name)
{
    size_t length = strlen(directory);
    const char *separator = length &&
        (directory[length - 1] == '/' || directory[length - 1] == '\\') ? "" : "/";
    int written = snprintf(output, output_size, "%s%s%s", directory, separator, name);
    return written > 0 && (size_t)written < output_size;
}

static b32 resolve_include(Scan_Context *context, const char *including_file,
                           const char *name, b32 quoted, Arena *arena,
                           String *resolved)
{
    char candidate[32768];
    u32 i;

    if (quoted) {
        char directory[32768];
        char *slash;
        size_t including_size = strlen(including_file) + 1;
        if (including_size <= sizeof(directory)) {
            memcpy(directory, including_file, including_size);
            slash = strrchr(directory, '\\');
            if (!slash) slash = strrchr(directory, '/');
            if (slash) {
                *slash = 0;
                if (join_path(candidate, sizeof(candidate), directory, name) &&
                    platform_file_info(candidate, &(Platform_File_Info){0}) &&
                    platform_absolute_path(arena, candidate, resolved)) {
                    return true;
                }
            }
        }
    }

    for (i = 0; i < context->include_directory_count; ++i) {
        if (join_path(candidate, sizeof(candidate), context->include_directories[i], name) &&
            platform_file_info(candidate, &(Platform_File_Info){0}) &&
            platform_absolute_path(arena, candidate, resolved)) {
            return true;
        }
    }
    for (i = 0; i < context->command_include_directories.count; ++i) {
        if (join_path(candidate, sizeof(candidate),
                      context->command_include_directories.items[i].data, name) &&
            platform_file_info(candidate, &(Platform_File_Info){0}) &&
            platform_absolute_path(arena, candidate, resolved)) {
            return true;
        }
    }
    return false;
}

static b32 scan_file(Scan_Context *context, const char *path, u32 depth)
{
    String absolute;
    Platform_File_Info info;
    b32 was_seen;
    String source;
    Scratch scratch;
    Parsed_Include_Array includes;
    u32 include_index;
    b32 succeeded = true;

    scratch = get_scratch();
    if (depth > 256 ||
        !platform_absolute_path(scratch.arena, path, &absolute) ||
        !platform_file_info(absolute.data, &info)) {
        end_scratch(scratch);
        return true;
    }
    if (!remember_path(context, absolute.data, &was_seen)) {
        end_scratch(scratch);
        return false;
    }
    if (was_seen) {
        end_scratch(scratch);
        return true;
    }
    if (info.write_time > context->result->newest_write_time) {
        context->result->newest_write_time = info.write_time;
    }
    if (!is_c_file(absolute.data)) {
        end_scratch(scratch);
        return true;
    }

    if (!platform_read_entire_file(scratch.arena, absolute.data, &source)) {
        end_scratch(scratch);
        return true;
    }
    includes = parse_file_includes(scratch.arena, source);

    for (include_index = 0; include_index < includes.count; ++include_index) {
        Parsed_Include include = includes.items[include_index];
        String resolved;
        u64 resolved_mark = arena_mark(scratch.arena);
        if (resolve_include(context, absolute.data, include.path.data, include.quoted,
                            scratch.arena, &resolved)) {
            if (!scan_file(context, resolved.data, depth + 1)) {
                succeeded = false;
                arena_restore(scratch.arena, resolved_mark);
                break;
            }
        } else if (include.quoted) {
            context->result->unresolved_quoted_include = true;
        }
        arena_restore(scratch.arena, resolved_mark);
    }
    end_scratch(scratch);
    return succeeded;
}

b32 c_include_scan(const char **inputs, u32 input_count,
                   const char **include_directories, u32 include_directory_count,
                   const char *command_line,
                   C_Include_Scan_Result *result)
{
    Scan_Context context = {0};
    Scratch scratch;
    u32 i;
    b32 succeeded = true;

    if (!result) return false;
    memset(result, 0, sizeof(*result));
    context.include_directories = include_directories;
    context.include_directory_count = include_directory_count;
    context.result = result;
    scratch = get_scratch();
    context.command_include_directories =
        parse_command_include_directories(scratch.arena, command_line);

    for (i = 0; i < input_count; ++i) {
        if (!scan_file(&context, inputs[i], 0)) {
            succeeded = false;
            break;
        }
    }
    for (i = 0; i < context.visited_count; ++i) free(context.visited[i]);
    free(context.visited);
    end_scratch(scratch);
    return succeeded;
}
