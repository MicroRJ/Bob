#ifndef C_INCLUDE_SCAN_H
#define C_INCLUDE_SCAN_H

#include "base.h"

typedef struct C_Include_Scan_Result {
    u64 newest_write_time;
    b32 unresolved_quoted_include;
} C_Include_Scan_Result;

b32 c_include_scan(const char **inputs, u32 input_count,
                   const char **include_directories, u32 include_directory_count,
                   const char *command_line,
                   C_Include_Scan_Result *result);

#endif
