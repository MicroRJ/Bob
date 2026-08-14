#include "bob_build.h"
#include "build_state.h"
#include "build_state_stream.h"
#include "script.h"
#include "compiler_command.h"
#include "make_depfile.h"
#include "logger.h"
#include "platform_adapter.h"
#include "platform.h"
#include "vcvars_cache.h"
#include "blake3.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run;
static int tests_failed;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);      \
            return false;                                                       \
        }                                                                       \
    } while (0)

#define CHECK_OK(expression) CHECK((expression) == BOB_OK)
#define STRING_ARRAY_FROM(array) ((String_Array){ .items = (array), .count = ARRAY_COUNT(array) })

static b32 environment_equals(const char *name, const char *expected)
{
    Scratch scratch = begin_scratch();
    String value = {0};
	b32 equal = bob_platform_get_environment(string_from_cstring(name), scratch.arena, &value) &&
        string_equal(value, string_from_cstring(expected));
    end_scratch(scratch);
    return equal;
}

static b32 test_vcvars_cache_application(void)
{
    static const char cache_text[] =
        "BOB_VCVARS_CACHE_V1\n"
        "prepend BOB_VCVARS_TEST_PREPEND=tool;\n"
        "append BOB_VCVARS_TEST_APPEND=;tail\n"
        "set BOB_VCVARS_TEST_SET=value\n";
    b32 result = false;

	bob_platform_set_environment(STRING_LITERAL("BOB_VCVARS_TEST_PREPEND"), STRING_LITERAL("base"));
	bob_platform_set_environment(STRING_LITERAL("BOB_VCVARS_TEST_APPEND"), STRING_LITERAL("base"));
	bob_platform_set_environment(STRING_LITERAL("BOB_VCVARS_TEST_SET"), (String){0});

    if (!vcvars_cache_apply(string_from_cstring(cache_text))) goto cleanup;
    if (!environment_equals("BOB_VCVARS_TEST_PREPEND", "tool;base")) goto cleanup;
    if (!environment_equals("BOB_VCVARS_TEST_APPEND", "base;tail")) goto cleanup;
    if (!environment_equals("BOB_VCVARS_TEST_SET", "value")) goto cleanup;
    result = true;

cleanup:
	bob_platform_set_environment(STRING_LITERAL("BOB_VCVARS_TEST_PREPEND"), (String){0});
	bob_platform_set_environment(STRING_LITERAL("BOB_VCVARS_TEST_APPEND"), (String){0});
	bob_platform_set_environment(STRING_LITERAL("BOB_VCVARS_TEST_SET"), (String){0});
    return result;
}

static b32 test_high_resolution_timer(void)
{
    u64 frequency = platform_counter_frequency();
    u64 before = platform_counter();
    Sleep(1);
    return frequency > 0 && platform_counter() >= before && platform_current_thread_id() != 0;
}

static b32 test_blake3(void)
{
	static const u8 empty_hash[BLAKE3_OUT_LEN] = {
		0xaf, 0x13, 0x49, 0xb9, 0xf5, 0xf9, 0xa1, 0xa6,
		0xa0, 0x40, 0x4d, 0xea, 0x36, 0xdc, 0xc9, 0x49,
		0x9b, 0xcb, 0x25, 0xc9, 0xad, 0xc1, 0x12, 0xb7,
		0xcc, 0x9a, 0x93, 0xca, 0xe4, 0x1f, 0x32, 0x62,
	};
	static const u8 abc_hash[BLAKE3_OUT_LEN] = {
		0x64, 0x37, 0xb3, 0xac, 0x38, 0x46, 0x51, 0x33,
		0xff, 0xb6, 0x3b, 0x75, 0x27, 0x3a, 0x8d, 0xb5,
		0x48, 0xc5, 0x58, 0x46, 0x5d, 0x79, 0xdb, 0x03,
		0xfd, 0x35, 0x9c, 0x6c, 0xd5, 0xbd, 0x9d, 0x85,
	};
	blake3_hasher hasher;
	u8 hash[BLAKE3_OUT_LEN];

	blake3_hasher_init(&hasher);
	blake3_hasher_finalize(&hasher, hash, sizeof(hash));
	CHECK(memcmp(hash, empty_hash, sizeof(hash)) == 0);

	blake3_hasher_init(&hasher);
	blake3_hasher_update(&hasher, "a", 1);
	blake3_hasher_update(&hasher, "bc", 2);
	blake3_hasher_finalize(&hasher, hash, sizeof(hash));
	CHECK(memcmp(hash, abc_hash, sizeof(hash)) == 0);
	return true;
}

static b32 test_build_paths(void)
{
	Bob_Build *build = bob_build_create();
	Bob *graph;
	Bob_Path root;
	Bob_Path first;
	Bob_Path second;
	Bob_Path absolute;
	String root_string;
	String first_string;
	CHECK(build != NULL);
	graph = bob_build_graph(build);
	CHECK(graph != NULL);

	root = bob_build_root(build);
	root_string = bob_path_string(build, root);
	CHECK(bob_path_is_valid(root));
	CHECK(root_string.size >= 3 && root_string.data[1] == ':');
	CHECK(root_string.data[0] >= 'A' && root_string.data[0] <= 'Z');
	CHECK(memchr(root_string.data, '\\', (size_t)root_string.size) == NULL);
	CHECK(bob_path_resolve(build, root, STRING_LITERAL("build\\path-test\\temporary\\..\\file.obj"), &first));
	CHECK(bob_path_resolve(build, root, STRING_LITERAL(".\\build/path-test/file.obj"), &second));
	CHECK(first.atom.id == second.atom.id);
	first_string = bob_path_string(build, first);
	CHECK(memchr(first_string.data, '\\', (size_t)first_string.size) == NULL);
	CHECK(bob_path_resolve(build, root, first_string, &absolute));
	CHECK(absolute.atom.id == first.atom.id);
	CHECK(!bob_path_resolve(build, root, STRING_LITERAL(""), &absolute));

	CHECK_OK(bob_prepare(graph));
	CHECK(bob_path_resolve(build, root, STRING_LITERAL("discovered after prepare"), &absolute));
	bob_build_destroy(build);
	return true;
}

static Bob_Node *add_node(Bob *graph, const char *name)
{
    Bob_Node *node = NULL;
    Bob_Error result = bob_add_node(graph, (Bob_Node_Desc){ .name = string_from_cstring(name) }, &node);
    if (result != BOB_OK) {
        printf("  unable to add node %s: %s\n", name, bob_error_string(result));
        exit(2);
    }
    return node;
}

static b32 string_array_matches(String_Array actual, const char **expected, u32 count)
{
	if (actual.count != count) return false;
	for (u32 i = 0; i < count; ++i) {
		if (!string_equal(actual.items[i], string_from_cstring(expected[i]))) return false;
	}
	return true;
}

static u32 test_crc32c(const void *data, u64 size)
{
	const u8 *bytes = data;
	u32 crc = UINT32_MAX;
	for (u64 i = 0; i < size; ++i) {
		crc ^= bytes[i];
		for (u32 bit = 0; bit < 8; ++bit) {
			u32 mask = 0U - (crc & 1U);
			crc = (crc >> 1) ^ (0x82F63B78U & mask);
		}
	}
	return ~crc;
}

static void test_store_u32(u8 *data, u32 value)
{
	data[0] = (u8)value;
	data[1] = (u8)(value >> 8);
	data[2] = (u8)(value >> 16);
	data[3] = (u8)(value >> 24);
}

