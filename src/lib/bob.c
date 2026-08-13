#include "bob.h"
#include "logger.h"
#include "platform.h"
#include "profiler.h"

#include <string.h>

typedef struct Bob_Node_Array
{
	Bob_Node **items;
	u32        count;
	u32        capacity;
}
Bob_Node_Array;

struct Bob_Node
{
	Bob_Node_Array     dependencies;
	Bob_Node_Array     dependents;
	u32                unfinished_dependencies;
	String             name;
	Bob_Node_Function *function;
	void              *user_data;
	Bob_Node_Result     result;
	Bob_Node_Status     state;
};

struct Bob
{
	Arena          arena;
	Bob_Interner  *interner;
	Bob_Path       build_root;
	Bob_Node     **nodes;
	u32            node_count;
	u32            node_capacity;
	Bob_Node     **ready;
	u32            ready_count;
	u32            ready_head;
	u32            terminal_count;
	Arena         *execution_arenas;
	u32            execution_arena_count;
	b32            prepared;
	b32            failed;
};

static void *bob_push(Bob *bob, u64 size, u64 alignment)
{
	return arena_push_zero_aligned(&bob->arena, size, alignment);
}

static b32 bob_reserve(Bob *bob, void **memory, u32 element_size, u32 count, u32 *capacity, u32 needed, u64 alignment)
{
	size_t new_capacity;
	void *new_memory;
	if (*capacity >= needed) return true;
	new_capacity = *capacity ? *capacity : 8;
	while (new_capacity < needed) {
		if (new_capacity > SIZE_MAX / 2) return false;
		new_capacity *= 2;
	}
	if (new_capacity > SIZE_MAX / element_size) return false;
	new_memory = bob_push(bob, new_capacity * element_size, alignment);
	if (!new_memory) return false;
	if (*memory && count) memcpy(new_memory, *memory, count * element_size);
	*memory = new_memory;
	*capacity = (u32)new_capacity;
	return true;
}

Bob_Atom bob_intern(Bob *bob, String value)
{
	return bob ? bob_interner_intern(bob->interner, value) : (Bob_Atom){0};
}

String bob_atom_string(const Bob *bob, Bob_Atom atom)
{
	return bob ? bob_interner_string(bob->interner, atom) : (String){0};
}

static b32 bob_path_is_absolute(String path)
{
	return path.size > 0 && (path.data[0] == '/' || path.data[0] == '\\' ||
		(path.size >= 3 && path.data[1] == ':' && (path.data[2] == '/' || path.data[2] == '\\')));
}

static b32 bob_path_platform_absolute(Arena *arena, String path, String *result)
{
	String terminated = path;
	if (!string_is_terminated(terminated)) {
		terminated = arena_push_string_copy(arena, path);
		if (!terminated.data) return false;
	}
	Platform_String_Result query = platform_get_absolute_path(terminated.data, NULL, 0);
	if (query.error || query.required_capacity == 0) return false;
	char *data = arena_reserve(arena, query.required_capacity);
	if (!data) return false;
	Platform_String_Result filled = platform_get_absolute_path(terminated.data, data, query.required_capacity);
	if (filled.error || !arena_push(arena, query.required_capacity)) return false;
	*result = string_from_data(data, filled.size);
	return true;
}

static String bob_path_normalize_separators(String path)
{
	u64 write = 0;
	for (u64 read = 0; read < path.size; ++read) {
		char character = path.data[read] == '\\' ? '/' : path.data[read];
		b32 preserve_unc_prefix = write < 2 && read < 2 && character == '/';
		if (character == '/' && write > 0 && path.data[write - 1] == '/' && !preserve_unc_prefix) continue;
		path.data[write++] = character;
	}
	if (write >= 2 && path.data[1] == ':' && path.data[0] >= 'a' && path.data[0] <= 'z') path.data[0] -= 'a' - 'A';
	b32 root = write == 1 && path.data[0] == '/';
	b32 drive_root = write == 3 && path.data[1] == ':' && path.data[2] == '/';
	if (write > 0 && path.data[write - 1] == '/' && !root && !drive_root) --write;
	path.data[write] = 0;
	path.size = write;
	return path;
}

