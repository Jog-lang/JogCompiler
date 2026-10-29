#ifndef JOG_PRELUDE_H
#define JOG_PRELUDE_H

#include "symbols.h"

#include "diag.h"
#include "intern.h"

/* Reads <core_root>/scalar.jog and binds its names into scope, which is
   the universe scope from jog_universe_scope. An @intrinsic(int|float|bool)
   declaration that matches the builtin (kind, signedness, width, and base)
   binds that builtin. type bytes = []byte binds the bytes view. type str:
   []const byte is a distinct type over const bytes. type rune: u32 is a
   distinct type over the u32 builtin. Operator methods on those intrinsics
   are recorded on the type and are not scope names. struct void, enum
   Option<T>, enum Ordering, and interface error are the core nominals.
   Option is the existing option constructor, not a second layout. void
   stays incomplete. After those bind, Scope, Chan, Task, ErrCancelled,
   and ErrClosed are bound over void and error. Any other declaration is
   an error. core_root NULL, or a missing scalar.jog, is an error.
   Returns false when a diagnostic was emitted or memory ran out. */
bool jog_bind_prelude(CtArena *arena, CtDiag *diag, CtIntern *intern, JogTypes *types, JogPackageScope *scope,
                      const char *core_root);

/* A user @intrinsic(int|float|bool) on a distinct type is checked against
   the same width and base rules as core. An unknown kind, a bad width, a
   base that does not match, or @intrinsic used as a value is an error.
   Returns false when one was reported. */
bool jog_reject_intrinsic(JogProgram *program);

#endif
