#ifndef JOG_LOWER_CORO_H
#define JOG_LOWER_CORO_H

#include "lower_internal.h"

/* 1 when the call was a coroutine builtin and *out is its value.
   0 when the call is not one. A failure sets L->failed. */
bool jog_lower_coro_call(Lower *L, JogNode *node, JirOperand *out);
JirOperand jog_lower_go(Lower *L, JogNode *node);

/* 1 when symbol is a captured local and *out is the loaded value. */
bool jog_lower_coro_name(Lower *L, JogNode *node, const JogSymbol *symbol, JirOperand *out);

/* 1 when the assignment stores through a capture. */
bool jog_lower_coro_store(Lower *L, JogNode *node);

/* 1 when symbol is ErrCancelled or ErrClosed. */
bool jog_lower_coro_error(Lower *L, JogNode *node, const JogSymbol *symbol, JirOperand *out);

/* Leave scopes that are still open. Root main then joins the root scope. */
void jog_lower_coro_on_exit(Lower *L, JogPos pos);

/* 1 when node is a return inside go. The caller skips the normal return. */
bool jog_lower_coro_return(Lower *L, JogNode *node);

#endif
