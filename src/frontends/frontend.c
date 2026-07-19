#include "frontend.h"

#include "elf_adapter.h"

#include <stdio.h>
typedef b32 Frontend_Load_Function(String path, Bob_Build *result);

typedef struct Frontend
{
	String                  extension;
	Frontend_Load_Function *load;
}
Frontend;

static const Frontend frontends[] =
{
	{ STRING_LITERAL(".elf"), elf_load_build },
};

static const Frontend *find_frontend(String path)
{
	for (u32 i = 0; i < ARRAY_COUNT(frontends); ++i) {
		if (string_ends_with_insensitive(path, frontends[i].extension)) return &frontends[i];
	}
	return NULL;
}

b32 frontend_supports_path(String path)
{
	return find_frontend(path) != NULL;
}

b32 frontend_load_build(String path, Bob_Build *result)
{
	if (!path.data || !result) return false;
	*result = (Bob_Build){0};

	const Frontend *frontend = find_frontend(path);
	if (frontend) return frontend->load(path, result);

	snprintf(result->error, sizeof(result->error), "no frontend supports '%.*s'", (int)path.size, path.data);
	return false;
}
