#include "bob_internal.h"
#include "logger.h"
#include "platform.h"
#include "profiler.h"

typedef struct Bob_Execution_Node
{
	u32             unfinished_dependencies;
	Bob_Node_Result result;
	Bob_Node_Status state;
}
Bob_Execution_Node;

struct Bob_Execution
{
	Arena               arena;
	Bob                *bob;
	Bob_Execution_Node *nodes;

	// TODO(RJ): this is scoped to the execution function, but we keep here for tests
	Bob_Node           **ready;
	u32                  ready_count;
	u32                  ready_head;

	u32                  terminal_count;
	Arena               *output_arenas;
	u32                  output_arena_count;
	b32                  failed;
	b32                  automatically_driven;
	b32                  manually_driven;
};

static Bob_Execution_Node *execution_node(Bob_Execution *execution, const Bob_Node *node)
{
	if (!execution || !bob_valid_node(execution->bob, node)) return NULL;
	return execution->nodes + node->index;
}

static const Bob_Execution_Node *execution_node_const(const Bob_Execution *execution, const Bob_Node *node)
{
	if (!execution || !bob_valid_node(execution->bob, node)) return NULL;
	return execution->nodes + node->index;
}

static void enqueue_ready(Bob_Execution *execution, Bob_Node *node)
{
	execution->ready[execution->ready_count++] = node;
	execution->nodes[node->index].state = BOB_NODE_READY;
}

Bob_Error bob_execution_create(Bob *bob, Bob_Execution **execution_out)
{
	Arena arena;
	Bob_Execution *execution;
	u32 visited = 0;
	if (!bob || !execution_out) return BOB_ERROR_INVALID_NODE;
	*execution_out = NULL;
	arena = arena_create(0);
	if (!arena.data) return BOB_ERROR_OUT_OF_MEMORY;
	arena_set_name(&arena, "Bob execution");
	execution = arena_push_zero_aligned(&arena, sizeof(*execution), _Alignof(Bob_Execution));
	if (!execution) goto out_of_memory;
	execution->arena = arena;
	execution->bob = bob;
	if (bob->node_count) {
		execution->nodes = arena_push_zero_aligned(&execution->arena, (u64)bob->node_count * sizeof(*execution->nodes), _Alignof(Bob_Execution_Node));
		execution->ready = arena_push_zero_aligned(&execution->arena, (u64)bob->node_count * sizeof(*execution->ready), _Alignof(Bob_Node *));
		if (!execution->nodes || !execution->ready) goto out_of_memory;
	}

	for (u32 i = 0; i < bob->node_count; ++i) {
		execution->nodes[i].unfinished_dependencies = bob->nodes[i]->dependencies.count;
		if (execution->nodes[i].unfinished_dependencies == 0) execution->ready[execution->ready_count++] = bob->nodes[i];
	}
	while (visited < execution->ready_count) {
		Bob_Node *node = execution->ready[visited++];
		for (u32 i = 0; i < node->dependents.count; ++i) {
			Bob_Node *dependent = node->dependents.items[i];
			Bob_Execution_Node *dependent_state = execution->nodes + dependent->index;
			--dependent_state->unfinished_dependencies;
			if (dependent_state->unfinished_dependencies == 0) execution->ready[execution->ready_count++] = dependent;
		}
	}
	if (visited != bob->node_count) {
		arena = execution->arena;
		arena_destroy(&arena);
		return BOB_ERROR_CYCLE;
	}

	execution->ready_count = 0;
	for (u32 i = 0; i < bob->node_count; ++i) {
		Bob_Node *node = bob->nodes[i];
		execution->nodes[i] = (Bob_Execution_Node){ .unfinished_dependencies = node->dependencies.count };
		if (node->dependencies.count == 0) enqueue_ready(execution, node);
	}
	bob->sealed = true;
	++bob->execution_count;
	*execution_out = execution;
	return BOB_OK;

out_of_memory:
	arena_destroy(&arena);
	return BOB_ERROR_OUT_OF_MEMORY;
}

