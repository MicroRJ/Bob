#include "bob_internal.h"

#include <string.h>

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

void *bob_allocate(Bob *bob, u64 size, u64 alignment)
{
	if (!bob || bob->sealed || alignment == 0) return NULL;
	return bob_push(bob, size, alignment);
}

String bob_copy_string(Bob *bob, String string)
{
	if (!bob || bob->sealed || (!string.data && string.size)) return (String){0};
	return arena_push_string_copy(&bob->arena, string);
}

static b32 node_array_push(Bob *bob, Bob_Node_Array *array, Bob_Node *node)
{
	if (!bob_reserve(bob, (void **)&array->items, sizeof(*array->items), array->count, &array->capacity, array->count + 1, _Alignof(Bob_Node *))) return false;
	array->items[array->count++] = node;
	return true;
}

b32 bob_valid_node(const Bob *bob, const Bob_Node *node)
{
	return bob && node && node->index < bob->node_count && bob->nodes[node->index] == node;
}

Bob *bob_create(void)
{
	Arena arena = arena_create(0);
	Bob *bob;
	if (!arena.data) return NULL;
	arena_set_name(&arena, "Bob graph");
	bob = arena_push_zero_aligned(&arena, sizeof(*bob), _Alignof(Bob));
	if (!bob) {
		arena_destroy(&arena);
		return NULL;
	}
	bob->arena = arena;
	return bob;
}

void bob_destroy(Bob *bob)
{
	Arena arena;
	if (!bob) return;
	ASSERT(bob->execution_count == 0);
	arena = bob->arena;
	arena_destroy(&arena);
}

Bob_Error bob_add_node(Bob *bob, Bob_Node_Desc description, Bob_Node **node_out)
{
	Bob_Node *node;
	if (!bob || !description.name.data || !node_out) return BOB_ERROR_INVALID_NODE;
	if (bob->sealed) return BOB_ERROR_GRAPH_SEALED;
	if (!bob_reserve(bob, (void **)&bob->nodes, sizeof(*bob->nodes), bob->node_count, &bob->node_capacity, bob->node_count + 1, _Alignof(Bob_Node *))) return BOB_ERROR_OUT_OF_MEMORY;
	node = bob_push(bob, sizeof(*node), _Alignof(Bob_Node));
	if (!node) return BOB_ERROR_OUT_OF_MEMORY;
	node->name = arena_push_string_copy(&bob->arena, description.name);
	if (!node->name.data) return BOB_ERROR_OUT_OF_MEMORY;
	node->index = bob->node_count;
	node->function = description.function;
	node->user_data = description.user_data;
	bob->nodes[bob->node_count++] = node;
	*node_out = node;
	return BOB_OK;
}

Bob_Error bob_set_node(Bob *bob, Bob_Node *node, Bob_Node_Desc description)
{
	String name;
	if (!bob_valid_node(bob, node)) return BOB_ERROR_INVALID_NODE;
	if (bob->sealed) return BOB_ERROR_GRAPH_SEALED;
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
	if (!bob_valid_node(bob, node)) return BOB_ERROR_INVALID_NODE;
	if (bob->sealed) return BOB_ERROR_GRAPH_SEALED;
	node->function = function;
	node->user_data = user_data;
	return BOB_OK;
}

Bob_Error bob_add_dependency(Bob *bob, Bob_Node *node, Bob_Node *dependency)
{
	if (!bob_valid_node(bob, node) || !bob_valid_node(bob, dependency)) return BOB_ERROR_INVALID_NODE;
	if (bob->sealed) return BOB_ERROR_GRAPH_SEALED;
	if (node == dependency) return BOB_ERROR_SELF_DEPENDENCY;
	for (u32 i = 0; i < node->dependencies.count; ++i) {
		if (node->dependencies.items[i] == dependency) return BOB_ERROR_DUPLICATE_DEPENDENCY;
	}
	if (!node_array_push(bob, &node->dependencies, dependency)) return BOB_ERROR_OUT_OF_MEMORY;
	if (!node_array_push(bob, &dependency->dependents, node)) {
		--node->dependencies.count;
		return BOB_ERROR_OUT_OF_MEMORY;
	}
	return BOB_OK;
}

b32 bob_is_sealed(const Bob *bob)
{
	ASSERT(bob);
	return bob->sealed;
}

u32 bob_node_count(const Bob *bob)
{
	ASSERT(bob);
	return bob->node_count;
}

Bob_Node *bob_node_at(const Bob *bob, u32 index)
{
	ASSERT(bob);
	ASSERT(index < bob->node_count);
	return bob->nodes[index];
}

// TODO(RJ) why is this returning a raw c string
const char *bob_node_name(const Bob_Node *node)
{
	ASSERT(node);
	return node->name.data;
}

Bob_Node_Function *bob_node_function(const Bob_Node *node)
{
	ASSERT(node);
	return node->function;
}

void *bob_node_user_data(const Bob_Node *node)
{
	ASSERT(node);
	return node->user_data;
}

u32 bob_dependency_count(const Bob_Node *node)
{
	ASSERT(node);
	return node->dependencies.count;
}

Bob_Node *bob_dependency(const Bob_Node *node, u32 index)
{
	ASSERT(node);
	ASSERT(index < node->dependencies.count);
	return node->dependencies.items[index];
}

const char *bob_error_string(Bob_Error result)
{
	switch (result) {
		case BOB_OK:                         return "ok";
		case BOB_ERROR_OUT_OF_MEMORY:        return "out of memory";
		case BOB_ERROR_INVALID_TASK:         return "invalid node";
		case BOB_ERROR_DUPLICATE_DEPENDENCY: return "duplicate dependency";
		case BOB_ERROR_SELF_DEPENDENCY:      return "self dependency";
		case BOB_ERROR_GRAPH_SEALED:         return "Bob graph is sealed";
		case BOB_ERROR_INVALID_STATE:        return "invalid node state";
		case BOB_ERROR_CYCLE:                return "dependency cycle";
	}
	return "unknown Bob result";
}
