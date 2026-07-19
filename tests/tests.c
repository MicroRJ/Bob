#include "bob.h"
#include "script.h"
#include "c_include_scan.h"
#include "logger.h"
#include "platform/platform.h"
#include "vcvars_cache.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run;
static int tests_failed;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);      \
            return false;                                                       \
        }                                                                       \
    } while (0)

#define CHECK_OK(expression) CHECK((expression) == BOB_OK)
#define STRING_ARRAY_FROM(array) ((String_Array){ .items = (array), .count = ARRAY_COUNT(array) })

static b32 environment_equals(const char *name, const char *expected)
{
    Scratch scratch = begin_scratch();
    String value = {0};
	b32 equal = platform_get_environment(string_from_cstring(name), scratch.arena, &value) &&
        string_equal(value, string_from_cstring(expected));
    end_scratch(scratch);
    return equal;
}

static b32 test_vcvars_cache_application(void)
{
    static const char cache_text[] =
        "BOB_VCVARS_CACHE_V1\n"
        "prepend BOB_VCVARS_TEST_PREPEND=tool;\n"
        "append BOB_VCVARS_TEST_APPEND=;tail\n"
        "set BOB_VCVARS_TEST_SET=value\n";
    b32 result = false;

	platform_set_environment(STRING_LITERAL("BOB_VCVARS_TEST_PREPEND"), STRING_LITERAL("base"));
	platform_set_environment(STRING_LITERAL("BOB_VCVARS_TEST_APPEND"), STRING_LITERAL("base"));
	platform_set_environment(STRING_LITERAL("BOB_VCVARS_TEST_SET"), (String){0});

    if (!vcvars_cache_apply(string_from_cstring(cache_text))) goto cleanup;
    if (!environment_equals("BOB_VCVARS_TEST_PREPEND", "tool;base")) goto cleanup;
    if (!environment_equals("BOB_VCVARS_TEST_APPEND", "base;tail")) goto cleanup;
    if (!environment_equals("BOB_VCVARS_TEST_SET", "value")) goto cleanup;
    result = true;

cleanup:
	platform_set_environment(STRING_LITERAL("BOB_VCVARS_TEST_PREPEND"), (String){0});
	platform_set_environment(STRING_LITERAL("BOB_VCVARS_TEST_APPEND"), (String){0});
	platform_set_environment(STRING_LITERAL("BOB_VCVARS_TEST_SET"), (String){0});
    return result;
}

static b32 test_high_resolution_timer(void)
{
    u64 frequency = platform_performance_frequency();
    u64 before = platform_performance_counter();
    Sleep(1);
    return frequency > 0 && platform_performance_counter() >= before && platform_current_thread_id() != 0;
}

static Bob_Node *add_node(Bob *graph, const char *name)
{
    Bob_Node *node = NULL;
    Bob_Error result = bob_add_task(graph, (Bob_Task){ .name = string_from_cstring(name) }, &node);
    if (result != BOB_OK) {
        printf("  unable to add node %s: %s\n", name, bob_error_string(result));
        exit(2);
    }
    return node;
}

static b32 run_tasks(Bob *graph, const Bob_Task *tasks, u32 task_count,
                     u32 worker_count)
{
    u32 i;
    if (bob_task_count(graph) != task_count) return false;
    for (i = 0; i < task_count; ++i) {
        if (bob_set_task(graph, bob_node_at(graph, i), tasks[i]) != BOB_OK) {
            return false;
        }
    }
    return bob_build(graph, worker_count);
}

