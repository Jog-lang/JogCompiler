#ifndef JOG_LOWER_COMPTIME_H
#define JOG_LOWER_COMPTIME_H

#include "lower_internal.h"

/* Pointer-free top-level val, @comptime, and @embed. Not a contract header.
   The step budget is 10 million; --comptime-steps is not parsed here. */

bool jog_lower_comptime_builtin(Lower *L, JogNode *node, JirOperand *out);
bool jog_lower_comptime_vals(Lower *L);

#endif
