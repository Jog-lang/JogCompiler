#include "lower_chan_for.h"

#include "lower_generic.h"

#include <string.h>

/* for v in ch receives until jogrt_chan_recv reports closed (2).
   Status 1 is cancellation: fail the current scope, then stop.
   The channel expression runs once. v is a constant copy of the element. */

static bool is_chan(const JogType *type) {
    return type && type->kind == JOG_TY_DISTINCT && type->as.nominal.package == NULL &&
           type->as.nominal.type_arg_count == 1 && type->as.nominal.name && strcmp(type->as.nominal.name, "Chan") == 0;
}

static bool new_block(Lower *L, const char *label, uint32_t *out) {
    size_t before;
    if (!L || !L->func || L->failed) {
        return false;
    }
    before = L->func->blocks.count;
    *out = jir_block_add(L->func, label);
    if (L->func->blocks.count == before) {
        jog_lower_fail(L, L->func->pos, "out of memory");
        return false;
    }
    return true;
}

static void use_block(Lower *L, uint32_t block) {
    JirBlock *b = jir_block_at(L->func, block);
    L->block = block;
    L->terminated = !b || b->term.kind != JIR_TERM_NONE;
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

static JirOperand call_rt(Lower *L, int32_t fn, JirOperand *args, size_t n, JogPos pos) {
    JirFunc *func;
    JogType *ret;
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
    if (ret && ret->kind != JOG_TY_UNIT && ret->size != 0) {
        if (!jog_lower_new_local(L, NULL, ret, &dst)) {
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

static JirOperand emit_cmp(Lower *L, JirBinOp op, JirOperand a, JirOperand b, JogType *boolean, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (!jog_lower_new_local(L, NULL, boolean, &dst)) {
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

static JirOperand as_void(Lower *L, JirOperand value, JogType *void_ptr, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (value.type == void_ptr) {
        return value;
    }
    if (!jog_lower_new_local(L, NULL, void_ptr, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_BITCAST, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = value;
    return jog_lower_local_op(L, dst);
}

bool jog_lower_chan_for(Lower *L, JogNode *node) {
    JogNode *expr;
    JogPos pos;
    JogType *base;
    JogType *elem;
    JogType *void_ptr;
    JogType *i32;
    JogType *usize;
    JogType *boolean;
    JogType *unit;
    JogType *recv_params[3];
    JogSymbol *elem_sym;
    JirOperand chan;
    JirOperand status;
    JirOperand closed;
    JirOperand ok;
    JirOperand args[3];
    JirOperand cancelled;
    uint32_t slot = 0;
    uint32_t head;
    uint32_t open_b;
    uint32_t body;
    uint32_t cancel;
    uint32_t exit_b;
    int32_t recv_fn;
    int32_t fail_fn;
    int32_t cancelled_fn;
    LoopFrame frame;
    if (!L || !node || node->kind != JOG_NODE_FOR || node->as.for_.form != JOG_FOR_IN) {
        return false;
    }
    expr = node->as.for_.expr;
    pos = node->pos;
    base = expr && expr->type ? jog_lower_type(L, expr->type) : NULL;
    if (!is_chan(base)) {
        return false;
    }
    if (L->failed || L->terminated) {
        return true;
    }
    if (node->as.for_.second) {
        jog_lower_fail(L, pos, "for-in over a channel has one name");
        return true;
    }
    elem = base->as.nominal.type_args ? jog_lower_type(L, base->as.nominal.type_args[0]) : NULL;
    void_ptr = jog_type_underlying(base);
    i32 = jog_type_builtin(L->program->types, JOG_BUILTIN_I32);
    usize = jog_type_builtin(L->program->types, JOG_BUILTIN_USIZE);
    boolean = jog_type_builtin(L->program->types, JOG_BUILTIN_BOOL);
    unit = jog_type_builtin(L->program->types, JOG_BUILTIN_UNIT);
    if (!elem || !void_ptr || void_ptr->kind != JOG_TY_PTR || !i32 || !usize || !boolean || !unit) {
        jog_lower_fail(L, pos, "channel element has no type");
        return true;
    }
    if (!jog_lower_new_local(L, NULL, elem, &slot)) {
        return true;
    }
    elem_sym = node->bound_symbol_count > 0 ? node->bound_symbols[0] : node->symbol;
    if (!jog_lower_bind_local(L, elem_sym, slot)) {
        return true;
    }
    jog_lower_zero(L, slot, pos);
    chan = jog_lower_expr(L, expr);
    if (L->failed || L->terminated) {
        return true;
    }
    chan = as_void(L, chan, void_ptr, pos);
    recv_params[0] = void_ptr;
    recv_params[1] = void_ptr;
    recv_params[2] = usize;
    recv_fn = ensure(L, "jogrt_chan_recv", recv_params, 3, i32, pos);
    fail_fn = ensure(L, "jogrt_scope_fail", &void_ptr, 1, unit, pos);
    cancelled_fn = ensure(L, "jogrt_err_cancelled", NULL, 0, void_ptr, pos);
    if (recv_fn < 0 || fail_fn < 0 || cancelled_fn < 0 || L->failed) {
        return true;
    }
    if (!new_block(L, "chan.head", &head) || !new_block(L, "chan.open", &open_b) || !new_block(L, "chan.body", &body) ||
        !new_block(L, "chan.cancel", &cancel) || !new_block(L, "chan.exit", &exit_b)) {
        return true;
    }
    emit_br(L, head, pos);
    use_block(L, head);
    args[0] = chan;
    if (elem->size == 0) {
        args[1] = jir_null(void_ptr);
    } else {
        args[1] = addr_of(L, slot, void_ptr, pos);
    }
    args[2] = jir_int(usize, elem->size);
    status = call_rt(L, recv_fn, args, 3, pos);
    closed = emit_cmp(L, JIR_EQ, status, jir_int(i32, 2), boolean, pos);
    emit_cond(L, closed, exit_b, open_b, pos);
    use_block(L, open_b);
    ok = emit_cmp(L, JIR_EQ, status, jir_int(i32, 0), boolean, pos);
    emit_cond(L, ok, body, cancel, pos);
    use_block(L, body);
    frame.cont = head;
    frame.brk = exit_b;
    frame.cleanup_base = L->cleanups.count;
    if (!ct_da_push(&L->loops, &frame)) {
        jog_lower_fail(L, pos, "out of memory");
        return true;
    }
    if (node->as.for_.body) {
        jog_lower_stmt(L, node->as.for_.body);
    }
    if (L->loops.count > 0) {
        L->loops.count--;
    }
    if (!L->failed && !L->terminated) {
        emit_br(L, head, pos);
    }
    use_block(L, cancel);
    cancelled = call_rt(L, cancelled_fn, NULL, 0, pos);
    call_rt(L, fail_fn, &cancelled, 1, pos);
    if (!L->failed && !L->terminated) {
        emit_br(L, exit_b, pos);
    }
    use_block(L, exit_b);
    return true;
}
