#include "graph.h"
#include "executor.h"
#include "elf_adapter.h"
#include "c_include_scan.h"
#include "logger.h"
#include "platform.h"
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

#define CHECK_OK(expression) CHECK((expression) == GRAPH_OK)

static b32 environment_equals(const char *name, const char *expected)
{
    Scratch scratch = get_scratch();
    String value = {0};
    b32 equal = platform_get_environment(name, scratch.arena, &value) &&
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

    platform_set_environment("BOB_VCVARS_TEST_PREPEND", "base");
    platform_set_environment("BOB_VCVARS_TEST_APPEND", "base");
    platform_set_environment("BOB_VCVARS_TEST_SET", NULL);

    if (!vcvars_cache_apply(string_from_cstring(cache_text))) goto cleanup;
    if (!environment_equals("BOB_VCVARS_TEST_PREPEND", "tool;base")) goto cleanup;
    if (!environment_equals("BOB_VCVARS_TEST_APPEND", "base;tail")) goto cleanup;
    if (!environment_equals("BOB_VCVARS_TEST_SET", "value")) goto cleanup;
    result = true;

cleanup:
    platform_set_environment("BOB_VCVARS_TEST_PREPEND", NULL);
    platform_set_environment("BOB_VCVARS_TEST_APPEND", NULL);
    platform_set_environment("BOB_VCVARS_TEST_SET", NULL);
    return result;
}

static b32 test_high_resolution_timer(void)
{
    u64 frequency = platform_performance_frequency();
    u64 before = platform_performance_counter();
    Sleep(1);
    return frequency > 0 && platform_performance_counter() >= before;
}

static Node_Id add_node(Graph *graph, const char *name)
{
    Node_Id node = GRAPH_INVALID_TASK;
    Graph_Error result = graph_add_node(graph, name, NULL, &node);
    if (result != GRAPH_OK) {
        printf("  unable to add node %s: %s\n", name, graph_error_str(result));
        exit(2);
    }
    return node;
}

static b32 run_tasks(Graph *graph, const Task *tasks, u32 task_count,
                     u32 worker_count)
{
    u32 i;
    if (graph_node_count(graph) != task_count) return false;
    for (i = 0; i < task_count; ++i) {
        if (graph_set_node_data(graph, (Node_Id)i, &tasks[i]) != GRAPH_OK) {
            return false;
        }
    }
    return executor_run(graph, worker_count);
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
    CHECK(arena_push_text(&arena, "hello") == start);
    CHECK(arena_append_str(&arena, STRING_LITERAL(" arena")) != NULL);
    CHECK(arena_pushf(&arena, " %d", 42) != NULL);
    built = string_from_range(start, arena_top(&arena));
    CHECK(arena_push_zero(&arena, 1) != NULL);
    CHECK(string_equal(built, STRING_LITERAL("hello arena 42")));
    CHECK(built.data[built.size] == 0);

    copy = arena_push_string_copy(&arena, built);
    CHECK(string_equal(copy, built));
    CHECK(copy.data[copy.size] == 0);
    CHECK(string_equal(string_slice(copy, 6, 5), STRING_LITERAL("arena")));

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

    outer = get_scratch();
    CHECK(arena_push_text(outer.arena, "outer") != NULL);
    inner = get_scratch();
    CHECK(arena_push_text(inner.arena, "inner") != NULL);
    end_scratch(inner);
    CHECK(arena_mark(outer.arena) == inner.restore_used);
    end_scratch(outer);
    CHECK(arena_mark(&global_scratch_arena) == outer.restore_used);
    destroy_global_scratch();
    return true;
}

static b32 test_empty_graph(void)
{
    Graph *graph = graph_create();
    CHECK(graph != NULL);
    CHECK_OK(graph_prepare(graph));
    CHECK(graph_is_finished(graph));
    graph_destroy(graph);
    return true;
}

static b32 test_linear_graph(void)
{
    Graph *graph = graph_create();
    Node_Id compile = add_node(graph, "compile");
    Node_Id link = add_node(graph, "link");
    Node_Id node;

    CHECK_OK(graph_add_dependency(graph, link, compile));
    CHECK_OK(graph_prepare(graph));

    CHECK(graph_take_ready(graph, &node));
    CHECK(node == compile);
    CHECK(!graph_take_ready(graph, &node));
    CHECK_OK(graph_complete(graph, compile, true));

    CHECK(graph_take_ready(graph, &node));
    CHECK(node == link);
    CHECK_OK(graph_complete(graph, link, true));
    CHECK(graph_is_finished(graph));
    CHECK(!graph_has_failed(graph));

    graph_destroy(graph);
    return true;
}

