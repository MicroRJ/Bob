#ifndef BOB_ATOM_H
#define BOB_ATOM_H

#include "base.h"

typedef struct Bob_Atom
{
	u32 id;
}
Bob_Atom;

typedef struct Bob_Interner Bob_Interner;

b32 bob_atom_is_valid(Bob_Atom atom);

Bob_Interner *bob_interner_create(Arena *arena);
void bob_interner_destroy(Bob_Interner *interner);
Bob_Atom bob_interner_intern(Bob_Interner *interner, String value);
String bob_interner_string(const Bob_Interner *interner, Bob_Atom atom);

#endif
