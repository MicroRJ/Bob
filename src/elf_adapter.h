#ifndef ELF_ADAPTER_H
#define ELF_ADAPTER_H

#include "base.h"

typedef struct Elf_Task_Description {
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
} Elf_Task_Description;

typedef struct Elf_Task_List {
    Elf_Task_Description *tasks;
    uint32_t count;
    uint32_t worker_count;
    int32_t verbosity;
    int has_worker_count;
    int has_verbosity;
    char error[256];
} Elf_Task_List;

int elf_load_task_list(const char *path, Arena *arena, Elf_Task_List *result);

#endif