static b32 test_parallel_fan_in(void)
{
    Graph *graph = graph_create();
    Node_Id a = add_node(graph, "a");
    Node_Id b = add_node(graph, "b");
    Node_Id link = add_node(graph, "link");
    Node_Id first;
    Node_Id second;
    Node_Id node;

    CHECK_OK(graph_add_dependency(graph, link, a));
    CHECK_OK(graph_add_dependency(graph, link, b));
    CHECK_OK(graph_prepare(graph));

    CHECK(graph_take_ready(graph, &first));
    CHECK(graph_take_ready(graph, &second));
    CHECK(first != second);
    CHECK(!graph_take_ready(graph, &node));

    /* Finishing either node first must not release link early. */
    CHECK_OK(graph_complete(graph, second, true));
    CHECK(!graph_take_ready(graph, &node));
    CHECK_OK(graph_complete(graph, first, true));
    CHECK(graph_take_ready(graph, &node));
    CHECK(node == link);

    CHECK_OK(graph_complete(graph, link, true));
    CHECK(graph_is_finished(graph));
    graph_destroy(graph);
    return true;
}

static b32 test_failure_blocks_dependents(void)
{
    Graph *graph = graph_create();
    Node_Id compile = add_node(graph, "compile");
    Node_Id link = add_node(graph, "link");
    Node_Id package = add_node(graph, "package");
    Node_Id independent = add_node(graph, "independent");
    Node_Id first;
    Node_Id second;

    CHECK_OK(graph_add_dependency(graph, link, compile));
    CHECK_OK(graph_add_dependency(graph, package, link));
    CHECK_OK(graph_prepare(graph));

    CHECK(graph_take_ready(graph, &first));
    CHECK(graph_take_ready(graph, &second));
    CHECK((first == compile && second == independent) ||
          (first == independent && second == compile));

    CHECK_OK(graph_complete(graph, compile, false));
    CHECK(graph_node_state(graph, link) == GRAPH_TASK_BLOCKED);
    CHECK(graph_node_state(graph, package) == GRAPH_TASK_BLOCKED);
    CHECK(!graph_is_finished(graph));

    CHECK_OK(graph_complete(graph, independent, true));
    CHECK(graph_is_finished(graph));
    CHECK(graph_has_failed(graph));
    graph_destroy(graph);
    return true;
}

static b32 test_cycle_is_rejected(void)
{
    Graph *graph = graph_create();
    Node_Id a = add_node(graph, "a");
    Node_Id b = add_node(graph, "b");
    Node_Id c = add_node(graph, "c");

    CHECK_OK(graph_add_dependency(graph, a, b));
    CHECK_OK(graph_add_dependency(graph, b, c));
    CHECK_OK(graph_add_dependency(graph, c, a));
    CHECK(graph_prepare(graph) == GRAPH_ERROR_CYCLE);
    graph_destroy(graph);
    return true;
}

static b32 test_invalid_edges_are_rejected(void)
{
    Graph *graph = graph_create();
    Node_Id a = add_node(graph, "a");
    Node_Id b = add_node(graph, "b");

    CHECK(graph_add_dependency(graph, a, a) == GRAPH_ERROR_SELF_DEPENDENCY);
    CHECK_OK(graph_add_dependency(graph, a, b));
    CHECK(graph_add_dependency(graph, a, b) == GRAPH_ERROR_DUPLICATE_DEPENDENCY);
    graph_destroy(graph);
    return true;
}

static b32 get_test_executable(char *buffer, u32 buffer_size)
{
    DWORD length = GetModuleFileNameA(NULL, buffer, buffer_size);
    return length > 0 && length < buffer_size;
}