static b32 test_arena_and_strings(void)
{
    Arena arena = arena_create(KILOBYTES(4));
    char *start;
    String built;
    String copy;
    void *aligned;
    u64 mark;
    Scratch outer;
    Scratch inner;

    CHECK(arena.data != NULL);
    start = arena_top(&arena);
    CHECK(arena_append_text(&arena, "hello") == start);
	CHECK(*(char *)arena_top(&arena) == 0);
    CHECK(arena_append_str(&arena, STRING_LITERAL(" arena")) != NULL);
	CHECK(*(char *)arena_top(&arena) == 0);
    CHECK(arena_appendf(&arena, " %d", 42) != NULL);
	CHECK(*(char *)arena_top(&arena) == 0);
	CHECK(arena_append_char(&arena, '!') != NULL);
	CHECK(*(char *)arena_top(&arena) == 0);
	built = arena_string_from(&arena, start);
	arena_finalize_string(&arena, built);
	CHECK(string_equal(built, STRING_LITERAL("hello arena 42!")));
    CHECK(built.data[built.size] == 0);

    copy = arena_push_string_copy(&arena, built);
    CHECK(string_equal(copy, built));
    CHECK(copy.data[copy.size] == 0);
    CHECK(string_equal(string_slice(copy, 6, 5), STRING_LITERAL("arena")));
	CHECK(string_is_terminated(STRING_LITERAL("hello")));
	CHECK(!string_is_terminated(string_slice(STRING_LITERAL("hello"), 0, 4)));
	CHECK(string_ends_with_insensitive(STRING_LITERAL("build.ELF"), STRING_LITERAL(".elf")));
	CHECK(!string_ends_with_insensitive(STRING_LITERAL("build.lua"), STRING_LITERAL(".elf")));
	{
		String_Array parts = string_split(&arena, STRING_LITERAL("a;;b;"), ';');
		CHECK(parts.count == 4);
		CHECK(string_equal(parts.items[0], STRING_LITERAL("a")));
		CHECK(parts.items[1].size == 0);
		CHECK(string_equal(parts.items[2], STRING_LITERAL("b")));
		CHECK(parts.items[3].size == 0);
	}
	{
		String_Array lines = string_split_lines(&arena, STRING_LITERAL("one\r\ntwo\n"));
		CHECK(lines.count == 3);
		CHECK(string_equal(lines.items[0], STRING_LITERAL("one")));
		CHECK(string_equal(lines.items[1], STRING_LITERAL("two")));
		CHECK(lines.items[2].size == 0);
	}
	{
		String_Array entries = string_split_block(&arena,
			(String){ .data = "one\0two\0", .size = 8 });
		CHECK(entries.count == 2);
		CHECK(string_equal(entries.items[0], STRING_LITERAL("one")));
		CHECK(string_equal(entries.items[1], STRING_LITERAL("two")));
	}
	{
		String left;
		String right;
		CHECK(string_split_first(STRING_LITERAL("NAME=a=b"), '=', &left, &right));
		CHECK(string_equal(left, STRING_LITERAL("NAME")));
		CHECK(string_equal(right, STRING_LITERAL("a=b")));
		CHECK(!string_split_first(STRING_LITERAL("NAME"), '=', &left, &right));
		CHECK(string_equal(
			string_trim_whitespace(STRING_LITERAL(" \t value \r\n")),
			STRING_LITERAL("value")));
	}

    CHECK(arena_push(&arena, 1) != NULL);
    aligned = arena_push_zero_aligned(&arena, 32, 32);
    CHECK(aligned != NULL);
    CHECK((uintptr_t)aligned % 32 == 0);
    CHECK(((u8 *)aligned)[0] == 0 && ((u8 *)aligned)[31] == 0);

    mark = arena_mark(&arena);
    CHECK(arena_push_zero(&arena, 128) != NULL);
    arena_restore(&arena, mark);
    CHECK(arena_mark(&arena) == mark);
    arena_destroy(&arena);

    outer = begin_scratch();
    CHECK(arena_append_text(outer.arena, "outer") != NULL);
    inner = begin_scratch();
	CHECK(inner.arena == outer.arena);
    CHECK(arena_append_text(inner.arena, "inner") != NULL);
    end_scratch(inner);
    CHECK(arena_mark(outer.arena) == inner.restore_used);
	{
		Scratch separate = begin_different_scratch(outer.arena);
		CHECK(separate.arena != outer.arena);
		CHECK(arena_append_text(separate.arena, "separate") != NULL);
		end_scratch(separate);
	}
    end_scratch(outer);
	CHECK(arena_mark(outer.arena) == outer.restore_used);
    destroy_global_scratch();
    return true;
}

typedef struct Scratch_Thread_Test {
    Arena *arena;
    HANDLE ready;
    HANDLE release;
} Scratch_Thread_Test;

static DWORD WINAPI scratch_thread_test_main(void *parameter)
{
    Scratch_Thread_Test *test = parameter;
    Scratch scratch = begin_scratch();
    arena_append_text(scratch.arena, "thread scratch");
    test->arena = scratch.arena;
    SetEvent(test->ready);
    WaitForSingleObject(test->release, INFINITE);
    end_scratch(scratch);
    destroy_global_scratch();
    return 0;
}

static b32 test_thread_local_scratch(void)
{
    Scratch main_scratch = begin_scratch();
    Scratch_Thread_Test tests[2] = {0};
    HANDLE threads[2] = {0};
    HANDLE ready[2] = {0};
    HANDLE release = CreateEventA(NULL, TRUE, FALSE, NULL);
    b32 passed = false;

    if (!release) goto cleanup;
    for (u32 i = 0; i < ARRAY_COUNT(tests); ++i)
    {
        tests[i].ready = CreateEventA(NULL, TRUE, FALSE, NULL);
        tests[i].release = release;
        ready[i] = tests[i].ready;
        if (!ready[i]) goto cleanup;
        threads[i] = CreateThread(NULL, 0, scratch_thread_test_main, tests + i, 0, NULL);
        if (!threads[i]) goto cleanup;
    }
    if (WaitForMultipleObjects(ARRAY_COUNT(ready), ready, TRUE, 5000) != WAIT_OBJECT_0) goto cleanup;
    passed = tests[0].arena && tests[1].arena &&
        tests[0].arena != tests[1].arena &&
        tests[0].arena != main_scratch.arena &&
        tests[1].arena != main_scratch.arena;

cleanup:
    if (release) SetEvent(release);
    for (u32 i = 0; i < ARRAY_COUNT(threads); ++i) {
        if (threads[i]) {
            WaitForSingleObject(threads[i], INFINITE);
            CloseHandle(threads[i]);
        }
        if (ready[i]) CloseHandle(ready[i]);
    }
    if (release) CloseHandle(release);
    end_scratch(main_scratch);
    destroy_global_scratch();
    return passed;
}