void bob_execution_destroy(Bob_Execution *execution)
{
	Arena arena;
	if (!execution) return;
	for (u32 i = 0; i < execution->output_arena_count; ++i) arena_destroy(execution->output_arenas + i);
	ASSERT(execution->bob);
	ASSERT(execution->bob->execution_count > 0);
	--execution->bob->execution_count;
	arena = execution->arena;
	arena_destroy(&arena);
}

static b32 take_ready(Bob_Execution *execution, Bob_Node **node_out)
{
	Bob_Node *node;
	if (!execution || !node_out || execution->ready_head == execution->ready_count) return false;
	node = execution->ready[execution->ready_head++];
	execution->nodes[node->index].state = BOB_NODE_RUNNING;
	*node_out = node;
	return true;
}

b32 bob_execution_take_ready(Bob_Execution *execution, Bob_Node **node_out)
{
	if (!execution || execution->automatically_driven) return false;
	execution->manually_driven = true;
	return take_ready(execution, node_out);
}

static void block_node_and_dependents(Bob_Execution *execution, Bob_Node *node)
{
	Bob_Execution_Node *state = execution->nodes + node->index;
	if (state->state == BOB_NODE_BLOCKED || state->state == BOB_NODE_SUCCEEDED || state->state == BOB_NODE_FAILED) return;
	state->state = BOB_NODE_BLOCKED;
	state->result = (Bob_Node_Result){0};
	++execution->terminal_count;
	for (u32 i = 0; i < node->dependents.count; ++i) block_node_and_dependents(execution, node->dependents.items[i]);
}

static Bob_Error complete_result(Bob_Execution *execution, Bob_Node *node, Bob_Node_Result result)
{
	Bob_Execution_Node *state = execution_node(execution, node);
	if (!state) return BOB_ERROR_INVALID_NODE;
	if (state->state != BOB_NODE_RUNNING) return BOB_ERROR_INVALID_STATE;
	state->result = result;
	state->state = result.succeeded ? BOB_NODE_SUCCEEDED : BOB_NODE_FAILED;
	++execution->terminal_count;
	if (!result.succeeded) {
		execution->failed = true;
		for (u32 i = 0; i < node->dependents.count; ++i) block_node_and_dependents(execution, node->dependents.items[i]);
		return BOB_OK;
	}
	for (u32 i = 0; i < node->dependents.count; ++i) {
		Bob_Node *dependent = node->dependents.items[i];
		Bob_Execution_Node *dependent_state = execution->nodes + dependent->index;
		if (dependent_state->state != BOB_NODE_PENDING) continue;
		--dependent_state->unfinished_dependencies;
		if (dependent_state->unfinished_dependencies == 0) enqueue_ready(execution, dependent);
	}
	return BOB_OK;
}

Bob_Error bob_execution_complete_result(Bob_Execution *execution, Bob_Node *node, Bob_Node_Result result)
{
	if (!execution) return BOB_ERROR_INVALID_NODE;
	if (execution->automatically_driven) return BOB_ERROR_INVALID_STATE;
	execution->manually_driven = true;
	return complete_result(execution, node, result);
}

Bob_Error bob_execution_complete(Bob_Execution *execution, Bob_Node *node, b32 succeeded)
{
	return bob_execution_complete_result(execution, node, (Bob_Node_Result){
		.succeeded = succeeded,
		.changed = succeeded,
	});
}

b32 bob_execution_is_finished(const Bob_Execution *execution)
{
	ASSERT(execution);
	return execution->terminal_count == execution->bob->node_count;
}

b32 bob_execution_has_failed(const Bob_Execution *execution)
{
	ASSERT(execution);
	return execution->failed;
}

Bob_Node_Status bob_execution_node_state(const Bob_Execution *execution, const Bob_Node *node)
{
	const Bob_Execution_Node *state = execution_node_const(execution, node);
	return state ? state->state : BOB_NODE_PENDING;
}