static b32 test_executor_runs_in_parallel(void)
{
    Graph *graph = graph_create();
    Node_Id a = add_node(graph, "slow a");
    Node_Id b = add_node(graph, "slow b");
    Node_Id link = add_node(graph, "link");
    Task tasks[3] = {0};
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

    CHECK(snprintf(command_a, sizeof(command_a), "\"%s\" --barrier %s %s a",
                   executable, event_a_name, event_b_name) > 0);
    CHECK(snprintf(command_b, sizeof(command_b), "\"%s\" --barrier %s %s b",
                   executable, event_b_name, event_a_name) > 0);
    CHECK(snprintf(command_link, sizeof(command_link), "\"%s\" --child 0 0 link", executable) > 0);

    tasks[a].command_line = command_a;
    tasks[b].command_line = command_b;
    tasks[link].command_line = command_link;

    CHECK_OK(graph_add_dependency(graph, link, a));
    CHECK_OK(graph_add_dependency(graph, link, b));

    executed = run_tasks(graph, tasks, 3, 2);
    CloseHandle(event_a);
    CloseHandle(event_b);

    CHECK(executed);
    CHECK(graph_is_finished(graph));
    graph_destroy(graph);
    return true;
}

static b32 test_executor_propagates_failure(void)
{
    Graph *graph = graph_create();
    Node_Id fail = add_node(graph, "fail");
    Node_Id blocked = add_node(graph, "blocked");
    Node_Id independent = add_node(graph, "independent");
    Task tasks[3] = {0};
    char executable[MAX_PATH];
    char fail_command[2 * MAX_PATH];
    char blocked_command[2 * MAX_PATH];
    char independent_command[2 * MAX_PATH];

    CHECK(get_test_executable(executable, sizeof(executable)));
    CHECK(snprintf(fail_command, sizeof(fail_command), "\"%s\" --child 0 1 fail", executable) > 0);
    CHECK(snprintf(blocked_command, sizeof(blocked_command), "\"%s\" --child 0 0 blocked", executable) > 0);
    CHECK(snprintf(independent_command, sizeof(independent_command), "\"%s\" --child 0 0 independent", executable) > 0);

    tasks[fail].command_line = fail_command;
    tasks[blocked].command_line = blocked_command;
    tasks[independent].command_line = independent_command;
    CHECK_OK(graph_add_dependency(graph, blocked, fail));

    CHECK(!run_tasks(graph, tasks, 3, 2));
    CHECK(graph_node_state(graph, fail) == GRAPH_TASK_FAILED);
    CHECK(graph_node_state(graph, blocked) == GRAPH_TASK_BLOCKED);
    CHECK(graph_node_state(graph, independent) == GRAPH_TASK_SUCCEEDED);
    CHECK(graph_is_finished(graph));
    graph_destroy(graph);
    return true;
}

static b32 test_executor_reports_missing_executable(void)
{
    Graph *graph = graph_create();
    Node_Id missing = add_node(graph, "missing executable");
    Task task = {
        .command_line = "bob_executable_that_does_not_exist_7f31.exe --input x.c"
    };

    CHECK(!run_tasks(graph, &task, 1, 1));
    CHECK(graph_node_state(graph, missing) == GRAPH_TASK_FAILED);
    graph_destroy(graph);
    return true;
}