static b32 test_empty_graph(void)
{
    Bob *graph = bob_create();
    CHECK(graph != NULL);
    CHECK_OK(bob_prepare(graph));
    CHECK(bob_is_finished(graph));
    bob_destroy(graph);
    return true;
}

static b32 test_linear_graph(void)
{
    Bob *graph = bob_create();
    Bob_Node *compile = add_node(graph, "compile");
    Bob_Node *link = add_node(graph, "link");
    Bob_Node *node;

    CHECK_OK(bob_add_dependency(graph, link, compile));
    CHECK_OK(bob_prepare(graph));

    CHECK(bob_take_ready(graph, &node));
    CHECK(node == compile);
    CHECK(!bob_take_ready(graph, &node));
    CHECK_OK(bob_complete(graph, compile, true));

    CHECK(bob_take_ready(graph, &node));
    CHECK(node == link);
    CHECK_OK(bob_complete(graph, link, true));
    CHECK(bob_is_finished(graph));
    CHECK(!bob_has_failed(graph));

    bob_destroy(graph);
    return true;
}

static b32 test_parallel_fan_in(void)
{
    Bob *graph = bob_create();
    Bob_Node *a = add_node(graph, "a");
    Bob_Node *b = add_node(graph, "b");
    Bob_Node *link = add_node(graph, "link");
    Bob_Node *first;
    Bob_Node *second;
    Bob_Node *node;

    CHECK_OK(bob_add_dependency(graph, link, a));
    CHECK_OK(bob_add_dependency(graph, link, b));
    CHECK_OK(bob_prepare(graph));

    CHECK(bob_take_ready(graph, &first));
    CHECK(bob_take_ready(graph, &second));
    CHECK(first != second);
    CHECK(!bob_take_ready(graph, &node));

    /* Finishing either node first must not release link early. */
    CHECK_OK(bob_complete(graph, second, true));
    CHECK(!bob_take_ready(graph, &node));
    CHECK_OK(bob_complete(graph, first, true));
    CHECK(bob_take_ready(graph, &node));
    CHECK(node == link);

    CHECK_OK(bob_complete(graph, link, true));
    CHECK(bob_is_finished(graph));
    bob_destroy(graph);
    return true;
}

static b32 test_failure_blocks_dependents(void)
{
    Bob *graph = bob_create();
    Bob_Node *compile = add_node(graph, "compile");
    Bob_Node *link = add_node(graph, "link");
    Bob_Node *package = add_node(graph, "package");
    Bob_Node *independent = add_node(graph, "independent");
    Bob_Node *first;
    Bob_Node *second;

    CHECK_OK(bob_add_dependency(graph, link, compile));
    CHECK_OK(bob_add_dependency(graph, package, link));
    CHECK_OK(bob_prepare(graph));

    CHECK(bob_take_ready(graph, &first));
    CHECK(bob_take_ready(graph, &second));
    CHECK((first == compile && second == independent) ||
          (first == independent && second == compile));

    CHECK_OK(bob_complete(graph, compile, false));
    CHECK(bob_task_state(link) == BOB_TASK_BLOCKED);
    CHECK(bob_task_state(package) == BOB_TASK_BLOCKED);
    CHECK(!bob_is_finished(graph));

    CHECK_OK(bob_complete(graph, independent, true));
    CHECK(bob_is_finished(graph));
    CHECK(bob_has_failed(graph));
    bob_destroy(graph);
    return true;
}

static b32 test_cycle_is_rejected(void)
{
    Bob *graph = bob_create();
    Bob_Node *a = add_node(graph, "a");
    Bob_Node *b = add_node(graph, "b");
    Bob_Node *c = add_node(graph, "c");

    CHECK_OK(bob_add_dependency(graph, a, b));
    CHECK_OK(bob_add_dependency(graph, b, c));
    CHECK_OK(bob_add_dependency(graph, c, a));
    CHECK(bob_prepare(graph) == BOB_ERROR_CYCLE);
    bob_destroy(graph);
    return true;
}

static b32 test_invalid_edges_are_rejected(void)
{
    Bob *graph = bob_create();
    Bob_Node *a = add_node(graph, "a");
    Bob_Node *b = add_node(graph, "b");

    CHECK(bob_add_dependency(graph, a, a) == BOB_ERROR_SELF_DEPENDENCY);
    CHECK_OK(bob_add_dependency(graph, a, b));
    CHECK(bob_add_dependency(graph, a, b) == BOB_ERROR_DUPLICATE_DEPENDENCY);
    bob_destroy(graph);
    return true;
}

static b32 get_test_executable(char *buffer, u32 buffer_size)
{
    DWORD length = GetModuleFileNameA(NULL, buffer, buffer_size);
    return length > 0 && length < buffer_size;
}

