#include "check_coro.h"

#include "check_context.h"
#include "intern.h"

#include <string.h>

/* Chan<T> and Task<T> are distinct pointers. Scope is one distinct pointer.
   Allocator is accepted by nominal name from an explicit with or from a
   context already in scope. The value is not passed to the runtime. */

typedef struct Inst {
    JogType *elem;
    int kind;
    JogType *type;
    struct Inst *next;
} Inst;

enum { INST_CHAN = 1, INST_TASK = 2, SEL_MAX = 32 };

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static bool is_error(const JogType *type) {
    return type && type->kind == JOG_TY_ERROR;
}

static void poison(Check *ck, JogNode *node) {
    if (node) {
        node->type = jog_check_error_type(ck);
    }
}

static JogType *builtin(Check *ck, JogBuiltin which) {
    if (!ck || !ck->program) {
        return NULL;
    }
    return jog_type_builtin(ck->program->types, which);
}

static bool nominal_name(const JogType *type, const char *name) {
    if (!type || !name) {
        return false;
    }
    if (type->kind == JOG_TY_PTR && type->as.elem.elem) {
        type = type->as.elem.elem;
    }
    type = jog_type_underlying((JogType *)type);
    if (!type) {
        return false;
    }
    if (type->kind != JOG_TY_DISTINCT && type->kind != JOG_TY_INTERFACE && type->kind != JOG_TY_STRUCT &&
        type->kind != JOG_TY_ENUM) {
        return false;
    }
    return type->as.nominal.name && strcmp(type->as.nominal.name, name) == 0;
}

static bool is_marker(const JogSymbol *symbol, const char *name) {
    return symbol && symbol->kind == JOG_SYM_TYPE && symbol->decl == NULL && symbol->package == NULL &&
           symbol->name && strcmp(symbol->name, name) == 0;
}

static bool is_root_main(const CheckJob *job) {
    JogNode *fn;
    if (!job || !job->decl || !job->check || !job->check->program) {
        return false;
    }
    fn = job->decl;
    return fn->kind == JOG_NODE_FN && fn->as.fn.name && strcmp(fn->as.fn.name, "main") == 0 && !fn->as.fn.is_extern &&
           job->package == job->check->program->root;
}

static JogSymbol *lookup_type(CheckJob *job, JogBlockScope *scope, const char *name) {
    JogSymbol *symbol = jog_block_scope_lookup(scope, name);
    if (!symbol && job) {
        symbol = jog_check_body_lookup(job, name);
    }
    if (!symbol && job) {
        symbol = jog_check_lookup_file(job, name);
    }
    return symbol;
}

static JogType *universe_type(Check *ck, CheckJob *job, const char *name) {
    JogSymbol *symbol = lookup_type(job, NULL, name);
    if (!symbol && ck && ck->program && ck->program->root && ck->program->root->scope) {
        JogPackageScope *pkg = ck->program->root->scope;
        const char *interned = ck->program->intern ? ct_intern_cstr(ck->program->intern, name) : name;
        symbol = jog_package_scope_lookup(pkg->universe, interned);
    }
    return symbol ? symbol->type : NULL;
}

static bool is_chan(const JogType *type) {
    return type && type->kind == JOG_TY_DISTINCT && type->as.nominal.package == NULL && type->as.nominal.type_arg_count == 1 &&
           type->as.nominal.name && strcmp(type->as.nominal.name, "Chan") == 0;
}

static bool is_task(const JogType *type) {
    return type && type->kind == JOG_TY_DISTINCT && type->as.nominal.package == NULL && type->as.nominal.type_arg_count == 1 &&
           type->as.nominal.name && strcmp(type->as.nominal.name, "Task") == 0;
}

static bool is_scope(const JogType *type) {
    return type && type->kind == JOG_TY_DISTINCT && type->as.nominal.package == NULL && type->as.nominal.type_arg_count == 0 &&
           type->as.nominal.name && strcmp(type->as.nominal.name, "Scope") == 0 && type->as.nominal.decl == NULL;
}

static JogType *elem_of(const JogType *type) {
    if (!type || type->as.nominal.type_arg_count != 1 || !type->as.nominal.type_args) {
        return NULL;
    }
    return type->as.nominal.type_args[0];
}

