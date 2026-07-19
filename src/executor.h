#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "bob.h"

/* Prepares and executes the graph with at most worker_count concurrent processes. */
b32 executor_run(Bob *bob, u32 worker_count);

#endif