static b32 test_builder_runs_in_parallel(void)
{
    Bob *graph = bob_create();
    Bob_Node *a = add_node(graph, "slow a");
    Bob_Node *b = add_node(graph, "slow b");
    Bob_Node *link = add_node(graph, "link");
    Bob_Task tasks[3] = {0};
    char executable[MAX_PATH];
    char command_a[2 * MAX_PATH];
    char command_b[2 * MAX_PATH];
    char command_link[2 * MAX_PATH];
    char event_a_name[128];
    char event_b_name[128];
    HANDLE event_a;
    HANDLE event_b;
    b32 executed;

    CHECK(get_test_executable(executable, sizeof(executable)));
    CHECK(snprintf(event_a_name, sizeof(event_a_name), "Local\\bob_graph_%lu_a",
                   GetCurrentProcessId()) > 0);
    CHECK(snprintf(event_b_name, sizeof(event_b_name), "Local\\bob_graph_%lu_b",
                   GetCurrentProcessId()) > 0);
    event_a = CreateEventA(NULL, TRUE, FALSE, event_a_name);
    event_b = CreateEventA(NULL, TRUE, FALSE, event_b_name);
    CHECK(event_a != NULL && event_b != NULL);

    CHECK(snprintf(command_a, sizeof(command_a), "\"%s\" --barrier %s %s a", executable, event_a_name, event_b_name) > 0);
    CHECK(snprintf(command_b, sizeof(command_b), "\"%s\" --barrier %s %s b", executable, event_b_name, event_a_name) > 0);
    CHECK(snprintf(command_link, sizeof(command_link), "\"%s\" --child 0 0 link", executable) > 0);

    tasks[0].command_line = string_from_cstring(command_a);
    tasks[1].command_line = string_from_cstring(command_b);
    tasks[2].command_line = string_from_cstring(command_link);

    CHECK_OK(bob_add_dependency(graph, link, a));
    CHECK_OK(bob_add_dependency(graph, link, b));

    executed = run_tasks(graph, tasks, 3, 2);
    CloseHandle(event_a);
    CloseHandle(event_b);

    CHECK(executed);
    CHECK(bob_is_finished(graph));
    bob_destroy(graph);
    return true;
}

static b32 test_builder_propagates_failure(void)
{
    Bob *graph = bob_create();
    Bob_Node *fail = add_node(graph, "fail");
    Bob_Node *blocked = add_node(graph, "blocked");
    Bob_Node *independent = add_node(graph, "independent");
    Bob_Task tasks[3] = {0};
    char executable[MAX_PATH];
    char fail_command[2 * MAX_PATH];
    char blocked_command[2 * MAX_PATH];
    char independent_command[2 * MAX_PATH];

    CHECK(get_test_executable(executable, sizeof(executable)));
    CHECK(snprintf(fail_command, sizeof(fail_command), "\"%s\" --child 0 1 fail", executable) > 0);
    CHECK(snprintf(blocked_command, sizeof(blocked_command), "\"%s\" --child 0 0 blocked", executable) > 0);
    CHECK(snprintf(independent_command, sizeof(independent_command), "\"%s\" --child 0 0 independent", executable) > 0);

    tasks[0].command_line = string_from_cstring(fail_command);
    tasks[1].command_line = string_from_cstring(blocked_command);
    tasks[2].command_line = string_from_cstring(independent_command);
    CHECK_OK(bob_add_dependency(graph, blocked, fail));

    CHECK(!run_tasks(graph, tasks, 3, 2));
    CHECK(bob_task_state(fail) == BOB_TASK_FAILED);
    CHECK(bob_task_state(blocked) == BOB_TASK_BLOCKED);
    CHECK(bob_task_state(independent) == BOB_TASK_SUCCEEDED);
    CHECK(bob_is_finished(graph));
    bob_destroy(graph);
    return true;
}

static b32 test_builder_reports_missing_executable(void)
{
    Bob *graph = bob_create();
    Bob_Node *missing = add_node(graph, "missing executable");
    Bob_Task task = {
        .command_line = STRING_LITERAL("bob_executable_that_does_not_exist_7f31.exe --input x.c")
    };

    CHECK(!run_tasks(graph, &task, 1, 1));
    CHECK(bob_task_state(missing) == BOB_TASK_FAILED);
    bob_destroy(graph);
    return true;
}

static b32 test_builder_skips_existing_output(void)
{
    const char *output_path = "build\\incremental_test.out";
    String outputs[] = { string_from_cstring(output_path) };
    Bob *first_graph;
    Bob *second_graph;
    Bob_Task task = {0};
    Platform_File_Info info;

    DeleteFileA(output_path);
    task.command_line = STRING_LITERAL("cmd /c echo built>build\\incremental_test.out");
    task.outputs = STRING_ARRAY_FROM(outputs);

    first_graph = bob_create();
    add_node(first_graph, "create output");
    CHECK(run_tasks(first_graph, &task, 1, 1));
	CHECK(platform_file_info(string_from_cstring(output_path), &info));
    bob_destroy(first_graph);

    task.command_line = STRING_LITERAL("bob_command_that_must_not_run.exe");
    second_graph = bob_create();
    add_node(second_graph, "skip existing output");
    CHECK(run_tasks(second_graph, &task, 1, 1));
    CHECK(bob_task_state(bob_node_at(second_graph, 0)) == BOB_TASK_SUCCEEDED);
    bob_destroy(second_graph);

    CHECK(DeleteFileA(output_path));
    return true;
}

