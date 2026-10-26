#ifndef JOG_LOWER_CONTEXT_H
#define JOG_LOWER_CONTEXT_H

#include "lower_internal.h"

/* with blocks and the hidden arguments of a call. Context parameters are
   already trailing JIR parameters. A pointer argument converts when the
   parameter is an interface that concrete type satisfies.
   Not a contract header. */

/* Evaluate with values into locals, then the body. The locals stay live
   so a defer inside the body still loads them. */
void jog_lower_context_with(Lower *L, JogNode *node);

/* Write call's resolved context arguments at args[base]. False after a
   diagnostic. A function with no context parameters writes nothing. */
bool jog_lower_context_args(Lower *L, JogNode *call, JirOperand *args, size_t base);

/* Arguments a test harness passes for fn's context parameters. Top-level
   context globals are in scope. std.testing.Testing is testing() when no
   global has that type. *args_out is NULL and *count_out is 0 when fn
   declares none. False after a diagnostic. */
bool jog_lower_context_supply(Lower *L, JogNode *fn, JirOperand **args_out, size_t *count_out);

/* True when symbol is one of root main's runtime defaults. The first use
   materializes the value and binds that local. False when symbol is not
   one of those defaults. */
bool jog_lower_context_default(Lower *L, JogNode *node, JogSymbol *symbol, JirOperand *out);

/* True when `as` is between a distinct type and its immediate base.
   The value is a bitcast. False when the cast is something else. */
bool jog_lower_context_cast(Lower *L, JogNode *node, JirOperand *out);

/* Call std.testing.note(name) before an @test when that package is loaded.
   A program that never loaded the package has nothing to name.
   False after a diagnostic. */
bool jog_lower_testing_note(Lower *L, const char *name, JogPos pos);

/* Call std.testing.report(ran) in the current function when that package
   is loaded. report prints the summary, then exits when a test recorded
   a failure. A program that never loaded the package has nothing to report.
   False after a diagnostic. */
bool jog_lower_testing_finish(Lower *L, JogPos pos, int32_t ran);

#endif
