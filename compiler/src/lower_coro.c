#include "lower_coro.h"

#include "lower_error.h"
#include "lower_memory.h"

#include <stdio.h>
#include <string.h>

/* A child is a real void(void *) function. Captures are addresses of the
   parent locals, stored in an array the child reloads on every use.
   A compound assignment loads through that address, then lowers the
   right-hand side, then stores, matching a = a + e. A captured pointer
   uses that order with pointer arithmetic for += and -=.
   return a, b inside go is one tuple task result. When its last value
   is error? and present, the scope fails.
   go f(x) lowers the call in the child, so each argument runs when the
   child runs. stack: and a with clause stay in the parent.
   scope leave is emitted on each exit path and does not pop the compiler
   stack, so another path can leave the same scope in its own block. */

enum { SEL_MAX = 32 };

typedef struct Cap {
    const JogSymbol *symbol;
    uint32_t parent_local;
    int indirect;
    uint32_t indirect_slot;
} Cap;

typedef struct Scan {
    Lower *L;
    Cap *caps;
    size_t count;
    size_t cap;
} Scan;

typedef struct Rt {
    JogType *void_ptr;
    JogType *void_const;
    JogType *scope_ty;
    JogType *err;
    JogType *err_opt;
    JogType *unit;
    JogType *i32;
    JogType *i64;
    JogType *u32;
    JogType *usize;
    JogType *boolean;
    int32_t scope_root;
    int32_t scope_enter;
    int32_t scope_leave;
    int32_t scope_cancel;
    int32_t scope_join;
    int32_t scope_fail;
    int32_t go;
    int32_t yield;
    int32_t task_set;
    int32_t task_join;
    int32_t chan_init;
    int32_t chan_free;
    int32_t chan_close;
    int32_t chan_send;
    int32_t chan_recv;
    int32_t sel_begin;
    int32_t sel_recv;
    int32_t sel_send;
    int32_t sel_default;
    int32_t sel_timeout;
    int32_t sel_wait;
    int32_t sel_status;
    int32_t sleep;
    int32_t err_cancelled;
    int32_t err_closed;
    int32_t cap_new;
    int32_t cap_set;
    int32_t cap_get;
} Rt;

typedef struct Saved {
    JirFunc *func;
    JogNode *ast_fn;
    uint32_t block;
    bool terminated;
    CtDa locals;
    CtDa loops;
    CtDa cleanups;
    void *caps;
    size_t cap_count;
    int in_coro;
    size_t scope_n;
    uint32_t *scope_copy;
} Saved;

static bool load_rt(Lower *L, Rt *rt, JogPos pos);
static JirOperand call_rt(Lower *L, int32_t fn, JirOperand *args, size_t n, JogType *as_ty, JogPos pos);
static void scan_node(Scan *s, JogNode *node);
static JirOperand error_from_status(Lower *L, const Rt *rt, JirOperand status, JogPos pos);
static JirOperand error_from_ptr(Lower *L, const Rt *rt, JirOperand ptr, JogPos pos);
static void maybe_fail(Lower *L, const Rt *rt, JirOperand value, JogType *type, JogPos pos);
static void task_set(Lower *L, const Rt *rt, JirOperand value, JogPos pos);
static void lower_result_body(Lower *L, const Rt *rt, JogNode *body, int publish);

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static JogNode *arg_value(JogNode *arg) {
    if (!arg) {
        return NULL;
    }
    if (arg->kind == JOG_NODE_ARG) {
        return arg->as.arg.value;
    }
    return arg;
}

static bool is_unit(const JogType *type) {
    return type && type->kind == JOG_TY_UNIT;
}

static bool is_error_option(const JogType *type) {
    const JogType *elem;
    if (!type || type->kind != JOG_TY_OPTION) {
        return false;
    }
    elem = type->as.elem.elem;
    return elem && elem->kind == JOG_TY_INTERFACE && elem->as.nominal.name && strcmp(elem->as.nominal.name, "error") == 0;
}

static bool nominal_is(const JogType *type, const char *name, size_t args) {
    return type && type->kind == JOG_TY_DISTINCT && type->as.nominal.package == NULL &&
           type->as.nominal.type_arg_count == args && type->as.nominal.name && strcmp(type->as.nominal.name, name) == 0;
}

static bool is_chan(const JogType *type) {
    return nominal_is(type, "Chan", 1);
}

static bool is_task(const JogType *type) {
    return nominal_is(type, "Task", 1);
}

static bool is_scope_type(const JogType *type) {
    return nominal_is(type, "Scope", 0) && type->as.nominal.decl == NULL;
}

static JogType *elem_of(const JogType *type) {
    if (!type || type->as.nominal.type_arg_count != 1 || !type->as.nominal.type_args) {
        return NULL;
    }
    return type->as.nominal.type_args[0];
}

static bool is_root_main(const Lower *L) {
    if (!L || !L->module || !L->func || L->module->main_func < 0) {
        return false;
    }
    return jir_func(L->module, (uint32_t)L->module->main_func) == L->func;
}

static JogType *universe_type(Lower *L, const char *name) {
    JogPackage *root;
    JogPackageScope *pkg;
    const char *interned;
    JogSymbol *symbol;
    if (!L || !L->program || !L->program->root || !L->program->intern) {
        return NULL;
    }
    root = L->program->root;
    pkg = root->scope;
    if (!pkg || !pkg->universe) {
        return NULL;
    }
    interned = ct_intern_cstr(L->program->intern, name);
    if (!interned) {
        return NULL;
    }
    symbol = jog_package_scope_lookup(pkg->universe, interned);
    return symbol ? symbol->type : NULL;
}

static void use_block(Lower *L, uint32_t block) {
    JirBlock *b = jir_block_at(L->func, block);
    L->block = block;
    L->terminated = !b || b->term.kind != JIR_TERM_NONE;
}

static bool new_block(Lower *L, uint32_t *out) {
    uint32_t before;
    if (!L->func) {
        return false;
    }
    before = (uint32_t)L->func->blocks.count;
    *out = jir_block_add(L->func, NULL);
    if (L->func->blocks.count == before) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
        return false;
    }
    return true;
}

static void seal(Lower *L, JirTerm term) {
    if (!L || L->failed || L->terminated) {
        return;
    }
    jir_terminate(L->func, L->block, term);
    L->terminated = true;
}

static void emit_br(Lower *L, uint32_t target, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_BR;
    term.target = target;
    term.pos = pos;
    seal(L, term);
}

static void emit_cond(Lower *L, JirOperand cond, uint32_t yes, uint32_t no, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_COND_BR;
    term.value = cond;
    term.target = yes;
    term.else_target = no;
    term.pos = pos;
    seal(L, term);
}

static void emit_ret(Lower *L, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_RET;
    term.value = jir_none();
    term.pos = pos;
    seal(L, term);
}