Bob_Node_Result bob_execution_node_result(const Bob_Execution *execution, const Bob_Node *node)
{
	const Bob_Execution_Node *state = execution_node_const(execution, node);
	return state ? state->result : (Bob_Node_Result){0};
}

typedef struct Bob_Executor Bob_Executor;

typedef struct Bob_Worker
{
	Bob_Executor   *executor;
	Platform_Thread thread;
	Arena          *output;
}
Bob_Worker;

struct Bob_Executor
{
	Bob_Execution        *execution;
	Bob_Exec_Params       options;
	Bob_Worker           *workers;
	u32                   worker_count;
	u32                   thread_count;
	u32                   running;
	Bob_Node            **work;
	u32                   work_count;
	Bob_Event            *events;
	u32                   event_capacity;
	u32                   event_head;
	u32                   event_count;
	Platform_Mutex        mutex;
	Platform_Condition    work_available;
	Platform_Condition    event_available;
	b32                   stopping;
};

static void request_stop_locked(Bob_Executor *executor)
{
	executor->stopping = true;
	platform_broadcast_condition(&executor->work_available);
	platform_broadcast_condition(&executor->event_available);
}

static b32 enqueue_event_locked(Bob_Executor *executor, Bob_Event event)
{
	if (executor->event_count == executor->event_capacity) return false;
	u32 index = (executor->event_head + executor->event_count) % executor->event_capacity;
	executor->events[index] = event;
	++executor->event_count;
	platform_signal_condition(&executor->event_available);
	return true;
}

static b32 dequeue_event_locked(Bob_Executor *executor, Bob_Event *event)
{
	if (executor->event_count == 0) return false;
	*event = executor->events[executor->event_head];
	executor->event_head = (executor->event_head + 1) % executor->event_capacity;
	--executor->event_count;
	return true;
}

static u32 worker_main(void *data)
{
	Bob_Worker *worker = data;
	Bob_Executor *executor = worker->executor;
	for (;;) {
		Bob_Node *node;
		Bob_Event completion;
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
		if (!enqueue_event_locked(executor, (Bob_Event){ .type = BOB_EVENT_STARTED, .node = node })) {
			log_fatal("executor event queue exhausted");
			request_stop_locked(executor);
			platform_unlock_mutex(&executor->mutex);
			break;
		}
		platform_unlock_mutex(&executor->mutex);

		completion = (Bob_Event){ .type = BOB_EVENT_COMPLETED, .node = node };
		context = (Bob_Node_Context){
			.execution = executor->execution,
			.bob = executor->execution->bob,
			.node = node,
			.arena = worker->output,
			.execution_data = executor->options.user_data,
		};
		{
			Profile_Scope scope = profile_scope_begin("node action");
			completion.result = node->function(&context, node->user_data);
			profile_scope_end(&scope);
		}
		if (!completion.result.succeeded) completion.result.changed = false;

		platform_lock_mutex(&executor->mutex);
		if (!enqueue_event_locked(executor, completion)) {
			log_fatal("executor event queue exhausted");
			request_stop_locked(executor);
		}
		platform_unlock_mutex(&executor->mutex);
	}
	destroy_global_scratch();
	return 0;
}

static void dispatch_ready(Bob_Executor *executor)
{
	u32 previous_work_count;
	Bob_Node *node;
	platform_lock_mutex(&executor->mutex);
	previous_work_count = executor->work_count;
	while (executor->running < executor->worker_count && take_ready(executor->execution, &node)) {
		executor->work[executor->work_count++] = node;
		++executor->running;
	}
	if (executor->work_count > previous_work_count) platform_broadcast_condition(&executor->work_available);
	platform_unlock_mutex(&executor->mutex);
}

