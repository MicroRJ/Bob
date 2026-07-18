#ifndef ELF_ADAPTER_H
#define ELF_ADAPTER_H

#include "base.h"

typedef struct Task_Desc {
    char *name;
    char *command_line;
    char **inputs;
    uint32_t input_count;
    char **outputs;
    uint32_t output_count;
    char **include_directories;
    uint32_t include_directory_count;
    uint32_t *dependencies;
    uint32_t dependency_count;
} Task_Desc;

typedef struct Task_Array_Desc {
    Task_Desc *tasks;
    uint32_t count;
    uint32_t worker_count;
    int32_t verbosity;
    int has_worker_count;
    int has_verbosity;
    char error[256];
} Task_Array_Desc;

int elf_load_task_list(const char *path, Arena *arena, Task_Array_Desc *result);

#endif
