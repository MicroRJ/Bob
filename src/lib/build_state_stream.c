#include "build_state_stream.h"
#include "build_state_internal.h"
#include "platform_adapter.h"
#include "platform.h"

#include <string.h>

typedef struct
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

typedef u32 Build_State_Path_Id;

#define BUILD_STATE_PATH_ID_NONE            ((Build_State_Path_Id)0)
#define BUILD_STATE_PATH_INITIAL_CAPACITY 16

static b32 build_state_stream_is_valid(const Build_State_Stream *stream)
{
	return stream && stream->build && stream->state && stream->state->initialized;
}

static void build_state_stream_replace_paths(Build_State_Stream *stream, const Build_State_Stream *replacement)
{
	stream->paths = replacement->paths;
	stream->path_count = replacement->path_count;
	stream->path_capacity = replacement->path_capacity;
	stream->ids_by_atom = replacement->ids_by_atom;
	stream->atom_capacity = replacement->atom_capacity;
}

void build_state_stream_init(Build_State_Stream *state_stream, Bob_Build *build, Build_State *state)
{
	ASSERT(state_stream);
	ASSERT(build);
	ASSERT(state);
	ASSERT(state->initialized);
	*state_stream = (Build_State_Stream){ .build = build, .state = state };
}

static b32 build_state_stream_reserve_paths(Arena *arena, Build_State_Stream *stream, u32 needed)
{
	if (stream->path_capacity >= needed) return true;
	u32 capacity = stream->path_capacity ? stream->path_capacity : BUILD_STATE_PATH_INITIAL_CAPACITY;
	while (capacity < needed) {
		if (capacity > UINT32_MAX / 2) return false;
		capacity *= 2;
	}
	Bob_Path *paths = arena_push_zero_aligned(arena, (u64)capacity * sizeof(*paths), _Alignof(Bob_Path));
	if (!paths) return false;
	if (stream->path_count) memcpy(paths, stream->paths, (u64)stream->path_count * sizeof(*stream->paths));
	stream->paths = paths;
	stream->path_capacity = capacity;
	return true;
}

static b32 build_state_stream_reserve_atoms(Arena *arena, Build_State_Stream *stream, u32 atom_id)
{
	if (atom_id < stream->atom_capacity) return true;
	u32 capacity = stream->atom_capacity ? stream->atom_capacity : BUILD_STATE_PATH_INITIAL_CAPACITY;
	while (capacity <= atom_id) {
		if (capacity > UINT32_MAX / 2) return false;
		capacity *= 2;
	}
	u32 *ids = arena_push_zero_aligned(arena, (u64)capacity * sizeof(*ids), _Alignof(u32));
	if (!ids) return false;
	if (stream->atom_capacity) memcpy(ids, stream->ids_by_atom, (u64)stream->atom_capacity * sizeof(*stream->ids_by_atom));
	stream->ids_by_atom = ids;
	stream->atom_capacity = capacity;
	return true;
}

static Build_State_Path_Id build_state_stream_path_id(const Build_State_Stream *stream, Bob_Path path)
{
	u32 atom = path.atom.id;
	if (!stream || !bob_path_is_valid(path) || atom >= stream->atom_capacity) return BUILD_STATE_PATH_ID_NONE;
	return stream->ids_by_atom[atom];
}

static Bob_Path build_state_stream_path(const Build_State_Stream *stream, Build_State_Path_Id id)
{
	if (!stream || id == BUILD_STATE_PATH_ID_NONE || id > stream->path_count) return (Bob_Path){0};
	return stream->paths[id - 1];
}

static Build_State_Path_Id build_state_stream_add_path(Arena *arena, Build_State_Stream *stream, Bob_Path path)
{
	Build_State_Path_Id existing = build_state_stream_path_id(stream, path);
	if (existing != BUILD_STATE_PATH_ID_NONE) return existing;
	if (!arena || !stream || !bob_path_is_valid(path) || stream->path_count == UINT32_MAX) return BUILD_STATE_PATH_ID_NONE;
	if (!build_state_stream_reserve_paths(arena, stream, stream->path_count + 1)) return BUILD_STATE_PATH_ID_NONE;
	if (!build_state_stream_reserve_atoms(arena, stream, path.atom.id)) return BUILD_STATE_PATH_ID_NONE;
	Build_State_Path_Id id = ++stream->path_count;
	stream->paths[id - 1] = path;
	stream->ids_by_atom[path.atom.id] = id;
	return id;
}

