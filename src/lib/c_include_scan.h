#ifndef C_INCLUDE_SCAN_H
#define C_INCLUDE_SCAN_H

#include "base.h"

typedef struct C_Include_Scan_Result {
    i64 newest_modified_unix_ms;
    b32 unresolved_quoted_include;
} C_Include_Scan_Result;

b32 c_include_scan(String_Array inputs, String_Array include_directories, String command_line, C_Include_Scan_Result *result);

#endif