b32 bob_execute(Bob_Execution *execution, Bob_Exec_Params options)
{
	Bob_Executor executor = { .execution = execution, .options = options };
	Scratch scratch;
	b32 internal_error = false;
	b32 synchronization_initialized = false;
	u32 node_count;
	if (!execution || execution->automatically_driven || execution->manually_driven || options.worker_count == 0) return false;
	execution->automatically_driven = true;
	node_count = execution->bob->node_count;
	for (u32 i = 0; i < node_count; ++i) {
		Bob_Node *node = execution->bob->nodes[i];
		if (!node->function) {
			log_error("node has no action: %s", bob_node_name(node));
			return false;
		}
	}
	if (bob_execution_is_finished(execution)) return true;
	if (options.worker_count > node_count) options.worker_count = node_count;
	executor.options = options;
	executor.worker_count = options.worker_count;
	scratch = begin_scratch();
	executor.workers = arena_push_zero_aligned(scratch.arena, executor.worker_count * sizeof(*executor.workers), _Alignof(Bob_Worker));
	executor.work = arena_push_zero_aligned(scratch.arena, executor.worker_count * sizeof(*executor.work), _Alignof(Bob_Node *));
	if (executor.worker_count > UINT32_MAX / 2) {
		internal_error = true;
		goto cleanup;
	}
	executor.event_capacity = executor.worker_count * 2;
	executor.events = arena_push_zero_aligned(scratch.arena, executor.event_capacity * sizeof(*executor.events), _Alignof(Bob_Event));
	execution->output_arenas = arena_push_zero_aligned(&execution->arena, executor.worker_count * sizeof(*execution->output_arenas), _Alignof(Arena));
	internal_error = !executor.workers || !executor.work || !executor.events || !execution->output_arenas;
	if (internal_error) goto cleanup;

	platform_init_mutex(&executor.mutex);
	platform_init_condition(&executor.work_available);
	platform_init_condition(&executor.event_available);
	synchronization_initialized = true;
	for (u32 i = 0; i < executor.worker_count; ++i) {
		Bob_Worker *worker = executor.workers + i;
		worker->executor = &executor;
		worker->output = execution->output_arenas + i;
		*worker->output = arena_create(MEGABYTES(256));
		arena_set_name(worker->output, "worker output");
		internal_error = !worker->output->data;
		if (internal_error) goto cleanup;
		++execution->output_arena_count;
		Platform_Thread_Start_Result start = platform_start_thread(worker_main, worker);
		internal_error = start.error != PLATFORM_ERROR_NONE;
		if (internal_error) goto cleanup;
		worker->thread = start.thread;
		++executor.thread_count;
	}

	while (!internal_error && !bob_execution_is_finished(execution)) {
		Bob_Event event = {0};
		b32 has_event = false;
		dispatch_ready(&executor);
		if (bob_execution_is_finished(execution)) break;
		if (executor.running == 0) {
			internal_error = true;
			break;
		}
		platform_lock_mutex(&executor.mutex);
		while (!executor.stopping && executor.event_count == 0) {
			if (platform_wait_condition(&executor.event_available, &executor.mutex).error) {
				log_fatal("failed waiting for worker event");
				request_stop_locked(&executor);
			}
		}
		has_event = dequeue_event_locked(&executor, &event);
		platform_unlock_mutex(&executor.mutex);
		if (!has_event) {
			internal_error = true;
			break;
		}
		if (event.type == BOB_EVENT_COMPLETED) {
			if (complete_result(execution, event.node, event.result) != BOB_OK) internal_error = true;
			--executor.running;
			if (!internal_error) dispatch_ready(&executor);
		}
		else if (event.type != BOB_EVENT_STARTED) internal_error = true;
		if (executor.options.event) executor.options.event(event, executor.options.user_data);
	}

cleanup:
	if (synchronization_initialized) {
		platform_lock_mutex(&executor.mutex);
		request_stop_locked(&executor);
		platform_unlock_mutex(&executor.mutex);
	}
	for (u32 i = 0; i < executor.thread_count; ++i) {
		platform_join_thread(executor.workers[i].thread);
		platform_close_thread(&executor.workers[i].thread);
	}
	if (synchronization_initialized) {
		platform_destroy_condition(&executor.event_available);
		platform_destroy_condition(&executor.work_available);
		platform_destroy_mutex(&executor.mutex);
	}
	end_scratch(scratch);
	return !internal_error && !bob_execution_has_failed(execution);
}