static JogType *instance(Check *ck, JogType *elem, int kind) {
    Inst *inst;
    JogType *type;
    JogType *void_ty;
    JogType *base;
    JogType **args;
    const char *name;
    if (!ck || !elem || is_error(elem)) {
        return elem;
    }
    for (inst = ck->coro_insts; inst; inst = inst->next) {
        if (inst->kind == kind && inst->elem == elem) {
            return inst->type;
        }
    }
    void_ty = universe_type(ck, ck->current_job, "void");
    base = void_ty ? jog_type_ptr(ck->program->types, void_ty, false) : NULL;
    name = kind == INST_CHAN ? "Chan" : "Task";
    if (ck->program->intern) {
        name = ct_intern_cstr(ck->program->intern, name);
    }
    type = jog_type_new_nominal(ck->program->types, JOG_TY_DISTINCT, name, NULL, NULL);
    args = ct_arena_alloc(ck->program->arena, sizeof(JogType *), _Alignof(JogType *));
    inst = ct_arena_alloc(ck->program->arena, sizeof(*inst), _Alignof(Inst));
    if (!type || !base || !args || !inst || !jog_type_layout(ck->program->types, base)) {
        jog_check_oom(ck);
        return NULL;
    }
    args[0] = elem;
    type->as.nominal.base = base;
    type->as.nominal.type_args = args;
    type->as.nominal.type_arg_count = 1;
    if (!jog_type_layout(ck->program->types, type)) {
        jog_check_oom(ck);
        return NULL;
    }
    inst->elem = elem;
    inst->kind = kind;
    inst->type = type;
    inst->next = ck->coro_insts;
    ck->coro_insts = inst;
    return type;
}

static JogNode *tail_expr(JogNode *block) {
    JogNode *last;
    if (!block || block->kind != JOG_NODE_BLOCK || block->as.block.stmts.count == 0) {
        return NULL;
    }
    last = list_at(block->as.block.stmts, block->as.block.stmts.count - 1);
    if (!last || last->kind != JOG_NODE_EXPR_STMT) {
        return NULL;
    }
    return last->as.expr_stmt.expr;
}

static bool has_allocator(Check *ck, CheckJob *job, JogBlockScope *scope, JogList contexts) {
    size_t i;
    bool found = false;
    for (i = 0; i < contexts.count; i++) {
        JogNode *expr = list_at(contexts, i);
        JogType *type;
        if (!expr) {
            continue;
        }
        type = jog_check_expr(ck, job, scope, expr, NULL);
        if (ck->stop) {
            return false;
        }
        if (nominal_name(type, "Allocator")) {
            found = true;
        }
    }
    if (found) {
        return true;
    }
    return jog_check_context_has_named(job, "Allocator");
}

static int require_allocator(Check *ck, CheckJob *job, JogBlockScope *scope, JogList contexts, JogPos pos) {
    if (ck->stop) {
        return -1;
    }
    if (!has_allocator(ck, job, scope, contexts)) {
        jog_check_fail(ck, pos, "missing context Allocator");
        return ck->stop ? -1 : 0;
    }
    return 1;
}

static JogType *check_go_body(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *body) {
    JogType *unit = jog_check_unit_type(ck);
    JogType *result;
    JogNode *tail;
    bool saved_in;
    bool saved_set;
    JogType *saved_ret;
    if (!body) {
        jog_check_fail(ck, (JogPos){0}, "go needs a body");
        return jog_check_error_type(ck);
    }
    saved_in = job->in_go;
    saved_set = job->go_ret_set;
    saved_ret = job->go_ret;
    job->in_go = true;
    job->go_ret_set = false;
    job->go_ret = NULL;
    if (body->kind == JOG_NODE_BLOCK) {
        (void)jog_check_when_arm(ck, job, scope, body, NULL, false, NULL);
        tail = tail_expr(body);
    } else {
        result = jog_check_expr(ck, job, scope, body, NULL);
        tail = body;
        if (result) {
            tail->type = result;
        }
    }
    if (ck->stop) {
        job->in_go = saved_in;
        job->go_ret_set = saved_set;
        job->go_ret = saved_ret;
        return NULL;
    }
    result = job->go_ret_set ? job->go_ret : (tail && tail->type ? tail->type : unit);
    if (tail && tail->type && job->go_ret_set && !is_error(tail->type) && !is_error(job->go_ret) &&
        tail->type != job->go_ret) {
        jog_check_fail(ck, body->pos, "return type does not match the go result");
        result = jog_check_error_type(ck);
    }
    if (result && result->kind == JOG_TY_UNTYPED_INT) {
        result = builtin(ck, JOG_BUILTIN_I64);
    }
    job->in_go = saved_in;
    job->go_ret_set = saved_set;
    job->go_ret = saved_ret;
    return result ? result : unit;
}