static b32 test_executor_skips_existing_output(void)
{
    const char *output_path = "build\\incremental_test.out";
    const char *outputs[] = { output_path };
    Graph *first_graph;
    Graph *second_graph;
    Task task = {0};
    Platform_File_Info info;

    DeleteFileA(output_path);
    task.command_line = "cmd /c echo built>build\\incremental_test.out";
    task.outputs = outputs;
    task.output_count = 1;

    first_graph = graph_create();
    add_node(first_graph, "create output");
    CHECK(run_tasks(first_graph, &task, 1, 1));
    CHECK(platform_file_info(output_path, &info));
    graph_destroy(first_graph);

    task.command_line = "bob_command_that_must_not_run.exe";
    second_graph = graph_create();
    add_node(second_graph, "skip existing output");
    CHECK(run_tasks(second_graph, &task, 1, 1));
    CHECK(graph_node_state(second_graph, 0) == GRAPH_TASK_SUCCEEDED);
    graph_destroy(second_graph);

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
    const char *inputs[] = { input_path };
    const char *outputs[] = { output_path };
    Task task = {0};
    Graph *clean_graph;
    Graph *dirty_graph;

    CHECK(write_test_file_at_time(input_path, 100000000000000000ULL));
    CHECK(write_test_file_at_time(output_path, 100000000000000100ULL));
    task.command_line = "bob_command_that_must_not_run.exe";
    task.inputs = inputs;
    task.input_count = 1;
    task.outputs = outputs;
    task.output_count = 1;

    clean_graph = graph_create();
    add_node(clean_graph, "clean timestamps");
    CHECK(run_tasks(clean_graph, &task, 1, 1));
    graph_destroy(clean_graph);

    CHECK(write_test_file_at_time(input_path, 100000000000000200ULL));
    task.command_line = "cmd /c echo rebuilt>build\\incremental_test.out";
    dirty_graph = graph_create();
    add_node(dirty_graph, "dirty timestamps");
    CHECK(run_tasks(dirty_graph, &task, 1, 1));
    graph_destroy(dirty_graph);

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
    const char *inputs[] = { input_a, input_b };
    const char *outputs[] = { output_a, output_b };
    Task task = {0};
    Graph *graph;
    Platform_File_Info info;

    DeleteFileA(marker);
    CHECK(write_test_file_at_time(input_a, 100000000000000100ULL));
    CHECK(write_test_file_at_time(input_b, 100000000000000150ULL));
    CHECK(write_test_file_at_time(output_a, 100000000000000200ULL));
    CHECK(write_test_file_at_time(output_b, 100000000000000250ULL));
    task.command_line = "bob_command_that_must_not_run.exe";
    task.inputs = inputs;
    task.input_count = 2;
    task.outputs = outputs;
    task.output_count = 2;

    graph = graph_create();
    add_node(graph, "clean multiple files");
    CHECK(run_tasks(graph, &task, 1, 1));
    graph_destroy(graph);

    CHECK(write_test_file_at_time(input_b, 100000000000000225ULL));
    task.command_line = "cmd /c echo a>build\\multi_a.out && echo b>build\\multi_b.out && echo rebuilt>build\\multi.marker";
    graph = graph_create();
    add_node(graph, "newest input wins");
    CHECK(run_tasks(graph, &task, 1, 1));
    CHECK(platform_file_info(marker, &info));
    graph_destroy(graph);

    CHECK(DeleteFileA(output_b));
    CHECK(DeleteFileA(marker));
    graph = graph_create();
    add_node(graph, "one output missing");
    CHECK(run_tasks(graph, &task, 1, 1));
    CHECK(platform_file_info(output_b, &info));
    CHECK(platform_file_info(marker, &info));
    graph_destroy(graph);

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
    const char *dependency_inputs[] = { dependency_input };
    const char *dependency_outputs[] = { dependency_output };
    const char *parent_outputs[] = { parent_output };
    Task tasks[2] = {0};
    Graph *graph;
    Node_Id dependency;
    Node_Id parent;
    Platform_File_Info info;

    DeleteFileA(marker);
    CHECK(write_test_file_at_time(dependency_input, 100000000000000100ULL));
    CHECK(write_test_file_at_time(dependency_output, 100000000000000200ULL));
    CHECK(write_test_file_at_time(parent_output, 100000000000000300ULL));

    tasks[0].command_line = "bob_dependency_that_must_not_run.exe";
    tasks[0].inputs = dependency_inputs;
    tasks[0].input_count = 1;
    tasks[0].outputs = dependency_outputs;
    tasks[0].output_count = 1;
    tasks[1].command_line = "bob_parent_that_must_not_run.exe";
    tasks[1].outputs = parent_outputs;
    tasks[1].output_count = 1;

    graph = graph_create();
    dependency = add_node(graph, "clean dependency");
    parent = add_node(graph, "clean parent");
    CHECK_OK(graph_add_dependency(graph, parent, dependency));
    CHECK(run_tasks(graph, tasks, 2, 1));
    graph_destroy(graph);

    CHECK(write_test_file_at_time(dependency_input, 100000000000000400ULL));
    tasks[0].command_line = "cmd /c echo dependency>build\\dependency.out";
    tasks[1].command_line = "cmd /c echo parent>build\\parent.out && echo rebuilt>build\\parent.marker";
    graph = graph_create();
    dependency = add_node(graph, "dirty dependency");
    parent = add_node(graph, "propagated parent");
    CHECK_OK(graph_add_dependency(graph, parent, dependency));
    CHECK(run_tasks(graph, tasks, 2, 1));
    CHECK(platform_file_info(marker, &info));
    graph_destroy(graph);

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
    const char *inputs[] = { source };
    const char *outputs[] = { output };
    const char *include_directories[] = { "build" };
    Task task = {0};
    Graph *graph;
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
    CHECK(c_include_scan(inputs, 1, NULL, 0,
                         "clang-cl /Ibuild -c build\\include_scan.c", &scan));
    CHECK(!scan.unresolved_quoted_include);
    CHECK(scan.newest_write_time == 100000000000000300ULL);

    task.command_line = "cmd /c echo object>build\\include_scan.obj && echo rebuilt>build\\include_scan.marker";
    task.inputs = inputs;
    task.input_count = 1;
    task.outputs = outputs;
    task.output_count = 1;
    task.include_directories = include_directories;
    task.include_directory_count = 1;

    graph = graph_create();
    add_node(graph, "recursive include dirty");
    CHECK(run_tasks(graph, &task, 1, 1));
    CHECK(platform_file_info(marker, &info));
    graph_destroy(graph);

    CHECK(write_test_file_at_time(output, 100000000000000400ULL));
    CHECK(DeleteFileA(marker));
    task.command_line = "bob_include_scanner_must_not_run.exe";
    graph = graph_create();
    add_node(graph, "recursive include clean");
    CHECK(run_tasks(graph, &task, 1, 1));
    CHECK(!platform_file_info(marker, &info));
    graph_destroy(graph);

    CHECK(DeleteFileA(source));
    CHECK(DeleteFileA(header_a));
    CHECK(DeleteFileA(header_b));
    CHECK(DeleteFileA(output));
    return true;
}

