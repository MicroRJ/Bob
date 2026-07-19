u64 platform_performance_counter(void)
{
	LARGE_INTEGER counter;
	if (!QueryPerformanceCounter(&counter)) return 0;
	return (u64)counter.QuadPart;
}

u64 platform_performance_frequency(void)
{
	LARGE_INTEGER frequency;
	if (!QueryPerformanceFrequency(&frequency)) return 0;
	return (u64)frequency.QuadPart;
}