static int check_stack_arg(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *go) {
    JogType *usize = builtin(ck, JOG_BUILTIN_USIZE);
    size_t i;
    if (go->as.go.args.count > 1) {
        jog_check_fail(ck, go->pos, "go accepts only stack:");
        return 0;
    }
    for (i = 0; i < go->as.go.args.count; i++) {
        JogNode *arg = list_at(go->as.go.args, i);
        if (!arg || arg->kind != JOG_NODE_ARG || !arg->as.arg.name || strcmp(arg->as.arg.name, "stack") != 0) {
            jog_check_fail(ck, arg ? arg->pos : go->pos, "go accepts only stack:");
            return 0;
        }
        (void)jog_check_expr(ck, job, scope, arg->as.arg.value, usize);
        if (ck->stop) {
            return -1;
        }
    }
    return 1;
}

static void remember_scope(Check *ck, JogNode *go, JogNode *expr) {
    JogNode **slot;
    if (!expr) {
        go->context_args = NULL;
        go->context_arg_count = 0;
        return;
    }
    slot = ct_arena_alloc(ck->program->arena, sizeof(JogNode *), _Alignof(JogNode *));
    if (!slot) {
        jog_check_oom(ck);
        return;
    }
    slot[0] = expr;
    go->context_args = slot;
    go->context_arg_count = 1;
}

JogType *jog_check_coro_go(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *go) {
    JogType *result;
    JogType *task;
    JogNode *scope_expr = NULL;
    size_t i;
    int alloc;
    if (!ck || !go || ck->stop) {
        return NULL;
    }
    if (!job) {
        jog_check_fail(ck, go->pos, "go is outside a function");
        poison(ck, go);
        return go->type;
    }
    if (check_stack_arg(ck, job, scope, go) < 0 || ck->stop) {
        return NULL;
    }
    for (i = 0; i < go->as.go.contexts.count && !ck->stop; i++) {
        JogNode *expr = list_at(go->as.go.contexts, i);
        JogType *type = expr ? jog_check_expr(ck, job, scope, expr, NULL) : NULL;
        if (nominal_name(type, "Scope")) {
            scope_expr = expr;
        }
    }
    if (ck->stop) {
        return NULL;
    }
    if (!scope_expr) {
        scope_expr = jog_check_context_find(job, "Scope");
    }
    if (!scope_expr && !is_root_main(job)) {
        jog_check_fail(ck, go->pos, "missing context Scope");
    }
    alloc = require_allocator(ck, job, scope, go->as.go.contexts, go->pos);
    if (alloc < 0) {
        return NULL;
    }
    remember_scope(ck, go, scope_expr);
    if (ck->stop) {
        return NULL;
    }
    result = check_go_body(ck, job, scope, go->as.go.body);
    if (!result || ck->stop) {
        return NULL;
    }
    if (is_error(result)) {
        go->type = result;
        return result;
    }
    task = instance(ck, result, INST_TASK);
    if (!task) {
        return NULL;
    }
    go->type = task;
    return task;
}

static JogType *concrete_int(Check *ck, JogType *type) {
    if (type && type->kind == JOG_TY_UNTYPED_INT) {
        return builtin(ck, JOG_BUILTIN_I64);
    }
    return type;
}

/* The first return a, b fixes the task result. A later return must match
   that tuple. An untyped integer with no earlier result becomes i64. */