static b32 write_test_file_at_time(const char *path, u64 time)
{
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    ULARGE_INTEGER value;
    FILETIME file_time;
    b32 succeeded;

    if (file == INVALID_HANDLE_VALUE) return false;
    value.QuadPart = time;
    file_time.dwLowDateTime = value.LowPart;
    file_time.dwHighDateTime = value.HighPart;
    succeeded = SetFileTime(file, NULL, NULL, &file_time);
    CloseHandle(file);
    return succeeded;
}

static b32 write_test_text_at_time(const char *path, const char *text, u64 time)
{
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    size_t text_size = strlen(text);
    DWORD written = 0;
    ULARGE_INTEGER value;
    FILETIME file_time;
    b32 succeeded;

    if (file == INVALID_HANDLE_VALUE || text_size > UINT32_MAX) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        return false;
    }
    value.QuadPart = time;
    file_time.dwLowDateTime = value.LowPart;
    file_time.dwHighDateTime = value.HighPart;
    succeeded = WriteFile(file, text, (DWORD)text_size, &written, NULL) &&
                written == (DWORD)text_size &&
                SetFileTime(file, NULL, NULL, &file_time);
    CloseHandle(file);
    return succeeded;
}

static b32 test_newer_input_rebuilds(void)
{
    const char *input_path = "build\\incremental_test.in";
    const char *output_path = "build\\incremental_test.out";
    String inputs[] = { string_from_cstring(input_path) };
    String outputs[] = { string_from_cstring(output_path) };
    Bob_Task task = {0};
    Bob *clean_graph;
    Bob *dirty_graph;

    CHECK(write_test_file_at_time(input_path, 100000000000000000ULL));
    CHECK(write_test_file_at_time(output_path, 100000000000000100ULL));
    task.command_line = STRING_LITERAL("bob_command_that_must_not_run.exe");
    task.inputs = STRING_ARRAY_FROM(inputs);
    task.outputs = STRING_ARRAY_FROM(outputs);

    clean_graph = bob_create();
    add_node(clean_graph, "clean timestamps");
    CHECK(run_tasks(clean_graph, &task, 1, 1));
    bob_destroy(clean_graph);

    CHECK(write_test_file_at_time(input_path, 100000000000000200ULL));
    task.command_line = STRING_LITERAL("cmd /c echo rebuilt>build\\incremental_test.out");
    dirty_graph = bob_create();
    add_node(dirty_graph, "dirty timestamps");
    CHECK(run_tasks(dirty_graph, &task, 1, 1));
    bob_destroy(dirty_graph);

    CHECK(DeleteFileA(input_path));
    CHECK(DeleteFileA(output_path));
    return true;
}

static b32 test_multiple_inputs_and_outputs(void)
{
    const char *input_a = "build\\multi_a.in";
    const char *input_b = "build\\multi_b.in";
    const char *output_a = "build\\multi_a.out";
    const char *output_b = "build\\multi_b.out";
    const char *marker = "build\\multi.marker";
    String inputs[] = { string_from_cstring(input_a), string_from_cstring(input_b) };
    String outputs[] = { string_from_cstring(output_a), string_from_cstring(output_b) };
    Bob_Task task = {0};
    Bob *graph;
    Platform_File_Info info;

    DeleteFileA(marker);
    CHECK(write_test_file_at_time(input_a, 100000000000000100ULL));
    CHECK(write_test_file_at_time(input_b, 100000000000000150ULL));
    CHECK(write_test_file_at_time(output_a, 100000000000000200ULL));
    CHECK(write_test_file_at_time(output_b, 100000000000000250ULL));
    task.command_line = STRING_LITERAL("bob_command_that_must_not_run.exe");
    task.inputs = STRING_ARRAY_FROM(inputs);
    task.outputs = STRING_ARRAY_FROM(outputs);

    graph = bob_create();
    add_node(graph, "clean multiple files");
    CHECK(run_tasks(graph, &task, 1, 1));
    bob_destroy(graph);

    CHECK(write_test_file_at_time(input_b, 100000000000000225ULL));
    task.command_line = STRING_LITERAL("cmd /c echo a>build\\multi_a.out && echo b>build\\multi_b.out && echo rebuilt>build\\multi.marker");
    graph = bob_create();
    add_node(graph, "newest input wins");
    CHECK(run_tasks(graph, &task, 1, 1));
	CHECK(platform_file_info(string_from_cstring(marker), &info));
    bob_destroy(graph);

    CHECK(DeleteFileA(output_b));
    CHECK(DeleteFileA(marker));
    graph = bob_create();
    add_node(graph, "one output missing");
    CHECK(run_tasks(graph, &task, 1, 1));
	CHECK(platform_file_info(string_from_cstring(output_b), &info));
	CHECK(platform_file_info(string_from_cstring(marker), &info));
    bob_destroy(graph);

    CHECK(DeleteFileA(input_a));
    CHECK(DeleteFileA(input_b));
    CHECK(DeleteFileA(output_a));
    CHECK(DeleteFileA(output_b));
    CHECK(DeleteFileA(marker));
    return true;
}

