#include "build_state.h"
#include "platform_adapter.h"
#include "platform.h"

#include <string.h>

#define BUILD_STATE_PATH_INITIAL_CAPACITY 16

static b32 build_state_reserve_paths(Arena *arena, Build_State_Path_Map *map, u32 needed)
{
	if (map->path_capacity >= needed) return true;
	u32 capacity = map->path_capacity ? map->path_capacity : BUILD_STATE_PATH_INITIAL_CAPACITY;
	while (capacity < needed) {
		if (capacity > UINT32_MAX / 2) return false;
		capacity *= 2;
	}
	Bob_Path *paths = arena_push_zero_aligned(arena, (u64)capacity * sizeof(*paths), _Alignof(Bob_Path));
	if (!paths) return false;
	if (map->path_count) memcpy(paths, map->paths, (u64)map->path_count * sizeof(*map->paths));
	map->paths = paths;
	map->path_capacity = capacity;
	return true;
}

static b32 build_state_reserve_atoms(Arena *arena, Build_State_Path_Map *map, u32 atom_id)
{
	if (atom_id < map->atom_capacity) return true;
	u32 capacity = map->atom_capacity ? map->atom_capacity : BUILD_STATE_PATH_INITIAL_CAPACITY;
	while (capacity <= atom_id) {
		if (capacity > UINT32_MAX / 2) return false;
		capacity *= 2;
	}
	u32 *ids = arena_push_zero_aligned(arena, (u64)capacity * sizeof(*ids), _Alignof(u32));
	if (!ids) return false;
	if (map->atom_capacity) memcpy(ids, map->ids_by_atom, (u64)map->atom_capacity * sizeof(*map->ids_by_atom));
	map->ids_by_atom = ids;
	map->atom_capacity = capacity;
	return true;
}

static Build_State_Path_Id build_state_path_id(const Build_State *state, Bob_Path path)
{
	u32 atom = path.atom.id;
	if (!state || !bob_path_is_valid(path) || atom >= state->paths.atom_capacity) return BUILD_STATE_PATH_ID_NONE;
	return state->paths.ids_by_atom[atom];
}

static Bob_Path build_state_path(const Build_State *state, Build_State_Path_Id id)
{
	if (!state || id == BUILD_STATE_PATH_ID_NONE || id > state->paths.path_count) return (Bob_Path){0};
	return state->paths.paths[id - 1];
}

static Build_State_Path_Id build_state_add_path(Arena *arena, Build_State *state, Bob_Path path)
{
	Build_State_Path_Id existing = build_state_path_id(state, path);
	if (existing != BUILD_STATE_PATH_ID_NONE) return existing;
	if (!arena || !state || !bob_path_is_valid(path) || state->paths.path_count == UINT32_MAX) return BUILD_STATE_PATH_ID_NONE;
	if (!build_state_reserve_paths(arena, &state->paths, state->paths.path_count + 1)) return BUILD_STATE_PATH_ID_NONE;
	if (!build_state_reserve_atoms(arena, &state->paths, path.atom.id)) return BUILD_STATE_PATH_ID_NONE;
	Build_State_Path_Id id = ++state->paths.path_count;
	state->paths.paths[id - 1] = path;
	state->paths.ids_by_atom[path.atom.id] = id;
	return id;
}

