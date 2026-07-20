#ifndef ELF_ADAPTER_H
#define ELF_ADAPTER_H

#include "script_internal.h"

b32 elf_script_load(Script *script, String path);
void elf_script_destroy(Script *script);
b32 elf_script_invoke(Script *script, String name);
b32 elf_script_read_build(Script *script, Script_Build *result);

#endif