static b32 build_state_stream_add_replayed_path(Arena *arena, Build_State_Stream *stream, Bob_Path path)
{
	if (!arena || !stream || !bob_path_is_valid(path) || stream->path_count == UINT32_MAX) return false;
	if (!build_state_stream_reserve_paths(arena, stream, stream->path_count + 1)) return false;
	if (!build_state_stream_reserve_atoms(arena, stream, path.atom.id)) return false;
	Build_State_Path_Id id = ++stream->path_count;
	stream->paths[id - 1] = path;
	stream->ids_by_atom[path.atom.id] = id;
	return true;
}

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
		(u8)(value >> 0),
		(u8)(value >> 8),
		(u8)(value >> 16),
		(u8)(value >> 24),
	};
	return build_state_encode_bytes(encoder, bytes, sizeof(bytes));
}

static b32 build_state_encode_u64(Build_State_Encoder *encoder, u64 value)
{
	u8 bytes[8] = {
		(u8)(value >> 0),
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

static b32 build_state_stream_size(const Bob_Build *build, const Build_State *state, const Build_State_Stream *state_stream, u64 *stream_size)
{
	u64 size = BUILD_STATE_STREAM_HEADER_SIZE;
	if (!build || !state || !state_stream || !stream_size) return false;
	if (state_stream->path_count && !state_stream->paths) return false;
	if (state->task_count && !state->tasks) return false;

	for (u32 i = 0; i < state_stream->path_count; ++i) {
		Bob_Path handle = state_stream->paths[i];
		String path = bob_path_string(build, handle);
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
		u64 content_size = 20 + BOB_FINGERPRINT_SIZE;
		if (build_state_stream_path_id(state_stream, task->output) == BUILD_STATE_PATH_ID_NONE) return false;
		if (build_state_task_index(state, task->output) != i) return false;
		if (task->dependencies.count && !task->dependencies.items) return false;
		if (!build_state_size_add(&content_size, task->dependencies.count, 4)) return false;
		if (content_size > UINT32_MAX) return false;
		for (u32 dependency = 0; dependency < task->dependencies.count; ++dependency) {
			if (build_state_stream_path_id(state_stream, task->dependencies.items[dependency]) == BUILD_STATE_PATH_ID_NONE) return false;
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

static b32 build_state_stream_encode_set(Build_State_Encoder *encoder, const Build_State_Stream *state_stream, const Build_State_Task *task)
{
	u64 content_size = 20 + BOB_FINGERPRINT_SIZE;
	u64 checksum_offset;
	Build_State_Encoder content;
	if (!encoder || !state_stream || !task || (task->dependencies.count && !task->dependencies.items)) return false;
	if (!build_state_size_add(&content_size, task->dependencies.count, 4) || content_size > UINT32_MAX) return false;
	if (!build_state_stream_begin_record(encoder, (u32)content_size, &content, &checksum_offset)) return false;
	if (!build_state_encode_u32(&content, STATE_OP_SET)) return false;
	if (!build_state_encode_u32(&content, build_state_stream_path_id(state_stream, task->output))) return false;
	if (!build_state_encode_u64(&content, task->output_stamp)) return false;
	if (!build_state_encode_bytes(&content, task->fingerprint.bytes, sizeof(task->fingerprint.bytes))) return false;
	if (!build_state_encode_u32(&content, task->dependencies.count)) return false;
	for (u32 dependency = 0; dependency < task->dependencies.count; ++dependency) {
		if (!build_state_encode_u32(&content, build_state_stream_path_id(state_stream, task->dependencies.items[dependency]))) return false;
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

static b32 build_state_stream_encode_unlocked(Arena *arena, const Bob_Build *build, const Build_State *state, const Build_State_Stream *state_stream, String *stream)
{
	u64 mark;
	u64 stream_size;
	Build_State_Encoder encoder = {0};
	if (!arena || !build || !stream) return false;
	*stream = (String){0};
	if (!build_state_stream_size(build, state, state_stream, &stream_size) || stream_size > SIZE_MAX) return false;
	mark = arena_mark(arena);
	encoder.data = arena_push(arena, stream_size);
	encoder.size = stream_size;
	if (!encoder.data) return false;

	if (!build_state_encode_bytes(&encoder, BUILD_STATE_STREAM_MAGIC, BUILD_STATE_STREAM_MAGIC_SIZE)) goto failure;
	if (!build_state_encode_u32(&encoder, BUILD_STATE_STREAM_VERSION)) goto failure;
	if (!build_state_encode_u32(&encoder, BUILD_STATE_STREAM_HEADER_SIZE)) goto failure;

	for (u32 i = 0; i < state_stream->path_count; ++i) {
		if (!build_state_stream_encode_intern(&encoder, bob_path_string(build, state_stream->paths[i]))) goto failure;
	}

	for (u32 i = 0; i < state->task_count; ++i) {
		if (!build_state_stream_encode_set(&encoder, state_stream, state->tasks + i)) goto failure;
	}

	if (encoder.cursor != encoder.size) goto failure;
	*stream = string_from_data(encoder.data, encoder.size);
	return true;

failure:
	arena_restore(arena, mark);
	return false;
}

static b32 build_state_stream_collect_paths(Arena *arena, const Build_State *state, Build_State_Stream *state_stream)
{
	if (!arena || !state || !state_stream) return false;
	for (u32 i = 0; i < state->task_count; ++i) {
		const Build_State_Task *task = state->tasks + i;
		if (build_state_stream_add_path(arena, state_stream, task->output) == BUILD_STATE_PATH_ID_NONE) return false;
		for (u32 dependency = 0; dependency < task->dependencies.count; ++dependency) {
			if (build_state_stream_add_path(arena, state_stream, task->dependencies.items[dependency]) == BUILD_STATE_PATH_ID_NONE) return false;
		}
	}
	return true;
}

static b32 build_state_stream_compact_unlocked(Arena *arena, const Bob_Build *build, const Build_State *state, Build_State_Stream *state_stream, String *stream)
{
	u64 mark;
	Build_State_Stream compacted = {0};
	if (!state || !state->arena || !state_stream) return false;
	mark = arena_mark(state->arena);
	if (!build_state_stream_collect_paths(state->arena, state, &compacted) || !build_state_stream_encode_unlocked(arena, build, state, &compacted, stream)) {
		arena_restore(state->arena, mark);
		return false;
	}
	build_state_stream_replace_paths(state_stream, &compacted);
	return true;
}

b32 build_state_stream_encode(Build_State_Stream *state_stream, Arena *arena, String *stream)
{
	if (!build_state_stream_is_valid(state_stream)) return false;
	Bob_Build *build = state_stream->build;
	Build_State *state = state_stream->state;
	platform_lock_mutex(&state->mutex);
	b32 result = build_state_stream_compact_unlocked(arena, build, state, state_stream, stream);
	platform_unlock_mutex(&state->mutex);
	return result;
}

static Build_State_Stream_Result build_state_stream_replay_unlocked(Bob_Build *build, String stream, Build_State *state, Build_State_Stream *state_stream)
{
	u64 mark;
	Build_State decoded = {0};
	Build_State_Stream decoded_stream = {0};
	Build_State_Decoder decoder = { (const u8 *)stream.data, stream.size, 0 };
	Arena *arena;
	const u8 *magic;
	u32 version;
	u32 header_size;
	if (!build || !state || !state->arena || !state_stream) return BUILD_STATE_STREAM_ERROR;
	arena = state->arena;
	decoded.arena = arena;
	build_state_replace_unlocked(state, &(Build_State){0});
	build_state_stream_replace_paths(state_stream, &(Build_State_Stream){0});
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
			if (!bob_path_resolve(build, bob_build_root(build), string_from_data((void *)path_data, path_size), &path)) goto error;
			if (build_state_stream_path_id(&decoded_stream, path) != BUILD_STATE_PATH_ID_NONE) goto invalid;
			if (!build_state_stream_add_replayed_path(arena, &decoded_stream, path)) goto error;
		} break;

		case STATE_OP_SET: {
			u32 output;
			u64 output_stamp;
			const u8 *fingerprint;
			u32 dependency_count;
			u32 existing;
			Build_State_Task task = {0};
			if (!build_state_decode_u32(&content, &output)) goto invalid;
			if (!build_state_decode_u64(&content, &output_stamp)) goto invalid;
			if (!build_state_decode_bytes(&content, &fingerprint, BOB_FINGERPRINT_SIZE)) goto invalid;
			if (!build_state_decode_u32(&content, &dependency_count)) goto invalid;
			if ((u64)dependency_count * 4 != content.size - content.cursor) goto invalid;
			if (output == BUILD_STATE_PATH_ID_NONE || output > decoded_stream.path_count) goto invalid;
			Bob_Path output_path = build_state_stream_path(&decoded_stream, output);
			existing = build_state_task_index(&decoded, output_path);
			if (existing == UINT32_MAX && decoded.task_count == UINT32_MAX) goto error;
			if (existing == UINT32_MAX && !build_state_reserve_tasks(&decoded, decoded.task_count + 1)) goto error;
			task.output = output_path;
			task.output_stamp = output_stamp;
			memcpy(task.fingerprint.bytes, fingerprint, sizeof(task.fingerprint.bytes));
			if (dependency_count) {
				task.dependencies.items = arena_push_zero_aligned(arena, (u64)dependency_count * sizeof(*task.dependencies.items), _Alignof(Bob_Path));
				if (!task.dependencies.items) goto error;
			}
			for (u32 dependency = 0; dependency < dependency_count; ++dependency) {
				u32 id;
				if (!build_state_decode_u32(&content, &id)) goto invalid;
				if (id == BUILD_STATE_PATH_ID_NONE || id > decoded_stream.path_count) goto invalid;
				task.dependencies.items[task.dependencies.count++] = build_state_stream_path(&decoded_stream, id);
			}
			if (existing == UINT32_MAX) decoded.tasks[decoded.task_count++] = task;
			else decoded.tasks[existing] = task;
		} break;

		case STATE_OP_REMOVE: {
			u32 output;
			if (!build_state_decode_u32(&content, &output)) goto invalid;
			if (content.cursor != content.size) goto invalid;
			if (output == BUILD_STATE_PATH_ID_NONE || output > decoded_stream.path_count) goto invalid;
			u32 index = build_state_task_index(&decoded, build_state_stream_path(&decoded_stream, output));
			if (index != UINT32_MAX) {
				if (index + 1 < decoded.task_count) memmove(decoded.tasks + index, decoded.tasks + index + 1, (u64)(decoded.task_count - index - 1) * sizeof(*decoded.tasks));
				--decoded.task_count;
			}
		} break;

		default: goto invalid;
		}

		if (content.cursor != content.size) goto invalid;
	}

	build_state_replace_unlocked(state, &decoded);
	build_state_stream_replace_paths(state_stream, &decoded_stream);
	return BUILD_STATE_STREAM_OK;

truncated:
	build_state_replace_unlocked(state, &decoded);
	build_state_stream_replace_paths(state_stream, &decoded_stream);
	return BUILD_STATE_STREAM_TRUNCATED;

invalid:
	arena_restore(arena, mark);
	build_state_replace_unlocked(state, &(Build_State){0});
	build_state_stream_replace_paths(state_stream, &(Build_State_Stream){0});
	return BUILD_STATE_STREAM_INVALID;

error:
	arena_restore(arena, mark);
	build_state_replace_unlocked(state, &(Build_State){0});
	build_state_stream_replace_paths(state_stream, &(Build_State_Stream){0});
	return BUILD_STATE_STREAM_ERROR;
}

Build_State_Stream_Result build_state_stream_replay(Build_State_Stream *state_stream, String stream)
{
	if (!build_state_stream_is_valid(state_stream)) return BUILD_STATE_STREAM_ERROR;
	Bob_Build *build = state_stream->build;
	Build_State *state = state_stream->state;
	platform_lock_mutex(&state->mutex);
	Build_State_Stream_Result result = build_state_stream_replay_unlocked(build, stream, state, state_stream);
	platform_unlock_mutex(&state->mutex);
	return result;
}

static b32 build_state_stream_append_bytes(String path, const void *data, u64 size)
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

static b32 build_state_stream_append_set_unlocked(String path, const Bob_Build *build, Build_State *state, Build_State_Stream *state_stream, Bob_Path output, Bob_Path_Array dependencies, u64 output_stamp, Bob_Fingerprint fingerprint)
{
	u32 first_new_path;
	u32 task_index;
	u64 append_size = 0;
	Scratch scratch;
	Arena *arena;
	Build_State_Encoder encoder = {0};
	Build_State_Task *task;
	if (!build || !state || !state->arena || !state_stream || !string_is_terminated(path) || path.size == 0) return false;
	arena = state->arena;
	first_new_path = state_stream->path_count;
	if (!build_state_set_unlocked(state, output, dependencies, fingerprint)) return false;
	if (build_state_stream_add_path(arena, state_stream, output) == BUILD_STATE_PATH_ID_NONE) return false;
	for (u32 i = 0; i < dependencies.count; ++i) {
		if (build_state_stream_add_path(arena, state_stream, dependencies.items[i]) == BUILD_STATE_PATH_ID_NONE) return false;
	}
	task_index = build_state_task_index(state, output);
	if (task_index == UINT32_MAX) return false;
	task = state->tasks + task_index;
	task->output_stamp = output_stamp;

	for (u32 i = first_new_path; i < state_stream->path_count; ++i) {
		String new_path = bob_path_string(build, state_stream->paths[i]);
		if (!build_state_size_add(&append_size, 1, BUILD_STATE_STREAM_RECORD_HEADER_SIZE + 8)) return false;
		if (!build_state_size_add(&append_size, 1, new_path.size)) return false;
	}
	if (!build_state_size_add(&append_size, 1, BUILD_STATE_STREAM_RECORD_HEADER_SIZE + 20 + BOB_FINGERPRINT_SIZE)) return false;
	if (!build_state_size_add(&append_size, task->dependencies.count, 4) || append_size > SIZE_MAX) return false;

	scratch = begin_different_scratch(arena);
	encoder.data = arena_push(scratch.arena, append_size);
	encoder.size = append_size;
	if (!encoder.data) {
		end_scratch(scratch);
		return false;
	}
	for (u32 i = first_new_path; i < state_stream->path_count; ++i) {
		if (!build_state_stream_encode_intern(&encoder, bob_path_string(build, state_stream->paths[i]))) goto failure;
	}
	if (!build_state_stream_encode_set(&encoder, state_stream, task)) goto failure;
	if (encoder.cursor != encoder.size || !build_state_stream_append_bytes(path, encoder.data, encoder.size)) goto failure;
	end_scratch(scratch);
	return true;

failure:
	end_scratch(scratch);
	return false;
}

b32 build_state_stream_append_set(Build_State_Stream *state_stream, String path, Bob_Path output, Bob_Path_Array dependencies, u64 output_stamp, Bob_Fingerprint fingerprint)
{
	if (!build_state_stream_is_valid(state_stream)) return false;
	Bob_Build *build = state_stream->build;
	Build_State *state = state_stream->state;
	platform_lock_mutex(&state->mutex);
	b32 result = build_state_stream_append_set_unlocked(path, build, state, state_stream, output, dependencies, output_stamp, fingerprint);
	platform_unlock_mutex(&state->mutex);
	return result;
}

static b32 build_state_stream_append_remove_unlocked(String path, Build_State *state, Build_State_Stream *state_stream, Bob_Path output)
{
	u8 bytes[BUILD_STATE_STREAM_RECORD_HEADER_SIZE + 8];
	Build_State_Path_Id output_id;
	Build_State_Encoder encoder = { bytes, sizeof(bytes), 0 };
	if (!state || !state_stream || !string_is_terminated(path) || path.size == 0) return false;
	output_id = build_state_stream_path_id(state_stream, output);
	if (build_state_task_index(state, output) == UINT32_MAX) return true;
	if (!build_state_stream_encode_remove(&encoder, output_id) || encoder.cursor != encoder.size) return false;
	if (!build_state_stream_append_bytes(path, encoder.data, encoder.size)) return false;
	return build_state_remove_unlocked(state, output);
}

b32 build_state_stream_append_remove(Build_State_Stream *state_stream, String path, Bob_Path output)
{
	if (!build_state_stream_is_valid(state_stream)) return false;
	Build_State *state = state_stream->state;
	platform_lock_mutex(&state->mutex);
	b32 result = build_state_stream_append_remove_unlocked(path, state, state_stream, output);
	platform_unlock_mutex(&state->mutex);
	return result;
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

static b32 build_state_stream_save_unlocked(String path, const Bob_Build *build, const Build_State *state, Build_State_Stream *state_stream)
{
	u64 state_mark;
	u64 stream_size;
	u64 arena_capacity = 64;
	Arena arena = {0};
	Build_State_Stream compacted = {0};
	String parent;
	String temporary = {0};
	String stream = {0};
	b32 result = false;

	if (!state || !state->arena || !state_stream || !string_is_terminated(path) || path.size == 0) return false;
	state_mark = arena_mark(state->arena);
	if (!build_state_stream_collect_paths(state->arena, state, &compacted)) goto done;
	if (!build_state_stream_size(build, state, &compacted, &stream_size) || stream_size > SIZE_MAX) goto done;
	if (!build_state_size_add(&arena_capacity, 1, stream_size)) goto done;
	if (!build_state_size_add(&arena_capacity, 3, path.size)) goto done;
	arena = arena_create(arena_capacity);
	arena_set_name(&arena, "build state stream");
	if (!arena.data) goto done;
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
	if (!build_state_stream_encode_unlocked(&arena, build, state, &compacted, &stream)) goto done;
	if (!bob_platform_write_entire_file(temporary, stream.data, (size_t)stream.size)) goto done;
	if (!platform_move_file(temporary.data, path.data, true)) goto done;
	result = true;

done:
	if (!result && temporary.data) platform_remove_file(temporary.data);
	if (result) build_state_stream_replace_paths(state_stream, &compacted);
	else arena_restore(state->arena, state_mark);
	arena_destroy(&arena);
	return result;
}

b32 build_state_stream_save(Build_State_Stream *state_stream, String path)
{
	if (!build_state_stream_is_valid(state_stream)) return false;
	Bob_Build *build = state_stream->build;
	Build_State *state = state_stream->state;
	platform_lock_mutex(&state->mutex);
	b32 result = build_state_stream_save_unlocked(path, build, state, state_stream);
	platform_unlock_mutex(&state->mutex);
	return result;
}

static Build_State_Load_Result build_state_stream_load_unlocked(Bob_Build *build, String path, Build_State *state, Build_State_Stream *state_stream)
{
	Bob_Platform_File_Info info;
	Arena source_arena = {0};
	String source;
	Build_State_Load_Result result;
	Build_State_Stream_Result stream_result;
	if (!state || !state->arena || !state_stream || !build || !string_is_terminated(path) || path.size == 0) return BUILD_STATE_LOAD_ERROR;
	build_state_replace_unlocked(state, &(Build_State){0});
	build_state_stream_replace_paths(state_stream, &(Build_State_Stream){0});
	if (!bob_platform_file_info(path, &info)) return BUILD_STATE_LOAD_MISSING;
	if (info.size == UINT64_MAX) return BUILD_STATE_LOAD_ERROR;
	source_arena = arena_create(info.size + 1);
	arena_set_name(&source_arena, "build state source");
	if (!source_arena.data) return BUILD_STATE_LOAD_ERROR;
	if (!bob_platform_read_entire_file(&source_arena, path, &source)) {
		arena_destroy(&source_arena);
		return BUILD_STATE_LOAD_ERROR;
	}
	stream_result = build_state_stream_replay_unlocked(build, source, state, state_stream);
	switch (stream_result) {
	case BUILD_STATE_STREAM_OK: result = BUILD_STATE_LOAD_OK; break;
	case BUILD_STATE_STREAM_TRUNCATED: result = BUILD_STATE_LOAD_RECOVERED; break;
	case BUILD_STATE_STREAM_INVALID: result = BUILD_STATE_LOAD_INVALID; break;
	default: result = BUILD_STATE_LOAD_ERROR; break;
	}
	arena_destroy(&source_arena);
	return result;
}

Build_State_Load_Result build_state_stream_load(Build_State_Stream *state_stream, String path)
{
	if (!build_state_stream_is_valid(state_stream)) return BUILD_STATE_LOAD_ERROR;
	Bob_Build *build = state_stream->build;
	Build_State *state = state_stream->state;
	platform_lock_mutex(&state->mutex);
	Build_State_Load_Result result = build_state_stream_load_unlocked(build, path, state, state_stream);
	platform_unlock_mutex(&state->mutex);
	return result;
}