static b32 test_dependency_rebuild_propagates(void)
{
    const char *dependency_input = "build\\dependency.in";
    const char *dependency_output = "build\\dependency.out";
    const char *parent_output = "build\\parent.out";
    const char *marker = "build\\parent.marker";
    String dependency_inputs[] = { string_from_cstring(dependency_input) };
    String dependency_outputs[] = { string_from_cstring(dependency_output) };
    String parent_outputs[] = { string_from_cstring(parent_output) };
    Bob_Task tasks[2] = {0};
    Bob *graph;
    Bob_Node *dependency;
    Bob_Node *parent;
    Platform_File_Info info;

    DeleteFileA(marker);
    CHECK(write_test_file_at_time(dependency_input, 100000000000000100ULL));
    CHECK(write_test_file_at_time(dependency_output, 100000000000000200ULL));
    CHECK(write_test_file_at_time(parent_output, 100000000000000300ULL));

    tasks[0].command_line = STRING_LITERAL("bob_dependency_that_must_not_run.exe");
    tasks[0].inputs = STRING_ARRAY_FROM(dependency_inputs);
    tasks[0].outputs = STRING_ARRAY_FROM(dependency_outputs);
    tasks[1].command_line = STRING_LITERAL("bob_parent_that_must_not_run.exe");
    tasks[1].outputs = STRING_ARRAY_FROM(parent_outputs);

    graph = bob_create();
    dependency = add_node(graph, "clean dependency");
    parent = add_node(graph, "clean parent");
    CHECK_OK(bob_add_dependency(graph, parent, dependency));
    CHECK(run_tasks(graph, tasks, 2, 1));
    bob_destroy(graph);

    CHECK(write_test_file_at_time(dependency_input, 100000000000000400ULL));
    tasks[0].command_line = STRING_LITERAL("cmd /c echo dependency>build\\dependency.out");
    tasks[1].command_line = STRING_LITERAL("cmd /c echo parent>build\\parent.out && echo rebuilt>build\\parent.marker");
    graph = bob_create();
    dependency = add_node(graph, "dirty dependency");
    parent = add_node(graph, "propagated parent");
    CHECK_OK(bob_add_dependency(graph, parent, dependency));
    CHECK(run_tasks(graph, tasks, 2, 1));
	CHECK(platform_file_info(string_from_cstring(marker), &info));
    bob_destroy(graph);

    CHECK(DeleteFileA(dependency_input));
    CHECK(DeleteFileA(dependency_output));
    CHECK(DeleteFileA(parent_output));
    CHECK(DeleteFileA(marker));
    return true;
}

static b32 test_recursive_include_rebuilds(void)
{
    const char *source = "build\\include_scan.c";
    const char *header_a = "build\\include_scan_a.h";
    const char *header_b = "build\\include_scan_b.h";
    const char *output = "build\\include_scan.obj";
    const char *marker = "build\\include_scan.marker";
    String inputs[] = { string_from_cstring(source) };
    String outputs[] = { string_from_cstring(output) };
    String include_directories[] = { STRING_LITERAL("build") };
    Bob_Task task = {0};
    Bob *graph;
    Platform_File_Info info;
    C_Include_Scan_Result scan;

    DeleteFileA(marker);
    CHECK(write_test_text_at_time(source,
                                  "// #include \"ignored_line.h\"\n"
                                  "const char *text = \"#include ignored_string.h\";\n"
                                  "const char marker = '#';\n"
                                  "#includefoo \"ignored_identifier.h\"\n"
                                  "# /* comment crosses\n"
                                  "     a line */ include \"ignored_continuation.h\"\n"
                                  "/* prefix comment */ #include <include_scan_a.h>\n",
                                  100000000000000100ULL));
    CHECK(write_test_text_at_time(header_a,
                                  "/* #include \"ignored_block.h\" */\n"
                                  "# /* directive gap */ include /* name gap */ \"include_scan_b.h\"\n",
                                  100000000000000150ULL));
    CHECK(write_test_text_at_time(header_b, "#include \"include_scan_a.h\"\n",
                                  100000000000000300ULL));
    CHECK(write_test_file_at_time(output, 100000000000000200ULL));
    CHECK(c_include_scan(STRING_ARRAY_FROM(inputs), (String_Array){0}, STRING_LITERAL("clang-cl /Ibuild -c build\\include_scan.c"), &scan));
    CHECK(!scan.unresolved_quoted_include);
    CHECK(scan.newest_write_time == 100000000000000300ULL);

    task.command_line = STRING_LITERAL("cmd /c echo object>build\\include_scan.obj && echo rebuilt>build\\include_scan.marker");
    task.inputs = STRING_ARRAY_FROM(inputs);
    task.outputs = STRING_ARRAY_FROM(outputs);
    task.include_directories = STRING_ARRAY_FROM(include_directories);

    graph = bob_create();
    add_node(graph, "recursive include dirty");
    CHECK(run_tasks(graph, &task, 1, 1));
	CHECK(platform_file_info(string_from_cstring(marker), &info));
    bob_destroy(graph);

    CHECK(write_test_file_at_time(output, 100000000000000400ULL));
    CHECK(DeleteFileA(marker));
    task.command_line = STRING_LITERAL("bob_include_scanner_must_not_run.exe");
    graph = bob_create();
    add_node(graph, "recursive include clean");
    CHECK(run_tasks(graph, &task, 1, 1));
	CHECK(!platform_file_info(string_from_cstring(marker), &info));
    bob_destroy(graph);

    CHECK(DeleteFileA(source));
    CHECK(DeleteFileA(header_a));
    CHECK(DeleteFileA(header_b));
    CHECK(DeleteFileA(output));
    return true;
}

