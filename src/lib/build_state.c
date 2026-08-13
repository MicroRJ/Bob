#include "build_state.h"
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
	if (capacity == 0) return build_state_path_table_rehash(arena, table, BUILD_STATE_PATH_INITIAL_CAPACITY);
	if ((u64)(table->slot_count + 1) * BUILD_STATE_PATH_LOAD_DENOMINATOR <= (u64)capacity * BUILD_STATE_PATH_LOAD_NUMERATOR) return true;
	if (capacity > UINT32_MAX / 2) return false;
	return build_state_path_table_rehash(arena, table, capacity * 2);
}

Build_State_Path_Id build_state_path_table_find(const Build_State_Path_Table *table, String path)
{
	if (!table || !path.data || path.size == 0 || !table->slots || table->slot_capacity == 0) return BUILD_STATE_PATH_ID_NONE;
	u64 hash = build_state_path_hash(path);
	u32 mask = table->slot_capacity - 1;
	u32 index = (u32)hash & mask;
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

String build_state_path_table_get(const Build_State_Path_Table *table, Build_State_Path_Id id)
{
	if (!table || id == BUILD_STATE_PATH_ID_NONE || id > table->path_count) return (String){0};
	return table->paths[id - 1].value;
}

static b32 build_state_reserve_tasks(Arena *arena, Build_State *state, u32 needed)
{
	if (state->task_capacity >= needed) return true;
	u32 capacity = state->task_capacity ? state->task_capacity : 16;
	while (capacity < needed) {
		if (capacity > UINT32_MAX / 2) return false;
		capacity *= 2;
	}

	Build_State_Task *tasks = arena_push_zero_aligned(arena, (u64)capacity * sizeof(*tasks), _Alignof(Build_State_Task));
	if (!tasks) return false;
	if (state->task_count) {
		memcpy(tasks, state->tasks, (u64)state->task_count * sizeof(*state->tasks));
	}
	state->tasks = tasks;
	state->task_capacity = capacity;
	return true;
}

static u32 build_state_task_index(const Build_State *state, Build_State_Path_Id output)
{
	if (!state || output == BUILD_STATE_PATH_ID_NONE) return UINT32_MAX;
	for (u32 i = 0; i < state->task_count; ++i) {
		if (state->tasks[i].output == output) return i;
	}
	return UINT32_MAX;
}

const Build_State_Task *build_state_find(const Build_State *state, String output)
{
	if (!state) return NULL;
	Build_State_Path_Id output_id = build_state_path_table_find(&state->paths, output);
	u32 index = build_state_task_index(state, output_id);
	return index == UINT32_MAX ? NULL : state->tasks + index;
}

b32 build_state_set(Arena *arena, Build_State *state, String output, String_Array dependencies)
{
	if (!arena || !state || !output.data || output.size == 0) return false;
	if (dependencies.count && !dependencies.items) return false;
	for (u32 i = 0; i < dependencies.count; ++i) {
		if (!dependencies.items[i].data || dependencies.items[i].size == 0) return false;
	}

	Build_State_Path_Id existing_output = build_state_path_table_find(&state->paths, output);
	u32 existing = build_state_task_index(state, existing_output);
	if (existing == UINT32_MAX && state->task_count == UINT32_MAX) return false;
	if (existing == UINT32_MAX && !build_state_reserve_tasks(arena, state, state->task_count + 1)) return false;

	Build_State_Task task = {0};
	if (dependencies.count) {
		task.dependencies.items = arena_push_zero_aligned(arena, (u64)dependencies.count * sizeof(*task.dependencies.items), _Alignof(Build_State_Path_Id));
		if (!task.dependencies.items) return false;
	}

	task.output = build_state_path_table_intern(arena, &state->paths, output);
	if (task.output == BUILD_STATE_PATH_ID_NONE) return false;
	for (u32 i = 0; i < dependencies.count; ++i) {
		Build_State_Path_Id id = build_state_path_table_intern(arena, &state->paths, dependencies.items[i]);
		if (id == BUILD_STATE_PATH_ID_NONE) return false;
		task.dependencies.items[task.dependencies.count++] = id;
	}

	if (existing != UINT32_MAX) state->tasks[existing] = task;
	else state->tasks[state->task_count++] = task;
	return true;
}

b32 build_state_remove(Build_State *state, String output)
{
	if (!state) return false;
	Build_State_Path_Id output_id = build_state_path_table_find(&state->paths, output);
	u32 index = build_state_task_index(state, output_id);
	if (index == UINT32_MAX) return false;
	if (index + 1 < state->task_count) {
		memmove(state->tasks + index, state->tasks + index + 1, (u64)(state->task_count - index - 1) * sizeof(*state->tasks));
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

static b32 build_state_encode_bytes(Build_State_Encoder *encoder, const void *data, u64 size)
{
	if (!encoder || (!data && size)) return false;
	if (encoder->cursor > encoder->size) return false;
	if (size > encoder->size - encoder->cursor) return false;
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

static b32 build_state_decode_bytes(Build_State_Decoder *decoder, const u8 **data, u64 size)
{
	if (!decoder || !data) return false;
	if (decoder->cursor > decoder->size) return false;
	if (size > decoder->size - decoder->cursor) return false;
	*data = decoder->data + decoder->cursor;
	decoder->cursor += size;
	return true;
}

static b32 build_state_decode_u32(Build_State_Decoder *decoder, u32 *value)
{
	const u8 *bytes;
	if (!value || !build_state_decode_bytes(decoder, &bytes, 4)) return false;
	*value = (u32)bytes[0] | ((u32)bytes[1] << 8) | ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24);
	return true;
}

static b32 build_state_decode_u64(Build_State_Decoder *decoder, u64 *value)
{
	const u8 *bytes;
	if (!value || !build_state_decode_bytes(decoder, &bytes, 8)) return false;
	*value = (u64)bytes[0] | ((u64)bytes[1] << 8) | ((u64)bytes[2] << 16) | ((u64)bytes[3] << 24) | ((u64)bytes[4] << 32) | ((u64)bytes[5] << 40) | ((u64)bytes[6] << 48) | ((u64)bytes[7] << 56);
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

static b32 build_state_stream_size(const Build_State *state, u64 *stream_size)
{
	u64 size = BUILD_STATE_STREAM_HEADER_SIZE;
	if (!state || !stream_size) return false;
	if (state->paths.path_count && !state->paths.paths) return false;
	if (state->task_count && !state->tasks) return false;

	for (u32 i = 0; i < state->paths.path_count; ++i) {
		String path = state->paths.paths[i].value;
		u64 content_size = 8;
		if (!path.data || path.size == 0 || path.size > UINT32_MAX) return false;
		if (build_state_path_table_find(&state->paths, path) != i + 1) return false;
		if (!build_state_size_add(&content_size, 1, path.size)) return false;
		if (content_size > UINT32_MAX) return false;
		if (!build_state_size_add(&size, 1, BUILD_STATE_STREAM_RECORD_HEADER_SIZE)) return false;
		if (!build_state_size_add(&size, 1, content_size)) return false;
	}

	for (u32 i = 0; i < state->task_count; ++i) {
		const Build_State_Task *task = state->tasks + i;
		u64 content_size = 20;
		if (task->output == BUILD_STATE_PATH_ID_NONE || task->output > state->paths.path_count) return false;
		if (build_state_task_index(state, task->output) != i) return false;
		if (task->dependencies.count && !task->dependencies.items) return false;
		if (!build_state_size_add(&content_size, task->dependencies.count, 4)) return false;
		if (content_size > UINT32_MAX) return false;
		for (u32 dependency = 0; dependency < task->dependencies.count; ++dependency) {
			Build_State_Path_Id id = task->dependencies.items[dependency];
			if (id == BUILD_STATE_PATH_ID_NONE || id > state->paths.path_count) return false;
		}
		if (!build_state_size_add(&size, 1, BUILD_STATE_STREAM_RECORD_HEADER_SIZE)) return false;
		if (!build_state_size_add(&size, 1, content_size)) return false;
	}

	*stream_size = size;
	return true;
}

static b32 build_state_stream_begin_record(Build_State_Encoder *stream, u32 content_size, Build_State_Encoder *content, u64 *checksum_offset)
{
	if (!stream || !content || !checksum_offset) return false;
	if (!build_state_encode_u32(stream, content_size)) return false;
	*checksum_offset = stream->cursor;
	if (!build_state_encode_u32(stream, 0)) return false;
	if (stream->cursor > stream->size || content_size > stream->size - stream->cursor) return false;
	*content = (Build_State_Encoder){ stream->data + stream->cursor, content_size, 0 };
	stream->cursor += content_size;
	return true;
}

static b32 build_state_stream_finish_record(Build_State_Encoder *stream, const Build_State_Encoder *content, u64 checksum_offset)
{
	Build_State_Encoder checksum;
	if (!stream || !content || content->cursor != content->size) return false;
	if (checksum_offset > stream->size || sizeof(u32) > stream->size - checksum_offset) return false;
	checksum = (Build_State_Encoder){ stream->data + checksum_offset, sizeof(u32), 0 };
	return build_state_encode_u32(&checksum, build_state_crc32c(content->data, content->size));
}

b32 build_state_stream_encode(Arena *arena, const Build_State *state, String *stream)
{
	u64 mark;
	u64 stream_size;
	Build_State_Encoder encoder = {0};
	if (!arena || !stream) return false;
	*stream = (String){0};
	if (!build_state_stream_size(state, &stream_size) || stream_size > SIZE_MAX) return false;
	mark = arena_mark(arena);
	encoder.data = arena_push(arena, stream_size);
	encoder.size = stream_size;
	if (!encoder.data) return false;

	if (!build_state_encode_bytes(&encoder, BUILD_STATE_STREAM_MAGIC, BUILD_STATE_STREAM_MAGIC_SIZE)) goto failure;
	if (!build_state_encode_u32(&encoder, BUILD_STATE_STREAM_VERSION)) goto failure;
	if (!build_state_encode_u32(&encoder, BUILD_STATE_STREAM_HEADER_SIZE)) goto failure;

	for (u32 i = 0; i < state->paths.path_count; ++i) {
		String path = state->paths.paths[i].value;
		u32 content_size = 8 + (u32)path.size;
		u64 checksum_offset;
		Build_State_Encoder content;
		if (!build_state_stream_begin_record(&encoder, content_size, &content, &checksum_offset)) goto failure;
		if (!build_state_encode_u32(&content, STATE_OP_INTERN)) goto failure;
		if (!build_state_encode_u32(&content, (u32)path.size)) goto failure;
		if (!build_state_encode_bytes(&content, path.data, path.size)) goto failure;
		if (!build_state_stream_finish_record(&encoder, &content, checksum_offset)) goto failure;
	}

	for (u32 i = 0; i < state->task_count; ++i) {
		const Build_State_Task *task = state->tasks + i;
		u32 content_size = 20 + task->dependencies.count * 4;
		u64 checksum_offset;
		Build_State_Encoder content;
		if (!build_state_stream_begin_record(&encoder, content_size, &content, &checksum_offset)) goto failure;
		if (!build_state_encode_u32(&content, STATE_OP_SET)) goto failure;
		if (!build_state_encode_u32(&content, task->output)) goto failure;
		if (!build_state_encode_u64(&content, task->output_stamp)) goto failure;
		if (!build_state_encode_u32(&content, task->dependencies.count)) goto failure;
		for (u32 dependency = 0; dependency < task->dependencies.count; ++dependency) {
			if (!build_state_encode_u32(&content, task->dependencies.items[dependency])) goto failure;
		}
		if (!build_state_stream_finish_record(&encoder, &content, checksum_offset)) goto failure;
	}

	if (encoder.cursor != encoder.size) goto failure;
	*stream = string_from_data(encoder.data, encoder.size);
	return true;

failure:
	arena_restore(arena, mark);
	return false;
}

Build_State_Stream_Result build_state_stream_replay(Arena *arena, String stream, Build_State *state)
{
	u64 mark;
	Build_State decoded = {0};
	Build_State_Decoder decoder = { (const u8 *)stream.data, stream.size, 0 };
	const u8 *magic;
	u32 version;
	u32 header_size;
	if (!arena || !state) return BUILD_STATE_STREAM_ERROR;
	*state = (Build_State){0};
	if (!stream.data || stream.size < BUILD_STATE_STREAM_HEADER_SIZE) return BUILD_STATE_STREAM_INVALID;
	mark = arena_mark(arena);

	if (!build_state_decode_bytes(&decoder, &magic, BUILD_STATE_STREAM_MAGIC_SIZE)) goto invalid;
	if (memcmp(magic, BUILD_STATE_STREAM_MAGIC, BUILD_STATE_STREAM_MAGIC_SIZE) != 0) goto invalid;
	if (!build_state_decode_u32(&decoder, &version)) goto invalid;
	if (!build_state_decode_u32(&decoder, &header_size)) goto invalid;
	if (version != BUILD_STATE_STREAM_VERSION) goto invalid;
	if (header_size != BUILD_STATE_STREAM_HEADER_SIZE) goto invalid;

	while (decoder.cursor < decoder.size) {
		u32 content_size;
		u32 checksum;
		const u8 *content_data;
		Build_State_Decoder content;
		u32 operation;

		if (decoder.size - decoder.cursor < BUILD_STATE_STREAM_RECORD_HEADER_SIZE) goto truncated;
		if (!build_state_decode_u32(&decoder, &content_size)) goto truncated;
		if (!build_state_decode_u32(&decoder, &checksum)) goto truncated;
		if (content_size > decoder.size - decoder.cursor) goto truncated;
		if (content_size < sizeof(u32)) goto invalid;
		if (!build_state_decode_bytes(&decoder, &content_data, content_size)) goto truncated;
		if (build_state_crc32c(content_data, content_size) != checksum) goto invalid;
		content = (Build_State_Decoder){ content_data, content_size, 0 };
		if (!build_state_decode_u32(&content, &operation)) goto invalid;

		switch ((Build_State_Op)operation) {
		case STATE_OP_INTERN: {
			u32 path_size;
			const u8 *path_data;
			Build_State_Path_Id expected = decoded.paths.path_count + 1;
			if (!build_state_decode_u32(&content, &path_size)) goto invalid;
			if (path_size == 0 || path_size != content.size - content.cursor) goto invalid;
			if (!build_state_decode_bytes(&content, &path_data, path_size)) goto invalid;
			Build_State_Path_Id id = build_state_path_table_intern(arena, &decoded.paths, string_from_data((void *)path_data, path_size));
			if (id == BUILD_STATE_PATH_ID_NONE) goto error;
			if (id != expected) goto invalid;
		} break;

		case STATE_OP_SET: {
			u32 output;
			u64 output_stamp;
			u32 dependency_count;
			u32 existing;
			Build_State_Task task = {0};
			if (!build_state_decode_u32(&content, &output)) goto invalid;
			if (!build_state_decode_u64(&content, &output_stamp)) goto invalid;
			if (!build_state_decode_u32(&content, &dependency_count)) goto invalid;
			if ((u64)dependency_count * 4 != content.size - content.cursor) goto invalid;
			if (output == BUILD_STATE_PATH_ID_NONE || output > decoded.paths.path_count) goto invalid;
			existing = build_state_task_index(&decoded, output);
			if (existing == UINT32_MAX && decoded.task_count == UINT32_MAX) goto error;
			if (existing == UINT32_MAX && !build_state_reserve_tasks(arena, &decoded, decoded.task_count + 1)) goto error;
			task.output = output;
			task.output_stamp = output_stamp;
			if (dependency_count) {
				task.dependencies.items = arena_push_zero_aligned(arena, (u64)dependency_count * sizeof(*task.dependencies.items), _Alignof(Build_State_Path_Id));
				if (!task.dependencies.items) goto error;
			}
			for (u32 dependency = 0; dependency < dependency_count; ++dependency) {
				u32 id;
				if (!build_state_decode_u32(&content, &id)) goto invalid;
				if (id == BUILD_STATE_PATH_ID_NONE || id > decoded.paths.path_count) goto invalid;
				task.dependencies.items[task.dependencies.count++] = id;
			}
			if (existing == UINT32_MAX) decoded.tasks[decoded.task_count++] = task;
			else decoded.tasks[existing] = task;
		} break;

		case STATE_OP_REMOVE: {
			u32 output;
			if (!build_state_decode_u32(&content, &output)) goto invalid;
			if (content.cursor != content.size) goto invalid;
			if (output == BUILD_STATE_PATH_ID_NONE || output > decoded.paths.path_count) goto invalid;
			u32 index = build_state_task_index(&decoded, output);
			if (index != UINT32_MAX) {
				if (index + 1 < decoded.task_count) memmove(decoded.tasks + index, decoded.tasks + index + 1, (u64)(decoded.task_count - index - 1) * sizeof(*decoded.tasks));
				--decoded.task_count;
			}
		} break;

		default: goto invalid;
		}

		if (content.cursor != content.size) goto invalid;
	}

	*state = decoded;
	return BUILD_STATE_STREAM_OK;

truncated:
	*state = decoded;
	return BUILD_STATE_STREAM_TRUNCATED;

invalid:
	arena_restore(arena, mark);
	*state = (Build_State){0};
	return BUILD_STATE_STREAM_INVALID;

error:
	arena_restore(arena, mark);
	*state = (Build_State){0};
	return BUILD_STATE_STREAM_ERROR;
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

b32 build_state_save(String path, const Build_State *state)
{
	u64 stream_size;
	u64 arena_capacity = 64;
	Arena arena = {0};
	String parent;
	String temporary = {0};
	String stream = {0};
	b32 result = false;

	if (!string_is_terminated(path) || path.size == 0) return false;
	if (!build_state_stream_size(state, &stream_size) || stream_size > SIZE_MAX) return false;
	if (!build_state_size_add(&arena_capacity, 1, stream_size)) return false;
	if (!build_state_size_add(&arena_capacity, 3, path.size)) return false;
	arena = arena_create(arena_capacity);
	arena_set_name(&arena, "build state stream");
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
	if (!build_state_stream_encode(&arena, state, &stream)) goto done;
	if (!bob_platform_write_entire_file(temporary, stream.data, (size_t)stream.size)) goto done;
	if (!platform_move_file(temporary.data, path.data, true)) goto done;
	result = true;

done:
	if (!result && temporary.data) platform_remove_file(temporary.data);
	arena_destroy(&arena);
	return result;
}

Build_State_Load_Result build_state_load(Arena *arena, String path, Build_State *state)
{
	Bob_Platform_File_Info info;
	Arena source_arena = {0};
	String source;
	Build_State_Load_Result result;
	Build_State_Stream_Result stream_result;
	if (!arena || !state || !string_is_terminated(path) || path.size == 0) return BUILD_STATE_LOAD_ERROR;
	*state = (Build_State){0};
	if (!bob_platform_file_info(path, &info)) return BUILD_STATE_LOAD_MISSING;
	if (info.size == UINT64_MAX) return BUILD_STATE_LOAD_ERROR;
	source_arena = arena_create(info.size + 1);
	arena_set_name(&source_arena, "build state source");
	if (!source_arena.data) return BUILD_STATE_LOAD_ERROR;
	if (!bob_platform_read_entire_file(&source_arena, path, &source)) {
		arena_destroy(&source_arena);
		return BUILD_STATE_LOAD_ERROR;
	}
	stream_result = build_state_stream_replay(arena, source, state);
	switch (stream_result) {
	case BUILD_STATE_STREAM_OK: result = BUILD_STATE_LOAD_OK; break;
	case BUILD_STATE_STREAM_TRUNCATED: result = BUILD_STATE_LOAD_RECOVERED; break;
	case BUILD_STATE_STREAM_INVALID: result = BUILD_STATE_LOAD_INVALID; break;
	default: result = BUILD_STATE_LOAD_ERROR; break;
	}
	arena_destroy(&source_arena);
	return result;
}