static b32 test_elf_descriptor(void)
{
    Arena arena = arena_create(0);
    Task_Array_Desc list;

    if (!elf_load_task_list("example/tasks.elf", &arena, &list)) {
        printf("  Elf error: %s\n", list.error);
        arena_destroy(&arena);
        return false;
    }
    CHECK(list.count == 4);
    CHECK(strcmp(list.tasks[0].name, "compile main") == 0);
    CHECK(list.tasks[0].input_count == 1);
    CHECK(list.tasks[0].output_count == 1);
    CHECK(list.tasks[0].include_directory_count == 1);
    CHECK(list.has_worker_count);
    CHECK(list.worker_count == 2);
    CHECK(list.has_verbosity);
    CHECK(list.verbosity == 0);
    CHECK(list.tasks[2].dependency_count == 2);
    CHECK(list.tasks[2].dependencies[0] == 0);
    arena_destroy(&arena);
    return true;
}

static b32 test_elf_generated_descriptor(void)
{
    Arena arena = arena_create(0);
    Task_Array_Desc list;

    if (!elf_load_task_list("example/tasks1.elf", &arena, &list)) {
        printf("  Elf error: %s\n", list.error);
        arena_destroy(&arena);
        return false;
    }
    CHECK(list.count == 33);
    CHECK(strcmp(list.tasks[0].name, "VS_Rect") == 0);
    CHECK(list.tasks[6].dependency_count == 6);
    CHECK(strcmp(list.tasks[27].name, "font_test") == 0);
    CHECK(list.tasks[27].dependency_count == 21);
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
    Graph *graph;
    Node_Id compile_main;
    Node_Id compile_message;
    Node_Id link;
    Node_Id run;
    Task tasks[4] = {0};
    b32 succeeded;

    if (!CreateDirectoryA("build\\example", NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "unable to create build\\example\n");
        return 1;
    }

    graph = graph_create();
    if (!graph) {
        return 1;
    }

    compile_main = add_node(graph, "compile example/main.c");
    compile_message = add_node(graph, "compile example/message.c");
    link = add_node(graph, "link hello.exe");
    run = add_node(graph, "run hello.exe");

    tasks[compile_main].command_line =
        "clang-cl /nologo /W4 /WX /c example\\main.c /Fobuild\\example\\main.obj";
    tasks[compile_message].command_line =
        "clang-cl /nologo /W4 /WX /c example\\message.c /Fobuild\\example\\message.obj";
    tasks[link].command_line =
        "clang-cl /nologo build\\example\\main.obj build\\example\\message.obj "
        "/Febuild\\example\\hello.exe";
    tasks[run].command_line = "build\\example\\hello.exe";

    if (graph_add_dependency(graph, link, compile_main) != GRAPH_OK ||
        graph_add_dependency(graph, link, compile_message) != GRAPH_OK ||
        graph_add_dependency(graph, run, link) != GRAPH_OK) {
        graph_destroy(graph);
        return 1;
    }

    succeeded = run_tasks(graph, tasks, 4, 2);
    graph_destroy(graph);
    return succeeded ? 0 : 1;
}