b32 bob_path_resolve(Bob *bob, Bob_Path directory, String source, Bob_Path *result)
{
	if (!bob || !result || !source.data || source.size == 0) return false;
	Scratch scratch = begin_scratch();
	String candidate = source;
	if (!bob_path_is_absolute(source)) {
		String base = bob_path_string(bob, directory);
		if (!base.data) goto failure;
		void *start = arena_top(scratch.arena);
		arena_append_str(scratch.arena, base);
		if (base.size && base.data[base.size - 1] != '/') arena_append_char(scratch.arena, '/');
		arena_append_str(scratch.arena, source);
		candidate = arena_string_from(scratch.arena, start);
		arena_finalize_string(scratch.arena, candidate);
	}
	String absolute;
	if (!bob_path_platform_absolute(scratch.arena, candidate, &absolute)) goto failure;
	absolute = bob_path_normalize_separators(absolute);
	if (!absolute.data || absolute.size == 0) goto failure;
	Bob_Atom atom = bob_intern(bob, absolute);
	if (!atom.id) goto failure;
	*result = (Bob_Path){ atom };
	end_scratch(scratch);
	return true;

failure:
	end_scratch(scratch);
	return false;
}

String bob_path_string(const Bob *bob, Bob_Path path)
{
	return bob_atom_string(bob, path.atom);
}

b32 bob_path_is_valid(Bob_Path path)
{
	return bob_atom_is_valid(path.atom);
}

Bob_Path bob_build_root(const Bob *bob)
{
	return bob ? bob->build_root : (Bob_Path){0};
}

void *bob_allocate(Bob *bob, u64 size, u64 alignment)
{
	if (!bob || bob->prepared || alignment == 0) return NULL;
	return bob_push(bob, size, alignment);
}

String bob_copy_string(Bob *bob, String string)
{
	if (!bob || bob->prepared || (!string.data && string.size)) return (String){0};
	return arena_push_string_copy(&bob->arena, string);
}

static b32 node_array_push(Bob *bob, Bob_Node_Array *array, Bob_Node *node)
{
	if (!bob_reserve(bob, (void **)&array->items, sizeof(*array->items), array->count, &array->capacity, array->count + 1, _Alignof(Bob_Node *))) {
		return false;
	}
	array->items[array->count++] = node;
	return true;
}

static b32 valid_node(const Bob *bob, const Bob_Node *node)
{
	if (!bob || !node) return false;
	for (u32 i = 0; i < bob->node_count; ++i) {
		if (bob->nodes[i] == node) return true;
	}
	return false;
}

static void enqueue_ready(Bob *bob, Bob_Node *node)
{
	/* A node is enqueued at most once, so node_count slots are sufficient. */
	bob->ready[bob->ready_count++] = node;
	node->state = BOB_NODE_READY;
}

Bob *bob_create(void)
{
	Arena arena = arena_create(0);
	Bob *bob;
	Scratch scratch;
	String directory;
	if (!arena.data) return NULL;
	arena_set_name(&arena, "Bob graph");
	bob = arena_push_zero_aligned(&arena, sizeof(*bob), _Alignof(Bob));
	if (!bob) {
		arena_destroy(&arena);
		return NULL;
	}
	bob->arena = arena;
	bob->interner = bob_interner_create(&bob->arena);
	if (!bob->interner) {
		arena_destroy(&arena);
		return NULL;
	}
	scratch = begin_scratch();
	Platform_String_Result query = platform_get_current_directory(NULL, 0);
	if (query.error || query.required_capacity == 0) goto failure;
	char *data = arena_reserve(scratch.arena, query.required_capacity);
	if (!data) goto failure;
	Platform_String_Result filled = platform_get_current_directory(data, query.required_capacity);
	if (filled.error || !arena_push(scratch.arena, query.required_capacity)) goto failure;
	directory = string_from_data(data, filled.size);
	if (!bob_path_resolve(bob, (Bob_Path){0}, directory, &bob->build_root)) goto failure;
	end_scratch(scratch);
	return bob;

failure:
	end_scratch(scratch);
	bob_interner_destroy(bob->interner);
	arena_destroy(&arena);
	return NULL;
}

void bob_destroy(Bob *bob)
{
	Arena arena;
	if (!bob) return;
	for (u32 i = 0; i < bob->execution_arena_count; ++i) {
		arena_destroy(bob->execution_arenas + i);
	}
	bob_interner_destroy(bob->interner);
	arena = bob->arena;
	arena_destroy(&arena);
}

