#include "bob_atom.h"
#include "platform.h"

#include <string.h>

#define BOB_ATOM_INITIAL_CAPACITY 16
#define BOB_ATOM_LOAD_NUMERATOR   3
#define BOB_ATOM_LOAD_DENOMINATOR 4

typedef struct Bob_Atom_Entry
{
	String value;
	u64    hash;
}
Bob_Atom_Entry;

typedef struct Bob_Atom_Slot
{
	u64 hash;
	u32 id;
}
Bob_Atom_Slot;

typedef struct Bob_Interner_Table
{
	Bob_Atom_Entry *entries;
	u32             entry_count;
	u32             entry_capacity;
	Bob_Atom_Slot  *slots;
	u32             slot_count;
	u32             slot_capacity;
}
Bob_Interner_Table;

struct Bob_Interner
{
	Arena              *arena;
	Platform_Mutex      mutex;
	Bob_Interner_Table  table;
};

static u64 bob_atom_hash(String value)
{
	u64 hash = 14695981039346656037ULL;
	for (u64 i = 0; i < value.size; ++i) {
		hash ^= (u8)value.data[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

static Bob_Atom_Slot *bob_atom_empty_slot(Bob_Atom_Slot *slots, u32 capacity, u64 hash)
{
	u32 mask = capacity - 1;
	u32 index = (u32)hash & mask;
	while (slots[index].id != 0) index = (index + 1) & mask;
	return slots + index;
}

static b32 bob_interner_reserve_entries(Bob_Interner *interner, u32 needed)
{
	Bob_Interner_Table *table = &interner->table;
	if (table->entry_capacity >= needed) return true;
	u32 capacity = table->entry_capacity ? table->entry_capacity : BOB_ATOM_INITIAL_CAPACITY;
	while (capacity < needed) {
		if (capacity > UINT32_MAX / 2) return false;
		capacity *= 2;
	}
	Bob_Atom_Entry *entries = arena_push_zero_aligned(interner->arena, (u64)capacity * sizeof(*entries), _Alignof(Bob_Atom_Entry));
	if (!entries) return false;
	if (table->entry_count) memcpy(entries, table->entries, (u64)table->entry_count * sizeof(*entries));
	table->entries = entries;
	table->entry_capacity = capacity;
	return true;
}

static b32 bob_interner_rehash(Bob_Interner *interner, u32 capacity)
{
	Bob_Interner_Table *table = &interner->table;
	if (capacity < BOB_ATOM_INITIAL_CAPACITY || (capacity & (capacity - 1)) != 0) return false;
	Bob_Atom_Slot *slots = arena_push_zero_aligned(interner->arena, (u64)capacity * sizeof(*slots), _Alignof(Bob_Atom_Slot));
	if (!slots) return false;
	for (u32 i = 0; i < table->entry_count; ++i) {
		Bob_Atom_Slot *slot = bob_atom_empty_slot(slots, capacity, table->entries[i].hash);
		slot->hash = table->entries[i].hash;
		slot->id = i + 1;
	}
	table->slots = slots;
	table->slot_count = table->entry_count;
	table->slot_capacity = capacity;
	return true;
}

static b32 bob_interner_reserve_slot(Bob_Interner *interner)
{
	Bob_Interner_Table *table = &interner->table;
	u32 capacity = table->slot_capacity;
	if (capacity == 0) return bob_interner_rehash(interner, BOB_ATOM_INITIAL_CAPACITY);
	if ((u64)(table->slot_count + 1) * BOB_ATOM_LOAD_DENOMINATOR <= (u64)capacity * BOB_ATOM_LOAD_NUMERATOR) return true;
	if (capacity > UINT32_MAX / 2) return false;
	return bob_interner_rehash(interner, capacity * 2);
}

static Bob_Atom bob_interner_find(const Bob_Interner_Table *table, String value, u64 hash)
{
	if (!table->slots || table->slot_capacity == 0) return (Bob_Atom){0};
	u32 mask = table->slot_capacity - 1;
	u32 index = (u32)hash & mask;
	for (u32 probe = 0; probe < table->slot_capacity; ++probe) {
		const Bob_Atom_Slot *slot = table->slots + index;
		if (slot->id == 0) return (Bob_Atom){0};
		if (slot->hash == hash && slot->id <= table->entry_count && string_equal(table->entries[slot->id - 1].value, value)) return (Bob_Atom){ slot->id };
		index = (index + 1) & mask;
	}
	return (Bob_Atom){0};
}

Bob_Interner *bob_interner_create(Arena *arena)
{
	if (!arena) return NULL;
	Bob_Interner *interner = arena_push_zero_aligned(arena, sizeof(*interner), _Alignof(Bob_Interner));
	if (!interner) return NULL;
	interner->arena = arena;
	platform_init_mutex(&interner->mutex);
	return interner;
}

void bob_interner_destroy(Bob_Interner *interner)
{
	if (interner) platform_destroy_mutex(&interner->mutex);
}

Bob_Atom bob_interner_intern(Bob_Interner *interner, String value)
{
	Bob_Atom atom = {0};
	if (!interner || !value.data) return atom;
	platform_lock_mutex(&interner->mutex);
	Bob_Interner_Table *table = &interner->table;
	if (table->entry_count == UINT32_MAX) goto done;
	u64 hash = bob_atom_hash(value);
	atom = bob_interner_find(table, value, hash);
	if (atom.id == 0) {
		Bob_Interner_Table original = *table;
		u64 mark = arena_mark(interner->arena);
		String copy = arena_push_string_copy(interner->arena, value);
		if (!copy.data || !bob_interner_reserve_entries(interner, table->entry_count + 1) || !bob_interner_reserve_slot(interner)) {
			*table = original;
			arena_restore(interner->arena, mark);
		}
		else {
			atom.id = ++table->entry_count;
			table->entries[atom.id - 1] = (Bob_Atom_Entry){ copy, hash };
			Bob_Atom_Slot *slot = bob_atom_empty_slot(table->slots, table->slot_capacity, hash);
			slot->hash = hash;
			slot->id = atom.id;
			++table->slot_count;
		}
	}

done:
	platform_unlock_mutex(&interner->mutex);
	return atom;
}

String bob_interner_string(const Bob_Interner *interner, Bob_Atom atom)
{
	String result = {0};
	if (!interner || atom.id == 0) return result;
	platform_lock_mutex((Platform_Mutex *)&interner->mutex);
	if (atom.id <= interner->table.entry_count) result = interner->table.entries[atom.id - 1].value;
	platform_unlock_mutex((Platform_Mutex *)&interner->mutex);
	return result;
}

b32 bob_atom_is_valid(Bob_Atom atom)
{
	return atom.id != 0;
}