static b32 build_state_add_replayed_path(Arena *arena, Build_State *state, Bob_Path path)
{
	if (!arena || !state || !bob_path_is_valid(path) || state->paths.path_count == UINT32_MAX) return false;
	if (!build_state_reserve_paths(arena, &state->paths, state->paths.path_count + 1)) return false;
	if (!build_state_reserve_atoms(arena, &state->paths, path.atom.id)) return false;
	Build_State_Path_Id id = ++state->paths.path_count;
	state->paths.paths[id - 1] = path;
	if (state->paths.ids_by_atom[path.atom.id] == BUILD_STATE_PATH_ID_NONE) state->paths.ids_by_atom[path.atom.id] = id;
	return true;
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

static u32 build_state_task_index(const Build_State *state, Bob_Path output)
{
	if (!state || !bob_path_is_valid(output)) return UINT32_MAX;
	for (u32 i = 0; i < state->task_count; ++i) {
		if (state->tasks[i].output.atom.id == output.atom.id) return i;
	}
	return UINT32_MAX;
}

const Build_State_Task *build_state_find(const Build_State *state, Bob_Path output)
{
	u32 index = build_state_task_index(state, output);
	return index == UINT32_MAX ? NULL : state->tasks + index;
}

b32 build_state_set(Arena *arena, Build_State *state, Bob_Path output, Bob_Path_Array dependencies)
{
	if (!arena || !state || !bob_path_is_valid(output)) return false;
	if (dependencies.count && !dependencies.items) return false;
	for (u32 i = 0; i < dependencies.count; ++i) {
		if (!bob_path_is_valid(dependencies.items[i])) return false;
	}

	u32 existing = build_state_task_index(state, output);
	if (existing == UINT32_MAX && state->task_count == UINT32_MAX) return false;
	if (existing == UINT32_MAX && !build_state_reserve_tasks(arena, state, state->task_count + 1)) return false;

	Build_State_Task task = {0};
	if (dependencies.count) {
		task.dependencies.items = arena_push_zero_aligned(arena, (u64)dependencies.count * sizeof(*task.dependencies.items), _Alignof(Bob_Path));
		if (!task.dependencies.items) return false;
	}

	task.output = output;
	if (build_state_add_path(arena, state, output) == BUILD_STATE_PATH_ID_NONE) return false;
	for (u32 i = 0; i < dependencies.count; ++i) {
		if (build_state_add_path(arena, state, dependencies.items[i]) == BUILD_STATE_PATH_ID_NONE) return false;
		task.dependencies.items[task.dependencies.count++] = dependencies.items[i];
	}

	if (existing != UINT32_MAX) state->tasks[existing] = task;
	else state->tasks[state->task_count++] = task;
	return true;
}

b32 build_state_remove(Build_State *state, Bob_Path output)
{
	u32 index = build_state_task_index(state, output);
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

static b32 build_state_stream_size(const Bob *bob, const Build_State *state, u64 *stream_size)
{
	u64 size = BUILD_STATE_STREAM_HEADER_SIZE;
	if (!bob || !state || !stream_size) return false;
	if (state->paths.path_count && !state->paths.paths) return false;
	if (state->task_count && !state->tasks) return false;

	for (u32 i = 0; i < state->paths.path_count; ++i) {
		Bob_Path handle = state->paths.paths[i];
		String path = bob_path_string(bob, handle);
		u64 content_size = 8;
		if (!path.data || path.size == 0 || path.size > UINT32_MAX) return false;
		if (!bob_path_is_valid(handle)) return false;
		if (!build_state_size_add(&content_size, 1, path.size)) return false;
		if (content_size > UINT32_MAX) return false;
		if (!build_state_size_add(&size, 1, BUILD_STATE_STREAM_RECORD_HEADER_SIZE)) return false;
		if (!build_state_size_add(&size, 1, content_size)) return false;
	}

	for (u32 i = 0; i < state->task_count; ++i) {
		const Build_State_Task *task = state->tasks + i;
		u64 content_size = 20;
		if (build_state_path_id(state, task->output) == BUILD_STATE_PATH_ID_NONE) return false;
		if (build_state_task_index(state, task->output) != i) return false;
		if (task->dependencies.count && !task->dependencies.items) return false;
		if (!build_state_size_add(&content_size, task->dependencies.count, 4)) return false;
		if (content_size > UINT32_MAX) return false;
		for (u32 dependency = 0; dependency < task->dependencies.count; ++dependency) {
			if (build_state_path_id(state, task->dependencies.items[dependency]) == BUILD_STATE_PATH_ID_NONE) return false;
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

static b32 build_state_stream_encode_intern(Build_State_Encoder *encoder, String path)
{
	u64 checksum_offset;
	Build_State_Encoder content;
	if (!encoder || !path.data || path.size == 0 || path.size > UINT32_MAX - 8) return false;
	if (!build_state_stream_begin_record(encoder, 8 + (u32)path.size, &content, &checksum_offset)) return false;
	if (!build_state_encode_u32(&content, STATE_OP_INTERN)) return false;
	if (!build_state_encode_u32(&content, (u32)path.size)) return false;
	if (!build_state_encode_bytes(&content, path.data, path.size)) return false;
	return build_state_stream_finish_record(encoder, &content, checksum_offset);
}

static b32 build_state_stream_encode_set(Build_State_Encoder *encoder, const Build_State *state, const Build_State_Task *task)
{
	u64 content_size = 20;
	u64 checksum_offset;
	Build_State_Encoder content;
	if (!encoder || !state || !task || (task->dependencies.count && !task->dependencies.items)) return false;
	if (!build_state_size_add(&content_size, task->dependencies.count, 4) || content_size > UINT32_MAX) return false;
	if (!build_state_stream_begin_record(encoder, (u32)content_size, &content, &checksum_offset)) return false;
	if (!build_state_encode_u32(&content, STATE_OP_SET)) return false;
	if (!build_state_encode_u32(&content, build_state_path_id(state, task->output))) return false;
	if (!build_state_encode_u64(&content, task->output_stamp)) return false;
	if (!build_state_encode_u32(&content, task->dependencies.count)) return false;
	for (u32 dependency = 0; dependency < task->dependencies.count; ++dependency) {
		if (!build_state_encode_u32(&content, build_state_path_id(state, task->dependencies.items[dependency]))) return false;
	}
	return build_state_stream_finish_record(encoder, &content, checksum_offset);
}

static b32 build_state_stream_encode_remove(Build_State_Encoder *encoder, Build_State_Path_Id output)
{
	u64 checksum_offset;
	Build_State_Encoder content;
	if (!encoder || output == BUILD_STATE_PATH_ID_NONE) return false;
	if (!build_state_stream_begin_record(encoder, 8, &content, &checksum_offset)) return false;
	if (!build_state_encode_u32(&content, STATE_OP_REMOVE)) return false;
	if (!build_state_encode_u32(&content, output)) return false;
	return build_state_stream_finish_record(encoder, &content, checksum_offset);
}

b32 build_state_stream_encode(Arena *arena, const Bob *bob, const Build_State *state, String *stream)
{
	u64 mark;
	u64 stream_size;
	Build_State_Encoder encoder = {0};
	if (!arena || !bob || !stream) return false;
	*stream = (String){0};
	if (!build_state_stream_size(bob, state, &stream_size) || stream_size > SIZE_MAX) return false;
	mark = arena_mark(arena);
	encoder.data = arena_push(arena, stream_size);
	encoder.size = stream_size;
	if (!encoder.data) return false;

	if (!build_state_encode_bytes(&encoder, BUILD_STATE_STREAM_MAGIC, BUILD_STATE_STREAM_MAGIC_SIZE)) goto failure;
	if (!build_state_encode_u32(&encoder, BUILD_STATE_STREAM_VERSION)) goto failure;
	if (!build_state_encode_u32(&encoder, BUILD_STATE_STREAM_HEADER_SIZE)) goto failure;

	for (u32 i = 0; i < state->paths.path_count; ++i) {
		if (!build_state_stream_encode_intern(&encoder, bob_path_string(bob, state->paths.paths[i]))) goto failure;
	}

	for (u32 i = 0; i < state->task_count; ++i) {
		if (!build_state_stream_encode_set(&encoder, state, state->tasks + i)) goto failure;
	}

	if (encoder.cursor != encoder.size) goto failure;
	*stream = string_from_data(encoder.data, encoder.size);
	return true;

failure:
	arena_restore(arena, mark);
	return false;
}

Build_State_Stream_Result build_state_stream_replay(Arena *arena, Bob *bob, String stream, Build_State *state)
{
	u64 mark;
	Build_State decoded = {0};
	Build_State_Decoder decoder = { (const u8 *)stream.data, stream.size, 0 };
	const u8 *magic;
	u32 version;
	u32 header_size;
	if (!arena || !bob || !state) return BUILD_STATE_STREAM_ERROR;
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
			Bob_Path path;
			if (!build_state_decode_u32(&content, &path_size)) goto invalid;
			if (path_size == 0 || path_size != content.size - content.cursor) goto invalid;
			if (!build_state_decode_bytes(&content, &path_data, path_size)) goto invalid;
			if (!bob_path_resolve(bob, bob_build_root(bob), string_from_data((void *)path_data, path_size), &path)) goto error;
			if (!build_state_add_replayed_path(arena, &decoded, path)) goto error;
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
			Bob_Path output_path = build_state_path(&decoded, output);
			existing = build_state_task_index(&decoded, output_path);
			if (existing == UINT32_MAX && decoded.task_count == UINT32_MAX) goto error;
			if (existing == UINT32_MAX && !build_state_reserve_tasks(arena, &decoded, decoded.task_count + 1)) goto error;
			task.output = output_path;
			task.output_stamp = output_stamp;
			if (dependency_count) {
				task.dependencies.items = arena_push_zero_aligned(arena, (u64)dependency_count * sizeof(*task.dependencies.items), _Alignof(Bob_Path));
				if (!task.dependencies.items) goto error;
			}
			for (u32 dependency = 0; dependency < dependency_count; ++dependency) {
				u32 id;
				if (!build_state_decode_u32(&content, &id)) goto invalid;
				if (id == BUILD_STATE_PATH_ID_NONE || id > decoded.paths.path_count) goto invalid;
				task.dependencies.items[task.dependencies.count++] = build_state_path(&decoded, id);
			}
			if (existing == UINT32_MAX) decoded.tasks[decoded.task_count++] = task;
			else decoded.tasks[existing] = task;
		} break;

		case STATE_OP_REMOVE: {
			u32 output;
			if (!build_state_decode_u32(&content, &output)) goto invalid;
			if (content.cursor != content.size) goto invalid;
			if (output == BUILD_STATE_PATH_ID_NONE || output > decoded.paths.path_count) goto invalid;
			u32 index = build_state_task_index(&decoded, build_state_path(&decoded, output));
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

static b32 build_state_append_bytes(String path, const void *data, u64 size)
{
	Platform_File file;
	u64 written = 0;
	b32 result;
	if (!string_is_terminated(path) || (!data && size)) return false;
	file = platform_access_file(path.data, PLATFORM_FILE_OPEN_EXISTING, PLATFORM_FILE_WRITE | PLATFORM_FILE_SHARE_READ);
	if (!platform_file_is_valid(file)) return false;
	result = platform_set_file_cursor(file, PLATFORM_SEEK_END, 0, NULL) && platform_write_file(file, data, size, &written) && written == size;
	platform_close_file(file);
	return result;
}

b32 build_state_append_set(Arena *arena, String path, const Bob *bob, Build_State *state, Bob_Path output, Bob_Path_Array dependencies, u64 output_stamp)
{
	u32 first_new_path;
	u32 task_index;
	u64 append_size = 0;
	Scratch scratch;
	Build_State_Encoder encoder = {0};
	Build_State_Task *task;
	if (!arena || !bob || !state || !string_is_terminated(path) || path.size == 0) return false;
	first_new_path = state->paths.path_count;
	if (!build_state_set(arena, state, output, dependencies)) return false;
	task_index = build_state_task_index(state, output);
	if (task_index == UINT32_MAX) return false;
	task = state->tasks + task_index;
	task->output_stamp = output_stamp;

	for (u32 i = first_new_path; i < state->paths.path_count; ++i) {
		String new_path = bob_path_string(bob, state->paths.paths[i]);
		if (!build_state_size_add(&append_size, 1, BUILD_STATE_STREAM_RECORD_HEADER_SIZE + 8)) return false;
		if (!build_state_size_add(&append_size, 1, new_path.size)) return false;
	}
	if (!build_state_size_add(&append_size, 1, BUILD_STATE_STREAM_RECORD_HEADER_SIZE + 20)) return false;
	if (!build_state_size_add(&append_size, task->dependencies.count, 4) || append_size > SIZE_MAX) return false;

	scratch = begin_different_scratch(arena);
	encoder.data = arena_push(scratch.arena, append_size);
	encoder.size = append_size;
	if (!encoder.data) {
		end_scratch(scratch);
		return false;
	}
	for (u32 i = first_new_path; i < state->paths.path_count; ++i) {
		if (!build_state_stream_encode_intern(&encoder, bob_path_string(bob, state->paths.paths[i]))) goto failure;
	}
	if (!build_state_stream_encode_set(&encoder, state, task)) goto failure;
	if (encoder.cursor != encoder.size || !build_state_append_bytes(path, encoder.data, encoder.size)) goto failure;
	end_scratch(scratch);
	return true;

failure:
	end_scratch(scratch);
	return false;
}

b32 build_state_append_remove(String path, Build_State *state, Bob_Path output)
{
	u8 bytes[BUILD_STATE_STREAM_RECORD_HEADER_SIZE + 8];
	Build_State_Path_Id output_id;
	Build_State_Encoder encoder = { bytes, sizeof(bytes), 0 };
	if (!state || !string_is_terminated(path) || path.size == 0) return false;
	output_id = build_state_path_id(state, output);
	if (build_state_task_index(state, output) == UINT32_MAX) return true;
	if (!build_state_stream_encode_remove(&encoder, output_id) || encoder.cursor != encoder.size) return false;
	if (!build_state_append_bytes(path, encoder.data, encoder.size)) return false;
	return build_state_remove(state, output);
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

b32 build_state_save(String path, const Bob *bob, const Build_State *state)
{
	u64 stream_size;
	u64 arena_capacity = 64;
	Arena arena = {0};
	String parent;
	String temporary = {0};
	String stream = {0};
	b32 result = false;

	if (!string_is_terminated(path) || path.size == 0) return false;
	if (!build_state_stream_size(bob, state, &stream_size) || stream_size > SIZE_MAX) return false;
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
	if (!build_state_stream_encode(&arena, bob, state, &stream)) goto done;
	if (!bob_platform_write_entire_file(temporary, stream.data, (size_t)stream.size)) goto done;
	if (!platform_move_file(temporary.data, path.data, true)) goto done;
	result = true;

done:
	if (!result && temporary.data) platform_remove_file(temporary.data);
	arena_destroy(&arena);
	return result;
}

Build_State_Load_Result build_state_load(Arena *arena, Bob *bob, String path, Build_State *state)
{
	Bob_Platform_File_Info info;
	Arena source_arena = {0};
	String source;
	Build_State_Load_Result result;
	Build_State_Stream_Result stream_result;
	if (!arena || !bob || !state || !string_is_terminated(path) || path.size == 0) return BUILD_STATE_LOAD_ERROR;
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
	stream_result = build_state_stream_replay(arena, bob, source, state);
	switch (stream_result) {
	case BUILD_STATE_STREAM_OK: result = BUILD_STATE_LOAD_OK; break;
	case BUILD_STATE_STREAM_TRUNCATED: result = BUILD_STATE_LOAD_RECOVERED; break;
	case BUILD_STATE_STREAM_INVALID: result = BUILD_STATE_LOAD_INVALID; break;
	default: result = BUILD_STATE_LOAD_ERROR; break;
	}
	arena_destroy(&source_arena);
	return result;
}