Bob_Error bob_add_node(Bob *bob, Bob_Node_Desc description, Bob_Node **node_out)
{
	Bob_Node *node;
	if (!bob || !description.name.data || !node_out) return BOB_ERROR_INVALID_NODE;
	if (bob->prepared) return BOB_ERROR_ALREADY_PREPARED;
	if (!bob_reserve(bob, (void **)&bob->nodes, sizeof(*bob->nodes), bob->node_count, &bob->node_capacity, bob->node_count + 1, _Alignof(Bob_Node *))) {
		return BOB_ERROR_OUT_OF_MEMORY;
	}
	node = bob_push(bob, sizeof(*node), _Alignof(Bob_Node));
	if (!node) return BOB_ERROR_OUT_OF_MEMORY;
	node->name = arena_push_string_copy(&bob->arena, description.name);
	if (!node->name.data) return BOB_ERROR_OUT_OF_MEMORY;
	node->function = description.function;
	node->user_data = description.user_data;
	node->state = BOB_NODE_PENDING;
	bob->nodes[bob->node_count++] = node;
	*node_out = node;
	return BOB_OK;
}

Bob_Error bob_set_node(Bob *bob, Bob_Node *node, Bob_Node_Desc description)
{
	String name;
	if (!valid_node(bob, node)) return BOB_ERROR_INVALID_NODE;
	if (bob->prepared) return BOB_ERROR_ALREADY_PREPARED;
	name = node->name;
	if (description.name.data) {
		name = arena_push_string_copy(&bob->arena, description.name);
		if (!name.data) return BOB_ERROR_OUT_OF_MEMORY;
	}
	node->name = name;
	node->function = description.function;
	node->user_data = description.user_data;
	return BOB_OK;
}

Bob_Error bob_set_node_action(Bob *bob, Bob_Node *node, Bob_Node_Function *function, void *user_data)
{
	if (!valid_node(bob, node)) return BOB_ERROR_INVALID_NODE;
	if (bob->prepared) return BOB_ERROR_ALREADY_PREPARED;

	node->function = function;
	node->user_data = user_data;
	return BOB_OK;
}

Bob_Error bob_add_dependency(Bob *bob, Bob_Node *node, Bob_Node *dependency)
{
	if (!valid_node(bob, node) || !valid_node(bob, dependency)) {
		return BOB_ERROR_INVALID_NODE;
	}
	if (bob->prepared) return BOB_ERROR_ALREADY_PREPARED;
	if (node == dependency) return BOB_ERROR_SELF_DEPENDENCY;
	for (u32 i = 0; i < node->dependencies.count; ++i) {
		if (node->dependencies.items[i] == dependency) {
			return BOB_ERROR_DUPLICATE_DEPENDENCY;
		}
	}
	if (!node_array_push(bob, &node->dependencies, dependency)) {
		return BOB_ERROR_OUT_OF_MEMORY;
	}
	if (!node_array_push(bob, &dependency->dependents, node)) {
		--node->dependencies.count;
		return BOB_ERROR_OUT_OF_MEMORY;
	}
	return BOB_OK;
}

static Bob_Error validate_acyclic(Bob *bob)
{
	u64 mark;
	Bob_Node **queue;
	u32 head = 0;
	u32 count = 0;
	u32 visited = 0;
	if (bob->node_count == 0) return BOB_OK;
	mark = arena_mark(&bob->arena);
	queue = bob_push(bob, bob->node_count * sizeof(*queue), _Alignof(Bob_Node *));
	if (!queue) {
		arena_restore(&bob->arena, mark);
		return BOB_ERROR_OUT_OF_MEMORY;
	}
	for (u32 i = 0; i < bob->node_count; ++i) {
		Bob_Node *node = bob->nodes[i];
		node->unfinished_dependencies = node->dependencies.count;
		if (node->unfinished_dependencies == 0) queue[count++] = node;
	}
	while (head < count) {
		Bob_Node *node = queue[head++];
		++visited;
		for (u32 i = 0; i < node->dependents.count; ++i) {
			Bob_Node *dependent = node->dependents.items[i];
			--dependent->unfinished_dependencies;
			if (dependent->unfinished_dependencies == 0) queue[count++] = dependent;
		}
	}
	arena_restore(&bob->arena, mark);
	return visited == bob->node_count ? BOB_OK : BOB_ERROR_CYCLE;
}

