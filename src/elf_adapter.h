#ifndef ELF_ADAPTER_H
#define ELF_ADAPTER_H

#include "task.h"

typedef struct Task_Array_Desc {
    Task *tasks;
    uint32_t count;
    uint32_t worker_count;
    int32_t verbosity;
    int has_worker_count;
    int has_verbosity;
    char error[256];
} Task_Array_Desc;

int elf_load_task_list(const char *path, Arena *arena, Task_Array_Desc *result);

#endif
