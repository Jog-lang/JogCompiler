#ifndef JOG_LOWER_INTERNAL_H
#define JOG_LOWER_INTERNAL_H

#include "phases.h"
#include "symbols.h"

/* Shared by lower.c and lower_struct.c. Not a contract header. */

typedef struct LocalBind {
    JogSymbol *symbol;
    uint32_t local;
} LocalBind;

typedef struct FnBind {
    JogNode *decl;
    int32_t index;
    bool body_done; /* the body has been lowered, including on demand for comptime */
} FnBind;

/* One top-level val or var. index is the JirGlobal. runtime means
   init_func stores the initializer; otherwise the data image is enough.
   rt is the index among runtime globals in source order, or -1. */
typedef struct GlobalBind {
    JogSymbol *symbol;
    JogNode *decl;
    int32_t index;
    bool runtime;
    int32_t rt;
    int32_t part;   /* -1, or the tuple index of a top-level val (a, b) */
    bool group_done; /* the destructure group was stored by init */
} GlobalBind;

typedef struct LoopFrame {
    uint32_t cont;
    uint32_t brk;
    size_t cleanup_base; /* defer stack depth on entry to the loop body */
} LoopFrame;

typedef struct Overlay {
    JogSymbol *symbol;
    JirOperand value;
} Overlay;

typedef struct Slot {
    bool present;
    JirOperand value;
} Slot;

typedef struct Lower {
    JogProgram *program;
    JirModule *module;
    JirFunc *func;
    JogNode *ast_fn; /* function whose body is being lowered, or NULL */
    uint32_t block;
    bool terminated;
    bool failed;
    CtDa locals;  /* LocalBind, innermost last */
    CtDa fns;     /* FnBind */
    CtDa loops;   /* LoopFrame */
    CtDa globals; /* GlobalBind, source order */
    const Overlay *overlay;
    size_t overlay_count;
    bool unchecked; /* @unchecked: skip JIR_BOUNDS_CHECK, keep integer checks */
    CtDa cleanups;  /* defer/errdefer stack; item type is private to lower_error.c */
    bool in_cleanup; /* lowering a cleanup body: return and break do not run the stack again */
    /* Active monomorphization. subst_from[i] (a type parameter) becomes
       subst_to[i]. generic_pending is JogGenericInst *, bodies not yet lowered. */
    JogType **subst_from;
    JogType **subst_to;
    size_t subst_n;
    CtDa generic_pending;
    /* Coroutine lowering. coro_caps is private to lower_coro.c.
       coro_scopes is a stack of scope locals that a return must leave. */
    uint32_t coro_seq;
    void *coro_caps;
    size_t coro_cap_count;
    int in_coro;
    uint32_t *coro_scopes;
    size_t coro_scope_n;
    size_t coro_scope_cap;
    /* Comptime lowering. in_comptime rejects reads of runtime locals.
       ensuring is the function index whose body is being lowered on demand,
       or -1. ct_seq names thunks. Not a contract field. */
    bool in_comptime;
    uint32_t ct_seq;
    int32_t ensuring;
    /* Nonzero while lowering a lambda body. An outer local is a capture.
       lambda_caps is private to lower_lambda.c. lambda_env is the *void
       parameter of a capturing lambda; 0 is a real local index, so a
       lambda with no captures is lambda_cap_count == 0. */
    int in_lambda;
    void *lambda_caps;
    size_t lambda_cap_count;
    uint32_t lambda_env;
} Lower;

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
void jog_lower_fail(Lower *L, JogPos pos, const char *fmt, ...);

bool jog_lower_new_local(Lower *L, const char *name, JogType *type, uint32_t *out);
JirInst *jog_lower_emit(Lower *L, JirOp op, int32_t dst, JogPos pos);
void jog_lower_copy(Lower *L, uint32_t dst, JirOperand src, JogPos pos);
void jog_lower_zero(Lower *L, uint32_t dst, JogPos pos);
JirOperand jog_lower_local_op(Lower *L, uint32_t local);
JirOperand jog_lower_expr(Lower *L, JogNode *node);
void jog_lower_stmt(Lower *L, JogNode *node);
JirOperand jog_lower_as_local(Lower *L, JirOperand src, JogPos pos);
bool jog_lower_find_local(Lower *L, const JogSymbol *symbol, uint32_t *out);
bool jog_lower_bind_local(Lower *L, JogSymbol *symbol, uint32_t local);
int32_t jog_lower_find_fn(Lower *L, const JogNode *decl);

/* Address of a global, or none when symbol is not one. Does not fail. */
JirOperand jog_lower_global_addr(Lower *L, const JogSymbol *symbol);

/* The global binding for symbol, or NULL. Does not fail. */
GlobalBind *jog_lower_global_bind(Lower *L, const JogSymbol *symbol);

/* Lower expr as a fresh () -> type thunk. Restores L. -1 on failure. */
int32_t jog_lower_isolate_expr(Lower *L, JogNode *expr, JogType *type, const char *name, const char *jog_name);

/* Lower the body of a declared function or generic instance if it has not
   been lowered yet. A thunk that already has blocks is left as it is. */
bool jog_lower_ensure_index(Lower *L, int32_t index);

/* Direct global uses in an initializer. row[other] is set when this
   initializer names runtime global `other`. `self` is this global's rt. */
void jog_lower_walk_init_deps(Lower *L, JogNode *node, int32_t self, uint8_t *row);

/* Non-escaping captures of one lambda. caps is malloc'd and owned by the
   caller. env is the *void pointer to the stack tuple, or none. */
typedef struct JogLambdaPrep {
    void *caps;
    size_t count;
    JirOperand env;
    JogType *void_ptr;
} JogLambdaPrep;

/* Scan node and, when it names an outer local, emit the stack tuple in the
   current function. False when lowering failed. */
bool jog_lower_lambda_prep(Lower *L, JogNode *node, JogLambdaPrep *prep);

/* Closure value { code, env } for a capturing lambda. */
JirOperand jog_lower_lambda_closure(Lower *L, JogType *fn_type, uint32_t func_index, JirOperand env, JogPos pos);

/* Load or address a captured local. False when symbol is not a capture.
   A go child returns false so it does not read this lambda's env. */
bool jog_lower_lambda_name(Lower *L, JogNode *node, JogSymbol *symbol, JirOperand *out);
bool jog_lower_lambda_addr(Lower *L, const JogSymbol *symbol, JirOperand *out);

/* Assign through a capture. False when the target is not one. */
bool jog_lower_lambda_store(Lower *L, JogNode *node);

/* Same compare as == on str and bytes. negate selects !=. */
JirOperand jog_lower_bytes_cmp(Lower *L, JirOperand lhs, JirOperand rhs, bool negate, JogPos pos);

/* Same compare as == on an enum. negate selects !=. */
JirOperand jog_lower_enum_eq(Lower *L, JirOperand lhs, JirOperand rhs, bool negate, JogPos pos);

#endif