Bob_Error bob_prepare(Bob *bob)
{
	Bob_Error result;
	if (!bob) return BOB_ERROR_INVALID_NODE;
	if (bob->prepared) return BOB_ERROR_ALREADY_PREPARED;
	result = validate_acyclic(bob);
	if (result != BOB_OK) return result;
	if (bob->node_count > 0) {
		bob->ready = bob_push(bob, bob->node_count * sizeof(*bob->ready),
		_Alignof(Bob_Node *));
		if (!bob->ready) return BOB_ERROR_OUT_OF_MEMORY;
	}
	bob->prepared = true;
	for (u32 i = 0; i < bob->node_count; ++i) {
		Bob_Node *node = bob->nodes[i];
		node->unfinished_dependencies = node->dependencies.count;
		if (node->unfinished_dependencies == 0) enqueue_ready(bob, node);
	}
	return BOB_OK;
}

b32 bob_take_ready(Bob *bob, Bob_Node **node_out)
{
	Bob_Node *node;
	if (!bob || !bob->prepared || !node_out || bob->ready_head == bob->ready_count) {
		return false;
	}
	node = bob->ready[bob->ready_head++];
	node->state = BOB_NODE_RUNNING;
	*node_out = node;
	return true;
}

static void block_node_and_dependents(Bob *bob, Bob_Node *node)
{
	if (node->state == BOB_NODE_BLOCKED || node->state == BOB_NODE_SUCCEEDED || node->state == BOB_NODE_FAILED) return;
	node->state = BOB_NODE_BLOCKED;
	node->result = (Bob_Node_Result){0};
	++ bob->terminal_count;
	for (u32 i = 0; i < node->dependents.count; ++i) {
		block_node_and_dependents(bob, node->dependents.items[i]);
	}
}

Bob_Error bob_complete_result(Bob *bob, Bob_Node *node, Bob_Node_Result result)
{
	if (!bob || !bob->prepared) return BOB_ERROR_NOT_PREPARED;
	if (!valid_node(bob, node)) return BOB_ERROR_INVALID_NODE;
	if (node->state != BOB_NODE_RUNNING) return BOB_ERROR_INVALID_STATE;
	node->result = result;
	node->state = result.succeeded ? BOB_NODE_SUCCEEDED : BOB_NODE_FAILED;
	++ bob->terminal_count;
	if (!result.succeeded) {
		bob->failed = true;
		for (u32 i = 0; i < node->dependents.count; ++i) {
			block_node_and_dependents(bob, node->dependents.items[i]);
		}
		return BOB_OK;
	}
	for (u32 i = 0; i < node->dependents.count; ++i) {
		Bob_Node *dependent = node->dependents.items[i];
		if (dependent->state != BOB_NODE_PENDING) continue;
		-- dependent->unfinished_dependencies;
		if (dependent->unfinished_dependencies == 0) enqueue_ready(bob, dependent);
	}
	return BOB_OK;
}

Bob_Error bob_complete(Bob *bob, Bob_Node *node, b32 succeeded)
{
	return bob_complete_result(bob, node, (Bob_Node_Result){
		.succeeded = succeeded,
		.changed = succeeded,
	});
}

b32 bob_is_finished(const Bob *bob)
{
	return bob && bob->prepared && bob->terminal_count == bob->node_count;
}

b32 bob_is_prepared(const Bob *bob)
{
	return bob && bob->prepared;
}

b32 bob_has_failed(const Bob *bob)
{
	return bob && bob->failed;
}

u32 bob_node_count(const Bob *bob)
{
	return bob ? bob->node_count : 0;
}

Bob_Node *bob_node_at(const Bob *bob, u32 index)
{
	return bob && index < bob->node_count ? bob->nodes[index] : NULL;
}

const char *bob_node_name(const Bob_Node *node)
{
	return node ? node->name.data : NULL;
}

Bob_Node_Status bob_node_state(const Bob_Node *node)
{
	return node ? node->state : BOB_NODE_BLOCKED;
}

Bob_Node_Result bob_node_result(const Bob_Node *node)
{
	return node ? node->result : (Bob_Node_Result){0};
}

Bob_Node_Function *bob_node_function(const Bob_Node *node)
{
	return node ? node->function : NULL;
}

void *bob_node_user_data(const Bob_Node *node)
{
	return node ? node->user_data : NULL;
}

u32 bob_dependency_count(const Bob_Node *node)
{
	return node ? node->dependencies.count : 0;
}

Bob_Node *bob_dependency(const Bob_Node *node, u32 index)
{
	return node && index < node->dependencies.count ? node->dependencies.items[index] : NULL;
}

