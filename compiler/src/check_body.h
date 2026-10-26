#ifndef JOG_CHECK_BODY_H
#define JOG_CHECK_BODY_H

#include "symbols.h"

#include <stdbool.h>
#include <stddef.h>

/* Internal to the checker. Not a contract header.
   check.c owns the coroutine scheduler. check_body.c typechecks the
   milestone 2 subset once a declaration's type is known: function bodies,
   val/var, integers, bool, str, if, for, calls, comparisons, and arithmetic.
   A pointer to a struct or enum converts to an interface it satisfies.
   Definite assignment rejects a read of a var that has no zero value
   and has not been assigned on every path. */

typedef struct Check Check;
typedef struct CheckJob CheckJob;

struct CheckJob {
    Check *check;
    JogNode *decl;
    JogPackage *package;
    size_t file_index;
    JogSymbol *symbol;
    JogSymbol **symbols;
    size_t symbol_count;
    struct CheckJob *waiting_for;
    bool parked;
    bool done;
    /* Active Env while a body is checked. Lets jog_check_expr share definite
       assignment with the body instead of starting from an empty set.
       Not a contract field. */
    void *body_env;
    /* Private-function context inference. infer_state and ctx_stack are
       owned by check_context.c. inferred_* is the solved with clause. */
    bool infer_contexts;
    bool infer_collecting;
    bool infer_solved;
    bool infer_parked;
    bool sig_ready;
    /* Collecting a body, parked until a top-level context type is set. */
    bool infer_type_wait;
    void *infer_state;
    void *ctx_stack;
    JogType **inferred_contexts;
    const char **infer_chains;
    size_t inferred_count;
    /* go body: the result type is the return or the block tail, not the
       enclosing function's result. Not a contract field. */
    bool in_go;
    bool go_ret_set;
    JogType *go_ret;
};

struct Check {
    JogProgram *program;
    CheckJob *jobs;
    size_t job_count;
    bool failed;
    bool stop;
    /* Job whose coroutine is running. Inference collection suppresses
       diagnostics only for this job. */
    CheckJob *current_job;
    /* While resolving a constraint or a type-set term, a type-set
       interface is a legal type. Value positions leave this at 0. */
    int allow_typeset;
    /* Chan<T> and Task<T> instances. Owned by check_coro.c. */
    void *coro_insts;
};

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
void jog_check_fail(Check *ck, JogPos pos, const char *fmt, ...);

void jog_check_oom(Check *ck);
JogType *jog_check_error_type(Check *ck);
JogType *jog_check_unit_type(Check *ck);
JogType *jog_check_resolve_type(Check *ck, CheckJob *job, JogNode *node);
bool jog_check_wait(Check *ck, CheckJob *job, JogSymbol *symbol);

/* Park until the symbol's job has finished. Unlike jog_check_wait, this
   does not return early for a struct whose type is already published. */
bool jog_check_wait_finished(Check *ck, CheckJob *job, JogSymbol *symbol);
JogSymbol *jog_check_lookup_file(const CheckJob *job, const char *name);

/* Poison and scalars have a zero value. Non-null pointers, function
   values, and interfaces do not. A struct has a zero value when every
   field does. An enum has one when a variant's tag is 0 and that
   payload has a zero value. */
bool jog_check_type_has_zero(const JogType *type);

/* Typecheck node. A NULL target infers, and an untyped integer becomes
   i64. Returns NULL only when the checker has stopped. */
JogType *jog_check_expr(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType *target);

/* Check a function body. The function type is already published, so a
   call from this body can wait on another signature without waiting on
   that function's body. */
void jog_check_fn_body(Check *ck, CheckJob *job);

/* The active body, while check_stmt or check_expr is on the stack.
   NULL when no body is active. */
JogSymbol *jog_check_body_lookup(CheckJob *job, const char *name);
JogSymbol *jog_check_body_bind(CheckJob *job, JogNode *at, JogSymbolKind kind, const char *name, JogType *type);
void jog_check_body_assigned(CheckJob *job, JogSymbol *symbol);
void jog_check_body_unreachable(CheckJob *job);

/* Typecheck stmt in the active body without keeping its assignments or
   its reachability. Used for defer bodies. */
void jog_check_body_stmt_isolated(CheckJob *job, JogNode *stmt);

/* The scope names in the active body resolve in. NULL when no body is active. */
JogBlockScope *jog_check_body_scope(CheckJob *job);
void jog_check_body_set_scope(CheckJob *job, JogBlockScope *scope);

/* Check one when arm in scope. Assignments and reachability from the arm
   are not kept. *reaches reports whether control left the arm. */
JogType *jog_check_when_arm(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *body, JogType *target,
                            bool as_value, bool *reaches);

#endif
