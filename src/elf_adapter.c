#include "elf_adapter.h"

#include "elf.h"

#include <stdio.h>
#include <string.h>

static elf_ValueView field(elf_State *state, elf_Table *table, const char *name)
{
    return elf_get_field(state, table, name);
}

static char *copy_string_value(Arena *arena, elf_ValueView value)
{
    const char *data;
    uint32_t size;
    char *copy;

    if (value.type != ELF_VALUE_TYPE_STRING) return NULL;
    data = elf_str_data(value.as.string);
    size = elf_str_size(value.as.string);
    copy = arena_push(arena, (u64)size + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, data, size);
    copy[size] = 0;
    return copy;
}

static int copy_string_array(elf_State *state, Arena *arena, elf_ValueView value,
                           const char *field_name, const char *task_name,
                           char ***items_out, uint32_t *count_out,
                           char *error, size_t error_size)
{
    elf_Table *table;
    char **items;
    uint32_t count;
    uint32_t i;

    if (value.type == ELF_VALUE_TYPE_NIL) return 1;
    if (value.type != ELF_VALUE_TYPE_TABLE) {
        snprintf(error, error_size, "%s for '%s' must be a table",
                 field_name, task_name);
        return 0;
    }

    table = value.as.table;
    count = elf_table_length(table);
    items = arena_push_zero_aligned(arena, count * sizeof(*items),
                                    _Alignof(char *));
    if (count && !items) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    *items_out = items;
    *count_out = count;

    for (i = 0; i < count; ++i) {
        items[i] = copy_string_value(arena, elf_get_index(state, table, i));
        if (!items[i]) {
            snprintf(error, error_size, "%s for '%s' must contain strings",
                     field_name, task_name);
            return 0;
        }
    }
    return 1;
}