static b32 test_elf_descriptor(void)
{
    Bob_Build build;
    const Bob_Task *task;

    if (!script_load_build(STRING_LITERAL("example/tasks.elf"), &build)) {
        printf("  elf error: %s\n", build.error);
        return false;
    }
    CHECK(bob_task_count(build.bob) == 4);
    CHECK(string_equal(bob_get_task(bob_node_at(build.bob, 0))->name, STRING_LITERAL("run hello.exe")));
    CHECK(bob_dependency_count(bob_node_at(build.bob, 0)) == 1);
    CHECK(bob_dependency(bob_node_at(build.bob, 0), 0) == bob_node_at(build.bob, 1));
    task = bob_get_task(bob_node_at(build.bob, 2));
    CHECK(string_equal(task->name, STRING_LITERAL("compile main")));
    CHECK(task->inputs.count == 1);
    CHECK(task->outputs.count == 1);
    CHECK(task->include_directories.count == 1);
    CHECK(build.options.has_worker_count);
    CHECK(build.options.worker_count == 2);
    CHECK(build.options.has_verbosity);
    CHECK(build.options.verbosity == 0);
    CHECK(bob_dependency_count(bob_node_at(build.bob, 1)) == 2);
    CHECK(bob_dependency(bob_node_at(build.bob, 1), 0) == bob_node_at(build.bob, 2));
    bob_destroy(build.bob);
    return true;
}

static b32 test_elf_generated_descriptor(void)
{
    Bob_Build build;

    if (!script_load_build(STRING_LITERAL("example/tasks1.elf"), &build)) {
        printf("  elf error: %s\n", build.error);
        return false;
    }
    CHECK(bob_task_count(build.bob) == 33);
    CHECK(string_equal(bob_get_task(bob_node_at(build.bob, 0))->name, STRING_LITERAL("font_test")));
    CHECK(bob_dependency_count(bob_node_at(build.bob, 0)) == 21);
    CHECK(bob_dependency_count(bob_node_at(build.bob, 6)) == 6);
    CHECK(string_equal(bob_get_task(bob_node_at(build.bob, 27))->name, STRING_LITERAL("VS_Rect")));
    bob_destroy(build.bob);
    return true;
}

static b32 test_bob_descriptor(void)
{
    Bob_Build build;

    if (!script_load_build(STRING_LITERAL("build.elf"), &build)) {
        printf("  elf error: %s\n", build.error);
        return false;
    }
    CHECK(bob_task_count(build.bob) == 13);
    CHECK(string_equal(bob_get_task(bob_node_at(build.bob, 0))->name, STRING_LITERAL("link Bob")));
    CHECK(string_equal(bob_get_task(bob_node_at(build.bob, 1))->name, STRING_LITERAL("compile base")));
    CHECK(string_equal(bob_get_task(bob_node_at(build.bob, 11))->name, STRING_LITERAL("compile bob")));
    CHECK(string_equal(bob_get_task(bob_node_at(build.bob, 12))->name, STRING_LITERAL("prepare object directory")));
    bob_destroy(build.bob);
    return true;
}

static b32 test_script_functions(void)
{
    Arena arena = arena_create(MEGABYTES(16));
    Script *script = script_load(&arena, STRING_LITERAL("example/functions.elf"));
    CHECK(script_is_loaded(script));
    String_Array functions = script_functions(script);
    CHECK(functions.count == 2);
    CHECK(script_has_function(script, STRING_LITERAL("build")));
    CHECK(script_has_function(script, STRING_LITERAL("clean")));
    CHECK(!script_has_function(script, STRING_LITERAL("missing")));
    CHECK(script_invoke(script, STRING_LITERAL("build")));
    CHECK(!script_invoke(script, STRING_LITERAL("missing")));
    script_destroy(script);
    arena_destroy(&arena);
    return true;
}

static void run_test(const char *name, b32 (*test)(void))
{
    b32 passed;
    ++tests_run;
    printf("%-32s", name);
    passed = test();
    if (passed) {
        printf("PASS\n");
    } else {
        ++tests_failed;
    }
}