static void check_multi_return(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *ret) {
    size_t count = ret->as.ret.values.count;
    size_t i;
    JogType **elems;
    JogType *established = NULL;
    bool fresh = !job->go_ret_set;
    bool bad = false;
    if (!fresh && job->go_ret && !is_error(job->go_ret) &&
        (job->go_ret->kind != JOG_TY_TUPLE || !job->go_ret->as.tuple.elems || job->go_ret->as.tuple.count != count)) {
        jog_check_fail(ck, ret->pos, "return type does not match the go result");
    } else if (!fresh && job->go_ret && !is_error(job->go_ret)) {
        established = job->go_ret;
    }
    elems = ct_arena_alloc(ck->program->arena, count * sizeof(JogType *), _Alignof(JogType *));
    if (!elems) {
        jog_check_oom(ck);
        return;
    }
    for (i = 0; i < count && !ck->stop; i++) {
        JogType *want = established ? established->as.tuple.elems[i] : NULL;
        JogType *got = jog_check_expr(ck, job, scope, list_at(ret->as.ret.values, i), want);
        if (!got || ck->stop) {
            return;
        }
        if (fresh) {
            got = concrete_int(ck, got);
            if (!got) {
                return;
            }
        }
        if (is_error(got)) {
            bad = true;
        }
        elems[i] = got;
    }
    if (ck->stop || !fresh) {
        return;
    }
    if (bad) {
        job->go_ret = jog_check_error_type(ck);
        job->go_ret_set = true;
        return;
    }
    job->go_ret = jog_type_tuple(ck->program->types, elems, count);
    if (!job->go_ret) {
        jog_check_oom(ck);
        return;
    }
    job->go_ret_set = true;
}

void jog_check_coro_return(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *ret) {
    JogType *unit;
    JogType *got;
    size_t count;
    if (!ck || !job || !ret) {
        return;
    }
    unit = jog_check_unit_type(ck);
    count = ret->as.ret.values.count;
    ret->type = unit;
    if (count > 1) {
        check_multi_return(ck, job, scope, ret);
        return;
    }
    if (count == 0) {
        got = unit;
    } else {
        JogNode *value = list_at(ret->as.ret.values, 0);
        got = jog_check_expr(ck, job, scope, value, job->go_ret_set ? job->go_ret : NULL);
        if (!got || ck->stop) {
            return;
        }
        if (got->kind == JOG_TY_UNTYPED_INT && !job->go_ret_set) {
            got = builtin(ck, JOG_BUILTIN_I64);
        }
    }
    if (!job->go_ret_set) {
        job->go_ret = got;
        job->go_ret_set = true;
        return;
    }
    if (got && job->go_ret && !is_error(got) && !is_error(job->go_ret) && got != job->go_ret) {
        jog_check_fail(ck, ret->pos, "return type does not match the go result");
    }
}

static int check_scope(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call) {
    JogType *scope_ty;
    JogType *err;
    JogSymbol *symbol;
    JogNode *body;
    void *mark;
    if (call->as.call.args.count != 0) {
        jog_check_fail(ck, call->pos, "scope does not take arguments");
        poison(ck, call);
        return ck->stop ? -1 : 1;
    }
    body = call->as.call.trailing;
    if (!body || body->kind != JOG_NODE_LAMBDA || !body->as.lambda.body) {
        jog_check_fail(ck, call->pos, "scope needs a block");
        poison(ck, call);
        return ck->stop ? -1 : 1;
    }
    scope_ty = universe_type(ck, job, "Scope");
    err = universe_type(ck, job, "error");
    if (!scope_ty || !err) {
        jog_check_oom(ck);
        return -1;
    }
    symbol = jog_symbol_new(ck->program->arena, JOG_SYM_CONTEXT, NULL, call, scope_ty, NULL);
    if (!symbol) {
        jog_check_oom(ck);
        return -1;
    }
    call->symbol = symbol;
    mark = jog_check_context_mark(job);
    if (jog_check_context_push_symbol(ck, job, symbol, scope_ty, call->pos) < 0) {
        return -1;
    }
    (void)jog_check_when_arm(ck, job, scope, body->as.lambda.body, NULL, false, NULL);
    jog_check_context_reset(job, mark);
    if (ck->stop) {
        return -1;
    }
    call->type = jog_type_option(ck->program->types, err);
    if (!call->type) {
        jog_check_oom(ck);
        return -1;
    }
    return 1;
}