static u32 test_load_u32(const u8 *data)
{
	return (u32)data[0] | ((u32)data[1] << 8) | ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

static u64 test_find_state_stream_operation(String stream, Build_State_Op operation, u32 occurrence)
{
	u64 cursor = BUILD_STATE_STREAM_HEADER_SIZE;
	while (cursor + BUILD_STATE_STREAM_RECORD_HEADER_SIZE <= stream.size) {
		u32 content_size = test_load_u32((const u8 *)stream.data + cursor);
		u64 content = cursor + BUILD_STATE_STREAM_RECORD_HEADER_SIZE;
		if (content_size < sizeof(u32) || content_size > stream.size - content) return UINT64_MAX;
		if (test_load_u32((const u8 *)stream.data + content) == (u32)operation) {
			if (occurrence == 0) return content;
			--occurrence;
		}
		cursor = content + content_size;
	}
	return UINT64_MAX;
}

static void test_update_state_stream_checksum(String stream, u64 content)
{
	u64 header = content - BUILD_STATE_STREAM_RECORD_HEADER_SIZE;
	u32 content_size = test_load_u32((const u8 *)stream.data + header);
	u32 checksum = test_crc32c((const u8 *)stream.data + content, content_size);
	test_store_u32((u8 *)stream.data + header + 4, checksum);
}

static Bob_Path test_path(Bob_Build *build, String source)
{
	Bob_Path result = {0};
	if (build) bob_path_resolve(build, bob_build_root(build), source, &result);
	return result;
}

static Bob_Fingerprint test_fingerprint(String value)
{
	Bob_Fingerprint result;
	blake3_hasher hasher;
	blake3_hasher_init(&hasher);
	blake3_hasher_update(&hasher, value.data, (size_t)value.size);
	blake3_hasher_finalize(&hasher, result.bytes, sizeof(result.bytes));
	return result;
}

static b32 fingerprints_equal(Bob_Fingerprint left, Bob_Fingerprint right)
{
	return memcmp(left.bytes, right.bytes, BOB_FINGERPRINT_SIZE) == 0;
}

static b32 test_state_set(Arena *arena, Bob_Build *build, Build_State *state, String output, String_Array dependencies)
{
	Bob_Path_Array paths = {0};
	if (dependencies.count) {
		paths.items = arena_push_zero_aligned(arena, (u64)dependencies.count * sizeof(*paths.items), _Alignof(Bob_Path));
		if (!paths.items) return false;
	}
	for (u32 i = 0; i < dependencies.count; ++i) {
		paths.items[paths.count] = test_path(build, dependencies.items[i]);
		if (!bob_path_is_valid(paths.items[paths.count])) return false;
		++paths.count;
	}
	return build_state_set(arena, state, test_path(build, output), paths, test_fingerprint(output));
}

static b32 test_state_append_set(Arena *arena, Bob_Build *build, String file, Build_State *state, String output, String_Array dependencies, u64 output_stamp, String fingerprint)
{
	Bob_Path_Array paths = {0};
	if (dependencies.count) {
		paths.items = arena_push_zero_aligned(arena, (u64)dependencies.count * sizeof(*paths.items), _Alignof(Bob_Path));
		if (!paths.items) return false;
	}
	for (u32 i = 0; i < dependencies.count; ++i) {
		paths.items[paths.count] = test_path(build, dependencies.items[i]);
		if (!bob_path_is_valid(paths.items[paths.count])) return false;
		++paths.count;
	}
	return build_state_append_set(arena, file, build, state, test_path(build, output), paths, output_stamp, test_fingerprint(fingerprint));
}

static b32 state_task_contains_path(Bob_Build *build, const Build_State_Task_Snapshot *task, String path)
{
	Bob_Path expected = test_path(build, path);
	if (!task || !bob_path_is_valid(expected)) return false;
	for (u32 i = 0; i < task->dependencies.count; ++i) {
		if (task->dependencies.items[i].atom.id == expected.atom.id) return true;
	}
	return false;
}

static b32 test_build_state_file(void)
{
	static const char malformed[] = "not a Bob state";
	Bob_Build *bob = bob_build_create();
	Arena state_arena = arena_create(KILOBYTES(64));
	Arena loaded_arena = arena_create(KILOBYTES(64));
	Arena io_arena = arena_create(KILOBYTES(64));
	Build_State state = {0};
	Build_State loaded = {0};
	String dependencies[] = {
		STRING_LITERAL("src/main file.c"),
		STRING_LITERAL("include/quoted\"name.h"),
	};
	String root = STRING_LITERAL("build\\build_state_file");
	String path = STRING_LITERAL("build\\build_state_file\\nested\\state");
	String temporary = STRING_LITERAL("build\\build_state_file\\nested\\state.tmp");
	String missing = STRING_LITERAL("build\\build_state_file\\missing");
	String directory = STRING_LITERAL("build\\build_state_file\\nested\\directory");
	String directory_temporary = STRING_LITERAL("build\\build_state_file\\nested\\directory.tmp");
	Build_State_Task_Snapshot task;
	Bob_Platform_File_Info info;
	String bytes;
	u64 operation;

	CHECK(bob && state_arena.data && loaded_arena.data && io_arena.data);
	CHECK(build_state_init(&state) && build_state_init(&loaded));
	CHECK(platform_remove_tree(root.data));
	CHECK(test_state_set(&state_arena, bob, &state,
		STRING_LITERAL("build/obj/main file.obj"),
		STRING_ARRAY_FROM(dependencies)));
	CHECK(test_state_set(&state_arena, bob, &state,
		STRING_LITERAL("build/obj/empty.obj"), (String_Array){0}));
	state.tasks[0].output_stamp = 101;
	state.tasks[1].output_stamp = 202;
	CHECK(build_state_save(path, bob, &state));
	CHECK(bob_platform_file_info(path, &info));
	CHECK(!bob_platform_file_info(temporary, &info));
	CHECK(build_state_load(&loaded_arena, bob, path, &loaded) ==
		BUILD_STATE_LOAD_OK);
	CHECK(loaded.task_count == 2);
	CHECK(build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build/obj/main file.obj")), &task));
	CHECK(task.output_stamp == 101 && task.dependencies.count == 2);
	CHECK(fingerprints_equal(task.fingerprint, test_fingerprint(STRING_LITERAL("build/obj/main file.obj"))));
	CHECK(state_task_contains_path(bob, &task, dependencies[0]));
	CHECK(state_task_contains_path(bob, &task, dependencies[1]));
	CHECK(build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build/obj/empty.obj")), &task));
	CHECK(task.output_stamp == 202 && task.dependencies.count == 0);

	arena_reset(&loaded_arena);
	CHECK(build_state_load(&loaded_arena, bob, missing, &loaded) ==
		BUILD_STATE_LOAD_MISSING);
	CHECK(loaded.task_count == 0);
	CHECK(bob_platform_write_entire_file(path, malformed, sizeof(malformed) - 1));
	CHECK(build_state_load(&loaded_arena, bob, path, &loaded) ==
		BUILD_STATE_LOAD_INVALID);

	CHECK(build_state_save(path, bob, &state));
	arena_reset(&io_arena);
	CHECK(bob_platform_read_entire_file(&io_arena, path, &bytes));
	CHECK(bytes.size > BUILD_STATE_STREAM_HEADER_SIZE);
	CHECK(bob_platform_write_entire_file(path, bytes.data, bytes.size - 1));
	arena_reset(&loaded_arena);
	CHECK(build_state_load(&loaded_arena, bob, path, &loaded) ==
		BUILD_STATE_LOAD_RECOVERED);
	CHECK(loaded.paths.path_count == state.paths.path_count);
	CHECK(loaded.task_count == 1);
	CHECK(build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build/obj/main file.obj")), &task));
	CHECK(!build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build/obj/empty.obj")), &task));
	CHECK(build_state_save(path, bob, &loaded));
	arena_reset(&loaded_arena);
	CHECK(build_state_load(&loaded_arena, bob, path, &loaded) == BUILD_STATE_LOAD_OK);
	CHECK(loaded.task_count == 1);

	CHECK(build_state_save(path, bob, &state));
	arena_reset(&io_arena);
	CHECK(bob_platform_read_entire_file(&io_arena, path, &bytes));
	bytes.data[bytes.size - 1] ^= 0x5A;
	CHECK(bob_platform_write_entire_file(path, bytes.data, bytes.size));
	arena_reset(&loaded_arena);
	CHECK(build_state_load(&loaded_arena, bob, path, &loaded) ==
		BUILD_STATE_LOAD_INVALID);

	CHECK(build_state_save(path, bob, &state));
	arena_reset(&io_arena);
	CHECK(bob_platform_read_entire_file(&io_arena, path, &bytes));
	test_store_u32((u8 *)bytes.data + 8, BUILD_STATE_STREAM_VERSION + 1);
	CHECK(bob_platform_write_entire_file(path, bytes.data, bytes.size));
	arena_reset(&loaded_arena);
	CHECK(build_state_load(&loaded_arena, bob, path, &loaded) ==
		BUILD_STATE_LOAD_INVALID);

	CHECK(build_state_save(path, bob, &state));
	arena_reset(&io_arena);
	CHECK(bob_platform_read_entire_file(&io_arena, path, &bytes));
	operation = test_find_state_stream_operation(bytes, STATE_OP_SET, 0);
	CHECK(operation != UINT64_MAX);
	test_store_u32((u8 *)bytes.data + operation + 4, BUILD_STATE_PATH_ID_NONE);
	test_update_state_stream_checksum(bytes, operation);
	CHECK(bob_platform_write_entire_file(path, bytes.data, bytes.size));
	arena_reset(&loaded_arena);
	CHECK(build_state_load(&loaded_arena, bob, path, &loaded) ==
		BUILD_STATE_LOAD_INVALID);

	CHECK(bob_platform_create_directory(directory));
	CHECK(build_state_load(&loaded_arena, bob, directory, &loaded) ==
		BUILD_STATE_LOAD_ERROR);
	CHECK(!build_state_save(directory, bob, &state));
	CHECK(!bob_platform_file_info(directory_temporary, &info));
	CHECK(platform_remove_tree(root.data));
	build_state_destroy(&loaded);
	build_state_destroy(&state);
	arena_destroy(&io_arena);
	arena_destroy(&loaded_arena);
	arena_destroy(&state_arena);
	bob_build_destroy(bob);
	return true;
}

static b32 test_build_state_stream(void)
{
	Bob_Build *bob = bob_build_create();
	Arena state_arena = arena_create(KILOBYTES(64));
	Arena stream_arena = arena_create(KILOBYTES(64));
	Arena loaded_arena = arena_create(KILOBYTES(64));
	Arena mutation_arena = arena_create(KILOBYTES(64));
	Build_State state = {0};
	Build_State loaded = {0};
	String dependencies[] = {
		STRING_LITERAL("src/main.c"),
		STRING_LITERAL("include/main.h"),
	};
	String stream;
	String mutated;
	Build_State_Task_Snapshot task;
	u64 operation;

	CHECK(bob && state_arena.data && stream_arena.data && loaded_arena.data && mutation_arena.data);
	CHECK(build_state_init(&state) && build_state_init(&loaded));
	CHECK(test_state_set(&state_arena, bob, &state, STRING_LITERAL("build/main.obj"), STRING_ARRAY_FROM(dependencies)));
	CHECK(test_state_set(&state_arena, bob, &state, STRING_LITERAL("build/empty.obj"), (String_Array){0}));
	state.tasks[0].output_stamp = 101;
	state.tasks[1].output_stamp = 202;
	CHECK(build_state_stream_encode(&stream_arena, bob, &state, &stream));
	CHECK(stream.size > BUILD_STATE_STREAM_HEADER_SIZE);
	CHECK(build_state_stream_replay(&loaded_arena, bob, stream, &loaded) == BUILD_STATE_STREAM_OK);
	CHECK(loaded.paths.path_count == state.paths.path_count && loaded.task_count == state.task_count);
	CHECK(build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build/main.obj")), &task));
	CHECK(task.output_stamp == 101 && task.dependencies.count == 2);
	CHECK(fingerprints_equal(task.fingerprint, test_fingerprint(STRING_LITERAL("build/main.obj"))));
	CHECK(state_task_contains_path(bob, &task, dependencies[0]));
	CHECK(state_task_contains_path(bob, &task, dependencies[1]));
	CHECK(build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build/empty.obj")), &task));
	CHECK(task.output_stamp == 202 && task.dependencies.count == 0);

	arena_reset(&loaded_arena);
	CHECK(build_state_stream_replay(&loaded_arena, bob, string_slice(stream, 0, stream.size - 1), &loaded) == BUILD_STATE_STREAM_TRUNCATED);
	CHECK(loaded.paths.path_count == state.paths.path_count);
	CHECK(loaded.task_count == 1);
	CHECK(build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build/main.obj")), &task));
	CHECK(!build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build/empty.obj")), &task));

	arena_reset(&mutation_arena);
	mutated = string_from_data(arena_push_copy(&mutation_arena, stream.size, stream.data), stream.size);
	CHECK(mutated.data != NULL);
	mutated.data[mutated.size - 1] ^= 0x5A;
	arena_reset(&loaded_arena);
	CHECK(build_state_stream_replay(&loaded_arena, bob, mutated, &loaded) == BUILD_STATE_STREAM_INVALID);
	CHECK(loaded.paths.path_count == 0 && loaded.task_count == 0);

	arena_reset(&mutation_arena);
	mutated = string_from_data(arena_push_copy(&mutation_arena, stream.size, stream.data), stream.size);
	CHECK(mutated.data != NULL);
	operation = test_find_state_stream_operation(mutated, STATE_OP_INTERN, 0);
	CHECK(operation != UINT64_MAX);
	mutated.data[operation + 4] ^= 0x40;
	arena_reset(&loaded_arena);
	CHECK(build_state_stream_replay(&loaded_arena, bob, mutated, &loaded) == BUILD_STATE_STREAM_INVALID);
	CHECK(loaded.paths.path_count == 0 && loaded.task_count == 0);

	arena_reset(&mutation_arena);
	mutated = string_from_data(arena_push_copy(&mutation_arena, stream.size, stream.data), stream.size);
	CHECK(mutated.data != NULL);
	operation = test_find_state_stream_operation(mutated, STATE_OP_INTERN, 0);
	CHECK(operation != UINT64_MAX);
	test_store_u32((u8 *)mutated.data + operation, 99);
	test_update_state_stream_checksum(mutated, operation);
	arena_reset(&loaded_arena);
	CHECK(build_state_stream_replay(&loaded_arena, bob, mutated, &loaded) == BUILD_STATE_STREAM_INVALID);

	arena_reset(&mutation_arena);
	mutated = string_from_data(arena_push_copy(&mutation_arena, stream.size, stream.data), stream.size);
	CHECK(mutated.data != NULL);
	operation = test_find_state_stream_operation(mutated, STATE_OP_SET, 0);
	CHECK(operation != UINT64_MAX);
	test_store_u32((u8 *)mutated.data + operation + 4, state.paths.path_count + 1);
	test_update_state_stream_checksum(mutated, operation);
	arena_reset(&loaded_arena);
	CHECK(build_state_stream_replay(&loaded_arena, bob, mutated, &loaded) == BUILD_STATE_STREAM_INVALID);

	arena_reset(&mutation_arena);
	{
		u64 remove_size = BUILD_STATE_STREAM_RECORD_HEADER_SIZE + 8;
		u8 *data = arena_push(&mutation_arena, stream.size + remove_size);
		CHECK(data != NULL);
		memcpy(data, stream.data, stream.size);
		test_store_u32(data + stream.size, 8);
		test_store_u32(data + stream.size + 8, STATE_OP_REMOVE);
		test_store_u32(data + stream.size + 12, state.paths.ids_by_atom[state.tasks[1].output.atom.id]);
		test_store_u32(data + stream.size + 4, test_crc32c(data + stream.size + 8, 8));
		mutated = string_from_data(data, stream.size + remove_size);
	}
	arena_reset(&loaded_arena);
	CHECK(build_state_stream_replay(&loaded_arena, bob, mutated, &loaded) == BUILD_STATE_STREAM_OK);
	CHECK(loaded.task_count == 1);
	CHECK(build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build/main.obj")), &task));
	CHECK(!build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build/empty.obj")), &task));

	build_state_destroy(&loaded);
	build_state_destroy(&state);
	arena_destroy(&mutation_arena);
	arena_destroy(&loaded_arena);
	arena_destroy(&stream_arena);
	arena_destroy(&state_arena);
	bob_build_destroy(bob);
	return true;
}

static b32 test_build_state_append(void)
{
	Bob_Build *bob = bob_build_create();
	Arena state_arena = arena_create(KILOBYTES(64));
	Arena loaded_arena = arena_create(KILOBYTES(64));
	Arena io_arena = arena_create(KILOBYTES(64));
	Build_State state = {0};
	Build_State loaded = {0};
	String root = STRING_LITERAL("build\\build_state_append");
	String path = STRING_LITERAL("build\\build_state_append\\state");
	String first_dependencies[] = {
		STRING_LITERAL("src/main.c"),
		STRING_LITERAL("include/common.h"),
	};
	String replacement_dependencies[] = {
		STRING_LITERAL("src/main.c"),
		STRING_LITERAL("include/next.h"),
	};
	Build_State_Task_Snapshot task;
	Bob_Platform_File_Info before;
	Bob_Platform_File_Info after;
	String bytes;

	CHECK(bob && state_arena.data && loaded_arena.data && io_arena.data);
	CHECK(build_state_init(&state) && build_state_init(&loaded));
	CHECK(platform_remove_tree(root.data));
	CHECK(build_state_save(path, bob, &state));
	CHECK(bob_platform_file_info(path, &before));
	CHECK(test_state_append_set(&state_arena, bob, path, &state, STRING_LITERAL("build/main.obj"), STRING_ARRAY_FROM(first_dependencies), 101, STRING_LITERAL("first fingerprint")));
	CHECK(bob_platform_file_info(path, &after));
	CHECK(after.size > before.size);
	CHECK(state.paths.path_count == 3 && state.task_count == 1);
	CHECK(test_state_append_set(&state_arena, bob, path, &state, STRING_LITERAL("build/main.obj"), STRING_ARRAY_FROM(replacement_dependencies), 202, STRING_LITERAL("replacement fingerprint")));
	CHECK(state.paths.path_count == 4 && state.task_count == 1);

	CHECK(build_state_load(&loaded_arena, bob, path, &loaded) == BUILD_STATE_LOAD_OK);
	CHECK(build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build/main.obj")), &task));
	CHECK(task.output_stamp == 202 && task.dependencies.count == 2);
	CHECK(fingerprints_equal(task.fingerprint, test_fingerprint(STRING_LITERAL("replacement fingerprint"))));
	CHECK(state_task_contains_path(bob, &task, replacement_dependencies[0]));
	CHECK(state_task_contains_path(bob, &task, replacement_dependencies[1]));
	CHECK(!state_task_contains_path(bob, &task, first_dependencies[1]));

	CHECK(build_state_append_remove(path, &state, test_path(bob, STRING_LITERAL("build/main.obj"))));
	CHECK(state.task_count == 0);
	arena_reset(&io_arena);
	CHECK(bob_platform_read_entire_file(&io_arena, path, &bytes));
	CHECK(bytes.size > BUILD_STATE_STREAM_RECORD_HEADER_SIZE);
	CHECK(bob_platform_write_entire_file(path, bytes.data, bytes.size - 1));
	arena_reset(&loaded_arena);
	CHECK(build_state_load(&loaded_arena, bob, path, &loaded) == BUILD_STATE_LOAD_RECOVERED);
	CHECK(build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build/main.obj")), &task));
	CHECK(task.output_stamp == 202);
	CHECK(build_state_save(path, bob, &loaded));
	arena_reset(&loaded_arena);
	CHECK(build_state_load(&loaded_arena, bob, path, &loaded) == BUILD_STATE_LOAD_OK);
	CHECK(build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build/main.obj")), &task));

	CHECK(platform_remove_tree(root.data));
	build_state_destroy(&loaded);
	build_state_destroy(&state);
	arena_destroy(&io_arena);
	arena_destroy(&loaded_arena);
	arena_destroy(&state_arena);
	bob_build_destroy(bob);
	return true;
}

static b32 test_build_state_path_map(void)
{
	Bob_Build *bob = bob_build_create();
	Arena arena = arena_create(KILOBYTES(64));
	Build_State state = {0};
	Bob_Path first = test_path(bob, STRING_LITERAL("build\\objects\\..\\main.obj"));
	Bob_Path same = test_path(bob, STRING_LITERAL("build/main.obj"));
	Bob_Path dependency = test_path(bob, STRING_LITERAL("include/main.h"));
	Bob_Path dependencies[] = { dependency };

	CHECK(bob && arena.data && bob_path_is_valid(first));
	CHECK(build_state_init(&state));
	CHECK(first.atom.id == same.atom.id);
	CHECK(build_state_set(&arena, &state, first, (Bob_Path_Array){ dependencies, 1 }, test_fingerprint(STRING_LITERAL("path map"))));
	CHECK(state.paths.path_count == 2);
	CHECK(state.paths.ids_by_atom[first.atom.id] == 1);
	CHECK(state.paths.paths[0].atom.id == first.atom.id);
	Build_State_Task_Snapshot task;
	CHECK(build_state_get_task(&state, same, &task));

	build_state_destroy(&state);
	arena_destroy(&arena);
	bob_build_destroy(bob);
	return true;
}

static b32 test_build_state_tasks(void)
{
	Bob_Build *bob = bob_build_create();
	Arena arena = arena_create(KILOBYTES(256));
	Build_State state = {0};
	String first_dependencies[] = {
		STRING_LITERAL("src/main.c"),
		STRING_LITERAL("include/common.h"),
	};
	String replacement_dependencies[] = {
		STRING_LITERAL("include/common.h"),
		STRING_LITERAL("include/next.h"),
	};
	Build_State_Task_Snapshot task;
	Build_State_Task_Snapshot first_task;
	Bob_Path output;

	arena_set_name(&arena, "build state task test");
	CHECK(bob && arena.data != NULL);
	CHECK(build_state_init(&state));
	output = test_path(bob, STRING_LITERAL("build/main.o"));
	CHECK(!build_state_get_task(&state, output, &task));
	CHECK(!build_state_set(&arena, &state, (Bob_Path){0}, (Bob_Path_Array){0}, (Bob_Fingerprint){0}));
	CHECK(!build_state_set(&arena, &state, output, (Bob_Path_Array){ .count = 1 }, (Bob_Fingerprint){0}));
	CHECK(state.task_count == 0 && state.paths.path_count == 0);

	CHECK(test_state_set(&arena, bob, &state, STRING_LITERAL("build/main.o"),
		STRING_ARRAY_FROM(first_dependencies)));
	CHECK(state.task_count == 1);
	CHECK(state.paths.path_count == 3);
	CHECK(build_state_get_task(&state, output, &task));
	CHECK(task.output.atom.id == output.atom.id);
	CHECK(task.dependencies.count == 2);
	CHECK(state_task_contains_path(bob, &task, STRING_LITERAL("src/main.c")));
	CHECK(state_task_contains_path(bob, &task, STRING_LITERAL("include/common.h")));
	first_task = task;

	CHECK(test_state_set(&arena, bob, &state, STRING_LITERAL("build/main.o"),
		STRING_ARRAY_FROM(replacement_dependencies)));
	CHECK(state.task_count == 1);
	CHECK(state.paths.path_count == 4);
	CHECK(build_state_get_task(&state, output, &task));
	CHECK(task.output.atom.id == output.atom.id);
	CHECK(task.dependencies.count == 2);
	CHECK(state_task_contains_path(bob, &task, STRING_LITERAL("include/common.h")));
	CHECK(state_task_contains_path(bob, &task, STRING_LITERAL("include/next.h")));
	CHECK(state_task_contains_path(bob, &first_task, STRING_LITERAL("src/main.c")));
	CHECK(!state_task_contains_path(bob, &first_task, STRING_LITERAL("include/next.h")));

	CHECK(!build_state_remove(&state, test_path(bob, STRING_LITERAL("build/missing.o"))));
	CHECK(test_state_set(&arena, bob, &state, STRING_LITERAL("build/second.o"), (String_Array){0}));
	CHECK(state.task_count == 2);
	CHECK(build_state_remove(&state, output));
	CHECK(state.task_count == 1);
	CHECK(!build_state_get_task(&state, output, &task));
	CHECK(build_state_get_task(&state, test_path(bob, STRING_LITERAL("build/second.o")), &task));
	CHECK(state.paths.path_count == 5);

	build_state_destroy(&state);
	arena_destroy(&arena);
	bob_build_destroy(bob);
	return true;
}

static b32 test_build_state_stress(void)
{
	enum { TASK_COUNT = 1886, DEPENDENCY_COUNT = 300 };
	Bob_Build *bob = bob_build_create();
	Arena source_arena = arena_create(MEGABYTES(1));
	Arena state_arena = arena_create(MEGABYTES(16));
	Arena loaded_arena = arena_create(MEGABYTES(16));
	Build_State state = {0};
	Build_State loaded = {0};
	String_Array dependencies = {0};
	Bob_Path_Array dependency_paths = {0};
	String root = STRING_LITERAL("build\\build_state_stress");
	String path = STRING_LITERAL("build\\build_state_stress\\state");
	Bob_Platform_File_Info info;
	u64 frequency = platform_counter_frequency();
	u64 construction_started;
	u64 construction_finished;
	u64 save_finished;
	u64 load_finished;
	b32 result = false;

	arena_set_name(&source_arena, "build state stress source");
	arena_set_name(&state_arena, "build state stress state");
	arena_set_name(&loaded_arena, "build state stress loaded state");

#define CHECK_STRESS(condition)                                                 \
	do {                                                                         \
		if (!(condition)) {                                                        \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
			goto cleanup;                                                           \
		}                                                                          \
	} while (0)

	CHECK_STRESS(bob && source_arena.data && state_arena.data && loaded_arena.data);
	CHECK_STRESS(build_state_init(&state) && build_state_init(&loaded));
	CHECK_STRESS(platform_remove_tree(root.data));
	dependencies.items = arena_push_zero_aligned(&source_arena,
		DEPENDENCY_COUNT * sizeof(*dependencies.items), _Alignof(String));
	dependency_paths.items = arena_push_zero_aligned(&source_arena, DEPENDENCY_COUNT * sizeof(*dependency_paths.items), _Alignof(Bob_Path));
	CHECK_STRESS(dependencies.items != NULL && dependency_paths.items != NULL);
	for (u32 i = 0; i < DEPENDENCY_COUNT; ++i) {
		char path[256];
		int length = snprintf(path, sizeof(path),
			"G:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\MSVC\\14.44.35207\\include\\synthetic\\header_%04u.h",
			i);
		CHECK_STRESS(length > 0 && (size_t)length < sizeof(path));
		dependencies.items[i] = arena_push_string_copy(&source_arena,
			string_from_data(path, (u64)length));
		CHECK_STRESS(dependencies.items[i].data != NULL);
		dependency_paths.items[i] = test_path(bob, dependencies.items[i]);
		CHECK_STRESS(bob_path_is_valid(dependency_paths.items[i]));
		++dependencies.count;
		++dependency_paths.count;
	}

	construction_started = platform_counter();
	for (u32 i = 0; i < TASK_COUNT; ++i) {
		char output[128];
		int length = snprintf(output, sizeof(output),
			"build\\godot\\synthetic_%04u.windows.template_debug.x86_64.o", i);
		CHECK_STRESS(length > 0 && (size_t)length < sizeof(output));
		CHECK_STRESS(build_state_set(&state_arena, &state, test_path(bob, string_from_data(output, (u64)length)), dependency_paths, test_fingerprint(string_from_data(output, (u64)length))));
	}
	construction_finished = platform_counter();
	CHECK_STRESS(state.task_count == TASK_COUNT);
	CHECK_STRESS(state.paths.path_count == TASK_COUNT + DEPENDENCY_COUNT);
	CHECK_STRESS(build_state_save(path, bob, &state));
	save_finished = platform_counter();
	CHECK_STRESS(bob_platform_file_info(path, &info));
	CHECK_STRESS(build_state_load(&loaded_arena, bob, path, &loaded) ==
		BUILD_STATE_LOAD_OK);
	load_finished = platform_counter();
	CHECK_STRESS(loaded.task_count == TASK_COUNT);
	CHECK_STRESS(loaded.paths.path_count == TASK_COUNT + DEPENDENCY_COUNT);
	Build_State_Task_Snapshot task;
	CHECK_STRESS(build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build\\godot\\synthetic_0000.windows.template_debug.x86_64.o")), &task));
	CHECK_STRESS(build_state_get_task(&loaded, test_path(bob, STRING_LITERAL("build\\godot\\synthetic_1885.windows.template_debug.x86_64.o")), &task));

	printf("\n  build-state stress measurements\n");
	printf("    tasks: %u\n", (u32)TASK_COUNT);
	printf("    dependency references: %u\n",
		(u32)(TASK_COUNT * DEPENDENCY_COUNT));
	printf("    unique dependency paths: %u\n", (u32)DEPENDENCY_COUNT);
	printf("    path reuse: %.1fx\n", (double)TASK_COUNT);
	printf("    in-memory state: %.2f MiB (%.2f s to construct)\n",
		(double)state_arena.used / (double)MEGABYTES(1),
		(double)(construction_finished - construction_started) / (double)frequency);
	printf("    binary state stream: %.2f MiB (%.2f s to save)\n",
		(double)info.size / (double)MEGABYTES(1),
		(double)(save_finished - construction_finished) / (double)frequency);
	printf("    loaded state: %.2f MiB (%.2f s to load)\n",
		(double)loaded_arena.used / (double)MEGABYTES(1),
		(double)(load_finished - save_finished) / (double)frequency);
	printf("    unique interned paths: %u\n", state.paths.path_count);
	result = true;

cleanup:
	platform_remove_tree(root.data);
	build_state_destroy(&loaded);
	build_state_destroy(&state);
	arena_destroy(&loaded_arena);
	arena_destroy(&state_arena);
	arena_destroy(&source_arena);
	bob_build_destroy(bob);
#undef CHECK_STRESS
	return result;
}

static b32 test_make_depfile(void)
{
	Arena arena = arena_create(KILOBYTES(64));
	String_Array dependencies;
	u64 mark;

	CHECK(arena.data != NULL);
	{
		const char *expected[] = { "src/main.c", "include/main.h" };
		CHECK(make_depfile_parse(&arena,
			STRING_LITERAL("build/main.o: src/main.c include/main.h\n"),
			&dependencies));
		CHECK(string_array_matches(dependencies, expected, ARRAY_COUNT(expected)));
	}

	arena_reset(&arena);
	{
		const char *expected[] = {
			"C:\\src\\main.c",
			"C:\\Program Files\\SDK\\header.h",
		};
		CHECK(make_depfile_parse(&arena,
			STRING_LITERAL(
				"C:\\build\\main.obj: C:\\src\\main.c \\\r\n"
				"  C:\\Program\\ Files\\SDK\\header.h\r\n"),
			&dependencies));
		CHECK(string_array_matches(dependencies, expected, ARRAY_COUNT(expected)));
	}

	arena_reset(&arena);
	{
		const char *expected[] = {
			"path with spaces/header.h",
			"hash#header.h",
			"cash$money.h",
			"colon:name.h",
			"slash\\name.h",
		};
		CHECK(make_depfile_parse(&arena,
			STRING_LITERAL(
				"out.o: path\\ with\\ spaces/header.h hash\\#header.h "
				"cash$$money.h colon\\:name.h slash\\\\name.h\n"),
			&dependencies));
		CHECK(string_array_matches(dependencies, expected, ARRAY_COUNT(expected)));
	}

	arena_reset(&arena);
	{
		const char *expected[] = { "one.c", "common.h" };
		CHECK(make_depfile_parse(&arena,
			STRING_LITERAL(
				"one.o one.d: one.c common.h common.h # ignored.h\n"
				"one.c:\n"
				"common.h:\n"),
			&dependencies));
		CHECK(string_array_matches(dependencies, expected, ARRAY_COUNT(expected)));
	}

	arena_reset(&arena);
	CHECK(make_depfile_parse(&arena, (String){0}, &dependencies));
	CHECK(dependencies.count == 0);
	mark = arena_mark(&arena);
	CHECK(!make_depfile_parse(&arena,
		STRING_LITERAL("build/main.o src/main.c\n"), &dependencies));
	CHECK(arena_mark(&arena) == mark);
	CHECK(dependencies.count == 0);

	arena_destroy(&arena);
	return true;
}

static b32 run_tasks(Bob_Build *build, const Bob_Task_Desc *tasks, u32 task_count,
                     u32 worker_count)
{
    Bob *graph = bob_build_graph(build);
    u32 i;
    if (bob_task_count(build) != task_count) return false;
    for (i = 0; i < task_count; ++i) {
        if (bob_set_task(build, bob_node_at(graph, i), tasks[i]) != BOB_OK) {
            return false;
        }
    }
    return bob_build(build, (Bob_Build_Params){ .worker_count = worker_count });
}

static b32 test_arena_and_strings(void)
{
    Arena arena = arena_create(KILOBYTES(4));
    char *start;
    String built;
    String copy;
    void *aligned;
    u64 mark;
    Scratch outer;
    Scratch inner;

    CHECK(arena.data != NULL);
    start = arena_top(&arena);
    CHECK(arena_append_text(&arena, "hello") == start);
	CHECK(*(char *)arena_top(&arena) == 0);
    CHECK(arena_append_str(&arena, STRING_LITERAL(" arena")) != NULL);
	CHECK(*(char *)arena_top(&arena) == 0);
    CHECK(arena_appendf(&arena, " %d", 42) != NULL);
	CHECK(*(char *)arena_top(&arena) == 0);
	CHECK(arena_append_char(&arena, '!') != NULL);
	CHECK(*(char *)arena_top(&arena) == 0);
	built = arena_string_from(&arena, start);
	arena_finalize_string(&arena, built);
	CHECK(string_equal(built, STRING_LITERAL("hello arena 42!")));
    CHECK(built.data[built.size] == 0);

    copy = arena_push_string_copy(&arena, built);
    CHECK(string_equal(copy, built));
    CHECK(copy.data[copy.size] == 0);
    CHECK(string_equal(string_slice(copy, 6, 5), STRING_LITERAL("arena")));
	CHECK(string_is_terminated(STRING_LITERAL("hello")));
	CHECK(!string_is_terminated(string_slice(STRING_LITERAL("hello"), 0, 4)));
	CHECK(string_ends_with_insensitive(STRING_LITERAL("build.ELF"), STRING_LITERAL(".elf")));
	CHECK(!string_ends_with_insensitive(STRING_LITERAL("build.lua"), STRING_LITERAL(".elf")));
	{
		String_Array parts = string_split(&arena, STRING_LITERAL("a;;b;"), ';');
		CHECK(parts.count == 4);
		CHECK(string_equal(parts.items[0], STRING_LITERAL("a")));
		CHECK(parts.items[1].size == 0);
		CHECK(string_equal(parts.items[2], STRING_LITERAL("b")));
		CHECK(parts.items[3].size == 0);
	}
	{
		String_Array lines = string_split_lines(&arena, STRING_LITERAL("one\r\ntwo\n"));
		CHECK(lines.count == 3);
		CHECK(string_equal(lines.items[0], STRING_LITERAL("one")));
		CHECK(string_equal(lines.items[1], STRING_LITERAL("two")));
		CHECK(lines.items[2].size == 0);
	}
	{
		String_Array entries = string_split_block(&arena,
			(String){ .data = "one\0two\0", .size = 8 });
		CHECK(entries.count == 2);
		CHECK(string_equal(entries.items[0], STRING_LITERAL("one")));
		CHECK(string_equal(entries.items[1], STRING_LITERAL("two")));
	}
	{
		String left;
		String right;
		CHECK(string_split_first(STRING_LITERAL("NAME=a=b"), '=', &left, &right));
		CHECK(string_equal(left, STRING_LITERAL("NAME")));
		CHECK(string_equal(right, STRING_LITERAL("a=b")));
		CHECK(!string_split_first(STRING_LITERAL("NAME"), '=', &left, &right));
		CHECK(string_equal(
			string_trim_whitespace(STRING_LITERAL(" \t value \r\n")),
			STRING_LITERAL("value")));
	}

    CHECK(arena_push(&arena, 1) != NULL);
    aligned = arena_push_zero_aligned(&arena, 32, 32);
    CHECK(aligned != NULL);
    CHECK((uintptr_t)aligned % 32 == 0);
    CHECK(((u8 *)aligned)[0] == 0 && ((u8 *)aligned)[31] == 0);

    mark = arena_mark(&arena);
    CHECK(arena_push_zero(&arena, 128) != NULL);
    arena_restore(&arena, mark);
    CHECK(arena_mark(&arena) == mark);
    arena_destroy(&arena);

    outer = begin_scratch();
    CHECK(arena_append_text(outer.arena, "outer") != NULL);
    inner = begin_scratch();
	CHECK(inner.arena == outer.arena);
    CHECK(arena_append_text(inner.arena, "inner") != NULL);
    end_scratch(inner);
    CHECK(arena_mark(outer.arena) == inner.restore_used);
	{
		Scratch separate = begin_different_scratch(outer.arena);
		CHECK(separate.arena != outer.arena);
		CHECK(arena_append_text(separate.arena, "separate") != NULL);
		end_scratch(separate);
	}
    end_scratch(outer);
	CHECK(arena_mark(outer.arena) == outer.restore_used);
    destroy_global_scratch();
    return true;
}

typedef struct Scratch_Thread_Test {
    Arena *arena;
    HANDLE ready;
    HANDLE release;
} Scratch_Thread_Test;

static DWORD WINAPI scratch_thread_test_main(void *parameter)
{
    Scratch_Thread_Test *test = parameter;
    Scratch scratch = begin_scratch();
    arena_append_text(scratch.arena, "thread scratch");
    test->arena = scratch.arena;
    SetEvent(test->ready);
    WaitForSingleObject(test->release, INFINITE);
    end_scratch(scratch);
    destroy_global_scratch();
    return 0;
}

static b32 test_thread_local_scratch(void)
{
    Scratch main_scratch = begin_scratch();
    Scratch_Thread_Test tests[2] = {0};
    HANDLE threads[2] = {0};
    HANDLE ready[2] = {0};
    HANDLE release = CreateEventA(NULL, TRUE, FALSE, NULL);
    b32 passed = false;

    if (!release) goto cleanup;
    for (u32 i = 0; i < ARRAY_COUNT(tests); ++i)
    {
        tests[i].ready = CreateEventA(NULL, TRUE, FALSE, NULL);
        tests[i].release = release;
        ready[i] = tests[i].ready;
        if (!ready[i]) goto cleanup;
        threads[i] = CreateThread(NULL, 0, scratch_thread_test_main, tests + i, 0, NULL);
        if (!threads[i]) goto cleanup;
    }
    if (WaitForMultipleObjects(ARRAY_COUNT(ready), ready, TRUE, 5000) != WAIT_OBJECT_0) goto cleanup;
    passed = tests[0].arena && tests[1].arena &&
        tests[0].arena != tests[1].arena &&
        tests[0].arena != main_scratch.arena &&
        tests[1].arena != main_scratch.arena;

cleanup:
    if (release) SetEvent(release);
    for (u32 i = 0; i < ARRAY_COUNT(threads); ++i) {
        if (threads[i]) {
            WaitForSingleObject(threads[i], INFINITE);
            CloseHandle(threads[i]);
        }
        if (ready[i]) CloseHandle(ready[i]);
    }
    if (release) CloseHandle(release);
    end_scratch(main_scratch);
    destroy_global_scratch();
    return passed;
}

static b32 test_empty_graph(void)
{
    Bob *graph = bob_create();
    CHECK(graph != NULL);
    CHECK_OK(bob_prepare(graph));
    CHECK(bob_is_finished(graph));
    bob_destroy(graph);
    return true;
}

static b32 test_linear_graph(void)
{
    Bob *graph = bob_create();
    Bob_Node *compile = add_node(graph, "compile");
    Bob_Node *link = add_node(graph, "link");
    Bob_Node *node;

    CHECK_OK(bob_add_dependency(graph, link, compile));
    CHECK_OK(bob_prepare(graph));

    CHECK(bob_take_ready(graph, &node));
    CHECK(node == compile);
    CHECK(!bob_take_ready(graph, &node));
    CHECK_OK(bob_complete(graph, compile, true));

    CHECK(bob_take_ready(graph, &node));
    CHECK(node == link);
    CHECK_OK(bob_complete(graph, link, true));
    CHECK(bob_is_finished(graph));
    CHECK(!bob_has_failed(graph));

    bob_destroy(graph);
    return true;
}

static b32 test_parallel_fan_in(void)
{
    Bob *graph = bob_create();
    Bob_Node *a = add_node(graph, "a");
    Bob_Node *b = add_node(graph, "b");
    Bob_Node *link = add_node(graph, "link");
    Bob_Node *first;
    Bob_Node *second;
    Bob_Node *node;

    CHECK_OK(bob_add_dependency(graph, link, a));
    CHECK_OK(bob_add_dependency(graph, link, b));
    CHECK_OK(bob_prepare(graph));

    CHECK(bob_take_ready(graph, &first));
    CHECK(bob_take_ready(graph, &second));
    CHECK(first != second);
    CHECK(!bob_take_ready(graph, &node));

    /* Finishing either node first must not release link early. */
    CHECK_OK(bob_complete(graph, second, true));
    CHECK(!bob_take_ready(graph, &node));
    CHECK_OK(bob_complete(graph, first, true));
    CHECK(bob_take_ready(graph, &node));
    CHECK(node == link);

    CHECK_OK(bob_complete(graph, link, true));
    CHECK(bob_is_finished(graph));
    bob_destroy(graph);
    return true;
}

static b32 test_failure_blocks_dependents(void)
{
    Bob *graph = bob_create();
    Bob_Node *compile = add_node(graph, "compile");
    Bob_Node *link = add_node(graph, "link");
    Bob_Node *package = add_node(graph, "package");
    Bob_Node *independent = add_node(graph, "independent");
    Bob_Node *first;
    Bob_Node *second;

    CHECK_OK(bob_add_dependency(graph, link, compile));
    CHECK_OK(bob_add_dependency(graph, package, link));
    CHECK_OK(bob_prepare(graph));

    CHECK(bob_take_ready(graph, &first));
    CHECK(bob_take_ready(graph, &second));
    CHECK((first == compile && second == independent) ||
          (first == independent && second == compile));

    CHECK_OK(bob_complete(graph, compile, false));
    CHECK(bob_task_state(link) == BOB_NODE_BLOCKED);
    CHECK(bob_task_state(package) == BOB_NODE_BLOCKED);
    CHECK(!bob_is_finished(graph));

    CHECK_OK(bob_complete(graph, independent, true));
    CHECK(bob_is_finished(graph));
    CHECK(bob_has_failed(graph));
    bob_destroy(graph);
    return true;
}

static b32 test_cycle_is_rejected(void)
{
    Bob *graph = bob_create();
    Bob_Node *a = add_node(graph, "a");
    Bob_Node *b = add_node(graph, "b");
    Bob_Node *c = add_node(graph, "c");

    CHECK_OK(bob_add_dependency(graph, a, b));
    CHECK_OK(bob_add_dependency(graph, b, c));
    CHECK_OK(bob_add_dependency(graph, c, a));
    CHECK(bob_prepare(graph) == BOB_ERROR_CYCLE);
    bob_destroy(graph);
    return true;
}

static b32 test_invalid_edges_are_rejected(void)
{
    Bob *graph = bob_create();
    Bob_Node *a = add_node(graph, "a");
    Bob_Node *b = add_node(graph, "b");

    CHECK(bob_add_dependency(graph, a, a) == BOB_ERROR_SELF_DEPENDENCY);
    CHECK_OK(bob_add_dependency(graph, a, b));
    CHECK(bob_add_dependency(graph, a, b) == BOB_ERROR_DUPLICATE_DEPENDENCY);
    bob_destroy(graph);
    return true;
}

typedef struct Generic_Execution_Test Generic_Execution_Test;

typedef struct Generic_Action_Test
{
	Generic_Execution_Test *execution;
	u32                     calls;
	i32                     value;
	b32                     changed;
	b32                     valid;
}
Generic_Action_Test;

struct Generic_Execution_Test
{
	u32 completed;
	b32 valid;
};

static Bob_Node_Result generic_test_action(Bob_Node_Context *context, void *user_data)
{
	Generic_Action_Test *action = user_data;
	i32 *output = arena_push_zero_aligned(context->arena, sizeof(*output), _Alignof(i32));
	i32 value = action->value;
	action->valid = context->bob && context->node && output &&
		context->execution_data == action->execution &&
		bob_node_user_data(context->node) == action;
	for (u32 i = 0; i < bob_dependency_count(context->node); ++i) {
		Bob_Node_Result dependency = bob_node_result(bob_dependency(context->node, i));
		if (!dependency.succeeded || !dependency.output) action->valid = false;
		else value += *(i32 *)dependency.output;
	}
	++action->calls;
	if (output) *output = value;
	return (Bob_Node_Result){
		.output = output,
		.succeeded = action->valid,
		.changed = action->changed,
	};
}

static void generic_test_completed(Bob_Node *node, Bob_Node_Result result, void *user_data)
{
	Generic_Execution_Test *execution = user_data;
	if (!node || !result.succeeded || !result.output) execution->valid = false;
	++execution->completed;
}

static Bob_Node_Result generic_test_failure(Bob_Node_Context *context, void *user_data)
{
	u32 *calls = user_data;
	(void)context;
	++*calls;
	return (Bob_Node_Result){ .succeeded = false, .changed = true };
}

static b32 test_generic_graph_actions(void)
{
	Bob *graph = bob_create();
	Bob_Node *left = NULL;
	Bob_Node *right = NULL;
	Bob_Node *sum = NULL;
	Generic_Execution_Test execution = { .valid = true };
	Generic_Action_Test actions[] = {
		{ .execution = &execution, .value = 3, .changed = true },
		{ .execution = &execution, .value = 5, .changed = false },
		{ .execution = &execution, .value = 1, .changed = true },
	};
	Bob_Node_Result result;
	i32 *graph_value;
	String graph_string;

	CHECK(graph != NULL);
	graph_value = bob_allocate(graph, sizeof(*graph_value), _Alignof(i32));
	graph_string = bob_copy_string(graph, STRING_LITERAL("graph storage"));
	CHECK(graph_value != NULL && graph_string.data != NULL);
	*graph_value = 17;
	CHECK(*graph_value == 17 && string_equal(graph_string, STRING_LITERAL("graph storage")));
	CHECK_OK(bob_add_node(graph, (Bob_Node_Desc){
		.name = STRING_LITERAL("left"),
		.function = generic_test_action,
		.user_data = actions + 0,
	}, &left));
	CHECK_OK(bob_add_node(graph, (Bob_Node_Desc){
		.name = STRING_LITERAL("right"),
		.function = generic_test_action,
		.user_data = actions + 1,
	}, &right));
	CHECK_OK(bob_add_node(graph, (Bob_Node_Desc){
		.name = STRING_LITERAL("sum"),
		.function = generic_test_action,
		.user_data = actions + 2,
	}, &sum));
	CHECK_OK(bob_add_dependency(graph, sum, left));
	CHECK_OK(bob_add_dependency(graph, sum, right));
	CHECK(bob_execute(graph, (Bob_Exec_Params){
		.worker_count = 2,
		.user_data = &execution,
		.completed = generic_test_completed,
	}));
	CHECK(execution.valid && execution.completed == 3);
	CHECK(actions[0].valid && actions[0].calls == 1);
	CHECK(actions[1].valid && actions[1].calls == 1);
	CHECK(actions[2].valid && actions[2].calls == 1);
	CHECK(bob_node_state(sum) == BOB_NODE_SUCCEEDED);
	result = bob_node_result(sum);
	CHECK(result.succeeded && result.changed && result.output);
	CHECK(*(i32 *)result.output == 9);
	CHECK(!bob_node_result(right).changed);
	CHECK(bob_allocate(graph, 1, 1) == NULL);
	CHECK(bob_copy_string(graph, STRING_LITERAL("too late")).data == NULL);
	bob_destroy(graph);

	{
		u32 failed_calls = 0;
		u32 blocked_calls = 0;
		graph = bob_create();
		CHECK(graph != NULL);
		CHECK_OK(bob_add_node(graph, (Bob_Node_Desc){
			.name = STRING_LITERAL("failure"),
			.function = generic_test_failure,
			.user_data = &failed_calls,
		}, &left));
		CHECK_OK(bob_add_node(graph, (Bob_Node_Desc){
			.name = STRING_LITERAL("blocked"),
			.function = generic_test_failure,
			.user_data = &blocked_calls,
		}, &right));
		CHECK_OK(bob_add_dependency(graph, right, left));
		CHECK(!bob_execute(graph, (Bob_Exec_Params){ .worker_count = 2 }));
		CHECK(failed_calls == 1 && blocked_calls == 0);
		CHECK(bob_node_state(left) == BOB_NODE_FAILED);
		CHECK(bob_node_state(right) == BOB_NODE_BLOCKED);
		CHECK(!bob_node_result(left).changed);
		bob_destroy(graph);
	}
	return true;
}

static b32 get_test_executable(char *buffer, u32 buffer_size)
{
    DWORD length = GetModuleFileNameA(NULL, buffer, buffer_size);
    return length > 0 && length < buffer_size;
}

static b32 test_builder_runs_in_parallel(void)
{
    Bob_Build *build = bob_build_create();
    Bob *graph = bob_build_graph(build);
    Bob_Node *a = add_node(graph, "slow a");
    Bob_Node *b = add_node(graph, "slow b");
    Bob_Node *link = add_node(graph, "link");
    Bob_Task_Desc tasks[3] = {0};
    char executable[MAX_PATH];
    char command_a[2 * MAX_PATH];
    char command_b[2 * MAX_PATH];
    char command_link[2 * MAX_PATH];
    char event_a_name[128];
    char event_b_name[128];
    HANDLE event_a;
    HANDLE event_b;
    b32 executed;

    CHECK(get_test_executable(executable, sizeof(executable)));
    CHECK(snprintf(event_a_name, sizeof(event_a_name), "Local\\bob_graph_%lu_a",
                   GetCurrentProcessId()) > 0);
    CHECK(snprintf(event_b_name, sizeof(event_b_name), "Local\\bob_graph_%lu_b",
                   GetCurrentProcessId()) > 0);
    event_a = CreateEventA(NULL, TRUE, FALSE, event_a_name);
    event_b = CreateEventA(NULL, TRUE, FALSE, event_b_name);
    CHECK(event_a != NULL && event_b != NULL);

    CHECK(snprintf(command_a, sizeof(command_a), "\"%s\" --barrier %s %s a", executable, event_a_name, event_b_name) > 0);
    CHECK(snprintf(command_b, sizeof(command_b), "\"%s\" --barrier %s %s b", executable, event_b_name, event_a_name) > 0);
    CHECK(snprintf(command_link, sizeof(command_link), "\"%s\" --child 0 0 link", executable) > 0);

    tasks[0].command_line = string_from_cstring(command_a);
    tasks[1].command_line = string_from_cstring(command_b);
    tasks[2].command_line = string_from_cstring(command_link);

    CHECK_OK(bob_add_dependency(graph, link, a));
    CHECK_OK(bob_add_dependency(graph, link, b));

    executed = run_tasks(build, tasks, 3, 2);
    CloseHandle(event_a);
    CloseHandle(event_b);

    CHECK(executed);
    CHECK(bob_is_finished(graph));
    bob_build_destroy(build);
    return true;
}

static b32 test_builder_propagates_failure(void)
{
    Bob_Build *build = bob_build_create();
    Bob *graph = bob_build_graph(build);
    Bob_Node *fail = add_node(graph, "fail");
    Bob_Node *blocked = add_node(graph, "blocked");
    Bob_Node *independent = add_node(graph, "independent");
    Bob_Task_Desc tasks[3] = {0};
    char executable[MAX_PATH];
    char fail_command[2 * MAX_PATH];
    char blocked_command[2 * MAX_PATH];
    char independent_command[2 * MAX_PATH];

    CHECK(get_test_executable(executable, sizeof(executable)));
    CHECK(snprintf(fail_command, sizeof(fail_command), "\"%s\" --child 0 1 fail", executable) > 0);
    CHECK(snprintf(blocked_command, sizeof(blocked_command), "\"%s\" --child 0 0 blocked", executable) > 0);
    CHECK(snprintf(independent_command, sizeof(independent_command), "\"%s\" --child 0 0 independent", executable) > 0);

    tasks[0].command_line = string_from_cstring(fail_command);
    tasks[1].command_line = string_from_cstring(blocked_command);
    tasks[2].command_line = string_from_cstring(independent_command);
    CHECK_OK(bob_add_dependency(graph, blocked, fail));

    CHECK(!run_tasks(build, tasks, 3, 2));
    CHECK(bob_task_state(fail) == BOB_NODE_FAILED);
    CHECK(bob_task_state(blocked) == BOB_NODE_BLOCKED);
    CHECK(bob_task_state(independent) == BOB_NODE_SUCCEEDED);
    CHECK(bob_is_finished(graph));
    bob_build_destroy(build);
    return true;
}

static b32 test_builder_reports_missing_executable(void)
{
    Bob_Build *build = bob_build_create();
    Bob *graph = bob_build_graph(build);
    Bob_Node *missing = add_node(graph, "missing executable");
    Bob_Task_Desc task = {
        .command_line = STRING_LITERAL("bob_executable_that_does_not_exist_7f31.exe --input x.c")
    };

    CHECK(!run_tasks(build, &task, 1, 1));
    CHECK(bob_task_state(missing) == BOB_NODE_FAILED);
    bob_build_destroy(build);
    return true;
}

static b32 test_builder_skips_existing_output(void)
{
    const char *output_path = "build\\incremental_test.out";
    String outputs[] = { string_from_cstring(output_path) };
    Bob_Build *first_build;
    Bob_Build *second_build;
    Bob *first_graph;
    Bob *second_graph;
    Bob_Task_Desc task = {0};
    Bob_Platform_File_Info info;

    DeleteFileA(output_path);
    task.command_line = STRING_LITERAL("cmd /c echo built>build\\incremental_test.out");
    task.outputs = STRING_ARRAY_FROM(outputs);

    first_build = bob_build_create();
    first_graph = bob_build_graph(first_build);
    add_node(first_graph, "create output");
    CHECK(run_tasks(first_build, &task, 1, 1));
	CHECK(bob_platform_file_info(string_from_cstring(output_path), &info));
    bob_build_destroy(first_build);

    second_build = bob_build_create();
    second_graph = bob_build_graph(second_build);
    add_node(second_graph, "skip existing output");
    CHECK(run_tasks(second_build, &task, 1, 1));
    CHECK(bob_task_state(bob_node_at(second_graph, 0)) == BOB_NODE_SUCCEEDED);
    bob_build_destroy(second_build);

    CHECK(DeleteFileA(output_path));
	return true;
}

static b32 test_directory_output_stays_clean(void)
{
	const char *directory = "build\\directory_output_test";
	const char *child = "build\\directory_output_test\\child.txt";
	String directory_outputs[] = { STRING_LITERAL("build/directory_output_test") };
	String child_outputs[] = { STRING_LITERAL("build/directory_output_test/child.txt") };
	Bob_Task_Desc tasks[2] = {
		{
			.command_line = STRING_LITERAL("cmd /c if not exist build\\directory_output_test mkdir build\\directory_output_test"),
			.outputs = STRING_ARRAY_FROM(directory_outputs),
		},
		{
			.command_line = STRING_LITERAL("cmd /c echo child>build\\directory_output_test\\child.txt"),
			.outputs = STRING_ARRAY_FROM(child_outputs),
		},
	};
	Bob_Platform_File_Info before;
	Bob_Platform_File_Info after;
	Bob_Build *build;
	Bob *graph;
	Bob_Node *prepare;
	Bob_Node *write_child;

	CHECK(platform_remove_tree(directory));
	build = bob_build_create();
	graph = bob_build_graph(build);
	CHECK(graph != NULL);
	prepare = add_node(graph, "prepare output directory");
	write_child = add_node(graph, "write child output");
	CHECK_OK(bob_add_dependency(graph, write_child, prepare));
	CHECK(run_tasks(build, tasks, 2, 1));
	bob_build_destroy(build);
	CHECK(bob_platform_file_info(string_from_cstring(child), &before));

	Sleep(20);
	build = bob_build_create();
	graph = bob_build_graph(build);
	CHECK(graph != NULL);
	prepare = add_node(graph, "prepare output directory");
	write_child = add_node(graph, "write child output");
	CHECK_OK(bob_add_dependency(graph, write_child, prepare));
	CHECK(run_tasks(build, tasks, 2, 1));
	bob_build_destroy(build);
	CHECK(bob_platform_file_info(string_from_cstring(child), &after));
	CHECK(after.modified_unix_ms == before.modified_unix_ms);
	CHECK(platform_remove_tree(directory));
	return true;
}

static b32 test_task_fingerprint_rebuilds(void)
{
	const char *output_path = "build\\fingerprint_test.out";
	String outputs[] = { STRING_LITERAL("build/fingerprint_test.out") };
	String first_includes[] = { STRING_LITERAL("include/first") };
	String second_includes[] = { STRING_LITERAL("include/second") };
	Bob_Task_Desc task = {
		.command_line = STRING_LITERAL("cmd /c echo first>build\\fingerprint_test.out"),
		.outputs = STRING_ARRAY_FROM(outputs),
		.include_directories = STRING_ARRAY_FROM(first_includes),
	};
	Bob_Platform_File_Info first;
	Bob_Platform_File_Info unchanged;
	Bob_Platform_File_Info command_changed;
	Bob_Platform_File_Info metadata_changed;
	Bob_Build *build;
	Bob *graph;

	CHECK(platform_remove_file(output_path));
	build = bob_build_create();
	graph = bob_build_graph(build);
	CHECK(graph != NULL);
	add_node(graph, "initial fingerprint");
	CHECK(run_tasks(build, &task, 1, 1));
	bob_build_destroy(build);
	CHECK(bob_platform_file_info(string_from_cstring(output_path), &first));

	Sleep(20);
	build = bob_build_create();
	graph = bob_build_graph(build);
	CHECK(graph != NULL);
	add_node(graph, "unchanged fingerprint");
	CHECK(run_tasks(build, &task, 1, 1));
	bob_build_destroy(build);
	CHECK(bob_platform_file_info(string_from_cstring(output_path), &unchanged));
	CHECK(unchanged.modified_unix_ms == first.modified_unix_ms);

	Sleep(20);
	task.command_line = STRING_LITERAL("cmd /c echo second>build\\fingerprint_test.out");
	build = bob_build_create();
	graph = bob_build_graph(build);
	CHECK(graph != NULL);
	add_node(graph, "changed command fingerprint");
	CHECK(run_tasks(build, &task, 1, 1));
	bob_build_destroy(build);
	CHECK(bob_platform_file_info(string_from_cstring(output_path), &command_changed));
	CHECK(command_changed.modified_unix_ms != unchanged.modified_unix_ms);

	Sleep(20);
	task.include_directories = STRING_ARRAY_FROM(second_includes);
	build = bob_build_create();
	graph = bob_build_graph(build);
	CHECK(graph != NULL);
	add_node(graph, "changed metadata fingerprint");
	CHECK(run_tasks(build, &task, 1, 1));
	bob_build_destroy(build);
	CHECK(bob_platform_file_info(string_from_cstring(output_path), &metadata_changed));
	CHECK(metadata_changed.modified_unix_ms != command_changed.modified_unix_ms);

	CHECK(platform_remove_file(output_path));
	return true;
}

static b32 write_test_file_at_time(const char *path, u64 time)
{
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    ULARGE_INTEGER value;
    FILETIME file_time;
    b32 succeeded;

    if (file == INVALID_HANDLE_VALUE) return false;
    value.QuadPart = 116444736000000000ULL + time * 10000ULL;
    file_time.dwLowDateTime = value.LowPart;
    file_time.dwHighDateTime = value.HighPart;
    succeeded = SetFileTime(file, NULL, NULL, &file_time);
    CloseHandle(file);
    return succeeded;
}

static b32 write_test_text_at_time(const char *path, const char *text, u64 time)
{
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    size_t text_size = strlen(text);
    DWORD written = 0;
    ULARGE_INTEGER value;
    FILETIME file_time;
    b32 succeeded;

    if (file == INVALID_HANDLE_VALUE || text_size > UINT32_MAX) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        return false;
    }
    value.QuadPart = 116444736000000000ULL + time * 10000ULL;
    file_time.dwLowDateTime = value.LowPart;
    file_time.dwHighDateTime = value.HighPart;
    succeeded = WriteFile(file, text, (DWORD)text_size, &written, NULL) &&
                written == (DWORD)text_size &&
                SetFileTime(file, NULL, NULL, &file_time);
    CloseHandle(file);
    return succeeded;
}

static b32 test_newer_input_rebuilds(void)
{
	const char *input_path = "build\\newer_input_test.in";
	const char *output_path = "build\\newer_input_test.out";
    String inputs[] = { string_from_cstring(input_path) };
    String outputs[] = { string_from_cstring(output_path) };
	Bob_Task_Desc task = {0};
	Bob_Build *clean_build;
	Bob_Build *dirty_build;
	Bob *clean_graph;
	Bob *dirty_graph;
	Bob_Platform_File_Info output_info;

	CHECK(write_test_file_at_time(input_path, 0ULL));
	CHECK(platform_remove_file(output_path));
	task.command_line = STRING_LITERAL("cmd /c echo rebuilt>build\\newer_input_test.out");
    task.inputs = STRING_ARRAY_FROM(inputs);
    task.outputs = STRING_ARRAY_FROM(outputs);

	clean_build = bob_build_create();
    clean_graph = bob_build_graph(clean_build);
	add_node(clean_graph, "prime timestamp state");
	CHECK(run_tasks(clean_build, &task, 1, 1));
	bob_build_destroy(clean_build);

	CHECK(bob_platform_file_info(string_from_cstring(output_path), &output_info));
	CHECK(write_test_file_at_time(input_path, (u64)output_info.modified_unix_ms + 1000));
	dirty_build = bob_build_create();
	dirty_graph = bob_build_graph(dirty_build);
    add_node(dirty_graph, "dirty timestamps");
    CHECK(run_tasks(dirty_build, &task, 1, 1));
    bob_build_destroy(dirty_build);

    CHECK(DeleteFileA(input_path));
    CHECK(DeleteFileA(output_path));
    return true;
}

static b32 test_multiple_inputs_and_outputs(void)
{
    const char *input_a = "build\\multi_a.in";
    const char *input_b = "build\\multi_b.in";
    const char *output_a = "build\\multi_a.out";
    const char *output_b = "build\\multi_b.out";
    const char *marker = "build\\multi.marker";
    String inputs[] = { string_from_cstring(input_a), string_from_cstring(input_b) };
    String outputs[] = { string_from_cstring(output_a), string_from_cstring(output_b) };
    Bob_Task_Desc task = {0};
    Bob_Build *build;
    Bob *graph;
    Bob_Platform_File_Info info;

    DeleteFileA(marker);
    CHECK(write_test_file_at_time(input_a, 100ULL));
    CHECK(write_test_file_at_time(input_b, 150ULL));
	CHECK(platform_remove_file(output_a));
	CHECK(platform_remove_file(output_b));
	task.command_line = STRING_LITERAL("cmd /c echo a>build\\multi_a.out && echo b>build\\multi_b.out && echo rebuilt>build\\multi.marker");
    task.inputs = STRING_ARRAY_FROM(inputs);
    task.outputs = STRING_ARRAY_FROM(outputs);

	build = bob_build_create();
    graph = bob_build_graph(build);
	add_node(graph, "prime multiple files");
	CHECK(run_tasks(build, &task, 1, 1));
	bob_build_destroy(build);
	CHECK(DeleteFileA(marker));

	build = bob_build_create();
	graph = bob_build_graph(build);
	add_node(graph, "clean multiple files");
	CHECK(run_tasks(build, &task, 1, 1));
	CHECK(!bob_platform_file_info(string_from_cstring(marker), &info));
	bob_build_destroy(build);

	CHECK(bob_platform_file_info(string_from_cstring(output_a), &info));
	CHECK(write_test_file_at_time(input_b, (u64)info.modified_unix_ms + 1000));
	build = bob_build_create();
    graph = bob_build_graph(build);
    add_node(graph, "newest input wins");
	CHECK(run_tasks(build, &task, 1, 1));
	CHECK(bob_platform_file_info(string_from_cstring(marker), &info));
    bob_build_destroy(build);

    CHECK(DeleteFileA(output_b));
    CHECK(DeleteFileA(marker));
	build = bob_build_create();
    graph = bob_build_graph(build);
    add_node(graph, "one output missing");
	CHECK(run_tasks(build, &task, 1, 1));
	CHECK(bob_platform_file_info(string_from_cstring(output_b), &info));
	CHECK(bob_platform_file_info(string_from_cstring(marker), &info));
    bob_build_destroy(build);

    CHECK(DeleteFileA(input_a));
    CHECK(DeleteFileA(input_b));
    CHECK(DeleteFileA(output_a));
    CHECK(DeleteFileA(output_b));
    CHECK(DeleteFileA(marker));
    return true;
}

static b32 test_dependency_rebuild_propagates(void)
{
    const char *dependency_input = "build\\dependency.in";
    const char *dependency_output = "build\\dependency.out";
    const char *parent_output = "build\\parent.out";
    const char *marker = "build\\parent.marker";
    String dependency_inputs[] = { string_from_cstring(dependency_input) };
    String dependency_outputs[] = { string_from_cstring(dependency_output) };
    String parent_outputs[] = { string_from_cstring(parent_output) };
    Bob_Task_Desc tasks[2] = {0};
    Bob_Build *build;
    Bob *graph;
    Bob_Node *dependency;
    Bob_Node *parent;
    Bob_Platform_File_Info info;

    DeleteFileA(marker);
    CHECK(write_test_file_at_time(dependency_input, 100ULL));
	CHECK(platform_remove_file(dependency_output));
	CHECK(platform_remove_file(parent_output));

	tasks[0].command_line = STRING_LITERAL("cmd /c echo dependency>build\\dependency.out");
    tasks[0].inputs = STRING_ARRAY_FROM(dependency_inputs);
    tasks[0].outputs = STRING_ARRAY_FROM(dependency_outputs);
	tasks[1].command_line = STRING_LITERAL("cmd /c echo parent>build\\parent.out && echo rebuilt>build\\parent.marker");
    tasks[1].outputs = STRING_ARRAY_FROM(parent_outputs);

	build = bob_build_create();
    graph = bob_build_graph(build);
	dependency = add_node(graph, "prime dependency");
	parent = add_node(graph, "prime parent");
    CHECK_OK(bob_add_dependency(graph, parent, dependency));
	CHECK(run_tasks(build, tasks, 2, 1));
	bob_build_destroy(build);
	CHECK(DeleteFileA(marker));

	CHECK(bob_platform_file_info(string_from_cstring(dependency_output), &info));
	CHECK(write_test_file_at_time(dependency_input, (u64)info.modified_unix_ms + 1000));
	build = bob_build_create();
    graph = bob_build_graph(build);
    dependency = add_node(graph, "dirty dependency");
    parent = add_node(graph, "propagated parent");
    CHECK_OK(bob_add_dependency(graph, parent, dependency));
	CHECK(run_tasks(build, tasks, 2, 1));
	CHECK(bob_platform_file_info(string_from_cstring(marker), &info));
    bob_build_destroy(build);

    CHECK(DeleteFileA(dependency_input));
    CHECK(DeleteFileA(dependency_output));
    CHECK(DeleteFileA(parent_output));
    CHECK(DeleteFileA(marker));
    return true;
}

static b32 test_transparent_dependency(void)
{
	const char *parent_output = "build\\transparent_parent.out";
	String parent_outputs[] = { string_from_cstring(parent_output) };
	Bob_Task_Desc tasks[2] = {0};
	CHECK(platform_remove_file(parent_output));
	tasks[0].command_line = STRING_LITERAL("cmd /c exit /b 0");
	tasks[0].transparent = true;
	tasks[1].command_line = STRING_LITERAL("cmd /c echo parent>build\\transparent_parent.out");
	tasks[1].outputs = STRING_ARRAY_FROM(parent_outputs);
	Bob_Build *build = bob_build_create();
	Bob *graph = bob_build_graph(build);
	Bob_Node *dependency = add_node(graph, "prime transparent dependency");
	Bob_Node *parent = add_node(graph, "prime transparent parent");
	CHECK_OK(bob_add_dependency(graph, parent, dependency));
	CHECK(run_tasks(build, tasks, 2, 1));
	bob_build_destroy(build);
	Bob_Platform_File_Info before;
	Bob_Platform_File_Info after;
	CHECK(bob_platform_file_info(string_from_cstring(parent_output), &before));
	Sleep(20);
	build = bob_build_create();
	graph = bob_build_graph(build);
	dependency = add_node(graph, "transparent dependency");
	parent = add_node(graph, "clean transparent parent");
	CHECK_OK(bob_add_dependency(graph, parent, dependency));
	CHECK(run_tasks(build, tasks, 2, 1));
	bob_build_destroy(build);
	CHECK(bob_platform_file_info(string_from_cstring(parent_output), &after));
	CHECK(after.modified_unix_ms == before.modified_unix_ms);
	CHECK(DeleteFileA(parent_output));
	return true;
}

static b32 test_task_working_directory(void)
{
	const char *directory = "build\\task_working_directory";
	const char *resolved_output =
		"build\\task_working_directory\\result.out";
	const char *unresolved_output = "result.out";
	String outputs[] = { STRING_LITERAL("result.out") };
	Bob_Task_Desc task = {
		.command_line = STRING_LITERAL("cmd /c echo built>result.out"),
		.working_directory = STRING_LITERAL("build\\task_working_directory"),
		.outputs = STRING_ARRAY_FROM(outputs),
	};
	Bob_Build *build;
	Bob *graph;
	Bob_Platform_File_Info info;

	DeleteFileA(resolved_output);
	DeleteFileA(unresolved_output);
	if (!CreateDirectoryA(directory, NULL) &&
		GetLastError() != ERROR_ALREADY_EXISTS) return false;
	build = bob_build_create();
	graph = bob_build_graph(build);
	add_node(graph, "working directory output");
	CHECK(run_tasks(build, &task, 1, 1));
	CHECK(string_equal(bob_get_task_desc(bob_node_at(graph, 0))->working_directory,
		STRING_LITERAL("build\\task_working_directory")));
	bob_build_destroy(build);
	CHECK(bob_platform_file_info(string_from_cstring(resolved_output), &info));
	CHECK(!bob_platform_file_info(string_from_cstring(unresolved_output), &info));

	build = bob_build_create();
	graph = bob_build_graph(build);
	add_node(graph, "working directory incremental output");
	CHECK(run_tasks(build, &task, 1, 1));
	bob_build_destroy(build);

	CHECK(DeleteFileA(resolved_output));
	CHECK(RemoveDirectoryA(directory));
	return true;
}

static b32 run_single_task(const Bob_Task_Desc *task, const char *name)
{
	Bob_Build *build = bob_build_create();
	Bob *graph = bob_build_graph(build);
	b32 result;
	if (!graph) return false;
	add_node(graph, name);
	result = run_tasks(build, task, 1, 1);
	bob_build_destroy(build);
	return result;
}

static b32 test_compiler_dependency_state(void)
{
	static const char malformed[] = "{ version = 1, tasks = 7 }";
	Arena arena = arena_create(KILOBYTES(64));
	Arena state_arena = arena_create(KILOBYTES(64));
	Bob_Build *state_bob = NULL;
	String original_directory = {0};
	String absolute_source = {0};
	String absolute_header = {0};
	String absolute_output = {0};
	String inputs[] = { STRING_LITERAL("source.c") };
	String outputs[] = { STRING_LITERAL("object.obj") };
	Bob_Task_Desc task = {
		.command_line = STRING_LITERAL("clang-cl /nologo /c source.c /Foobject.obj"),
		.working_directory = STRING_LITERAL("work"),
		.inputs = STRING_ARRAY_FROM(inputs),
		.outputs = STRING_ARRAY_FROM(outputs),
	};
	Build_State state = {0};
	Build_State_Task_Snapshot state_task;
	Bob_Platform_File_Info before;
	Bob_Platform_File_Info after;
	b32 changed_directory = false;
	b32 result = false;

#define CHECK_DEPENDENCY_STATE(condition)                                      \
	do {                                                                         \
		if (!(condition)) {                                                        \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
			goto cleanup;                                                           \
		}                                                                          \
	} while (0)

	CHECK_DEPENDENCY_STATE(arena.data && state_arena.data);
	CHECK_DEPENDENCY_STATE(build_state_init(&state));
	CHECK_DEPENDENCY_STATE(bob_platform_current_directory(&arena, &original_directory));
	CHECK_DEPENDENCY_STATE(platform_remove_tree("build\\compiler_dependency_state"));
	CHECK_DEPENDENCY_STATE(platform_create_directories("build\\compiler_dependency_state"));
	CHECK_DEPENDENCY_STATE(platform_set_current_directory("build\\compiler_dependency_state"));
	changed_directory = true;
	CHECK_DEPENDENCY_STATE(platform_create_directories("work"));
	CHECK_DEPENDENCY_STATE(write_test_text_at_time("work\\header.h", "#define VALUE 1\n", 100ULL));
	CHECK_DEPENDENCY_STATE(write_test_text_at_time("work\\source.c",
		"#include \"header.h\"\nint dependency_value = VALUE;\n", 100ULL));
	CHECK_DEPENDENCY_STATE(bob_platform_absolute_path(&arena,
		STRING_LITERAL("work/source.c"), &absolute_source));
	CHECK_DEPENDENCY_STATE(bob_platform_absolute_path(&arena,
		STRING_LITERAL("work/header.h"), &absolute_header));
	CHECK_DEPENDENCY_STATE(bob_platform_absolute_path(&arena,
		STRING_LITERAL("work/object.obj"), &absolute_output));
	state_bob = bob_build_create();
	CHECK_DEPENDENCY_STATE(state_bob != NULL);

	CHECK_DEPENDENCY_STATE(run_single_task(&task, "capture compiler dependencies"));
	CHECK_DEPENDENCY_STATE(build_state_load(&state_arena, state_bob,
		STRING_LITERAL(".bob/state"), &state) == BUILD_STATE_LOAD_OK);
	CHECK_DEPENDENCY_STATE(build_state_get_task(&state, test_path(state_bob, absolute_output), &state_task));
	CHECK_DEPENDENCY_STATE(state_task_contains_path(state_bob, &state_task,
		absolute_source));
	CHECK_DEPENDENCY_STATE(state_task_contains_path(state_bob, &state_task,
		absolute_header));
	CHECK_DEPENDENCY_STATE(bob_platform_file_info(absolute_output, &before));

	Sleep(20);
	CHECK_DEPENDENCY_STATE(run_single_task(&task, "reuse compiler dependencies"));
	CHECK_DEPENDENCY_STATE(bob_platform_file_info(absolute_output, &after));
	CHECK_DEPENDENCY_STATE(after.modified_unix_ms == before.modified_unix_ms);

	CHECK_DEPENDENCY_STATE(write_test_text_at_time("work\\header.h", "#define VALUE 2\n",
		(u64)after.modified_unix_ms + 1000));
	Sleep(20);
	CHECK_DEPENDENCY_STATE(run_single_task(&task, "rebuild changed compiler dependency"));
	CHECK_DEPENDENCY_STATE(bob_platform_file_info(absolute_output, &before));
	CHECK_DEPENDENCY_STATE(before.modified_unix_ms != after.modified_unix_ms);

	CHECK_DEPENDENCY_STATE(write_test_text_at_time("work\\header.h", "#define VALUE 3\n", 100ULL));
	CHECK_DEPENDENCY_STATE(platform_remove_file(".bob\\state"));
	Sleep(20);
	CHECK_DEPENDENCY_STATE(run_single_task(&task, "rebuild missing compiler state"));
	CHECK_DEPENDENCY_STATE(bob_platform_file_info(absolute_output, &after));
	CHECK_DEPENDENCY_STATE(after.modified_unix_ms != before.modified_unix_ms);

	CHECK_DEPENDENCY_STATE(bob_platform_write_entire_file(STRING_LITERAL(".bob/state"),
		malformed, sizeof(malformed) - 1));
	Sleep(20);
	CHECK_DEPENDENCY_STATE(run_single_task(&task, "rebuild malformed compiler state"));
	CHECK_DEPENDENCY_STATE(bob_platform_file_info(absolute_output, &before));
	CHECK_DEPENDENCY_STATE(before.modified_unix_ms != after.modified_unix_ms);

	CHECK_DEPENDENCY_STATE(platform_remove_file("work\\header.h"));
	CHECK_DEPENDENCY_STATE(!run_single_task(&task, "rebuild missing compiler dependency"));
	arena_reset(&state_arena);
	CHECK_DEPENDENCY_STATE(build_state_load(&state_arena, state_bob,
		STRING_LITERAL(".bob/state"), &state) == BUILD_STATE_LOAD_OK);
	CHECK_DEPENDENCY_STATE(!build_state_get_task(&state, test_path(state_bob, absolute_output), &state_task));
	CHECK_DEPENDENCY_STATE(!bob_platform_file_info(
		STRING_LITERAL("work/object.obj.d.tmp"), &after));
	result = true;

cleanup:
	if (changed_directory) {
		platform_remove_tree(".bob");
		platform_remove_tree("work");
		if (!platform_set_current_directory(original_directory.data)) result = false;
		else platform_remove_tree("build\\compiler_dependency_state");
	}
	else platform_remove_tree("build\\compiler_dependency_state");
	build_state_destroy(&state);
	arena_destroy(&state_arena);
	arena_destroy(&arena);
	bob_build_destroy(state_bob);
#undef CHECK_DEPENDENCY_STATE
	return result;
}

static b32 test_elf_descriptor(void)
{
    static const char source[] =
        "compile_main := {\n"
        "    name = \"compile main\",\n"
        "    command_line = \"clang-cl /c main.c\",\n"
        "    working_directory = \".\",\n"
        "    inputs = {\"main.c\"},\n"
        "    outputs = {\"main.obj\"},\n"
        "    include_dirs = {\"include\"},\n"
        "    dependencies = {},\n"
        "}\n"
        "compile_message := {\n"
        "    name = \"compile message\",\n"
        "    command_line = \"clang-cl /c message.c\",\n"
        "    dependencies = {},\n"
        "}\n"
        "link := {\n"
        "    name = \"link hello.exe\",\n"
        "    command_line = \"clang-cl main.obj message.obj\",\n"
        "    dependencies = {compile_main, compile_message},\n"
        "}\n"
        "run := {\n"
        "    name = \"run hello.exe\",\n"
        "    command_line = \"hello.exe\",\n"
        "    dependencies = {link},\n"
        "}\n"
        "ret {\n"
        "    targets = {run},\n"
        "    options = {workers = 2, verbosity = 0},\n"
        "}\n";
    String path = STRING_LITERAL("build/test_elf_descriptor.elf");
    Script_Build build;
    const Bob_Task_Desc *task;
    Bob *graph;

    CHECK(platform_create_directories("build"));
    CHECK(bob_platform_write_entire_file(path, source, sizeof(source) - 1));
    if (!script_load_build(path, &build)) {
        platform_remove_file(path.data);
        printf("  elf error: %s\n", build.error);
        return false;
    }
    CHECK(platform_remove_file(path.data));
    graph = bob_build_graph(build.build);
    CHECK(bob_task_count(build.build) == 4);
    CHECK(string_equal(bob_get_task_desc(bob_node_at(graph, 0))->name, STRING_LITERAL("run hello.exe")));
    CHECK(bob_dependency_count(bob_node_at(graph, 0)) == 1);
    CHECK(bob_dependency(bob_node_at(graph, 0), 0) == bob_node_at(graph, 1));
    task = bob_get_task_desc(bob_node_at(graph, 2));
    CHECK(string_equal(task->name, STRING_LITERAL("compile main")));
    CHECK(task->inputs.count == 1);
    CHECK(task->outputs.count == 1);
    CHECK(task->include_directories.count == 1);
	CHECK(string_equal(task->working_directory, STRING_LITERAL(".")));
    CHECK(build.options.has_worker_count);
    CHECK(build.options.worker_count == 2);
    CHECK(build.options.has_verbosity);
    CHECK(build.options.verbosity == 0);
    CHECK(bob_dependency_count(bob_node_at(graph, 1)) == 2);
    CHECK(bob_dependency(bob_node_at(graph, 1), 0) == bob_node_at(graph, 2));
    bob_build_destroy(build.build);
    return true;
}

static b32 test_elf_generated_descriptor(void)
{
    static const char source[] =
        "tasks := {}\n"
        "dependencies := {}\n"
        "for index := 0 ... 8 ? {\n"
        "    task := {\n"
        "        name = f\"generated ${index}\",\n"
        "        command_line = f\"generate ${index}\",\n"
        "        dependencies = dependencies,\n"
        "    }\n"
        "    tasks:add(task)\n"
        "    dependencies = {task}\n"
        "}\n"
        "ret {targets = {tasks[7]}}\n";
    String path = STRING_LITERAL("build/test_elf_generated_descriptor.elf");
    Script_Build build;
    Bob *graph;

    CHECK(platform_create_directories("build"));
    CHECK(bob_platform_write_entire_file(path, source, sizeof(source) - 1));
    if (!script_load_build(path, &build)) {
        platform_remove_file(path.data);
        printf("  elf error: %s\n", build.error);
        return false;
    }
    CHECK(platform_remove_file(path.data));
    graph = bob_build_graph(build.build);
    CHECK(bob_task_count(build.build) == 8);
    CHECK(string_equal(bob_get_task_desc(bob_node_at(graph, 0))->name, STRING_LITERAL("generated 7")));
    CHECK(bob_dependency_count(bob_node_at(graph, 0)) == 1);
    CHECK(bob_dependency(bob_node_at(graph, 0), 0) == bob_node_at(graph, 1));
    CHECK(string_equal(bob_get_task_desc(bob_node_at(graph, 7))->name, STRING_LITERAL("generated 0")));
    CHECK(bob_dependency_count(bob_node_at(graph, 7)) == 0);
    bob_build_destroy(build.build);
    return true;
}

static b32 test_bob_script(void)
{
    Arena arena = arena_create(MEGABYTES(16));
    Script *script = script_load(&arena, STRING_LITERAL("build.elf"));
    CHECK(script_is_loaded(script));
    CHECK(script_has_function(script, STRING_LITERAL("build")));
    CHECK(script_has_function(script, STRING_LITERAL("test")));
    CHECK(script_has_function(script, STRING_LITERAL("bless")));
    script_destroy(script);
    arena_destroy(&arena);
    return true;
}

// NOTE(RJ) we must be able to pet bob
static b32 test_pet_bob(void)
{
	Arena arena = arena_create(MEGABYTES(16));
	Script *script = script_load(&arena, STRING_LITERAL("build.elf"));
	CHECK(script_is_loaded(script));
	CHECK(script_has_function(script, STRING_LITERAL("pet")));
	CHECK(script_invoke(script, STRING_LITERAL("pet")));
	script_destroy(script);
	arena_destroy(&arena);
	return true;
}

static b32 test_option_resolution(void)
{
	Script_Options script = {
		.worker_count = 2,
		.verbosity = 0,
		.has_worker_count = true,
		.has_verbosity = true,
	};
	Cmd_Options command_line = {
		.verbosity = 3,
		.has_verbosity = true,
	};
	Script_Options merged = script_options_resolve(script, command_line);
	CHECK(merged.worker_count == 2);
	CHECK(merged.verbosity == 3);
	CHECK(merged.has_worker_count);
	CHECK(merged.has_verbosity);
	return true;
}

static b32 test_compiler_command(void)
{
	Arena arena = arena_create(KILOBYTES(64));
	Compiler_Command command;
	String augmented;
	CHECK(compiler_command_parse(&arena, STRING_LITERAL("   "), &command));
	CHECK(!command.executable.data && command.kind == COMPILER_KIND_UNKNOWN);
	CHECK(compiler_command_parse(&arena, STRING_LITERAL("/c source.c"), &command));
	CHECK(string_equal(command.executable, STRING_LITERAL("/c")) && !command.compiles);
	CHECK(compiler_command_parse(&arena, STRING_LITERAL("clang-cl /Ione /I \"two words\" -Ithree -I four -isystem system"), &command));
	CHECK(command.kind == COMPILER_KIND_CLANG_CL);
	CHECK(!command.compiles);
	CHECK(compiler_command_parse(&arena,
		STRING_LITERAL("\"C:\\Program Files\\LLVM\\bin\\clang-cl.exe\" /c source.c /Foobject.obj"),
		&command));
	CHECK(compiler_command_can_add_make_dependencies(&command));
	CHECK(compiler_command_add_dependencies(&arena, &command,
		STRING_LITERAL("\"C:\\Program Files\\LLVM\\bin\\clang-cl.exe\" /c source.c /Foobject.obj"),
		STRING_LITERAL("object.obj.d"), &augmented));
	CHECK(string_ends_with(augmented, STRING_LITERAL(" /clang:-MD /clang:-MF\"object.obj.d\"")));
	CHECK(compiler_command_parse(&arena, STRING_LITERAL("cl.exe /nologo /c source.c"), &command));
	CHECK(command.kind == COMPILER_KIND_MSVC && command.compiles);
	CHECK(!compiler_command_can_add_make_dependencies(&command));
	CHECK(compiler_command_add_dependencies(&arena, &command, STRING_LITERAL("cl /c source.c"),
		STRING_LITERAL("object.json"), &augmented));
	CHECK(string_ends_with(augmented, STRING_LITERAL(" /sourceDependencies \"object.json\"")));
	CHECK(compiler_command_parse(&arena, STRING_LITERAL("gcc -MMD -c source.c"), &command));
	CHECK(command.kind == COMPILER_KIND_GCC && command.compiles && command.generates_dependencies);
	CHECK(!compiler_command_can_add_make_dependencies(&command));
	CHECK(!compiler_command_add_dependencies(&arena, &command,
		STRING_LITERAL("gcc -MMD -c source.c"),
		STRING_LITERAL("object.d"), &augmented));
	CHECK(compiler_command_parse(&arena,
		STRING_LITERAL("x86_64-w64-mingw32-gcc -c source.c -o object.o"), &command));
	CHECK(command.kind == COMPILER_KIND_GCC && command.compiles);
	CHECK(compiler_command_can_add_make_dependencies(&command));
	CHECK(compiler_command_parse(&arena,
		STRING_LITERAL("C:\\toolchain\\aarch64-linux-gnu-g++.exe -c source.cpp -o object.o"), &command));
	CHECK(command.kind == COMPILER_KIND_GCC && command.compiles);
	CHECK(compiler_command_parse(&arena, STRING_LITERAL("notgcc -c source.c"), &command));
	CHECK(command.kind == COMPILER_KIND_UNKNOWN && command.compiles);
	arena_destroy(&arena);
	return true;
}

static b32 test_script_functions(void)
{
    static const char source[] =
        "build := fun() {\n"
        "    ret bob.build({\n"
        "        targets = {},\n"
        "        options = {workers = 1, verbosity = 0},\n"
        "    })\n"
        "}\n"
        "clean := fun() { ret 1 }\n"
        "ret {build = build, clean = clean}\n";
    String path = STRING_LITERAL("build/test_script_functions.elf");
    Arena arena = arena_create(MEGABYTES(16));
    CHECK(platform_create_directories("build"));
    CHECK(bob_platform_write_entire_file(path, source, sizeof(source) - 1));
    Script *script = script_load(&arena, path);
    CHECK(platform_remove_file(path.data));
    CHECK(script_is_loaded(script));
    String_Array functions = script_functions(script);
    CHECK(functions.count == 2);
    CHECK(script_has_function(script, STRING_LITERAL("build")));
    CHECK(script_has_function(script, STRING_LITERAL("clean")));
    CHECK(!script_has_function(script, STRING_LITERAL("missing")));
    CHECK(script_invoke(script, STRING_LITERAL("build")));
    CHECK(script_invoke(script, STRING_LITERAL("clean")));
    CHECK(!script_invoke(script, STRING_LITERAL("missing")));
    script_destroy(script);
    arena_destroy(&arena);
    return true;
}

static void run_test(const char *name, b32 (*test)(void))
{
    b32 passed;
    ++tests_run;
    printf("%-32s", name);
    fflush(stdout);
    passed = test();
    if (passed) {
        printf("PASS\n");
    } else {
        ++tests_failed;
    }
}

static int build_example(void)
{
    Bob_Build *build;
    Bob *graph;
    Bob_Node *compile_main;
    Bob_Node *compile_message;
    Bob_Node *link;
    Bob_Node *run;
    Bob_Task_Desc tasks[4] = {0};
    b32 succeeded;

    if (!CreateDirectoryA("build\\example", NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "unable to create build\\example\n");
        return 1;
    }

    build = bob_build_create();
    graph = bob_build_graph(build);
    if (!graph) {
        return 1;
    }

    compile_main = add_node(graph, "compile example/main.c");
    compile_message = add_node(graph, "compile example/message.c");
    link = add_node(graph, "link hello.exe");
    run = add_node(graph, "run hello.exe");

    tasks[0].command_line = STRING_LITERAL("clang-cl /nologo /W4 /WX /c example\\main.c /Fobuild\\example\\main.obj");
    tasks[1].command_line = STRING_LITERAL("clang-cl /nologo /W4 /WX /c example\\message.c /Fobuild\\example\\message.obj");
    tasks[2].command_line = STRING_LITERAL("clang-cl /nologo build\\example\\main.obj build\\example\\message.obj /Febuild\\example\\hello.exe");
    tasks[3].command_line = STRING_LITERAL("build\\example\\hello.exe");

    if (bob_add_dependency(graph, link, compile_main) != BOB_OK ||
        bob_add_dependency(graph, link, compile_message) != BOB_OK ||
        bob_add_dependency(graph, run, link) != BOB_OK) {
        bob_build_destroy(build);
        return 1;
    }

    succeeded = run_tasks(build, tasks, 4, 2);
    bob_build_destroy(build);
    return succeeded ? 0 : 1;
}

static int build_tasks_from_file(String path)
{
    Script_Build build = {0};
    u32 workers;
    int exit_code;
    if (!script_load_build(path, &build)) {
        fprintf(stderr, "%s: %s\n", path.data, build.error);
        return 1;
    }
    workers = build.options.has_worker_count ? build.options.worker_count : 4;
    exit_code = bob_build(build.build,
		(Bob_Build_Params){ .worker_count = workers }) ? 0 : 1;
    bob_build_destroy(build.build);
    return exit_code;
}

static int run_all_tests(void)
{
    run_test("arena and strings", test_arena_and_strings);
    run_test("option resolution", test_option_resolution);
	run_test("compiler command", test_compiler_command);
	run_test("build state file", test_build_state_file);
	run_test("build state stream", test_build_state_stream);
	run_test("build state append", test_build_state_append);
	run_test("build state path map", test_build_state_path_map);
	run_test("build state tasks", test_build_state_tasks);
    run_test("Make depfile", test_make_depfile);
    run_test("thread-local scratch", test_thread_local_scratch);
    run_test("vcvars cache", test_vcvars_cache_application);
	run_test("high resolution timer", test_high_resolution_timer);
	run_test("BLAKE3", test_blake3);
	run_test("build paths", test_build_paths);
    run_test("empty graph", test_empty_graph);
    run_test("linear graph", test_linear_graph);
    run_test("parallel fan-in", test_parallel_fan_in);
    run_test("failure blocks dependents", test_failure_blocks_dependents);
    run_test("cycle rejection", test_cycle_is_rejected);
    run_test("invalid edge rejection", test_invalid_edges_are_rejected);
	run_test("generic graph actions", test_generic_graph_actions);
    run_test("builder parallelism", test_builder_runs_in_parallel);
    run_test("builder failure", test_builder_propagates_failure);
    run_test("missing executable", test_builder_reports_missing_executable);
    run_test("incremental output", test_builder_skips_existing_output);
	run_test("directory output", test_directory_output_stays_clean);
	run_test("task fingerprints", test_task_fingerprint_rebuilds);
	run_test("task working directory", test_task_working_directory);
    run_test("newer input", test_newer_input_rebuilds);
    run_test("multiple inputs and outputs", test_multiple_inputs_and_outputs);
    run_test("dependency rebuild", test_dependency_rebuild_propagates);
    run_test("transparent dependency", test_transparent_dependency);
    run_test("compiler dependency state", test_compiler_dependency_state);
    run_test("elf build descriptor", test_elf_descriptor);
    run_test("elf generated descriptor", test_elf_generated_descriptor);
    run_test("Bob build script", test_bob_script);
    run_test("pet Bob", test_pet_bob);
    run_test("script functions", test_script_functions);

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}

int main(int argument_count, char **arguments)
{
    logger_init();

    if (argument_count == 5 && strcmp(arguments[1], "--child") == 0) {
        DWORD delay = (DWORD)strtoul(arguments[2], NULL, 10);
        int exit_code = atoi(arguments[3]);
        Sleep(delay);
        printf("%s\n", arguments[4]);
        return exit_code;
    }
    if (argument_count == 5 && strcmp(arguments[1], "--barrier") == 0) {
        HANDLE own_event = OpenEventA(EVENT_MODIFY_STATE, FALSE, arguments[2]);
        HANDLE other_event = OpenEventA(SYNCHRONIZE, FALSE, arguments[3]);
        DWORD wait_result;
        if (!own_event || !other_event) return 1;
        SetEvent(own_event);
        wait_result = WaitForSingleObject(other_event, 5000);
        CloseHandle(own_event);
        CloseHandle(other_event);
        if (wait_result != WAIT_OBJECT_0) return 1;
        printf("%s\n", arguments[4]);
        return 0;
    }
    if (argument_count == 2 && strcmp(arguments[1], "--build-example") == 0) {
        return build_example();
    }
	if (argument_count == 2 && strcmp(arguments[1], "--test") == 0) {
		return run_all_tests();
	}
	if (argument_count == 2 && strcmp(arguments[1], "--stress") == 0) {
		run_test("build state stress", test_build_state_stress);
		printf("\n%d/%d stress tests passed\n", tests_run - tests_failed, tests_run);
		return tests_failed ? 1 : 0;
	}
    if (argument_count == 1) {
        return build_tasks_from_file(STRING_LITERAL("build.elf"));
    }

	fprintf(stderr, "usage: bob [--test|--stress]\n");
    return 2;
}
