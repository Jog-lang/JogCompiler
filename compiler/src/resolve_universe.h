#ifndef JOG_RESOLVE_UNIVERSE_H
#define JOG_RESOLVE_UNIVERSE_H

#include "symbols.h"

#include "intern.h"

/* The names every package can see without an import, before the core
   prelude is bound: byte, and the constants true, false, and null.
   str, Option, Ordering, error, and void are bound from core/scalar.jog
   into this same scope. Scope, Chan, Task, ErrCancelled, and ErrClosed
   are bound after that prelude, because they are distinct *void and
   error values. The scope's package pointer is NULL. Each name is
   interned in intern. Type names are JOG_SYM_TYPE; true, false, and null
   are JOG_SYM_CONSTANT. NULL if arena, intern, or types is NULL, or
   memory runs out. */
JogPackageScope *jog_universe_scope(CtArena *arena, CtIntern *intern, JogTypes *types);

/* Binds Scope, Chan, and Task as distinct *void, and ErrCancelled and
   ErrClosed as error values. void and error must already be in scope.
   Their decls stay NULL. 0 when a name is missing or memory runs out. */
int jog_bind_lang_items(CtArena *arena, CtIntern *intern, JogTypes *types, JogPackageScope *scope);

#endif