static int bind_recv(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *lambda, JogType *elem) {
    JogBlockScope *inner;
    JogSymbol *symbol;
    const char *name;
    JogNode *param;
    if (!lambda || lambda->kind != JOG_NODE_LAMBDA) {
        jog_check_fail(ck, lambda ? lambda->pos : (JogPos){0}, "recv needs a block");
        return 0;
    }
    if (lambda->as.lambda.params.count > 1) {
        jog_check_fail(ck, lambda->pos, "recv binds one value");
        return 0;
    }
    if (lambda->as.lambda.params.count == 1) {
        param = list_at(lambda->as.lambda.params, 0);
        name = param ? param->as.param.name : NULL;
        if (param && param->as.param.type) {
            JogType *written = jog_check_resolve_type(ck, job, param->as.param.type);
            if (written && written != elem && !is_error(written)) {
                jog_check_fail(ck, param->pos, "recv binding does not match the channel element");
            }
        }
    } else if (lambda->as.lambda.implicit_it) {
        param = NULL;
        name = ck->program->intern ? ct_intern_cstr(ck->program->intern, "it") : "it";
    } else {
        param = NULL;
        name = NULL;
    }
    if (!name) {
        return 1;
    }
    inner = jog_block_scope_new(ck->program->arena, scope, scope ? scope->package : NULL);
    symbol = jog_symbol_new(ck->program->arena, JOG_SYM_VARIABLE, name, param, elem, NULL);
    if (!inner || !symbol || !jog_block_scope_bind(inner, symbol)) {
        jog_check_oom(ck);
        return -1;
    }
    if (param) {
        param->symbol = symbol;
        param->type = elem;
    }
    lambda->symbol = symbol;
    jog_check_body_set_scope(job, inner);
    jog_check_body_assigned(job, symbol);
    (void)jog_check_when_arm(ck, job, inner, lambda->as.lambda.body, NULL, false, NULL);
    jog_check_body_set_scope(job, scope);
    return ck->stop ? -1 : 1;
}

static int check_case_body(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *lambda, bool binds, JogType *elem) {
    if (!lambda || lambda->kind != JOG_NODE_LAMBDA) {
        jog_check_fail(ck, lambda ? lambda->pos : (JogPos){0}, "select case needs a block");
        return 0;
    }
    if (binds) {
        return bind_recv(ck, job, scope, lambda, elem);
    }
    if (lambda->as.lambda.params.count > 0) {
        jog_check_fail(ck, lambda->pos, "select case does not bind a value");
        return 0;
    }
    (void)jog_check_when_arm(ck, job, scope, lambda->as.lambda.body, NULL, false, NULL);
    return ck->stop ? -1 : 1;
}