static int32_t find_extern(Lower *L, const char *name) {
    size_t i;
    for (i = 0; i < L->module->funcs.count; i++) {
        JirFunc *fn = jir_func(L->module, (uint32_t)i);
        if (fn && fn->name && strcmp(fn->name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t ensure(Lower *L, const char *name, JogType **params, size_t n, JogType *ret, JogPos pos) {
    int32_t found = find_extern(L, name);
    JogType *fn;
    int32_t index;
    if (found >= 0) {
        return found;
    }
    fn = jog_type_fn(L->program->types, n ? params : NULL, n, ret, NULL, 0, false, false, true);
    if (!fn) {
        jog_lower_fail(L, pos, "out of memory");
        return -1;
    }
    index = jir_func_add(L->module, name, name, JIR_LINK_EXTERN, fn, NULL);
    if (index < 0) {
        jog_lower_fail(L, pos, "out of memory");
        return -1;
    }
    return index;
}

static JirOperand call_rt(Lower *L, int32_t fn, JirOperand *args, size_t n, JogType *as_ty, JogPos pos) {
    JirFunc *func;
    JogType *ret;
    JogType *local_ty;
    uint32_t dst = 0;
    int32_t dst_i = -1;
    JirInst *inst;
    if (fn < 0 || !L || L->failed || L->terminated) {
        if (L && fn < 0 && !L->failed) {
            jog_lower_fail(L, pos, "missing coroutine runtime");
        }
        return jir_none();
    }
    func = jir_func(L->module, (uint32_t)fn);
    ret = func && func->type && func->type->kind == JOG_TY_FN ? func->type->as.fn.ret : NULL;
    local_ty = as_ty ? as_ty : ret;
    if (local_ty && !is_unit(local_ty)) {
        if (!jog_lower_new_local(L, NULL, local_ty, &dst)) {
            return jir_none();
        }
        dst_i = (int32_t)dst;
    }
    inst = jog_lower_emit(L, JIR_CALL, dst_i, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = jir_func_ref(L->module, (uint32_t)fn);
    if (!jir_set_args(L->module, inst, args, n)) {
        jog_lower_fail(L, pos, "out of memory");
        return jir_none();
    }
    if (dst_i < 0) {
        return jir_none();
    }
    return jog_lower_local_op(L, dst);
}

static bool load_rt(Lower *L, Rt *rt, JogPos pos) {
    JogTypes *types;
    JogType *void_ty;
    JogType *one;
    JogType *two[2];
    JogType *go_params[6];
    JogType *fn_ty;
    memset(rt, 0, sizeof(*rt));
    if (!L || !L->program || !L->program->types) {
        return false;
    }
    types = L->program->types;
    rt->unit = jog_type_builtin(types, JOG_BUILTIN_UNIT);
    rt->i32 = jog_type_builtin(types, JOG_BUILTIN_I32);
    rt->i64 = jog_type_builtin(types, JOG_BUILTIN_I64);
    rt->u32 = jog_type_builtin(types, JOG_BUILTIN_U32);
    rt->usize = jog_type_builtin(types, JOG_BUILTIN_USIZE);
    rt->boolean = jog_type_builtin(types, JOG_BUILTIN_BOOL);
    rt->err = universe_type(L, "error");
    void_ty = universe_type(L, "void");
    rt->scope_ty = universe_type(L, "Scope");
    rt->void_ptr = void_ty ? jog_type_ptr(types, void_ty, false) : NULL;
    rt->void_const = void_ty ? jog_type_ptr(types, void_ty, true) : NULL;
    rt->err_opt = rt->err ? jog_type_option(types, rt->err) : NULL;
    if (!rt->unit || !rt->i32 || !rt->i64 || !rt->u32 || !rt->usize || !rt->boolean || !rt->err || !rt->void_ptr ||
        !rt->void_const || !rt->scope_ty || !rt->err_opt) {
        jog_lower_fail(L, pos, "coroutine runtime types are missing");
        return false;
    }
    one = rt->void_ptr;
    fn_ty = jog_type_fn(types, &one, 1, rt->unit, NULL, 0, false, false, true);
    if (!fn_ty) {
        jog_lower_fail(L, pos, "out of memory");
        return false;
    }
    go_params[0] = fn_ty;
    go_params[1] = rt->void_ptr;
    go_params[2] = rt->usize;
    go_params[3] = rt->void_ptr;
    go_params[4] = rt->void_const;
    go_params[5] = rt->u32;
    two[0] = rt->void_ptr;
    two[1] = rt->usize;
    rt->scope_root = ensure(L, "jogrt_scope_root", NULL, 0, rt->void_ptr, pos);
    rt->scope_enter = ensure(L, "jogrt_scope_enter", &rt->void_ptr, 1, rt->void_ptr, pos);
    rt->scope_leave = ensure(L, "jogrt_scope_leave", &rt->void_ptr, 1, rt->void_ptr, pos);
    rt->scope_cancel = ensure(L, "jogrt_scope_cancel", &rt->void_ptr, 1, rt->unit, pos);
    rt->scope_join = ensure(L, "jogrt_scope_join", &rt->void_ptr, 1, rt->unit, pos);
    rt->scope_fail = ensure(L, "jogrt_scope_fail", &rt->void_ptr, 1, rt->unit, pos);
    rt->go = ensure(L, "jogrt_go", go_params, 6, rt->void_ptr, pos);
    rt->yield = ensure(L, "jogrt_yield", NULL, 0, rt->unit, pos);
    rt->task_set = ensure(L, "jogrt_task_set", two, 2, rt->unit, pos);
    {
        JogType *join_params[3];
        JogType *send_params[3];
        JogType *cap_set_params[3];
        JogType *cap_get_params[2];
        JogType *chan_init_params[2];
        join_params[0] = rt->void_ptr;
        join_params[1] = rt->void_ptr;
        join_params[2] = rt->usize;
        send_params[0] = rt->void_ptr;
        send_params[1] = rt->void_ptr;
        send_params[2] = rt->usize;
        cap_set_params[0] = rt->void_ptr;
        cap_set_params[1] = rt->usize;
        cap_set_params[2] = rt->void_ptr;
        cap_get_params[0] = rt->void_ptr;
        cap_get_params[1] = rt->usize;
        chan_init_params[0] = rt->usize;
        chan_init_params[1] = rt->usize;
        rt->task_join = ensure(L, "jogrt_task_join", join_params, 3, rt->unit, pos);
        rt->chan_init = ensure(L, "jogrt_chan_init", chan_init_params, 2, rt->void_ptr, pos);
        rt->chan_free = ensure(L, "jogrt_chan_free", &rt->void_ptr, 1, rt->unit, pos);
        rt->chan_close = ensure(L, "jogrt_chan_close", &rt->void_ptr, 1, rt->unit, pos);
        rt->chan_send = ensure(L, "jogrt_chan_send", send_params, 3, rt->i32, pos);
        rt->chan_recv = ensure(L, "jogrt_chan_recv", send_params, 3, rt->i32, pos);
        rt->sel_begin = ensure(L, "jogrt_select_begin", NULL, 0, rt->unit, pos);
        rt->sel_recv = ensure(L, "jogrt_select_add_recv", send_params, 3, rt->i32, pos);
        rt->sel_send = ensure(L, "jogrt_select_add_send", send_params, 3, rt->i32, pos);
        rt->sel_default = ensure(L, "jogrt_select_add_default", NULL, 0, rt->i32, pos);
        rt->sel_timeout = ensure(L, "jogrt_select_add_timeout", &rt->i64, 1, rt->i32, pos);
        rt->sel_wait = ensure(L, "jogrt_select_wait", NULL, 0, rt->i32, pos);
        rt->sel_status = ensure(L, "jogrt_select_status", NULL, 0, rt->i32, pos);
        rt->sleep = ensure(L, "jogrt_sleep", &rt->i64, 1, rt->i32, pos);
        rt->err_cancelled = ensure(L, "jogrt_err_cancelled", NULL, 0, rt->void_ptr, pos);
        rt->err_closed = ensure(L, "jogrt_err_closed", NULL, 0, rt->void_ptr, pos);
        rt->cap_new = ensure(L, "jogrt_cap_new", &rt->usize, 1, rt->void_ptr, pos);
        rt->cap_set = ensure(L, "jogrt_cap_set", cap_set_params, 3, rt->unit, pos);
        rt->cap_get = ensure(L, "jogrt_cap_get", cap_get_params, 2, rt->void_ptr, pos);
    }
    if (L->failed || rt->go < 0 || rt->chan_send < 0 || rt->scope_leave < 0 || rt->cap_get < 0) {
        if (!L->failed) {
            jog_lower_fail(L, pos, "missing coroutine runtime");
        }
        return false;
    }
    return true;
}

static JirOperand compare(Lower *L, const Rt *rt, JirBinOp op, JirOperand a, JirOperand b, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (!L || L->failed || L->terminated) {
        return jir_none();
    }
    if (!jog_lower_new_local(L, NULL, rt->boolean, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_BINARY, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->aux = (uint32_t)op;
    inst->checked = false;
    inst->a = a;
    inst->b = b;
    return jog_lower_local_op(L, dst);
}

static JirOperand bitcast(Lower *L, JirOperand value, JogType *type, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (!L || L->failed || L->terminated) {
        return jir_none();
    }
    if (value.type == type) {
        return value;
    }
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_BITCAST, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = value;
    return jog_lower_local_op(L, dst);
}

static JirOperand aggregate(Lower *L, JogType *type, JirOperand *args, size_t n, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_AGGREGATE, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    if (!jir_set_args(L->module, inst, args, n)) {
        jog_lower_fail(L, pos, "out of memory");
        return jir_none();
    }
    return jog_lower_local_op(L, dst);
}

static JirOperand extract(Lower *L, JirOperand agg, uint32_t aux, JogType *type, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (agg.kind != JIR_OPND_LOCAL) {
        agg = jog_lower_as_local(L, agg, pos);
    }
    if (!L || L->failed || agg.kind != JIR_OPND_LOCAL) {
        return jir_none();
    }
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_EXTRACT, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = agg;
    inst->aux = aux;
    return jog_lower_local_op(L, dst);
}

static JirOperand addr_of(Lower *L, uint32_t local, JogType *void_ptr, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (!jog_lower_new_local(L, NULL, void_ptr, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_ADDR, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = jog_lower_local_op(L, local);
    return jog_lower_local_op(L, dst);
}

static JirOperand elem_ptr(Lower *L, JirOperand value, JogType *void_ptr, JogPos pos) {
    if (value.kind != JIR_OPND_LOCAL) {
        value = jog_lower_as_local(L, value, pos);
    }
    if (L->failed || value.kind != JIR_OPND_LOCAL) {
        return jir_none();
    }
    return addr_of(L, value.as.local, void_ptr, pos);
}

static JirOperand load_at(Lower *L, JirOperand ptr, JogType *type, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_LOAD, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = ptr;
    return jog_lower_local_op(L, dst);
}

static void store_at(Lower *L, JirOperand ptr, JirOperand value, JogPos pos) {
    JirInst *inst = jog_lower_emit(L, JIR_STORE, -1, pos);
    if (!inst) {
        return;
    }
    inst->a = ptr;
    inst->b = value;
}

static JirOperand iface_of(Lower *L, const Rt *rt, JirOperand ptr, JogPos pos) {
    JirOperand args[2];
    if (ptr.kind != JIR_OPND_LOCAL) {
        ptr = jog_lower_as_local(L, ptr, pos);
    }
    args[0] = ptr;
    args[1] = ptr;
    return aggregate(L, rt->err, args, 2, pos);
}

static JirOperand error_from_ptr(Lower *L, const Rt *rt, JirOperand ptr, JogPos pos) {
    JirOperand has;
    JirOperand iface;
    JirOperand args[2];
    if (ptr.kind != JIR_OPND_LOCAL && ptr.kind != JIR_OPND_NULL) {
        ptr = jog_lower_as_local(L, ptr, pos);
    }
    has = compare(L, rt, JIR_NE, ptr, jir_null(rt->void_ptr), pos);
    iface = iface_of(L, rt, ptr, pos);
    if (L->failed) {
        return jir_none();
    }
    args[0] = has;
    args[1] = iface;
    return aggregate(L, rt->err_opt, args, 2, pos);
}

static JirOperand error_from_status(Lower *L, const Rt *rt, JirOperand status, JogPos pos) {
    uint32_t ptr_l;
    uint32_t yes;
    uint32_t join;
    JirOperand cancelled;
    JirOperand is_closed;
    JirOperand has;
    JirOperand iface;
    JirOperand args[2];
    if (status.kind != JIR_OPND_LOCAL) {
        status = jog_lower_as_local(L, status, pos);
    }
    if (L->failed || status.kind != JIR_OPND_LOCAL) {
        return jir_none();
    }
    if (!jog_lower_new_local(L, NULL, rt->void_ptr, &ptr_l)) {
        return jir_none();
    }
    cancelled = call_rt(L, rt->err_cancelled, NULL, 0, NULL, pos);
    jog_lower_copy(L, ptr_l, cancelled, pos);
    if (!new_block(L, &yes) || !new_block(L, &join)) {
        return jir_none();
    }
    is_closed = compare(L, rt, JIR_EQ, status, jir_int(rt->i32, 2), pos);
    emit_cond(L, is_closed, yes, join, pos);
    use_block(L, yes);
    {
        JirOperand closed = call_rt(L, rt->err_closed, NULL, 0, NULL, pos);
        jog_lower_copy(L, ptr_l, closed, pos);
        emit_br(L, join, pos);
    }
    use_block(L, join);
    has = compare(L, rt, JIR_NE, status, jir_int(rt->i32, 0), pos);
    iface = iface_of(L, rt, jog_lower_local_op(L, ptr_l), pos);
    if (L->failed) {
        return jir_none();
    }
    args[0] = has;
    args[1] = iface;
    return aggregate(L, rt->err_opt, args, 2, pos);
}

static JirOperand finish_status(Lower *L, const Rt *rt, JirOperand chosen, JirOperand status, JogPos pos) {
    uint32_t bad;
    uint32_t neg;
    uint32_t keep;
    uint32_t join;
    uint32_t closed_b;
    uint32_t one_b;
    uint32_t after;
    JirOperand lt;
    JirOperand eq;
    if (!jog_lower_new_local(L, NULL, rt->i32, &bad)) {
        return jir_none();
    }
    jog_lower_copy(L, bad, status, pos);
    if (!new_block(L, &neg) || !new_block(L, &keep) || !new_block(L, &join)) {
        return jir_none();
    }
    lt = compare(L, rt, JIR_LT, chosen, jir_int(rt->i32, 0), pos);
    emit_cond(L, lt, neg, keep, pos);
    use_block(L, neg);
    if (!new_block(L, &closed_b) || !new_block(L, &one_b) || !new_block(L, &after)) {
        return jir_none();
    }
    eq = compare(L, rt, JIR_EQ, chosen, jir_int(rt->i32, (uint64_t)(int64_t)-2), pos);
    emit_cond(L, eq, closed_b, one_b, pos);
    use_block(L, closed_b);
    jog_lower_copy(L, bad, jir_int(rt->i32, 2), pos);
    emit_br(L, after, pos);
    use_block(L, one_b);
    jog_lower_copy(L, bad, jir_int(rt->i32, 1), pos);
    emit_br(L, after, pos);
    use_block(L, after);
    emit_br(L, join, pos);
    use_block(L, keep);
    emit_br(L, join, pos);
    use_block(L, join);
    return error_from_status(L, rt, jog_lower_local_op(L, bad), pos);
}

static void maybe_fail(Lower *L, const Rt *rt, JirOperand value, JogType *type, JogPos pos) {
    JirOperand has;
    JirOperand iface;
    JirOperand ptr;
    uint32_t yes;
    uint32_t join;
    if (!is_error_option(type) || !L || L->failed || L->terminated) {
        return;
    }
    if (value.kind != JIR_OPND_LOCAL) {
        value = jog_lower_as_local(L, value, pos);
    }
    has = extract(L, value, 0, rt->boolean, pos);
    iface = extract(L, value, 1, rt->err, pos);
    ptr = extract(L, iface, 0, rt->void_ptr, pos);
    if (L->failed || !new_block(L, &yes) || !new_block(L, &join)) {
        return;
    }
    emit_cond(L, has, yes, join, pos);
    use_block(L, yes);
    call_rt(L, rt->scope_fail, &ptr, 1, NULL, pos);
    emit_br(L, join, pos);
    use_block(L, join);
}

static void task_set(Lower *L, const Rt *rt, JirOperand value, JogPos pos) {
    JirOperand args[2];
    JogType *type = value.type;
    if (!L || L->failed || L->terminated || !type || is_unit(type) || type->size == 0) {
        return;
    }
    args[0] = elem_ptr(L, value, rt->void_ptr, pos);
    args[1] = jir_int(rt->usize, type->size);
    call_rt(L, rt->task_set, args, 2, NULL, pos);
}

static bool push_scope(Lower *L, uint32_t local, JogPos pos) {
    if (L->coro_scope_n >= L->coro_scope_cap) {
        size_t cap = L->coro_scope_cap ? L->coro_scope_cap * 2 : 8;
        uint32_t *next = ct_arena_alloc(L->program->arena, cap * sizeof(uint32_t), _Alignof(uint32_t));
        if (!next) {
            jog_lower_fail(L, pos, "out of memory");
            return false;
        }
        if (L->coro_scopes && L->coro_scope_n) {
            memcpy(next, L->coro_scopes, L->coro_scope_n * sizeof(uint32_t));
        }
        L->coro_scopes = next;
        L->coro_scope_cap = cap;
    }
    L->coro_scopes[L->coro_scope_n++] = local;
    return true;
}

void jog_lower_coro_on_exit(Lower *L, JogPos pos) {
    Rt rt;
    size_t i;
    if (!L || L->failed || L->terminated) {
        return;
    }
    /* Programs that never enter a scope or launch a task keep their IR.
       A go lowered earlier in this function sets coro_seq, so a later
       return of root main still joins. A return that precedes every go
       has not launched anything yet. */
    if (L->coro_scope_n == 0 && (L->in_coro || L->coro_seq == 0 || !is_root_main(L))) {
        return;
    }
    if (!load_rt(L, &rt, pos)) {
        return;
    }
    for (i = L->coro_scope_n; i-- > 0;) {
        JirOperand scope = jog_lower_local_op(L, L->coro_scopes[i]);
        call_rt(L, rt.scope_leave, &scope, 1, NULL, pos);
        if (L->failed || L->terminated) {
            return;
        }
    }
    if (!L->in_coro && is_root_main(L)) {
        JirOperand root = call_rt(L, rt.scope_root, NULL, 0, NULL, pos);
        call_rt(L, rt.scope_join, &root, 1, NULL, pos);
    }
}

static JirOperand leave_top(Lower *L, const Rt *rt, JogPos pos) {
    uint32_t local;
    JirOperand scope;
    if (!L || L->coro_scope_n == 0 || L->failed || L->terminated) {
        return jir_null(rt->void_ptr);
    }
    local = L->coro_scopes[--L->coro_scope_n];
    scope = jog_lower_local_op(L, local);
    return call_rt(L, rt->scope_leave, &scope, 1, NULL, pos);
}

static void lower_result_body(Lower *L, const Rt *rt, JogNode *body, int publish) {
    size_t n;
    size_t i;
    size_t base;
    size_t limit;
    JogNode *last;
    int tail;
    if (!body || !L || L->failed) {
        return;
    }
    if (body->kind != JOG_NODE_BLOCK) {
        JirOperand value = jog_lower_expr(L, body);
        if (L->failed || L->terminated) {
            return;
        }
        maybe_fail(L, rt, value, body->type, body->pos);
        if (publish) {
            task_set(L, rt, value, body->pos);
        }
        return;
    }
    n = body->as.block.stmts.count;
    base = L->cleanups.count;
    last = n ? list_at(body->as.block.stmts, n - 1) : NULL;
    tail = last && last->kind == JOG_NODE_EXPR_STMT && last->as.expr_stmt.expr &&
           last->as.expr_stmt.expr->kind != JOG_NODE_IF && last->as.expr_stmt.expr->kind != JOG_NODE_WHEN;
    limit = tail ? n - 1 : n;
    for (i = 0; i < limit && !L->failed && !L->terminated; i++) {
        jog_lower_stmt(L, list_at(body->as.block.stmts, i));
    }
    if (tail && !L->failed && !L->terminated) {
        JogNode *expr = last->as.expr_stmt.expr;
        JirOperand value = expr ? jog_lower_expr(L, expr) : jir_none();
        if (!L->failed && !L->terminated) {
            maybe_fail(L, rt, value, expr ? expr->type : NULL, last->pos);
            if (publish) {
                task_set(L, rt, value, last->pos);
            }
        }
    }
    jog_lower_error_block_end(L, base, body->pos);
}

static JirOperand lower_scope(Lower *L, const Rt *rt, JogNode *call) {
    JogNode *lambda = call->as.call.trailing;
    JogNode *body = lambda ? lambda->as.lambda.body : NULL;
    JirOperand parent = jir_null(rt->void_ptr);
    JirOperand entered;
    JirOperand left;
    if (!body) {
        jog_lower_fail(L, call->pos, "scope needs a block");
        return jir_none();
    }
    entered = call_rt(L, rt->scope_enter, &parent, 1, rt->scope_ty, call->pos);
    if (L->failed || entered.kind != JIR_OPND_LOCAL) {
        return jir_none();
    }
    if (call->symbol && !jog_lower_bind_local(L, call->symbol, entered.as.local)) {
        return jir_none();
    }
    if (!push_scope(L, entered.as.local, call->pos)) {
        return jir_none();
    }
    lower_result_body(L, rt, body, 0);
    if (L->failed || L->terminated) {
        return jir_none();
    }
    left = leave_top(L, rt, call->pos);
    return error_from_ptr(L, rt, left, call->pos);
}

static const char *case_name(JogNode *expr) {
    JogNode *callee;
    if (!expr || expr->kind != JOG_NODE_CALL) {
        return NULL;
    }
    callee = expr->as.call.callee;
    if (!callee || callee->kind != JOG_NODE_IDENT) {
        return NULL;
    }
    return callee->as.ident.name;
}

static void lower_case_body(Lower *L, JogNode *lambda, int has_slot, uint32_t slot) {
    size_t base;
    JogNode *body;
    if (!lambda || lambda->kind != JOG_NODE_LAMBDA) {
        return;
    }
    base = L->locals.count;
    if (has_slot && lambda->symbol) {
        if (!jog_lower_bind_local(L, lambda->symbol, slot)) {
            L->locals.count = base;
            return;
        }
    }
    body = lambda->as.lambda.body;
    if (body) {
        if (body->kind == JOG_NODE_BLOCK) {
            jog_lower_stmt(L, body);
        } else {
            (void)jog_lower_expr(L, body);
        }
    }
    L->locals.count = base;
}

static JirOperand lower_select(Lower *L, const Rt *rt, JogNode *call) {
    typedef struct SelCase {
        JirOperand id;
        JogNode *lambda;
        uint32_t slot;
        int has_slot;
    } SelCase;
    JogNode *lambda = call->as.call.trailing;
    JogNode *block = lambda ? lambda->as.lambda.body : NULL;
    SelCase cases[SEL_MAX];
    size_t n;
    size_t i;
    JirOperand chosen;
    JirOperand status;
    JirOperand status_ok;
    uint32_t done;
    memset(cases, 0, sizeof(cases));
    if (!block || block->kind != JOG_NODE_BLOCK) {
        jog_lower_fail(L, call->pos, "select needs a block");
        return jir_none();
    }
    n = block->as.block.stmts.count;
    if (n == 0 || n > SEL_MAX) {
        jog_lower_fail(L, call->pos, n == 0 ? "select needs a case" : "too many select cases");
        return jir_none();
    }
    call_rt(L, rt->sel_begin, NULL, 0, NULL, call->pos);
    for (i = 0; i < n && !L->failed && !L->terminated; i++) {
        JogNode *stmt = list_at(block->as.block.stmts, i);
        JogNode *expr = stmt && stmt->kind == JOG_NODE_EXPR_STMT ? stmt->as.expr_stmt.expr : stmt;
        const char *name = case_name(expr);
        JogNode *chan_node;
        JogType *chan_ty;
        JogType *elem;
        JirOperand chan;
        JirOperand args[3];
        if (!name) {
            jog_lower_fail(L, stmt ? stmt->pos : call->pos, "select case must be recv, send, timeout, or default");
            return jir_none();
        }
        cases[i].lambda = expr->as.call.trailing;
        if (strcmp(name, "default") == 0) {
            cases[i].id = call_rt(L, rt->sel_default, NULL, 0, NULL, expr->pos);
            continue;
        }
        if (strcmp(name, "timeout") == 0) {
            JogNode *ms = arg_value(list_at(expr->as.call.args, 0));
            JirOperand ms_op = ms ? jog_lower_expr(L, ms) : jir_int(rt->i64, 0);
            cases[i].id = call_rt(L, rt->sel_timeout, &ms_op, 1, NULL, expr->pos);
            continue;
        }
        chan_node = arg_value(list_at(expr->as.call.args, 0));
        chan = chan_node ? jog_lower_expr(L, chan_node) : jir_none();
        chan_ty = chan_node ? chan_node->type : NULL;
        elem = elem_of(chan_ty);
        if (!elem) {
            jog_lower_fail(L, expr->pos, "select case needs a Chan");
            return jir_none();
        }
        args[0] = chan;
        args[2] = jir_int(rt->usize, elem->size);
        if (strcmp(name, "recv") == 0) {
            uint32_t slot;
            if (!jog_lower_new_local(L, NULL, elem, &slot)) {
                return jir_none();
            }
            jog_lower_zero(L, slot, expr->pos);
            args[1] = addr_of(L, slot, rt->void_ptr, expr->pos);
            cases[i].id = call_rt(L, rt->sel_recv, args, 3, NULL, expr->pos);
            cases[i].slot = slot;
            cases[i].has_slot = 1;
            continue;
        }
        {
            JogNode *value_node = arg_value(list_at(expr->as.call.args, 1));
            JirOperand value = value_node ? jog_lower_expr(L, value_node) : jir_none();
            args[1] = elem_ptr(L, value, rt->void_ptr, expr->pos);
            cases[i].id = call_rt(L, rt->sel_send, args, 3, NULL, expr->pos);
        }
    }
    if (L->failed || L->terminated) {
        return jir_none();
    }
    chosen = call_rt(L, rt->sel_wait, NULL, 0, NULL, call->pos);
    status = call_rt(L, rt->sel_status, NULL, 0, NULL, call->pos);
    status_ok = compare(L, rt, JIR_EQ, status, jir_int(rt->i32, 0), call->pos);
    if (L->failed || !new_block(L, &done)) {
        return jir_none();
    }
    for (i = 0; i < n && !L->failed; i++) {
        uint32_t gate;
        uint32_t yes;
        uint32_t no;
        JirOperand eq;
        if (!new_block(L, &gate) || !new_block(L, &yes) || !new_block(L, &no)) {
            return jir_none();
        }
        eq = compare(L, rt, JIR_EQ, chosen, cases[i].id, call->pos);
        emit_cond(L, eq, gate, no, call->pos);
        use_block(L, gate);
        emit_cond(L, status_ok, yes, no, call->pos);
        use_block(L, yes);
        lower_case_body(L, cases[i].lambda, cases[i].has_slot, cases[i].slot);
        if (!L->terminated) {
            emit_br(L, done, call->pos);
        }
        use_block(L, no);
    }
    if (!L->terminated) {
        emit_br(L, done, call->pos);
    }
    use_block(L, done);
    return finish_status(L, rt, chosen, status, call->pos);
}

static Cap *cap_of(const Lower *L, const JogSymbol *symbol) {
    Cap *caps;
    size_t i;
    if (!L || !symbol || !L->coro_caps) {
        return NULL;
    }
    caps = L->coro_caps;
    for (i = 0; i < L->coro_cap_count; i++) {
        if (caps[i].symbol == symbol) {
            return &caps[i];
        }
    }
    return NULL;
}

static bool grow_caps(Scan *s) {
    size_t cap = s->cap ? s->cap * 2 : 8;
    Cap *next = ct_arena_alloc(s->L->program->arena, cap * sizeof(Cap), _Alignof(Cap));
    if (!next) {
        jog_lower_fail(s->L, (JogPos){0}, "out of memory");
        return false;
    }
    if (s->caps && s->count) {
        memcpy(next, s->caps, s->count * sizeof(Cap));
    }
    s->caps = next;
    s->cap = cap;
    return true;
}

static void add_cap(Scan *s, const JogSymbol *symbol) {
    Cap *parent;
    uint32_t local = 0;
    size_t i;
    if (!symbol || !s || s->L->failed) {
        return;
    }
    for (i = 0; i < s->count; i++) {
        if (s->caps[i].symbol == symbol) {
            return;
        }
    }
    parent = cap_of(s->L, symbol);
    if (!parent && !jog_lower_find_local(s->L, symbol, &local)) {
        return;
    }
    if (s->count >= s->cap && !grow_caps(s)) {
        return;
    }
    s->caps[s->count].symbol = symbol;
    s->caps[s->count].indirect = parent != NULL;
    if (parent) {
        s->caps[s->count].indirect_slot = (uint32_t)(parent - (Cap *)s->L->coro_caps);
        s->caps[s->count].parent_local = 0;
    } else {
        s->caps[s->count].parent_local = local;
        s->caps[s->count].indirect_slot = 0;
    }
    s->count++;
}

static void scan_list(Scan *s, JogList list) {
    size_t i;
    for (i = 0; i < list.count && !s->L->failed; i++) {
        scan_node(s, list_at(list, i));
    }
}

static void scan_node(Scan *s, JogNode *node) {
    if (!node || !s || s->L->failed) {
        return;
    }
    switch (node->kind) {
    case JOG_NODE_IDENT:
        if (node->symbol) {
            add_cap(s, node->symbol);
        }
        break;
    case JOG_NODE_BLOCK:
    case JOG_NODE_INTERP:
    case JOG_NODE_TUPLE:
        scan_list(s, node->kind == JOG_NODE_BLOCK ? node->as.block.stmts
                          : node->kind == JOG_NODE_INTERP ? node->as.interp.parts
                                                          : node->as.tuple.elems);
        break;
    case JOG_NODE_EXPR_STMT:
        scan_node(s, node->as.expr_stmt.expr);
        break;
    case JOG_NODE_ASSIGN:
        scan_node(s, node->as.assign.target);
        scan_node(s, node->as.assign.value);
        break;
    case JOG_NODE_DESTRUCTURE:
        scan_list(s, node->as.destructure.targets);
        scan_node(s, node->as.destructure.value);
        break;
    case JOG_NODE_WITH:
        scan_list(s, node->as.with.values);
        scan_node(s, node->as.with.body);
        break;
    case JOG_NODE_DEFER:
    case JOG_NODE_ERRDEFER:
        scan_node(s, node->as.defer.body);
        break;
    case JOG_NODE_ATTR_BLOCK:
        scan_node(s, node->as.attr_block.body);
        break;
    case JOG_NODE_RETURN:
        scan_list(s, node->as.ret.values);
        break;
    case JOG_NODE_FOR:
        scan_node(s, node->as.for_.expr);
        scan_node(s, node->as.for_.body);
        break;
    case JOG_NODE_VAL:
    case JOG_NODE_VAR:
        scan_node(s, node->as.binding.init);
        break;
    case JOG_NODE_UNARY:
    case JOG_NODE_FORCE:
        scan_node(s, node->as.unary.operand);
        break;
    case JOG_NODE_BINARY:
        scan_node(s, node->as.binary.lhs);
        scan_node(s, node->as.binary.rhs);
        break;
    case JOG_NODE_CAST:
        scan_node(s, node->as.cast.expr);
        break;
    case JOG_NODE_IS:
        scan_node(s, node->as.is.expr);
        break;
    case JOG_NODE_CALL:
        scan_node(s, node->as.call.callee);
        scan_list(s, node->as.call.args);
        scan_node(s, node->as.call.trailing);
        scan_list(s, node->as.call.contexts);
        break;
    case JOG_NODE_ARG:
        scan_node(s, node->as.arg.value);
        break;
    case JOG_NODE_INDEX:
        scan_node(s, node->as.index.base);
        scan_node(s, node->as.index.index);
        break;
    case JOG_NODE_SLICE:
        scan_node(s, node->as.slice.base);
        scan_node(s, node->as.slice.lo);
        scan_node(s, node->as.slice.hi);
        break;
    case JOG_NODE_MEMBER:
        scan_node(s, node->as.member.base);
        break;
    case JOG_NODE_IF:
        scan_node(s, node->as.if_.cond);
        scan_node(s, node->as.if_.then_block);
        scan_node(s, node->as.if_.else_branch);
        break;
    case JOG_NODE_WHEN:
        scan_node(s, node->as.when.subject);
        scan_list(s, node->as.when.arms);
        break;
    case JOG_NODE_WHEN_ARM:
        scan_list(s, node->as.when_arm.conds);
        scan_node(s, node->as.when_arm.body);
        break;
    case JOG_NODE_LAMBDA:
        scan_node(s, node->as.lambda.body);
        break;
    case JOG_NODE_GO:
        scan_node(s, node->as.go.body);
        break;
    case JOG_NODE_ARRAY_LIT:
        scan_list(s, node->as.array_lit.elems);
        break;
    case JOG_NODE_BUILTIN:
        scan_list(s, node->as.builtin.args);
        break;
    case JOG_NODE_PAT_EXPR:
        scan_node(s, node->as.pat_expr.expr);
        break;
    case JOG_NODE_PAT_FIELD:
        scan_node(s, node->as.pat_field.pattern);
        break;
    case JOG_NODE_PAT_VARIANT:
        scan_list(s, node->as.pat_variant.fields);
        break;
    default:
        break;
    }
}

static JirOperand fill_caps(Lower *L, const Rt *rt, Cap *caps, size_t n, JogPos pos) {
    JirOperand env;
    JirOperand count;
    size_t i;
    if (n == 0) {
        return jir_null(rt->void_ptr);
    }
    count = jir_int(rt->usize, n);
    env = call_rt(L, rt->cap_new, &count, 1, NULL, pos);
    for (i = 0; i < n && !L->failed && !L->terminated; i++) {
        JirOperand ptr;
        JirOperand args[3];
        if (caps[i].indirect) {
            JirOperand get_args[2];
            get_args[0] = jog_lower_local_op(L, 0);
            get_args[1] = jir_int(rt->usize, caps[i].indirect_slot);
            ptr = call_rt(L, rt->cap_get, get_args, 2, NULL, pos);
        } else {
            ptr = addr_of(L, caps[i].parent_local, rt->void_ptr, pos);
        }
        args[0] = env;
        args[1] = jir_int(rt->usize, i);
        args[2] = ptr;
        call_rt(L, rt->cap_set, args, 3, NULL, pos);
    }
    return env;
}

static JirOperand capture_ptr(Lower *L, const Rt *rt, const Cap *cap, JogPos pos) {
    JirOperand args[2];
    size_t index = (size_t)(cap - (const Cap *)L->coro_caps);
    args[0] = jog_lower_local_op(L, 0);
    args[1] = jir_int(rt->usize, index);
    return call_rt(L, rt->cap_get, args, 2, NULL, pos);
}

static void lower_list(Lower *L, JogList list) {
    size_t i;
    for (i = 0; i < list.count && !L->failed && !L->terminated; i++) {
        JogNode *expr = list_at(list, i);
        if (expr) {
            (void)jog_lower_expr(L, expr);
        }
    }
}

static JirOperand lower_stack(Lower *L, const Rt *rt, JogNode *go) {
    JogNode *value;
    if (go->as.go.args.count == 0) {
        return jir_int(rt->usize, 0);
    }
    value = arg_value(list_at(go->as.go.args, 0));
    if (!value) {
        return jir_int(rt->usize, 0);
    }
    return jog_lower_expr(L, value);
}

static JirOperand lower_go_scope(Lower *L, const Rt *rt, JogNode *go) {
    if (go->context_arg_count > 0 && go->context_args && go->context_args[0]) {
        return jog_lower_expr(L, go->context_args[0]);
    }
    if (is_root_main(L)) {
        return call_rt(L, rt->scope_root, NULL, 0, NULL, go->pos);
    }
    jog_lower_fail(L, go->pos, "missing context Scope");
    return jir_none();
}

static int swap_child(Lower *L, Saved *saved, JogPos pos) {
    size_t clean_size = saved->cleanups.item_size ? saved->cleanups.item_size : sizeof(void *);
    saved->locals = L->locals;
    saved->loops = L->loops;
    saved->cleanups = L->cleanups;
    if (!ct_da_init(&L->locals, sizeof(LocalBind)) || !ct_da_init(&L->loops, sizeof(LoopFrame)) ||
        !ct_da_init(&L->cleanups, clean_size)) {
        ct_da_free(&L->locals);
        ct_da_free(&L->loops);
        ct_da_free(&L->cleanups);
        L->locals = saved->locals;
        L->loops = saved->loops;
        L->cleanups = saved->cleanups;
        jog_lower_fail(L, pos, "out of memory");
        return 0;
    }
    return 1;
}

static void restore_child(Lower *L, Saved *saved) {
    ct_da_free(&L->locals);
    ct_da_free(&L->loops);
    ct_da_free(&L->cleanups);
    L->locals = saved->locals;
    L->loops = saved->loops;
    L->cleanups = saved->cleanups;
    L->func = saved->func;
    L->ast_fn = saved->ast_fn;
    L->block = saved->block;
    L->terminated = saved->terminated;
    L->coro_caps = saved->caps;
    L->coro_cap_count = saved->cap_count;
    L->in_coro = saved->in_coro;
    if (saved->scope_n && saved->scope_copy && L->coro_scopes) {
        memcpy(L->coro_scopes, saved->scope_copy, saved->scope_n * sizeof(uint32_t));
    }
    L->coro_scope_n = saved->scope_n;
}

JirOperand jog_lower_go(Lower *L, JogNode *node) {
    Rt rt;
    Scan scan;
    Saved saved;
    JirFunc *child;
    JirOperand env;
    JirOperand scope_op;
    JirOperand stack;
    JirOperand file_op;
    JirOperand args[6];
    JogType *fn_ty;
    JogType *env_ty;
    const char *param_name = "env";
    char buf[32];
    char *name;
    int n;
    int32_t index;
    int swapped = 0;
    uint32_t entry;
    if (!L || !node || L->failed) {
        return jir_none();
    }
    if (!load_rt(L, &rt, node->pos)) {
        return jir_none();
    }
    memset(&scan, 0, sizeof(scan));
    memset(&saved, 0, sizeof(saved));
    scan.L = L;
    /* Body only. stack: and with-clause expressions are launch-site values.
       Arguments of a call body are part of that body, so they run in the child. */
    scan_node(&scan, node->as.go.body);
    if (L->failed) {
        return jir_none();
    }
    lower_list(L, node->as.go.contexts);
    scope_op = lower_go_scope(L, &rt, node);
    env = fill_caps(L, &rt, scan.caps, scan.count, node->pos);
    if (L->failed || L->terminated) {
        return jir_none();
    }
    n = snprintf(buf, sizeof(buf), "jog_coro_%u", L->coro_seq++);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        jog_lower_fail(L, node->pos, "out of memory");
        return jir_none();
    }
    name = ct_arena_alloc(L->program->arena, (size_t)n + 1, 1);
    env_ty = rt.void_ptr;
    fn_ty = name ? jog_type_fn(L->program->types, &env_ty, 1, rt.unit, NULL, 0, false, false, true) : NULL;
    if (!name || !fn_ty) {
        jog_lower_fail(L, node->pos, "out of memory");
        return jir_none();
    }
    memcpy(name, buf, (size_t)n + 1);
    index = jir_func_add(L->module, name, name, JIR_LINK_INTERNAL, fn_ty, &param_name);
    if (index < 0) {
        jog_lower_fail(L, node->pos, "out of memory");
        return jir_none();
    }
    child = jir_func(L->module, (uint32_t)index);
    saved.func = L->func;
    saved.ast_fn = L->ast_fn;
    saved.block = L->block;
    saved.terminated = L->terminated;
    saved.caps = L->coro_caps;
    saved.cap_count = L->coro_cap_count;
    saved.in_coro = L->in_coro;
    saved.scope_n = L->coro_scope_n;
    if (saved.scope_n) {
        saved.scope_copy = ct_arena_alloc(L->program->arena, saved.scope_n * sizeof(uint32_t), _Alignof(uint32_t));
        if (!saved.scope_copy) {
            jog_lower_fail(L, node->pos, "out of memory");
            return jir_none();
        }
        memcpy(saved.scope_copy, L->coro_scopes, saved.scope_n * sizeof(uint32_t));
    }
    saved.cleanups = L->cleanups;
    if (!swap_child(L, &saved, node->pos)) {
        return jir_none();
    }
    L->func = child;
    L->ast_fn = NULL;
    L->terminated = false;
    L->coro_caps = scan.caps;
    L->coro_cap_count = scan.count;
    L->in_coro = 1;
    L->coro_scope_n = 0;
    entry = jir_block_add(child, "entry");
    L->block = entry;
    swapped = 1;
    lower_result_body(L, &rt, node->as.go.body, 1);
    if (!L->failed && !L->terminated) {
        jog_lower_coro_on_exit(L, node->pos);
        if (!L->terminated) {
            emit_ret(L, node->pos);
        }
    }
    restore_child(L, &saved);
    swapped = 0;
    if (L->failed) {
        return jir_none();
    }
    (void)swapped;
    stack = lower_stack(L, &rt, node);
    if (node->pos.file) {
        file_op = jir_cstring(rt.void_const, node->pos.file, strlen(node->pos.file));
    } else {
        file_op = jir_null(rt.void_const);
    }
    args[0] = jir_func_ref(L->module, (uint32_t)index);
    args[1] = env;
    args[2] = stack;
    args[3] = scope_op;
    args[4] = file_op;
    args[5] = jir_int(rt.u32, node->pos.line);
    return call_rt(L, rt.go, args, 6, node->type, node->pos);
}

bool jog_lower_coro_name(Lower *L, JogNode *node, const JogSymbol *symbol, JirOperand *out) {
    Rt rt;
    Cap *cap;
    JogType *ptr_ty;
    JirOperand raw;
    JirOperand ptr;
    if (!L || !L->in_coro || !symbol || !out) {
        return false;
    }
    cap = cap_of(L, symbol);
    if (!cap) {
        return false;
    }
    if (!load_rt(L, &rt, node ? node->pos : (JogPos){0})) {
        return true;
    }
    raw = capture_ptr(L, &rt, cap, node->pos);
    ptr_ty = jog_type_ptr(L->program->types, symbol->type, false);
    ptr = bitcast(L, raw, ptr_ty, node->pos);
    *out = load_at(L, ptr, symbol->type, node->pos);
    return true;
}

static JogTokenKind compound_base(JogTokenKind op) {
    switch (op) {
    case JOG_TOK_PLUS_ASSIGN:
        return JOG_TOK_PLUS;
    case JOG_TOK_MINUS_ASSIGN:
        return JOG_TOK_MINUS;
    case JOG_TOK_STAR_ASSIGN:
        return JOG_TOK_STAR;
    case JOG_TOK_SLASH_ASSIGN:
        return JOG_TOK_SLASH;
    case JOG_TOK_PERCENT_ASSIGN:
        return JOG_TOK_PERCENT;
    case JOG_TOK_AMP_ASSIGN:
        return JOG_TOK_AMP;
    case JOG_TOK_PIPE_ASSIGN:
        return JOG_TOK_PIPE;
    case JOG_TOK_CARET_ASSIGN:
        return JOG_TOK_CARET;
    case JOG_TOK_SHL_ASSIGN:
        return JOG_TOK_SHL;
    case JOG_TOK_SHR_ASSIGN:
        return JOG_TOK_SHR;
    default:
        return JOG_TOK_EOF;
    }
}

static bool compound_binop(JogTokenKind op, JirBinOp *out, bool *checked) {
    *checked = false;
    switch (op) {
    case JOG_TOK_PLUS:
        *out = JIR_ADD;
        *checked = true;
        return true;
    case JOG_TOK_MINUS:
        *out = JIR_SUB;
        *checked = true;
        return true;
    case JOG_TOK_STAR:
        *out = JIR_MUL;
        *checked = true;
        return true;
    case JOG_TOK_SLASH:
        *out = JIR_DIV;
        *checked = true;
        return true;
    case JOG_TOK_PERCENT:
        *out = JIR_REM;
        *checked = true;
        return true;
    case JOG_TOK_AMP:
        *out = JIR_AND;
        return true;
    case JOG_TOK_PIPE:
        *out = JIR_OR;
        return true;
    case JOG_TOK_CARET:
        *out = JIR_XOR;
        return true;
    case JOG_TOK_SHL:
        *out = JIR_SHL;
        *checked = true;
        return true;
    case JOG_TOK_SHR:
        *out = JIR_SHR;
        *checked = true;
        return true;
    default:
        return false;
    }
}

static JirOperand emit_binop(Lower *L, JirBinOp op, bool checked, JirOperand a, JirOperand b, JogType *type, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (!L || L->failed || L->terminated) {
        return jir_none();
    }
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_BINARY, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->aux = (uint32_t)op;
    inst->checked = checked;
    inst->a = a;
    inst->b = b;
    return jog_lower_local_op(L, dst);
}

bool jog_lower_coro_store(Lower *L, JogNode *node) {
    Rt rt;
    JogNode *target;
    Cap *cap;
    JogType *ptr_ty;
    JogType *ty;
    JogTokenKind base;
    JirOperand value;
    JirOperand raw;
    JirOperand ptr;
    if (!L || !L->in_coro || !node || node->kind != JOG_NODE_ASSIGN) {
        return false;
    }
    target = node->as.assign.target;
    if (!target || target->kind != JOG_NODE_IDENT || !target->symbol) {
        return false;
    }
    cap = cap_of(L, target->symbol);
    if (!cap) {
        return false;
    }
    base = compound_base(node->as.assign.op);
    if (node->as.assign.op != JOG_TOK_ASSIGN && base == JOG_TOK_EOF) {
        jog_lower_fail(L, node->pos, "operator '%s' is not lowered yet", jog_token_spelling(node->as.assign.op));
        return true;
    }
    if (!load_rt(L, &rt, node->pos)) {
        return true;
    }
    ty = target->symbol->type ? target->symbol->type : target->type;
    raw = capture_ptr(L, &rt, cap, node->pos);
    ptr_ty = jog_type_ptr(L->program->types, ty, false);
    ptr = bitcast(L, raw, ptr_ty, node->pos);
    if (L->failed || L->terminated) {
        return true;
    }
    if (base != JOG_TOK_EOF) {
        JirBinOp bin;
        bool checked = false;
        /* Read the capture before the right-hand side, matching a = a + e.
           Pointer += / -= loads that pointer, then the offset, then stores. */
        if ((base == JOG_TOK_PLUS || base == JOG_TOK_MINUS) && ty && ty->kind == JOG_TY_PTR) {
            jog_lower_memory_ptr_update(L, ptr, base, node->as.assign.value, ty, node->pos);
            return true;
        }
        {
            JirOperand lhs = load_at(L, ptr, ty, node->pos);
            JirOperand rhs;
            if (L->failed || L->terminated) {
                return true;
            }
            rhs = jog_lower_expr(L, node->as.assign.value);
            if (L->failed || L->terminated) {
                return true;
            }
            if (!compound_binop(base, &bin, &checked)) {
                jog_lower_fail(L, node->pos, "operator '%s' is not lowered yet", jog_token_spelling(node->as.assign.op));
                return true;
            }
            value = emit_binop(L, bin, checked, lhs, rhs, ty ? ty : rhs.type, node->pos);
            if (L->failed || L->terminated) {
                return true;
            }
            store_at(L, ptr, value, node->pos);
            return true;
        }
    }
    value = jog_lower_expr(L, node->as.assign.value);
    if (L->failed || L->terminated) {
        return true;
    }
    store_at(L, ptr, value, node->pos);
    return true;
}

bool jog_lower_coro_error(Lower *L, JogNode *node, const JogSymbol *symbol, JirOperand *out) {
    Rt rt;
    int32_t fn;
    JirOperand ptr;
    if (!symbol || symbol->kind != JOG_SYM_CONSTANT || symbol->decl || !symbol->name || !out) {
        return false;
    }
    if (strcmp(symbol->name, "ErrCancelled") == 0) {
        fn = 1;
    } else if (strcmp(symbol->name, "ErrClosed") == 0) {
        fn = 2;
    } else {
        return false;
    }
    if (!load_rt(L, &rt, node ? node->pos : (JogPos){0})) {
        return true;
    }
    ptr = call_rt(L, fn == 1 ? rt.err_cancelled : rt.err_closed, NULL, 0, NULL, node->pos);
    *out = iface_of(L, &rt, ptr, node->pos);
    return true;
}

static JirOperand lower_go_tuple(Lower *L, JogNode *node, size_t count) {
    JirOperand *args;
    JogType **elems;
    JogType *tuple;
    size_t i;
    args = ct_arena_alloc(L->program->arena, count * sizeof(JirOperand), _Alignof(JirOperand));
    elems = ct_arena_alloc(L->program->arena, count * sizeof(JogType *), _Alignof(JogType *));
    if (!args || !elems) {
        jog_lower_fail(L, node->pos, "out of memory");
        return jir_none();
    }
    for (i = 0; i < count; i++) {
        JogNode *value_node = list_at(node->as.ret.values, i);
        args[i] = value_node ? jog_lower_expr(L, value_node) : jir_none();
        if (L->failed || L->terminated) {
            return jir_none();
        }
        elems[i] = args[i].type;
        if (!elems[i]) {
            jog_lower_fail(L, value_node ? value_node->pos : node->pos, "return value has no type");
            return jir_none();
        }
    }
    tuple = jog_type_tuple(L->program->types, elems, count);
    if (!tuple) {
        jog_lower_fail(L, node->pos, "out of memory");
        return jir_none();
    }
    return aggregate(L, tuple, args, count, node->pos);
}

/* A tuple's last error? fails the scope, matching a bare error? result. */
static void fail_last_error(Lower *L, const Rt *rt, JirOperand value, JogPos pos) {
    JogType *type = value.type;
    JogType *last;
    size_t n;
    if (!type || type->kind != JOG_TY_TUPLE || type->as.tuple.count == 0 || !type->as.tuple.elems) {
        return;
    }
    n = type->as.tuple.count;
    last = type->as.tuple.elems[n - 1];
    if (!is_error_option(last)) {
        return;
    }
    maybe_fail(L, rt, extract(L, value, (uint32_t)(n - 1), last, pos), last, pos);
}

bool jog_lower_coro_return(Lower *L, JogNode *node) {
    Rt rt;
    size_t count;
    size_t saved;
    if (!L || !L->in_coro || !node) {
        return false;
    }
    if (!load_rt(L, &rt, node->pos)) {
        return true;
    }
    count = node->as.ret.values.count;
    if (count > 1) {
        JirOperand value = lower_go_tuple(L, node, count);
        if (!L->failed && !L->terminated) {
            fail_last_error(L, &rt, value, node->pos);
            task_set(L, &rt, value, node->pos);
        }
    }
    if (count == 1) {
        JogNode *value_node = list_at(node->as.ret.values, 0);
        JirOperand value = value_node ? jog_lower_expr(L, value_node) : jir_none();
        if (!L->failed && !L->terminated) {
            maybe_fail(L, &rt, value, value_node ? value_node->type : NULL, node->pos);
            task_set(L, &rt, value, node->pos);
        }
    }
    if (!L->failed && !L->terminated) {
        jog_lower_coro_on_exit(L, node->pos);
    }
    saved = node->as.ret.values.count;
    node->as.ret.values.count = 0;
    if (!L->failed && !L->terminated) {
        jog_lower_error_return(L, node);
    }
    node->as.ret.values.count = saved;
    return true;
}

static JirOperand lower_init(Lower *L, const Rt *rt, JogNode *call) {
    JogNode *base = call->as.call.callee->as.member.base;
    JogType *elem = elem_of(base->type);
    JogNode *cap_node = arg_value(list_at(call->as.call.args, 0));
    JirOperand args[2];
    if (!elem) {
        jog_lower_fail(L, call->pos, "channel element has no type");
        return jir_none();
    }
    lower_list(L, call->as.call.contexts);
    args[0] = jir_int(rt->usize, elem->size);
    args[1] = cap_node ? jog_lower_expr(L, cap_node) : jir_int(rt->usize, 0);
    return call_rt(L, rt->chan_init, args, 2, call->type ? call->type : base->type, call->pos);
}

static JirOperand lower_send(Lower *L, const Rt *rt, JogNode *call, JogNode *base) {
    JogType *elem = elem_of(base->type);
    JogNode *value_node = arg_value(list_at(call->as.call.args, 0));
    JirOperand chan;
    JirOperand value;
    JirOperand args[3];
    JirOperand status;
    if (!elem) {
        jog_lower_fail(L, call->pos, "channel element has no type");
        return jir_none();
    }
    chan = jog_lower_expr(L, base);
    value = value_node ? jog_lower_expr(L, value_node) : jir_none();
    args[0] = chan;
    args[1] = elem_ptr(L, value, rt->void_ptr, call->pos);
    args[2] = jir_int(rt->usize, elem->size);
    status = call_rt(L, rt->chan_send, args, 3, NULL, call->pos);
    return error_from_status(L, rt, status, call->pos);
}

static JirOperand lower_recv(Lower *L, const Rt *rt, JogNode *call, JogNode *base) {
    JogType *elem = elem_of(base->type);
    uint32_t slot;
    JirOperand chan;
    JirOperand args[3];
    JirOperand status;
    JirOperand err;
    JirOperand parts[2];
    if (!elem || !call->type) {
        jog_lower_fail(L, call->pos, "channel element has no type");
        return jir_none();
    }
    if (!jog_lower_new_local(L, NULL, elem, &slot)) {
        return jir_none();
    }
    jog_lower_zero(L, slot, call->pos);
    chan = jog_lower_expr(L, base);
    args[0] = chan;
    args[1] = addr_of(L, slot, rt->void_ptr, call->pos);
    args[2] = jir_int(rt->usize, elem->size);
    status = call_rt(L, rt->chan_recv, args, 3, NULL, call->pos);
    err = error_from_status(L, rt, status, call->pos);
    parts[0] = jog_lower_local_op(L, slot);
    parts[1] = err;
    return aggregate(L, call->type, parts, 2, call->pos);
}

static JirOperand lower_join(Lower *L, const Rt *rt, JogNode *call, JogNode *base) {
    JogType *elem = call->type ? call->type : elem_of(base->type);
    uint32_t slot;
    JirOperand task;
    JirOperand args[3];
    if (!elem) {
        jog_lower_fail(L, call->pos, "task result has no type");
        return jir_none();
    }
    if (is_unit(elem) || elem->size == 0) {
        JirOperand task = jog_lower_expr(L, base);
        JirOperand args[3];
        args[0] = task;
        args[1] = jir_null(rt->void_ptr);
        args[2] = jir_int(rt->usize, 0);
        call_rt(L, rt->task_join, args, 3, NULL, call->pos);
        return jir_none();
    }
    if (!jog_lower_new_local(L, NULL, elem, &slot)) {
        return jir_none();
    }
    task = jog_lower_expr(L, base);
    args[0] = task;
    args[1] = addr_of(L, slot, rt->void_ptr, call->pos);
    args[2] = jir_int(rt->usize, elem->size);
    call_rt(L, rt->task_join, args, 3, NULL, call->pos);
    return jog_lower_local_op(L, slot);
}

bool jog_lower_coro_call(Lower *L, JogNode *node, JirOperand *out) {
    Rt rt;
    JogNode *callee;
    JogNode *base;
    const char *name;
    if (!L || !node || node->kind != JOG_NODE_CALL || !out) {
        return false;
    }
    callee = node->as.call.callee;
    if (!callee) {
        return false;
    }
    if (callee->kind == JOG_NODE_IDENT) {
        if (callee->symbol) {
            return false;
        }
        name = callee->as.ident.name;
        if (!name) {
            return false;
        }
        if (strcmp(name, "scope") != 0 && strcmp(name, "select") != 0 && strcmp(name, "yield") != 0 &&
            strcmp(name, "sleep") != 0) {
            return false;
        }
        if (!load_rt(L, &rt, node->pos)) {
            *out = jir_none();
            return true;
        }
        if (strcmp(name, "scope") == 0) {
            *out = lower_scope(L, &rt, node);
        } else if (strcmp(name, "select") == 0) {
            *out = lower_select(L, &rt, node);
        } else if (strcmp(name, "yield") == 0) {
            call_rt(L, rt.yield, NULL, 0, NULL, node->pos);
            *out = jir_none();
        } else {
            JogNode *ms = arg_value(list_at(node->as.call.args, 0));
            JirOperand ms_op = ms ? jog_lower_expr(L, ms) : jir_int(rt.i64, 0);
            JirOperand status = call_rt(L, rt.sleep, &ms_op, 1, NULL, node->pos);
            *out = error_from_status(L, &rt, status, node->pos);
        }
        return true;
    }
    if (callee->kind != JOG_NODE_MEMBER || !callee->as.member.base || !callee->as.member.name) {
        return false;
    }
    base = callee->as.member.base;
    name = callee->as.member.name;
    if (base->type && is_chan(base->type) && strcmp(name, "init") == 0) {
        if (!load_rt(L, &rt, node->pos)) {
            *out = jir_none();
            return true;
        }
        *out = lower_init(L, &rt, node);
        return true;
    }
    if (!base->type || (!is_chan(base->type) && !is_task(base->type) && !is_scope_type(base->type))) {
        return false;
    }
    if (!load_rt(L, &rt, node->pos)) {
        *out = jir_none();
        return true;
    }
    if (is_scope_type(base->type)) {
        JirOperand scope = jog_lower_expr(L, base);
        call_rt(L, rt.scope_cancel, &scope, 1, NULL, node->pos);
        *out = jir_none();
        return true;
    }
    if (is_task(base->type)) {
        *out = lower_join(L, &rt, node, base);
        return true;
    }
    if (strcmp(name, "send") == 0) {
        *out = lower_send(L, &rt, node, base);
        return true;
    }
    if (strcmp(name, "recv") == 0) {
        *out = lower_recv(L, &rt, node, base);
        return true;
    }
    if (strcmp(name, "close") == 0 || strcmp(name, "free") == 0) {
        JirOperand chan = jog_lower_expr(L, base);
        call_rt(L, strcmp(name, "close") == 0 ? rt.chan_close : rt.chan_free, &chan, 1, NULL, node->pos);
        *out = jir_none();
        return true;
    }
    jog_lower_fail(L, node->pos, "undefined method '%s'", name);
    *out = jir_none();
    return true;
}
