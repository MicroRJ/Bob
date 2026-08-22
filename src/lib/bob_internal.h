#ifndef BOB_INTERNAL_H
#define BOB_INTERNAL_H

#include "bob.h"

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
	u32                index;
	String             name;
	Bob_Node_Function *function;
	void              *user_data;
};

struct Bob
{
	Arena      arena;
	Bob_Node **nodes;
	u32        node_count;
	u32        node_capacity;
	u32        execution_count;
	b32        sealed;
};

b32 bob_valid_node(const Bob *bob, const Bob_Node *node);

#endif