static int check_select(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call) {
    JogNode *lambda;
    JogNode *block;
    JogType *err;
    size_t i;
    if (call->as.call.args.count != 0) {
        jog_check_fail(ck, call->pos, "select does not take arguments");
        poison(ck, call);
        return ck->stop ? -1 : 1;
    }
    lambda = call->as.call.trailing;
    if (!lambda || lambda->kind != JOG_NODE_LAMBDA || !lambda->as.lambda.body ||
        lambda->as.lambda.body->kind != JOG_NODE_BLOCK) {
        jog_check_fail(ck, call->pos, "select needs a block");
        poison(ck, call);
        return ck->stop ? -1 : 1;
    }
    block = lambda->as.lambda.body;
    if (block->as.block.stmts.count == 0 || block->as.block.stmts.count > SEL_MAX) {
        jog_check_fail(ck, call->pos, block->as.block.stmts.count == 0 ? "select needs a case" : "too many select cases");
        poison(ck, call);
        return ck->stop ? -1 : 1;
    }
    for (i = 0; i < block->as.block.stmts.count && !ck->stop; i++) {
        JogNode *stmt = list_at(block->as.block.stmts, i);
        JogNode *expr = stmt && stmt->kind == JOG_NODE_EXPR_STMT ? stmt->as.expr_stmt.expr : stmt;
        JogNode *callee;
        JogNode *chan_arg;
        JogType *chan;
        const char *name;
        if (!expr || expr->kind != JOG_NODE_CALL) {
            jog_check_fail(ck, stmt ? stmt->pos : call->pos, "select case must be recv, send, timeout, or default");
            continue;
        }
        callee = expr->as.call.callee;
        name = callee && callee->kind == JOG_NODE_IDENT ? callee->as.ident.name : NULL;
        if (!name || (strcmp(name, "recv") != 0 && strcmp(name, "send") != 0 && strcmp(name, "timeout") != 0 &&
                      strcmp(name, "default") != 0)) {
            jog_check_fail(ck, expr->pos, "select case must be recv, send, timeout, or default");
            continue;
        }
        if (strcmp(name, "default") == 0) {
            if (expr->as.call.args.count != 0) {
                jog_check_fail(ck, expr->pos, "default does not take arguments");
            }
            (void)check_case_body(ck, job, scope, expr->as.call.trailing, false, NULL);
            continue;
        }
        if (strcmp(name, "timeout") == 0) {
            if (expr->as.call.args.count != 1) {
                jog_check_fail(ck, expr->pos, "timeout expects milliseconds");
            } else {
                JogNode *arg = list_at(expr->as.call.args, 0);
                JogNode *value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : arg;
                (void)jog_check_expr(ck, job, scope, value, builtin(ck, JOG_BUILTIN_I64));
            }
            (void)check_case_body(ck, job, scope, expr->as.call.trailing, false, NULL);
            continue;
        }
        if (expr->as.call.args.count < 1) {
            jog_check_fail(ck, expr->pos, "channel case needs a channel");
            continue;
        }
        chan_arg = list_at(expr->as.call.args, 0);
        chan_arg = chan_arg && chan_arg->kind == JOG_NODE_ARG ? chan_arg->as.arg.value : chan_arg;
        chan = jog_check_expr(ck, job, scope, chan_arg, NULL);
        if (!is_chan(chan)) {
            jog_check_fail(ck, expr->pos, "select case needs a Chan");
            continue;
        }
        if (strcmp(name, "recv") == 0) {
            if (expr->as.call.args.count != 1) {
                jog_check_fail(ck, expr->pos, "recv expects a channel");
            }
            (void)check_case_body(ck, job, scope, expr->as.call.trailing, true, elem_of(chan));
            continue;
        }
        if (expr->as.call.args.count != 2) {
            jog_check_fail(ck, expr->pos, "send expects a channel and a value");
            continue;
        }
        {
            JogNode *arg = list_at(expr->as.call.args, 1);
            JogNode *value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : arg;
            (void)jog_check_expr(ck, job, scope, value, elem_of(chan));
        }
        (void)check_case_body(ck, job, scope, expr->as.call.trailing, false, NULL);
    }
    if (ck->stop) {
        return -1;
    }
    err = universe_type(ck, job, "error");
    call->type = err ? jog_type_option(ck->program->types, err) : NULL;
    if (!call->type) {
        jog_check_oom(ck);
        return -1;
    }
    return 1;
}

static int check_yield(Check *ck, JogNode *call) {
    if (call->as.call.args.count != 0 || call->as.call.trailing) {
        jog_check_fail(ck, call->pos, "yield takes no arguments");
        poison(ck, call);
        return ck->stop ? -1 : 1;
    }
    call->type = jog_check_unit_type(ck);
    return 1;
}

static int check_sleep(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call) {
    JogType *err;
    JogNode *arg;
    JogNode *value;
    if (call->as.call.trailing || call->as.call.args.count != 1) {
        jog_check_fail(ck, call->pos, "sleep expects milliseconds");
        poison(ck, call);
        return ck->stop ? -1 : 1;
    }
    arg = list_at(call->as.call.args, 0);
    value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : arg;
    (void)jog_check_expr(ck, job, scope, value, builtin(ck, JOG_BUILTIN_I64));
    if (ck->stop) {
        return -1;
    }
    err = universe_type(ck, job, "error");
    call->type = err ? jog_type_option(ck->program->types, err) : NULL;
    if (!call->type) {
        jog_check_oom(ck);
        return -1;
    }
    return 1;
}

