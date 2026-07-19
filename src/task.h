#ifndef TASK_H
#define TASK_H

#include "base.h"

typedef struct Task {
   String name;
   String command_line;
   String_Array inputs;
   String_Array outputs;
   String_Array include_directories;
   u32 *dependencies;
   u32 dependency_count;
} Task;

#endif
