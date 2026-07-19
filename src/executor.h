#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "graph.h"

typedef struct Task
{
   const char *command_line;
   const char **inputs;
   u32 input_count;
   const char **outputs;
   u32 output_count;
   const char **include_directories;
   u32 include_directory_count;
} Task;

/* Prepares and executes the graph with at most worker_count concurrent processes. */
b32 executor_run(Graph *graph, u32 worker_count);

#endif
