#include "profiler.h"


#include "logger.h"
#include "platform/platform.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PROFILE_THREADS        256
#define MAX_THREAD_PROFILE_ENTRIES 64
#define MAX_PROFILE_SUMMARIES      512

typedef struct Profile_Entry
{
	const char *name;
	u64         elapsed;
	u64         calls;
}
Profile_Entry;

typedef struct Thread_Profile
{
	u64           thread_id;
	u32          nentries;
	Profile_Entry entries[MAX_THREAD_PROFILE_ENTRIES];
}
Thread_Profile;

static Thread_Profile thread_profiles[MAX_PROFILE_THREADS];
static atomic_uint thread_profile_count;
static atomic_uint profile_generation;
static THREAD_LOCAL Thread_Profile *local_profile;
static THREAD_LOCAL u32 local_profile_generation;
static u64 profile_start;
static u64 profile_frequency;
static u64 main_thread_id;
static b32 profiling_enabled;

void profiler_reset(void)
{
	memset(thread_profiles, 0, sizeof(thread_profiles));
	atomic_store_explicit(&thread_profile_count, 0, memory_order_relaxed);
	atomic_fetch_add_explicit(&profile_generation, 1, memory_order_relaxed);
	profile_frequency = platform_performance_frequency();
	profile_start = platform_performance_counter();
	main_thread_id = platform_current_thread_id();
}


static Thread_Profile *get_thread_profile(void)
{
	u32 generation = atomic_load_explicit(&profile_generation, memory_order_relaxed);
	if (local_profile && local_profile_generation == generation) return local_profile;

	u32 index = atomic_fetch_add_explicit(&thread_profile_count, 1, memory_order_relaxed);
	if (index >= MAX_PROFILE_THREADS) return NULL;
	local_profile = &thread_profiles[index];
	local_profile_generation = generation;
	local_profile->thread_id = platform_current_thread_id();
	return local_profile;
}

static Profile_Entry *find_or_add_entry(Thread_Profile *profile, const char *name)
{
	for (u32 i = 0; i < profile->nentries; ++i) {
		if (strcmp(profile->entries[i].name, name) == 0) return &profile->entries[i];
	}
	if (profile->nentries >= MAX_THREAD_PROFILE_ENTRIES) return NULL;
	Profile_Entry *result = &profile->entries[profile->nentries++];
	result->name = name;
	return result;
}

void profiler_set_enabled(b32 enabled)
{
	profiling_enabled = enabled;
}

Profile_Scope profile_scope_begin(const char *name)
{
	Profile_Scope scope = {0};
	if (!profiling_enabled || !name) return scope;
	Thread_Profile *profile = get_thread_profile();
	if (!profile) return scope;
	scope.entry = find_or_add_entry(profile, name);
	if (scope.entry) {
		scope.start = platform_performance_counter();
		scope.recording = true;
	}
	return scope;
}

void profile_scope_end(Profile_Scope *scope)
{
	if (!scope || !scope->recording) return;
	u64 end = platform_performance_counter();
	Profile_Entry *entry = scope->entry;
	entry->elapsed += end - scope->start;
	++entry->calls;
	scope->recording = false;
}

static int compare_elapsed(const void *left_pointer, const void *right_pointer)
{
	const Profile_Entry *left = left_pointer;
	const Profile_Entry *right = right_pointer;
	if (left->elapsed < right->elapsed) return 1;
	if (left->elapsed > right->elapsed) return -1;
	return 0;
}

static Profile_Entry *find_or_add_summary(Profile_Entry *summaries, u32 *count, const char *name)
{
	for (u32 i = 0; i < *count; ++i) {
		if (strcmp(summaries[i].name, name) == 0) return &summaries[i];
	}
	if (*count >= MAX_PROFILE_SUMMARIES) return NULL;
	Profile_Entry *result = &summaries[(*count)++];
	result->name = name;
	return result;
}

static void print_entry(const Profile_Entry *entry, f64 ticks_to_ms)
{
	f64 elapsed_ms = entry->elapsed * ticks_to_ms;
	f64 average_ms = entry->calls ? elapsed_ms / entry->calls : 0;
	printf("  %-26s %8llu %12.3f %12.3f\n", entry->name, (unsigned long long)entry->calls, elapsed_ms, average_ms);
}

void profiler_print(b32 include_threads)
{
	if (!profiling_enabled || profile_frequency == 0) return;

	u64 wall_ticks = platform_performance_counter() - profile_start;
	f64 ticks_to_ms = 1000.0 / (f64)profile_frequency;
	u32 profile_count = atomic_load_explicit(&thread_profile_count, memory_order_relaxed);
	if (profile_count > MAX_PROFILE_THREADS) profile_count = MAX_PROFILE_THREADS;

	Profile_Entry summaries[MAX_PROFILE_SUMMARIES] = {0};
	u32 summary_count = 0;
	for (u32 profile_index = 0; profile_index < profile_count; ++profile_index) {
		Thread_Profile *profile = &thread_profiles[profile_index];
		for (u32 entry_index = 0; entry_index < profile->nentries; ++entry_index) {
			Profile_Entry *entry = &profile->entries[entry_index];
			Profile_Entry *summary = find_or_add_summary(summaries, &summary_count, entry->name);
			if (!summary) continue;
			summary->elapsed += entry->elapsed;
			summary->calls += entry->calls;
		}
	}
	qsort(summaries, summary_count, sizeof(*summaries), compare_elapsed);

	log_info("profile wall time: %.3f ms", wall_ticks * ticks_to_ms);
	printf("\naggregate\n");
	printf("  %-26s %8s %12s %12s\n", "scope", "calls", "aggregate ms", "average ms");
	for (u32 i = 0; i < summary_count; ++i) print_entry(&summaries[i], ticks_to_ms);
	if (!include_threads) return;

	for (u32 profile_index = 0; profile_index < profile_count; ++profile_index)
	{
		Thread_Profile *profile = &thread_profiles[profile_index];
		Profile_Entry sorted_entries[MAX_THREAD_PROFILE_ENTRIES];
		memcpy(sorted_entries, profile->entries, profile->nentries * sizeof(*sorted_entries));
		qsort(sorted_entries, profile->nentries, sizeof(*sorted_entries), compare_elapsed);
		printf("\n%s %llu\n", profile->thread_id == main_thread_id ? "main thread" : "thread", (unsigned long long)profile->thread_id);
		printf("  %-26s %8s %12s %12s\n", "scope", "calls", "elapsed ms", "average ms");
		for (u32 i = 0; i < profile->nentries; ++i) print_entry(&sorted_entries[i], ticks_to_ms);
	}
}
