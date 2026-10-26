#ifndef JOG_RESOLVE_UNIVERSE_H
#define JOG_RESOLVE_UNIVERSE_H

#include "symbols.h"

#include "intern.h"

/* The names every package can see without an import: the scalar types,
   bytes, str, void, and the constants true, false, and null. The scope's
   package pointer is NULL. Each name is interned in intern. Type names
   are JOG_SYM_TYPE; true, false, and null are JOG_SYM_CONSTANT. str is
   the const-bytes builtin. void is an incomplete nominal struct with no
   fields and no layout, distinct from the unit type (). decl is NULL on
   every binding. NULL if arena, intern, or types is NULL, or memory
   runs out. Do not pass void to jog_type_layout: an empty struct would
   then be given size 0, and void has no size. */
JogPackageScope *jog_universe_scope(CtArena *arena, CtIntern *intern, JogTypes *types);

#endif
