#ifndef JOG_CHECK_CONTEXT_H
#define JOG_CHECK_CONTEXT_H

#include "check_body.h"

/* with clauses, context bindings, and inference for private functions.
   Not a contract header. Contexts match by exact type, or by conversion of
   a pointer to an interface that concrete type satisfies. */

/* True when fn is private, has a body, and did not write a with clause.
   @test, @export, extern, and generic functions are not inferred. */
bool jog_check_context_should_infer(const CheckJob *job);

/* Publish partial as the signature, infer the with clause, and return the
   function type with those context parameters. NULL when the checker stops.
   partial already includes a method receiver. */
JogType *jog_check_context_infer(Check *ck, CheckJob *job, JogType *partial);

/* Drop the context stack at the start of a function body. A bare root
   fn main() then receives Allocator, ErrorAllocator, Scope, and Logger
   when those types are already loaded. */
void jog_check_context_begin(CheckJob *job);

/* True when explicit `as` is between a distinct type and its immediate
   base, either direction. Sets node->type. Not an implicit conversion. */
bool jog_check_context_cast(Check *ck, JogNode *node, JogType *got, JogType *dest);

/* Remember the stack pointer. reset restores it, dropping bindings and
   with blocks pushed inside the block. */
void *jog_check_context_mark(const CheckJob *job);
void jog_check_context_reset(CheckJob *job, void *mark);

/* Push the function's own context parameters as the outermost level.
   symbols[i] is the binding for params item i; NULL entries are skipped. */
void jog_check_context_push_params(Check *ck, CheckJob *job, JogSymbol **symbols, JogType **types, size_t count);

/* context name [: T] = expr. Binds the name and pushes one level that
   lasts until the enclosing block is reset. */
int jog_check_context_binding(Check *ck, CheckJob *job, JogNode *stmt);

/* Typecheck with values and push one level. 1 when the level was pushed,
   0 when it was not, -1 when the checker has stopped. The caller pops
   after the body. */
int jog_check_context_push_with(Check *ck, CheckJob *job, JogNode *stmt);
void jog_check_context_pop(CheckJob *job);

/* Resolve the callee's context parameters. 0 when the call was handled,
   -1 when the checker has stopped. Collecting inference records demands
   instead of diagnostics. */
int jog_check_context_resolve(Check *ck, CheckJob *job, JogNode *call);

/* True when a context currently in scope has this nominal name. */
bool jog_check_context_has_named(const CheckJob *job, const char *name);

/* The expression that provides a context with this nominal name, or NULL.
   The innermost level wins. */
JogNode *jog_check_context_find(const CheckJob *job, const char *name);

/* Push one symbol as its own context level. 1 when pushed, 0 when it was
   not, -1 when the checker has stopped. */
int jog_check_context_push_symbol(Check *ck, CheckJob *job, JogSymbol *symbol, JogType *type, JogPos pos);

/* When every remaining job is parked, solve recursive private-function
   groups. True when at least one signature was solved. */
bool jog_check_context_solve(Check *ck);

/* True when a collecting function was waiting only for a top-level
   context type that is now published. The scheduler resumes it. */
bool jog_check_context_wake_type(Check *ck);

/* Free inference scratch arrays. Safe when none were allocated. */
void jog_check_context_cleanup(Check *ck);

#endif
