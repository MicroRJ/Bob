#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "graph.h"
#include "task.h"

/* Prepares and executes the graph with at most worker_count concurrent processes. */
b32 executor_run(Graph *graph, u32 worker_count);

#endif
