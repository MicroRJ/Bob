#include "profiler.h"

#include "logger.h"
#include "platform.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PROFILE_ENTRIES 128

typedef struct Profile_Entry {
    const char *name;
    u64 elapsed;
    u64 calls;
} Profile_Entry;

static Profile_Entry entries[MAX_PROFILE_ENTRIES];
static u32 entry_count;
static u64 profile_start;
static u64 profile_frequency;
static b32 profiling_enabled;
static atomic_flag profile_lock = ATOMIC_FLAG_INIT;

static void lock_profiles(void)
{
    while (atomic_flag_test_and_set_explicit(&profile_lock, memory_order_acquire)) {}
}

static void unlock_profiles(void)
{
    atomic_flag_clear_explicit(&profile_lock, memory_order_release);
}

static Profile_Entry *find_or_add_entry(const char *name)
{
    Profile_Entry *result = NULL;
    u32 i;
    lock_profiles();
    for (i = 0; i < entry_count; ++i) {
        if (strcmp(entries[i].name, name) == 0) {
            result = &entries[i];
            break;
        }
    }
    if (!result && entry_count < MAX_PROFILE_ENTRIES) {
        result = &entries[entry_count++];
        result->name = name;
    }
    unlock_profiles();
    return result;
}

void profiler_set_enabled(b32 enabled)
{
    profiling_enabled = enabled;
}

void profiler_reset(void)
{
    lock_profiles();
    memset(entries, 0, sizeof(entries));
    entry_count = 0;
    profile_frequency = platform_performance_frequency();
    profile_start = platform_performance_counter();
    unlock_profiles();
}

Profile_Scope profile_scope_begin(const char *name)
{
    Profile_Scope scope = {0};
    if (!profiling_enabled || !name) return scope;
    scope.entry = find_or_add_entry(name);
    if (scope.entry) {
        scope.start = platform_performance_counter();
        scope.recording = true;
    }
    return scope;
}

void profile_scope_end(Profile_Scope *scope)
{
    u64 end;
    Profile_Entry *entry;
    if (!scope || !scope->recording) return;
    end = platform_performance_counter();
    entry = scope->entry;
    lock_profiles();
    entry->elapsed += end - scope->start;
    ++entry->calls;
    unlock_profiles();
    scope->recording = false;
}

static int compare_entries(const void *left_pointer, const void *right_pointer)
{
    const Profile_Entry *left = left_pointer;
    const Profile_Entry *right = right_pointer;
    if (left->elapsed < right->elapsed) return 1;
    if (left->elapsed > right->elapsed) return -1;
    return 0;
}

void profiler_print(void)
{
    Profile_Entry snapshot[MAX_PROFILE_ENTRIES];
    u32 count;
    u32 i;
    u64 wall_ticks;
    f64 ticks_to_ms;

    if (!profiling_enabled || profile_frequency == 0) return;
    wall_ticks = platform_performance_counter() - profile_start;
    ticks_to_ms = 1000.0 / (f64)profile_frequency;

    lock_profiles();
    count = entry_count;
    memcpy(snapshot, entries, sizeof(*snapshot) * count);
    unlock_profiles();
    qsort(snapshot, count, sizeof(*snapshot), compare_entries);

    log_info("profile wall time: %.3f ms", wall_ticks * ticks_to_ms);
    printf("  %-26s %8s %12s %12s\n", "scope", "calls", "total ms", "average ms");
    for (i = 0; i < count; ++i) {
        f64 total_ms = snapshot[i].elapsed * ticks_to_ms;
        f64 average_ms = snapshot[i].calls ? total_ms / snapshot[i].calls : 0;
        printf("  %-26s %8llu %12.3f %12.3f\n", snapshot[i].name,
               (unsigned long long)snapshot[i].calls, total_ms, average_ms);
    }
}