const char *bob_error_string(Bob_Error result)
{
	switch (result) {
		case BOB_OK: return "ok";
		case BOB_ERROR_OUT_OF_MEMORY: return "out of memory";
		case BOB_ERROR_INVALID_TASK: return "invalid node";
		case BOB_ERROR_DUPLICATE_DEPENDENCY: return "duplicate dependency";
		case BOB_ERROR_SELF_DEPENDENCY: return "self dependency";
		case BOB_ERROR_ALREADY_PREPARED: return "Bob already prepared";
		case BOB_ERROR_NOT_PREPARED: return "Bob not prepared";
		case BOB_ERROR_INVALID_STATE: return "invalid node state";
		case BOB_ERROR_CYCLE: return "dependency cycle";
	}
	return "unknown Bob result";
}

typedef struct Bob_Executor Bob_Executor;

typedef struct Bob_Completion
{
	Bob_Node        *node;
	Bob_Node_Result  result;
}
Bob_Completion;

typedef struct Bob_Worker
{
	Bob_Executor   *executor;
	Platform_Thread thread;
	Arena          *output;
}
Bob_Worker;

struct Bob_Executor
{
	Bob                  *bob;
	Bob_Exec_Params   options;
	Bob_Worker           *workers;
	u32                   worker_count;
	u32                   thread_count;
	u32                   running;
	Bob_Node            **work;
	u32                   work_count;
	Bob_Completion      **completions;
	u32                   completion_count;
	Platform_Mutex        mutex;
	Platform_Condition    work_available;
	Platform_Condition    completion_available;
	b32                   synchronization_initialized;
	b32                   stopping;
};

static void request_stop_locked(Bob_Executor *executor)
{
	executor->stopping = true;
	platform_broadcast_condition(&executor->work_available);
	platform_broadcast_condition(&executor->completion_available);
}

static u32 worker_main(void *data)
{
	Bob_Worker *worker = data;
	Bob_Executor *executor = worker->executor;
	for (;;)
	{
		Bob_Node *node;
		Bob_Completion *completion;
		Bob_Node_Context context;
		platform_lock_mutex(&executor->mutex);
		while (!executor->stopping && executor->work_count == 0) {
			if (platform_wait_condition(&executor->work_available, &executor->mutex).error) {
				log_fatal("failed waiting for worker queue");
				request_stop_locked(executor);
			}
		}
		if (executor->stopping) {
			platform_unlock_mutex(&executor->mutex);
			break;
		}
		node = executor->work[--executor->work_count];
		platform_unlock_mutex(&executor->mutex);

		completion = arena_push_zero_aligned(worker->output, sizeof(*completion), _Alignof(Bob_Completion));
		if (!completion) {
			log_fatal("worker output arena exhausted");
			platform_lock_mutex(&executor->mutex);
			request_stop_locked(executor);
			platform_unlock_mutex(&executor->mutex);
			break;
		}
		completion->node = node;
		context = (Bob_Node_Context){
			.bob = executor->bob,
			.node = node,
			.arena = worker->output,
			.execution_data = executor->options.user_data,
		};
		{
			Profile_Scope scope = profile_scope_begin("node action");
			completion->result = node->function(&context, node->user_data);
			profile_scope_end(&scope);
		}
		if (!completion->result.succeeded) completion->result.changed = false;

		platform_lock_mutex(&executor->mutex);
		executor->completions[executor->completion_count++] = completion;
		platform_signal_condition(&executor->completion_available);
		platform_unlock_mutex(&executor->mutex);
	}
	destroy_global_scratch();
	return 0;
}

static void stop_workers(Bob_Executor *executor)
{
	if (executor->synchronization_initialized) {
		platform_lock_mutex(&executor->mutex);
		request_stop_locked(executor);
		platform_unlock_mutex(&executor->mutex);
	}
	for (u32 i = 0; i < executor->thread_count; ++i) {
		platform_join_thread(executor->workers[i].thread);
		platform_close_thread(&executor->workers[i].thread);
	}
}

static void dispatch_ready(Bob_Executor *executor)
{
	u32 previous_work_count;
	Bob_Node *node;
	platform_lock_mutex(&executor->mutex);
	previous_work_count = executor->work_count;
	while (executor->running < executor->worker_count && bob_take_ready(executor->bob, &node)) {
		executor->work[executor->work_count++] = node;
		++executor->running;
	}
	if (executor->work_count > previous_work_count) {
		platform_broadcast_condition(&executor->work_available);
	}
	platform_unlock_mutex(&executor->mutex);
}