static int build_example(void)
{
    Bob *graph;
    Bob_Node *compile_main;
    Bob_Node *compile_message;
    Bob_Node *link;
    Bob_Node *run;
    Bob_Task tasks[4] = {0};
    b32 succeeded;

    if (!CreateDirectoryA("build\\example", NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "unable to create build\\example\n");
        return 1;
    }

    graph = bob_create();
    if (!graph) {
        return 1;
    }

    compile_main = add_node(graph, "compile example/main.c");
    compile_message = add_node(graph, "compile example/message.c");
    link = add_node(graph, "link hello.exe");
    run = add_node(graph, "run hello.exe");

    tasks[0].command_line = STRING_LITERAL("clang-cl /nologo /W4 /WX /c example\\main.c /Fobuild\\example\\main.obj");
    tasks[1].command_line = STRING_LITERAL("clang-cl /nologo /W4 /WX /c example\\message.c /Fobuild\\example\\message.obj");
    tasks[2].command_line = STRING_LITERAL("clang-cl /nologo build\\example\\main.obj build\\example\\message.obj /Febuild\\example\\hello.exe");
    tasks[3].command_line = STRING_LITERAL("build\\example\\hello.exe");

    if (bob_add_dependency(graph, link, compile_main) != BOB_OK ||
        bob_add_dependency(graph, link, compile_message) != BOB_OK ||
        bob_add_dependency(graph, run, link) != BOB_OK) {
        bob_destroy(graph);
        return 1;
    }

    succeeded = run_tasks(graph, tasks, 4, 2);
    bob_destroy(graph);
    return succeeded ? 0 : 1;
}

static int build_tasks_from_file(String path)
{
    Bob_Build build = {0};
    u32 workers;
    int exit_code;
    if (!script_load_build(path, &build)) {
        fprintf(stderr, "%s: %s\n", path.data, build.error);
        return 1;
    }
    workers = build.options.has_worker_count ? build.options.worker_count : 4;
    exit_code = bob_build(build.bob, workers) ? 0 : 1;
    bob_destroy(build.bob);
    return exit_code;
}

static int run_all_tests(void)
{
    run_test("arena and strings", test_arena_and_strings);
    run_test("thread-local scratch", test_thread_local_scratch);
    run_test("vcvars cache", test_vcvars_cache_application);
    run_test("high resolution timer", test_high_resolution_timer);
    run_test("empty graph", test_empty_graph);
    run_test("linear graph", test_linear_graph);
    run_test("parallel fan-in", test_parallel_fan_in);
    run_test("failure blocks dependents", test_failure_blocks_dependents);
    run_test("cycle rejection", test_cycle_is_rejected);
    run_test("invalid edge rejection", test_invalid_edges_are_rejected);
    run_test("builder parallelism", test_builder_runs_in_parallel);
    run_test("builder failure", test_builder_propagates_failure);
    run_test("missing executable", test_builder_reports_missing_executable);
    run_test("incremental output", test_builder_skips_existing_output);
    run_test("newer input", test_newer_input_rebuilds);
    run_test("multiple inputs and outputs", test_multiple_inputs_and_outputs);
    run_test("dependency rebuild", test_dependency_rebuild_propagates);
    run_test("recursive includes", test_recursive_include_rebuilds);
    run_test("elf build descriptor", test_elf_descriptor);
    run_test("elf generated descriptor", test_elf_generated_descriptor);
    run_test("Bob build descriptor", test_bob_descriptor);
    run_test("script functions", test_script_functions);

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}

int main(int argument_count, char **arguments)
{
    logger_init();

    if (argument_count == 5 && strcmp(arguments[1], "--child") == 0) {
        DWORD delay = (DWORD)strtoul(arguments[2], NULL, 10);
        int exit_code = atoi(arguments[3]);
        Sleep(delay);
        printf("%s\n", arguments[4]);
        return exit_code;
    }
    if (argument_count == 5 && strcmp(arguments[1], "--barrier") == 0) {
        HANDLE own_event = OpenEventA(EVENT_MODIFY_STATE, FALSE, arguments[2]);
        HANDLE other_event = OpenEventA(SYNCHRONIZE, FALSE, arguments[3]);
        DWORD wait_result;
        if (!own_event || !other_event) return 1;
        SetEvent(own_event);
        wait_result = WaitForSingleObject(other_event, 5000);
        CloseHandle(own_event);
        CloseHandle(other_event);
        if (wait_result != WAIT_OBJECT_0) return 1;
        printf("%s\n", arguments[4]);
        return 0;
    }
    if (argument_count == 2 && strcmp(arguments[1], "--build-example") == 0) {
        return build_example();
    }
    if (argument_count == 2 && strcmp(arguments[1], "--test") == 0) {
        return run_all_tests();
    }
    if (argument_count == 2 && strcmp(arguments[1], "--build-task-table") == 0) {
        if (!CreateDirectoryA("build\\example", NULL) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return 1;
        }
        return build_tasks_from_file(STRING_LITERAL("example/tasks.elf"));
    }
    if (argument_count == 1) {
        return build_tasks_from_file(STRING_LITERAL("build.elf"));
    }

    fprintf(stderr, "usage: bob [--test]\n");
    return 2;
}
