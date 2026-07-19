void *platform_virtual_reserve(u64 size)
{
	if (size > SIZE_MAX) return NULL;
	return VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_READWRITE);
}

b32 platform_virtual_commit(void *memory, u64 size)
{
	if (!memory || size > SIZE_MAX) return false;
	return VirtualAlloc(memory, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

void platform_virtual_free(void *memory)
{
	if (memory) VirtualFree(memory, 0, MEM_RELEASE);
}