b32 bob_execute(Bob *bob, Bob_Exec_Params options)
{
	Bob_Executor executor = {0};
	Bob_Error prepare_result;
	Scratch scratch;
	b32 internal_error = false;
	u32 node_count;

	if (!bob || options.worker_count == 0) return false;
	node_count = bob_node_count(bob);
	for (u32 i = 0; i < node_count; ++i) {
		Bob_Node *node = bob_node_at(bob, i);
		if (!node || !node->function) {
			log_error("node has no action: %s", node ? bob_node_name(node) : "<invalid>");
			return false;
		}
	}
	{
		Profile_Scope scope = profile_scope_begin("prepare Bob graph");
		prepare_result = bob_prepare(bob);
		profile_scope_end(&scope);
	}
	if (prepare_result != BOB_OK) {
		log_error("unable to prepare Bob graph: %s", bob_error_string(prepare_result));
		return false;
	}
	if (bob_is_finished(bob)) return true;
	if (options.worker_count > node_count) options.worker_count = node_count;

	executor.bob = bob;
	executor.options = options;
	executor.worker_count = options.worker_count;
	scratch = begin_scratch();
	executor.workers = arena_push_zero_aligned(scratch.arena, executor.worker_count * sizeof(*executor.workers), _Alignof(Bob_Worker));
	executor.work = arena_push_zero_aligned(scratch.arena, executor.worker_count * sizeof(*executor.work), _Alignof(Bob_Node *));
	executor.completions = arena_push_zero_aligned(scratch.arena, executor.worker_count * sizeof(*executor.completions), _Alignof(Bob_Completion *));
	bob->execution_arenas = arena_push_zero_aligned(&bob->arena, executor.worker_count * sizeof(*bob->execution_arenas), _Alignof(Arena));
	internal_error = !executor.workers || !executor.work || !executor.completions || !bob->execution_arenas;
	if (internal_error) goto cleanup;

	platform_init_mutex(&executor.mutex);
	platform_init_condition(&executor.work_available);
	platform_init_condition(&executor.completion_available);
	executor.synchronization_initialized = true;
	for (u32 i = 0; i < executor.worker_count; ++i) {
		Bob_Worker *worker = executor.workers + i;
		worker->executor = &executor;
		worker->output = bob->execution_arenas + i;
		*worker->output = arena_create(MEGABYTES(256));
		arena_set_name(worker->output, "worker output");
		internal_error = !worker->output->data;
		if (internal_error) goto cleanup;
		++bob->execution_arena_count;
		Platform_Thread_Start_Result start = platform_start_thread(worker_main, worker);
		internal_error = start.error != PLATFORM_ERROR_NONE;
		if (internal_error) goto cleanup;
		worker->thread = start.thread;
		++executor.thread_count;
	}

	while (!internal_error && !bob_is_finished(bob)) {
		Bob_Completion *completion = NULL;
		dispatch_ready(&executor);
		if (bob_is_finished(bob)) break;
		if (executor.running == 0) {
			internal_error = true;
			break;
		}
		platform_lock_mutex(&executor.mutex);
		while (!executor.stopping && executor.completion_count == 0) {
			if (platform_wait_condition(&executor.completion_available, &executor.mutex).error) {
				log_fatal("failed waiting for worker completion");
				request_stop_locked(&executor);
			}
		}
		if (executor.completion_count > 0) {
			completion = executor.completions[--executor.completion_count];
		}
		platform_unlock_mutex(&executor.mutex);
		if (!completion) {
			internal_error = true;
			break;
		}
		if (bob_complete_result(bob, completion->node, completion->result) != BOB_OK) internal_error = true;
		-- executor.running;
		if (!internal_error) dispatch_ready(&executor);
		if (executor.options.completed) {
			executor.options.completed(completion->node, completion->result, executor.options.user_data);
		}
	}

	cleanup:
	stop_workers(&executor);
	if (executor.synchronization_initialized) {
		platform_destroy_condition(&executor.completion_available);
		platform_destroy_condition(&executor.work_available);
		platform_destroy_mutex(&executor.mutex);
	}
	end_scratch(scratch);
	return !internal_error && !bob_has_failed(bob);
}