int elf_load_task_list(const char *path, Arena *arena, Task_Array_Desc *result)
{
    u64 arena_start;
    Scratch scratch;
    elf_State *state;
    elf_ValueView returned;
    elf_ValueView tasks_value;
    elf_ValueView options_value;
    elf_Table *root = NULL;
    elf_Table *tasks;
    elf_Table **task_tables = NULL;
    uint32_t i;
    int success = 0;

    if (!path || !arena || !result) return 0;
    arena_start = arena_mark(arena);
    scratch = get_scratch();
    memset(result, 0, sizeof(*result));

    state = elf_create_state();
    if (!state) {
        snprintf(result->error, sizeof(result->error), "unable to create Elf state");
        goto cleanup;
    }

    if (!elf_push_code_file(state, path)) {
        snprintf(result->error, sizeof(result->error), "unable to load '%s'", path);
        goto cleanup;
    }
    elf_push_nil(state);
    elf_call(state, 1, 1);

    returned = elf_peek_value(state, 0);
    if (returned.type != ELF_VALUE_TYPE_TABLE) {
        snprintf(result->error, sizeof(result->error), "script must return a table");
        goto cleanup;
    }
    root = elf_retain_table(returned.as.table);
    elf_pop_values(state, 1);
    options_value = field(state, root, "options");
    if (options_value.type != ELF_VALUE_TYPE_NIL) {
        elf_ValueView workers_value;
        elf_ValueView verbosity_value;
        int64_t integer;

        if (options_value.type != ELF_VALUE_TYPE_TABLE) {
            snprintf(result->error, sizeof(result->error),
                     "returned 'options' field must be a table");
            goto cleanup;
        }
        workers_value = field(state, options_value.as.table, "workers");
        if (workers_value.type != ELF_VALUE_TYPE_NIL) {
            if (workers_value.type != ELF_VALUE_TYPE_INTEGER) {
                snprintf(result->error, sizeof(result->error),
                         "options.workers must be a positive integer");
                goto cleanup;
            }
            integer = workers_value.as.integer;
            if (integer < 1 || (uint64_t)integer > UINT32_MAX) {
                snprintf(result->error, sizeof(result->error),
                         "options.workers must be a positive integer");
                goto cleanup;
            }
            result->worker_count = (uint32_t)integer;
            result->has_worker_count = 1;
        }
        verbosity_value = field(state, options_value.as.table, "verbosity");
        if (verbosity_value.type != ELF_VALUE_TYPE_NIL) {
            if (verbosity_value.type != ELF_VALUE_TYPE_INTEGER) {
                snprintf(result->error, sizeof(result->error),
                         "options.verbosity must be a non-negative integer");
                goto cleanup;
            }
            integer = verbosity_value.as.integer;
            if (integer < 0 || (uint64_t)integer > INT32_MAX) {
                snprintf(result->error, sizeof(result->error),
                         "options.verbosity must be a non-negative integer");
                goto cleanup;
            }
            result->verbosity = (int32_t)integer;
            result->has_verbosity = 1;
        }
    }
    tasks_value = field(state, root, "tasks");
    if (tasks_value.type != ELF_VALUE_TYPE_TABLE) {
        snprintf(result->error, sizeof(result->error), "returned table requires a 'tasks' table");
        goto cleanup;
    }

    tasks = tasks_value.as.table;
    result->count = elf_table_length(tasks);
    result->tasks = arena_push_zero_aligned(
        arena, result->count * sizeof(*result->tasks),
        _Alignof(Task_Desc));
    task_tables = arena_push_zero_aligned(
        scratch.arena, result->count * sizeof(*task_tables),
        _Alignof(elf_Table *));
    if (result->count && (!result->tasks || !task_tables)) {
        snprintf(result->error, sizeof(result->error), "out of memory");
        goto cleanup;
    }

    for (i = 0; i < result->count; ++i) {
        Task_Desc *output = &result->tasks[i];
        elf_ValueView description_value = elf_get_index(state, tasks, i);
        elf_Table *description;
        if (description_value.type != ELF_VALUE_TYPE_TABLE) {
            snprintf(result->error, sizeof(result->error), "task %u must be a table", i);
            goto cleanup;
        }
        description = description_value.as.table;
        task_tables[i] = description;
        output->name = copy_string_value(
            arena, field(state, description, "name"));
        output->command_line = copy_string_value(arena,
            field(state, description, "command_line"));
        if (!output->name || !output->command_line) {
            snprintf(result->error, sizeof(result->error),
                     "task %u requires string fields 'name' and 'command_line'", i);
            goto cleanup;
        }
        if (!copy_string_array(state, arena,
                             field(state, description, "inputs"), "inputs",
                             output->name, &output->inputs, &output->input_count,
                             result->error, sizeof(result->error)) ||
            !copy_string_array(state, arena,
                             field(state, description, "outputs"), "outputs",
                             output->name, &output->outputs, &output->output_count,
                             result->error, sizeof(result->error)) ||
            !copy_string_array(state, arena,
                             field(state, description, "include_dirs"),
                             "include_dirs", output->name,
                             &output->include_directories,
                             &output->include_directory_count,
                             result->error, sizeof(result->error))) {
            goto cleanup;
        }

    }

    for (i = 0; i < result->count; ++i) {
        Task_Desc *output = &result->tasks[i];
        elf_ValueView dependencies_value = field(
            state, task_tables[i], "dependencies");
        uint32_t dependency;

        if (dependencies_value.type == ELF_VALUE_TYPE_NIL) continue;
        if (dependencies_value.type != ELF_VALUE_TYPE_TABLE) {
            snprintf(result->error, sizeof(result->error),
                     "dependencies for '%s' must be a table", output->name);
            goto cleanup;
        }

        output->dependency_count = elf_table_length(dependencies_value.as.table);
        output->dependencies = arena_push_zero_aligned(
            arena, output->dependency_count * sizeof(*output->dependencies),
            _Alignof(uint32_t));
        if (output->dependency_count && !output->dependencies) {
            snprintf(result->error, sizeof(result->error), "out of memory");
            goto cleanup;
        }
        for (dependency = 0; dependency < output->dependency_count; ++dependency) {
            elf_ValueView dependency_value = elf_get_index(
                state, dependencies_value.as.table, dependency);
            elf_Table *dependency_table;
            uint32_t resolved;

            if (dependency_value.type != ELF_VALUE_TYPE_TABLE) {
                snprintf(result->error, sizeof(result->error),
                         "dependencies for '%s' must contain task tables", output->name);
                goto cleanup;
            }
            dependency_table = dependency_value.as.table;
            for (resolved = 0; resolved < result->count; ++resolved) {
                if (task_tables[resolved] == dependency_table) break;
            }
            if (resolved == result->count) {
                snprintf(result->error, sizeof(result->error),
                         "dependency %u for '%s' is not present in tasks", dependency,
                         output->name);
                goto cleanup;
            }
            output->dependencies[dependency] = resolved;
        }
    }

    success = 1;

cleanup:
    end_scratch(scratch);
    elf_release_table(root);
    elf_destroy_state(state);
    if (!success) {
        char error[sizeof(result->error)];
        memcpy(error, result->error, sizeof(error));
        arena_restore(arena, arena_start);
        memset(result, 0, sizeof(*result));
        memcpy(result->error, error, sizeof(error));
    }
    return success;
}