static int build_tasks_from_elf(const char *path)
{
    Arena arena = arena_create(0);
    Task_Array_Desc loaded = {0};
    Graph *graph = NULL;
    Task *tasks = NULL;
    Node_Id *nodes = NULL;
    u32 task_count = 0;
    u32 i;
    int exit_code = 1;

    if (!elf_load_task_list(path, &arena, &loaded)) {
        fprintf(stderr, "%s: %s\n", path, loaded.error);
        goto cleanup;
    }

    task_count = loaded.count;
    graph = graph_create();
    tasks = arena_push_zero_aligned(&arena, task_count * sizeof(*tasks),
                                    _Alignof(Task));
    nodes = arena_push_aligned(&arena, task_count * sizeof(*nodes),
                               _Alignof(Node_Id));
    if (!graph || !tasks || !nodes) {
        fprintf(stderr, "out of memory while loading tasks\n");
        goto cleanup;
    }

    for (i = 0; i < task_count; ++i) {
        if (graph_add_node(graph, loaded.tasks[i].name, &tasks[i],
                           &nodes[i]) != GRAPH_OK) {
            fprintf(stderr, "%s: unable to create task '%s'\n",
                    path, loaded.tasks[i].name);
            goto cleanup;
        }
        tasks[nodes[i]].command_line = loaded.tasks[i].command_line;
    }

    for (i = 0; i < task_count; ++i) {
        u32 dependency_index;

        for (dependency_index = 0; dependency_index < loaded.tasks[i].dependency_count;
             ++dependency_index) {
            u32 dependency = loaded.tasks[i].dependencies[dependency_index];
            if (dependency >= task_count) {
                fprintf(stderr, "%s: task '%s' has invalid dependency %u\n",
                        path, loaded.tasks[i].name, dependency);
                goto cleanup;
            }
            if (graph_add_dependency(graph, nodes[i], nodes[dependency]) != GRAPH_OK) {
                fprintf(stderr, "%s: unable to add dependency %u to '%s'\n",
                        path, dependency, loaded.tasks[i].name);
                goto cleanup;
            }
        }
    }

    exit_code = run_tasks(graph, tasks, task_count, 4) ? 0 : 1;

cleanup:
    graph_destroy(graph);
    arena_destroy(&arena);
    return exit_code;
}

static int run_all_tests(void)
{
    run_test("arena and strings", test_arena_and_strings);
    run_test("vcvars cache", test_vcvars_cache_application);
    run_test("high resolution timer", test_high_resolution_timer);
    run_test("empty graph", test_empty_graph);
    run_test("linear graph", test_linear_graph);
    run_test("parallel fan-in", test_parallel_fan_in);
    run_test("failure blocks dependents", test_failure_blocks_dependents);
    run_test("cycle rejection", test_cycle_is_rejected);
    run_test("invalid edge rejection", test_invalid_edges_are_rejected);
    run_test("executor parallelism", test_executor_runs_in_parallel);
    run_test("executor failure", test_executor_propagates_failure);
    run_test("missing executable", test_executor_reports_missing_executable);
    run_test("incremental output", test_executor_skips_existing_output);
    run_test("newer input", test_newer_input_rebuilds);
    run_test("multiple inputs and outputs", test_multiple_inputs_and_outputs);
    run_test("dependency rebuild", test_dependency_rebuild_propagates);
    run_test("recursive includes", test_recursive_include_rebuilds);
    run_test("Elf build descriptor", test_elf_descriptor);
    run_test("Elf generated descriptor", test_elf_generated_descriptor);

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
        return build_tasks_from_elf("example/tasks.elf");
    }
    if (argument_count == 1) {
        return build_tasks_from_elf("build.elf");
    }

    fprintf(stderr, "usage: bob [--test]\n");
    return 2;
}