static JogType *error_option(Check *ck, CheckJob *job) {
    JogType *err = universe_type(ck, job, "error");
    JogType *opt = err ? jog_type_option(ck->program->types, err) : NULL;
    if (!opt) {
        jog_check_oom(ck);
    }
    return opt;
}

static int check_type_method(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call) {
    JogNode *callee = call->as.call.callee;
    JogNode *base;
    JogSymbol *symbol;
    JogType *elem;
    JogType *inst;
    JogList args;
    const char *method;
    if (!callee || callee->kind != JOG_NODE_MEMBER || !callee->as.member.base || callee->as.member.base->kind != JOG_NODE_IDENT) {
        return 0;
    }
    base = callee->as.member.base;
    symbol = lookup_type(job, scope, base->as.ident.name);
    if (!is_marker(symbol, "Chan") && !is_marker(symbol, "Task")) {
        return 0;
    }
    method = callee->as.member.name;
    if (base->as.ident.type_args.count != 1) {
        jog_check_fail(ck, base->pos, "missing type arguments for '%s'", symbol->name);
        poison(ck, call);
        return ck->stop ? -1 : 1;
    }
    elem = jog_check_resolve_type(ck, job, list_at(base->as.ident.type_args, 0));
    if (!elem || ck->stop) {
        return ck->stop ? -1 : 1;
    }
    if (is_error(elem)) {
        poison(ck, call);
        return 1;
    }
    inst = instance(ck, elem, is_marker(symbol, "Chan") ? INST_CHAN : INST_TASK);
    if (!inst) {
        return -1;
    }
    base->type = inst;
    base->symbol = symbol;
    if (!is_marker(symbol, "Chan") || !method || strcmp(method, "init") != 0) {
        jog_check_fail(ck, call->pos, "undefined method '%s'", method ? method : "?");
        poison(ck, call);
        return ck->stop ? -1 : 1;
    }
    if (require_allocator(ck, job, scope, call->as.call.contexts, call->pos) < 0) {
        return -1;
    }
    args = call->as.call.args;
    if (args.count != 1) {
        jog_check_fail(ck, call->pos, "init expects a capacity");
        poison(ck, call);
        return ck->stop ? -1 : 1;
    }
    {
        JogNode *arg = list_at(args, 0);
        JogNode *value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : arg;
        if (arg && arg->kind == JOG_NODE_ARG && arg->as.arg.name && strcmp(arg->as.arg.name, "cap") != 0) {
            jog_check_fail(ck, arg->pos, "init expects cap:");
        }
        (void)jog_check_expr(ck, job, scope, value, builtin(ck, JOG_BUILTIN_USIZE));
    }
    if (ck->stop) {
        return -1;
    }
    call->type = inst;
    return 1;
}

