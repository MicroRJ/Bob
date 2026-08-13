#include "build_state_binary.h"
#include "platform_adapter.h"
#include "platform.h"

#include <string.h>

#define BUILD_STATE_PATH_INITIAL_CAPACITY 16
#define BUILD_STATE_PATH_LOAD_NUMERATOR   3
#define BUILD_STATE_PATH_LOAD_DENOMINATOR 4

u64 build_state_path_hash(String path)
{
	u64 hash = 14695981039346656037ULL;
	for (u64 i = 0; i < path.size; ++i) {
		hash ^= (u8)path.data[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

static Build_State_Path_Slot *build_state_path_slot(Build_State_Path_Slot *slots, u32 capacity, u64 hash)
{
	u32 mask = capacity - 1;
	u32 index = (u32)hash & mask;
	while (slots[index].id != BUILD_STATE_PATH_ID_NONE) {
		index = (index + 1) & mask;
	}
	return slots + index;
}

static b32 build_state_path_table_reserve_paths(Arena *arena, Build_State_Path_Table *table, u32 needed)
{
	if (table->path_capacity >= needed) return true;
	u32 capacity = table->path_capacity ? table->path_capacity : BUILD_STATE_PATH_INITIAL_CAPACITY;
	while (capacity < needed) {
		if (capacity > UINT32_MAX / 2) return false;
		capacity *= 2;
	}

	Build_State_Path *paths = arena_push_zero_aligned(arena, (u64)capacity * sizeof(*paths), _Alignof(Build_State_Path));
	if (!paths) return false;

	if (table->path_count) {
		memcpy(paths, table->paths, (u64)table->path_count * sizeof(*table->paths));
	}

	table->paths = paths;
	table->path_capacity = capacity;
	return true;
}

static b32 build_state_path_table_rehash(Arena *arena, Build_State_Path_Table *table, u32 capacity)
{

	if (capacity < BUILD_STATE_PATH_INITIAL_CAPACITY || (capacity & (capacity - 1)) != 0) return false;

	Build_State_Path_Slot *slots = arena_push_zero_aligned(arena, (u64)capacity * sizeof(*slots), _Alignof(Build_State_Path_Slot));
	if (!slots) return false;
	for (u32 i = 0; i < table->path_count; ++i) {
		Build_State_Path_Id id = i + 1;
		Build_State_Path_Slot *slot = build_state_path_slot(slots, capacity, table->paths[i].hash);
		slot->hash = table->paths[i].hash;
		slot->id = id;
	}
	table->slots = slots;
	table->slot_count = table->path_count;
	table->slot_capacity = capacity;
	return true;
}

static b32 build_state_path_table_reserve_slot(Arena *arena, Build_State_Path_Table *table)
{
	u32 capacity = table->slot_capacity;
	if (capacity == 0) {
		return build_state_path_table_rehash(arena, table, BUILD_STATE_PATH_INITIAL_CAPACITY);
	}
	if ((u64)(table->slot_count + 1) * BUILD_STATE_PATH_LOAD_DENOMINATOR <= (u64)capacity * BUILD_STATE_PATH_LOAD_NUMERATOR) return true;
	if (capacity > UINT32_MAX / 2) return false;
	return build_state_path_table_rehash(arena, table, capacity * 2);
}

Build_State_Path_Id build_state_path_table_find(const Build_State_Path_Table *table, String path)
{
	if (!table || !path.data || path.size == 0 || !table->slots || table->slot_capacity == 0) return BUILD_STATE_PATH_ID_NONE;
	u64 hash = build_state_path_hash(path);
	u32 mask = table->slot_capacity - 1;
	u32 index = (u32) hash & mask;
	for (u32 probe = 0; probe < table->slot_capacity; ++probe) {
		const Build_State_Path_Slot *slot = table->slots + index;
		if (slot->id == BUILD_STATE_PATH_ID_NONE) return BUILD_STATE_PATH_ID_NONE;
		if (slot->hash == hash && slot->id <= table->path_count) {
			String stored = table->paths[slot->id - 1].value;
			if (string_equal(stored, path)) return slot->id;
		}
		index = (index + 1) & mask;
	}
	return BUILD_STATE_PATH_ID_NONE;
}

Build_State_Path_Id build_state_path_table_intern(Arena *arena, Build_State_Path_Table *table, String path)
{

	if (!arena || !table || !path.data || path.size == 0 || table->path_count == UINT32_MAX) return BUILD_STATE_PATH_ID_NONE;
	Build_State_Path_Id id = build_state_path_table_find(table, path);
	if (id != BUILD_STATE_PATH_ID_NONE) return id;

	Build_State_Path_Table original = *table;
	u64 mark = arena_mark(arena);
	String copy = arena_push_string_copy(arena, path);
	if (!copy.data || !build_state_path_table_reserve_paths(arena, table, table->path_count + 1) || !build_state_path_table_reserve_slot(arena, table)) goto failure;

	u64 hash = build_state_path_hash(path);
	id = table->path_count + 1;
	table->paths[table->path_count++] = (Build_State_Path){ copy, hash };
	Build_State_Path_Slot *slot = build_state_path_slot(table->slots, table->slot_capacity, hash);
	slot->hash = hash;
	slot->id = id;
	++table->slot_count;
	return id;

failure:
	*table = original;
	arena_restore(arena, mark);
	return BUILD_STATE_PATH_ID_NONE;
}

String build_state_path_table_get(
	const Build_State_Path_Table *table, Build_State_Path_Id id)
{
	if (!table || id == BUILD_STATE_PATH_ID_NONE || id > table->path_count) {
		return (String){0};
	}
	return table->paths[id - 1].value;
}

static b32 build_state_binary_reserve_tasks(Arena *arena, Build_State_Binary *state, u32 needed)
{
	if (state->task_capacity >= needed) return true;
	u32 capacity = state->task_capacity ? state->task_capacity : 16;
	while (capacity < needed) {
		if (capacity > UINT32_MAX / 2) return false;
		capacity *= 2;
	}

	Build_State_Binary_Task *tasks = arena_push_zero_aligned(arena,
		(u64)capacity * sizeof(*tasks), _Alignof(Build_State_Binary_Task));
	if (!tasks) return false;
	if (state->task_count) {
		memcpy(tasks, state->tasks, (u64)state->task_count * sizeof(*state->tasks));
	}
	state->tasks = tasks;
	state->task_capacity = capacity;
	return true;
}

static u32 build_state_binary_task_index(
	const Build_State_Binary *state, Build_State_Path_Id output)
{
	if (!state || output == BUILD_STATE_PATH_ID_NONE) return UINT32_MAX;
	for (u32 i = 0; i < state->task_count; ++i) {
		if (state->tasks[i].output == output) return i;
	}
	return UINT32_MAX;
}

const Build_State_Binary_Task *build_state_binary_find(
	const Build_State_Binary *state, String output)
{
	if (!state) return NULL;
	Build_State_Path_Id output_id = build_state_path_table_find(&state->paths, output);
	u32 index = build_state_binary_task_index(state, output_id);
	return index == UINT32_MAX ? NULL : state->tasks + index;
}

b32 build_state_binary_set(Arena *arena, Build_State_Binary *state,
	String output, String_Array dependencies)
{
	if (!arena || !state || !output.data || output.size == 0 ||
		(dependencies.count && !dependencies.items)) return false;
	for (u32 i = 0; i < dependencies.count; ++i) {
		if (!dependencies.items[i].data || dependencies.items[i].size == 0) return false;
	}

	Build_State_Path_Id existing_output = build_state_path_table_find(&state->paths, output);
	u32 existing = build_state_binary_task_index(state, existing_output);
	if (existing == UINT32_MAX && (state->task_count == UINT32_MAX ||
		!build_state_binary_reserve_tasks(arena, state, state->task_count + 1))) return false;

	Build_State_Binary_Task task = {0};
	if (dependencies.count) {
		task.dependencies.items = arena_push_zero_aligned(arena,
			(u64)dependencies.count * sizeof(*task.dependencies.items),
			_Alignof(Build_State_Path_Id));
		if (!task.dependencies.items) return false;
	}

	task.output = build_state_path_table_intern(arena, &state->paths, output);
	if (task.output == BUILD_STATE_PATH_ID_NONE) return false;
	for (u32 i = 0; i < dependencies.count; ++i) {
		Build_State_Path_Id id = build_state_path_table_intern(
			arena, &state->paths, dependencies.items[i]);
		if (id == BUILD_STATE_PATH_ID_NONE) return false;
		task.dependencies.items[task.dependencies.count++] = id;
	}

	if (existing != UINT32_MAX) state->tasks[existing] = task;
	else state->tasks[state->task_count++] = task;
	return true;
}

b32 build_state_binary_remove(Build_State_Binary *state, String output)
{
	if (!state) return false;
	Build_State_Path_Id output_id = build_state_path_table_find(&state->paths, output);
	u32 index = build_state_binary_task_index(state, output_id);
	if (index == UINT32_MAX) return false;
	if (index + 1 < state->task_count) {
		memmove(state->tasks + index, state->tasks + index + 1,
			(u64)(state->task_count - index - 1) * sizeof(*state->tasks));
	}
	--state->task_count;
	return true;
}

typedef struct Build_State_Encoder
{
	u8  *data;
	u64  size;
	u64  cursor;
}
Build_State_Encoder;

typedef struct Build_State_Decoder
{
	const u8 *data;
	u64       size;
	u64       cursor;
}
Build_State_Decoder;

static b32 build_state_size_add(u64 *total, u64 count, u64 size)
{
	if (!total || (count && size > UINT64_MAX / count)) return false;
	u64 addition = count * size;
	if (*total > UINT64_MAX - addition) return false;
	*total += addition;
	return true;
}

static b32 build_state_encode_bytes(
	Build_State_Encoder *encoder, const void *data, u64 size)
{
	if (!encoder || (!data && size) || encoder->cursor > encoder->size ||
		size > encoder->size - encoder->cursor) return false;
	if (size) memcpy(encoder->data + encoder->cursor, data, (size_t)size);
	encoder->cursor += size;
	return true;
}

static b32 build_state_encode_u32(Build_State_Encoder *encoder, u32 value)
{
	u8 bytes[4] = {
		(u8)value,
		(u8)(value >> 8),
		(u8)(value >> 16),
		(u8)(value >> 24),
	};
	return build_state_encode_bytes(encoder, bytes, sizeof(bytes));
}

static b32 build_state_encode_u64(Build_State_Encoder *encoder, u64 value)
{
	u8 bytes[8] = {
		(u8)value,
		(u8)(value >> 8),
		(u8)(value >> 16),
		(u8)(value >> 24),
		(u8)(value >> 32),
		(u8)(value >> 40),
		(u8)(value >> 48),
		(u8)(value >> 56),
	};
	return build_state_encode_bytes(encoder, bytes, sizeof(bytes));
}

static b32 build_state_decode_bytes(
	Build_State_Decoder *decoder, const u8 **data, u64 size)
{
	if (!decoder || !data || decoder->cursor > decoder->size ||
		size > decoder->size - decoder->cursor) return false;
	*data = decoder->data + decoder->cursor;
	decoder->cursor += size;
	return true;
}

static b32 build_state_decode_u32(Build_State_Decoder *decoder, u32 *value)
{
	const u8 *bytes;
	if (!value || !build_state_decode_bytes(decoder, &bytes, 4)) return false;
	*value = (u32)bytes[0] |
		((u32)bytes[1] << 8) |
		((u32)bytes[2] << 16) |
		((u32)bytes[3] << 24);
	return true;
}

static b32 build_state_decode_u64(Build_State_Decoder *decoder, u64 *value)
{
	const u8 *bytes;
	if (!value || !build_state_decode_bytes(decoder, &bytes, 8)) return false;
	*value = (u64)bytes[0] |
		((u64)bytes[1] << 8) |
		((u64)bytes[2] << 16) |
		((u64)bytes[3] << 24) |
		((u64)bytes[4] << 32) |
		((u64)bytes[5] << 40) |
		((u64)bytes[6] << 48) |
		((u64)bytes[7] << 56);
	return true;
}

static u32 build_state_crc32c(const void *data, u64 size)
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

static b32 build_state_snapshot_size(
	const Build_State_Binary *state, u64 *payload_size, u64 *file_size)
{
	u64 payload = 0;
	if (!state || !payload_size || !file_size ||
		(state->paths.path_count && !state->paths.paths) ||
		(state->task_count && !state->tasks)) return false;
	for (u32 i = 0; i < state->paths.path_count; ++i) {
		String path = state->paths.paths[i].value;
		if (!path.data || path.size == 0 || path.size > UINT32_MAX ||
			!build_state_size_add(&payload, 1, 4) ||
			!build_state_size_add(&payload, 1, path.size)) return false;
	}
	for (u32 i = 0; i < state->task_count; ++i) {
		const Build_State_Binary_Task *task = state->tasks + i;
		u64 record_size = 8;
		if (task->output == BUILD_STATE_PATH_ID_NONE ||
			task->output > state->paths.path_count ||
			(task->dependencies.count && !task->dependencies.items) ||
			!build_state_size_add(&record_size, task->dependencies.count, 4) ||
			record_size > UINT32_MAX ||
			!build_state_size_add(&payload, 1, 4) ||
			!build_state_size_add(&payload, 1, record_size)) return false;
		for (u32 dependency = 0; dependency < task->dependencies.count;
			++dependency) {
			Build_State_Path_Id id = task->dependencies.items[dependency];
			if (id == BUILD_STATE_PATH_ID_NONE || id > state->paths.path_count) {
				return false;
			}
		}
	}
	*payload_size = payload;
	*file_size = BUILD_STATE_SNAPSHOT_HEADER_SIZE;
	return build_state_size_add(file_size, 1, payload);
}

static b32 build_state_encode_snapshot(
	Build_State_Encoder *encoder, const Build_State_Binary *state)
{
	u64 payload_size;
	u64 file_size;
	if (!encoder || !build_state_snapshot_size(state, &payload_size, &file_size) ||
		encoder->size != file_size) return false;

	Build_State_Encoder payload = {
		.data = encoder->data + BUILD_STATE_SNAPSHOT_HEADER_SIZE,
		.size = payload_size,
	};
	for (u32 i = 0; i < state->paths.path_count; ++i) {
		String path = state->paths.paths[i].value;
		if (!build_state_encode_u32(&payload, (u32)path.size) ||
			!build_state_encode_bytes(&payload, path.data, path.size)) return false;
	}
	for (u32 i = 0; i < state->task_count; ++i) {
		const Build_State_Binary_Task *task = state->tasks + i;
		u32 record_size = 8 + task->dependencies.count * 4;
		if (!build_state_encode_u32(&payload, record_size) ||
			!build_state_encode_u32(&payload, task->output) ||
			!build_state_encode_u32(&payload, task->dependencies.count)) return false;
		for (u32 dependency = 0; dependency < task->dependencies.count;
			++dependency) {
			if (!build_state_encode_u32(&payload,
				task->dependencies.items[dependency])) return false;
		}
	}
	if (payload.cursor != payload.size) return false;

	u32 checksum = build_state_crc32c(payload.data, payload.size);
	if (!build_state_encode_bytes(encoder, BUILD_STATE_SNAPSHOT_MAGIC,
			BUILD_STATE_SNAPSHOT_MAGIC_SIZE) ||
		!build_state_encode_u32(encoder, BUILD_STATE_BINARY_VERSION) ||
		!build_state_encode_u32(encoder, BUILD_STATE_SNAPSHOT_HEADER_SIZE) ||
		!build_state_encode_u64(encoder, state->generation) ||
		!build_state_encode_u64(encoder, payload_size) ||
		!build_state_encode_u32(encoder, state->paths.path_count) ||
		!build_state_encode_u32(encoder, state->task_count) ||
		!build_state_encode_u32(encoder, checksum) ||
		!build_state_encode_u32(encoder, 0)) return false;
	encoder->cursor = file_size;
	return true;
}

static String build_state_parent_directory(String path)
{
	for (u64 i = path.size; i > 0; --i) {
		u64 separator = i - 1;
		if (path.data[separator] != '/' && path.data[separator] != '\\') continue;
		if (separator == 0 || (separator == 2 && path.data[1] == ':')) ++separator;
		return string_slice(path, 0, separator);
	}
	return (String){0};
}

b32 build_state_binary_save(String path, const Build_State_Binary *state)
{
	u64 payload_size;
	u64 file_size;
	u64 arena_capacity = 64;
	Arena arena = {0};
	String parent;
	String temporary = {0};
	Build_State_Encoder encoder = {0};
	b32 result = false;

	if (!string_is_terminated(path) || path.size == 0 ||
		!build_state_snapshot_size(state, &payload_size, &file_size) ||
		file_size > SIZE_MAX ||
		!build_state_size_add(&arena_capacity, 1, file_size) ||
		!build_state_size_add(&arena_capacity, 3, path.size)) return false;
	arena = arena_create(arena_capacity);
	arena_set_name(&arena, "binary build state snapshot");
	if (!arena.data) return false;
	parent = build_state_parent_directory(path);
	if (parent.size) {
		parent = arena_push_string_copy(&arena, parent);
		if (!parent.data || !platform_create_directories(parent.data)) goto done;
	}
	{
		void *start = arena_top(&arena);
		arena_append_str(&arena, path);
		arena_append_text(&arena, ".tmp");
		temporary = arena_string_from(&arena, start);
		arena_finalize_string(&arena, temporary);
	}
	encoder.data = arena_push(&arena, file_size);
	encoder.size = file_size;
	if (!encoder.data || !build_state_encode_snapshot(&encoder, state) ||
		!bob_platform_write_entire_file(temporary, encoder.data,
			(size_t)encoder.size) ||
		!platform_move_file(temporary.data, path.data, true)) goto done;
	result = true;

done:
	if (!result && temporary.data) platform_remove_file(temporary.data);
	arena_destroy(&arena);
	return result;
}

static Build_State_Load_Result build_state_decode_snapshot(
	Arena *arena, String source, Build_State_Binary *state)
{
	Build_State_Decoder header = { (const u8 *)source.data, source.size, 0 };
	const u8 *magic;
	u32 version;
	u32 header_size;
	u64 generation;
	u64 payload_size;
	u32 path_count;
	u32 task_count;
	u32 checksum;
	u32 reserved;
	u64 mark = arena_mark(arena);
	Build_State_Binary decoded = {0};

	if (source.size < BUILD_STATE_SNAPSHOT_HEADER_SIZE ||
		!build_state_decode_bytes(&header, &magic,
			BUILD_STATE_SNAPSHOT_MAGIC_SIZE) ||
		memcmp(magic, BUILD_STATE_SNAPSHOT_MAGIC,
			BUILD_STATE_SNAPSHOT_MAGIC_SIZE) != 0 ||
		!build_state_decode_u32(&header, &version) ||
		!build_state_decode_u32(&header, &header_size) ||
		!build_state_decode_u64(&header, &generation) ||
		!build_state_decode_u64(&header, &payload_size) ||
		!build_state_decode_u32(&header, &path_count) ||
		!build_state_decode_u32(&header, &task_count) ||
		!build_state_decode_u32(&header, &checksum) ||
		!build_state_decode_u32(&header, &reserved) ||
		version != BUILD_STATE_BINARY_VERSION ||
		header_size != BUILD_STATE_SNAPSHOT_HEADER_SIZE || reserved != 0 ||
		payload_size != source.size - BUILD_STATE_SNAPSHOT_HEADER_SIZE) {
		return BUILD_STATE_LOAD_INVALID;
	}

	Build_State_Decoder payload = {
		.data = (const u8 *)source.data + BUILD_STATE_SNAPSHOT_HEADER_SIZE,
		.size = payload_size,
	};
	if (build_state_crc32c(payload.data, payload.size) != checksum ||
		(u64)path_count > payload_size / 5 ||
		(u64)task_count > payload_size / 12) return BUILD_STATE_LOAD_INVALID;

	for (u32 i = 0; i < path_count; ++i) {
		u32 size;
		const u8 *data;
		if (!build_state_decode_u32(&payload, &size) || size == 0 ||
			!build_state_decode_bytes(&payload, &data, size)) goto invalid;
		Build_State_Path_Id id = build_state_path_table_intern(arena,
			&decoded.paths, string_from_data((void *)data, size));
		if (id == BUILD_STATE_PATH_ID_NONE) goto error;
		if (id != i + 1) goto invalid;
	}
	for (u32 i = 0; i < task_count; ++i) {
		u32 record_size;
		const u8 *record_data;
		if (!build_state_decode_u32(&payload, &record_size) || record_size < 8 ||
			!build_state_decode_bytes(&payload, &record_data, record_size)) goto invalid;
		Build_State_Decoder record = { record_data, record_size, 0 };
		u32 output;
		u32 dependency_count;
		if (!build_state_decode_u32(&record, &output) ||
			!build_state_decode_u32(&record, &dependency_count) ||
			(u64)dependency_count * 4 != record.size - record.cursor ||
			output == BUILD_STATE_PATH_ID_NONE || output > path_count ||
			build_state_binary_task_index(&decoded, output) != UINT32_MAX ||
			!build_state_binary_reserve_tasks(arena, &decoded,
				decoded.task_count + 1)) goto invalid;

		Build_State_Binary_Task task = { .output = output };
		if (dependency_count) {
			task.dependencies.items = arena_push_zero_aligned(arena,
				(u64)dependency_count * sizeof(*task.dependencies.items),
				_Alignof(Build_State_Path_Id));
			if (!task.dependencies.items) goto error;
		}
		for (u32 dependency = 0; dependency < dependency_count; ++dependency) {
			u32 id;
			if (!build_state_decode_u32(&record, &id) ||
				id == BUILD_STATE_PATH_ID_NONE || id > path_count) goto invalid;
			task.dependencies.items[task.dependencies.count++] = id;
		}
		if (record.cursor != record.size) goto invalid;
		decoded.tasks[decoded.task_count++] = task;
	}
	if (payload.cursor != payload.size) goto invalid;
	decoded.generation = generation;
	*state = decoded;
	return BUILD_STATE_LOAD_OK;

invalid:
	arena_restore(arena, mark);
	*state = (Build_State_Binary){0};
	return BUILD_STATE_LOAD_INVALID;

error:
	arena_restore(arena, mark);
	*state = (Build_State_Binary){0};
	return BUILD_STATE_LOAD_ERROR;
}

Build_State_Load_Result build_state_binary_load(
	Arena *arena, String path, Build_State_Binary *state)
{
	Bob_Platform_File_Info info;
	Arena source_arena = {0};
	String source;
	Build_State_Load_Result result;
	if (!arena || !state || !string_is_terminated(path) || path.size == 0) {
		return BUILD_STATE_LOAD_ERROR;
	}
	*state = (Build_State_Binary){0};
	if (!bob_platform_file_info(path, &info)) return BUILD_STATE_LOAD_MISSING;
	if (info.size == UINT64_MAX) return BUILD_STATE_LOAD_ERROR;
	source_arena = arena_create(info.size + 1);
	arena_set_name(&source_arena, "binary build state source");
	if (!source_arena.data ||
		!bob_platform_read_entire_file(&source_arena, path, &source)) {
		arena_destroy(&source_arena);
		return BUILD_STATE_LOAD_ERROR;
	}
	result = build_state_decode_snapshot(arena, source, state);
	arena_destroy(&source_arena);
	return result;
}