static int check_value_method(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call) {
    JogNode *callee = call->as.call.callee;
    JogNode *base;
    JogType *base_ty;
    const char *method;
    JogType *elem;
    JogType *err_opt;
    if (!callee || callee->kind != JOG_NODE_MEMBER || !callee->as.member.base || !callee->as.member.base->type) {
        return 0;
    }
    base = callee->as.member.base;
    base_ty = base->type;
    method = callee->as.member.name ? callee->as.member.name : "";
    if (is_scope(base_ty)) {
        if (strcmp(method, "cancel") != 0 || call->as.call.args.count != 0) {
            jog_check_fail(ck, call->pos, "undefined method '%s'", method);
            poison(ck, call);
            return ck->stop ? -1 : 1;
        }
        (void)scope;
        call->type = jog_check_unit_type(ck);
        return 1;
    }
    if (!is_chan(base_ty) && !is_task(base_ty)) {
        return 0;
    }
    elem = elem_of(base_ty);
    err_opt = error_option(ck, job);
    if (!elem || !err_opt) {
        return ck->stop ? -1 : 1;
    }
    (void)job;
    (void)scope;
    if (is_task(base_ty)) {
        if (strcmp(method, "join") != 0 || call->as.call.args.count != 0) {
            jog_check_fail(ck, call->pos, "undefined method '%s'", method);
            poison(ck, call);
            return ck->stop ? -1 : 1;
        }
        call->type = elem;
        return 1;
    }
    if (strcmp(method, "send") == 0) {
        JogNode *arg;
        JogNode *value;
        if (call->as.call.args.count != 1) {
            jog_check_fail(ck, call->pos, "send expects a value");
            poison(ck, call);
            return ck->stop ? -1 : 1;
        }
        arg = list_at(call->as.call.args, 0);
        value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : arg;
        (void)jog_check_expr(ck, job, scope, value, elem);
        call->type = err_opt;
        return ck->stop ? -1 : 1;
    }
    if (strcmp(method, "recv") == 0) {
        JogType *parts[2];
        if (call->as.call.args.count != 0) {
            jog_check_fail(ck, call->pos, "recv takes no arguments");
            poison(ck, call);
            return ck->stop ? -1 : 1;
        }
        parts[0] = elem;
        parts[1] = err_opt;
        call->type = jog_type_tuple(ck->program->types, parts, 2);
        if (!call->type) {
            jog_check_oom(ck);
            return -1;
        }
        return 1;
    }
    if (strcmp(method, "close") == 0 || strcmp(method, "free") == 0) {
        if (call->as.call.args.count != 0) {
            jog_check_fail(ck, call->pos, "undefined method '%s'", method);
            poison(ck, call);
            return ck->stop ? -1 : 1;
        }
        call->type = jog_check_unit_type(ck);
        return 1;
    }
    jog_check_fail(ck, call->pos, "undefined method '%s'", method);
    poison(ck, call);
    return ck->stop ? -1 : 1;
}

static bool user_named(CheckJob *job, JogBlockScope *scope, const char *name) {
    JogSymbol *symbol = lookup_type(job, scope, name);
    return symbol != NULL;
}

int jog_check_coro_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call) {
    JogNode *callee;
    const char *name;
    int type_method;
    if (!ck || !call || call->kind != JOG_NODE_CALL || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    type_method = check_type_method(ck, job, scope, call);
    if (type_method != 0) {
        return type_method;
    }
    callee = call->as.call.callee;
    if (!callee || callee->kind != JOG_NODE_IDENT || !callee->as.ident.name) {
        return 0;
    }
    name = callee->as.ident.name;
    if (user_named(job, scope, name)) {
        return 0;
    }
    if (strcmp(name, "scope") == 0) {
        return check_scope(ck, job, scope, call);
    }
    if (strcmp(name, "select") == 0) {
        return check_select(ck, job, scope, call);
    }
    if (strcmp(name, "yield") == 0) {
        return check_yield(ck, call);
    }
    if (strcmp(name, "sleep") == 0) {
        return check_sleep(ck, job, scope, call);
    }
    return 0;
}

int jog_check_coro_method(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call) {
    if (!ck || !call || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    return check_value_method(ck, job, scope, call);
}

int jog_check_coro_type(Check *ck, CheckJob *job, JogSymbol *symbol, JogNode *type_name, JogType **out) {
    JogType *elem;
    JogType *inst;
    int kind;
    if (!ck || !symbol || !type_name || !out) {
        return 0;
    }
    *out = NULL;
    if (!is_marker(symbol, "Chan") && !is_marker(symbol, "Task")) {
        return 0;
    }
    kind = is_marker(symbol, "Chan") ? INST_CHAN : INST_TASK;
    if (type_name->as.type_name.args.count != 1) {
        jog_check_fail(ck, type_name->pos, "missing type arguments for '%s'", symbol->name);
        *out = jog_check_error_type(ck);
        return ck->stop ? -1 : 1;
    }
    elem = jog_check_resolve_type(ck, job, list_at(type_name->as.type_name.args, 0));
    if (!elem || ck->stop) {
        return -1;
    }
    if (is_error(elem)) {
        *out = elem;
        return 1;
    }
    inst = instance(ck, elem, kind);
    if (!inst) {
        return -1;
    }
    *out = inst;
    return 1;
}
