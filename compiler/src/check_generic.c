#include "check_generic.h"

#include "check_constraint.h"
#include "float_lit.h"
#include "str.h"

#include <stdio.h>
#include <string.h>

/* A type parameter with no constraint accepts any type and can only be
   stored, copied, and passed. Calls monomorphize: the solved arguments are
   stored on the call, and lowering emits one function per concrete list.
   An untyped integer stays soft so a later typed integer can win; if every
   candidate is an untyped integer, the parameter becomes i64. An untyped
   float stays soft until a typed float wins; otherwise the parameter is f64.
   A method receiver List<T> declares T for every instance and copies the
   constraint from the struct or enum parameter in that position. The
   method's own type parameters follow those. fn List<i32>.get names a type
   parameter i32; it is not a method of the single instance List<i32>. */

typedef struct Solved {
    JogType *type;
    int soft;       /* 1 when an untyped integer was seen and no type is bound */
    int soft_float; /* 1 when an untyped float was seen and no type is bound */
} Solved;

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static bool has_attr(const JogNode *fn, const char *name) {
    size_t i;
    if (!fn || !name) {
        return false;
    }
    for (i = 0; i < fn->attrs.count; i++) {
        JogNode *attr = list_at(fn->attrs, i);
        if (attr && attr->kind == JOG_NODE_ATTRIBUTE && attr->as.attribute.name &&
            strcmp(attr->as.attribute.name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void type_text(const JogType *type, char *buf, size_t cap) {
    CtSb sb;
    const char *text;
    if (!buf || cap == 0) {
        return;
    }
    buf[0] = '\0';
    ct_sb_init(&sb);
    jog_type_print(type, &sb);
    text = ct_sb_cstr(&sb);
    if (!sb.failed && text) {
        snprintf(buf, cap, "%s", text);
    } else {
        snprintf(buf, cap, "?");
    }
    ct_sb_free(&sb);
}

static int is_arith_op(JogTokenKind op) {
    switch (op) {
    case JOG_TOK_PLUS:
    case JOG_TOK_MINUS:
    case JOG_TOK_STAR:
    case JOG_TOK_SLASH:
    case JOG_TOK_PERCENT:
    case JOG_TOK_AMP:
    case JOG_TOK_PIPE:
    case JOG_TOK_CARET:
    case JOG_TOK_SHL:
    case JOG_TOK_SHR:
        return 1;
    default:
        return 0;
    }
}

static int is_int_tree(const JogNode *node) {
    JogTokenKind op;
    if (!node) {
        return 0;
    }
    if (node->kind == JOG_NODE_INT_LIT) {
        return 1;
    }
    if (node->kind == JOG_NODE_UNARY) {
        op = node->as.unary.op;
        return (op == JOG_TOK_MINUS || op == JOG_TOK_TILDE) && is_int_tree(node->as.unary.operand);
    }
    if (node->kind != JOG_NODE_BINARY || !is_arith_op(node->as.binary.op)) {
        return 0;
    }
    return is_int_tree(node->as.binary.lhs) && is_int_tree(node->as.binary.rhs);
}

/* 1: untyped integer (including a character literal). 2: untyped float. */
static int soft_kind(const JogNode *node) {
    if (!node) {
        return 0;
    }
    if (node->kind == JOG_NODE_CHAR_LIT || is_int_tree(node)) {
        return 1;
    }
    if (jog_float_lit_tree(node)) {
        return 2;
    }
    return 0;
}

static bool has_param_depth(const JogType *type, int depth) {
    size_t i;
    if (!type || depth > 64) {
        return false;
    }
    switch (type->kind) {
    case JOG_TY_TYPE_PARAM:
        return true;
    case JOG_TY_PTR:
    case JOG_TY_SLICE:
    case JOG_TY_OPTION:
        return has_param_depth(type->as.elem.elem, depth + 1);
    case JOG_TY_ARRAY:
        return has_param_depth(type->as.array.elem, depth + 1);
    case JOG_TY_TUPLE:
        for (i = 0; i < type->as.tuple.count; i++) {
            if (has_param_depth(type->as.tuple.elems ? type->as.tuple.elems[i] : NULL, depth + 1)) {
                return true;
            }
        }
        return false;
    case JOG_TY_FN:
        for (i = 0; i < type->as.fn.param_count; i++) {
            if (has_param_depth(type->as.fn.params ? type->as.fn.params[i] : NULL, depth + 1)) {
                return true;
            }
        }
        for (i = 0; i < type->as.fn.context_count; i++) {
            if (has_param_depth(type->as.fn.contexts ? type->as.fn.contexts[i] : NULL, depth + 1)) {
                return true;
            }
        }
        return has_param_depth(type->as.fn.ret, depth + 1);
    case JOG_TY_STRUCT:
    case JOG_TY_ENUM:
    case JOG_TY_INTERFACE:
    case JOG_TY_DISTINCT:
        /* Instances record their arguments. Walking fields would cycle
           through a pointer back to the same struct. */
        for (i = 0; i < type->as.nominal.type_arg_count; i++) {
            if (has_param_depth(type->as.nominal.type_args ? type->as.nominal.type_args[i] : NULL, depth + 1)) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

bool jog_generic_has_param(const JogType *type) {
    return has_param_depth(type, 0);
}

static JogType **dup_subst(CtArena *arena, JogType *const *src, size_t n, JogTypes *types, JogType *const *params,
                           JogType *const *args, size_t count, int *changed) {
    JogType **out;
    size_t i;
    int any = 0;
    if (n == 0) {
        if (changed) {
            *changed = 0;
        }
        return NULL;
    }
    out = ct_arena_alloc(arena, n * sizeof(*out), _Alignof(JogType *));
    if (!out) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        JogType *part = jog_generic_subst(types, arena, src ? src[i] : NULL, params, args, count);
        if (part != (src ? src[i] : NULL)) {
            any = 1;
        }
        out[i] = part;
    }
    if (changed) {
        *changed = any;
    }
    return out;
}

JogType *jog_generic_subst(JogTypes *types, CtArena *arena, JogType *type, JogType *const *params, JogType *const *args,
                          size_t count) {
    size_t i;
    if (!type || count == 0 || !params || !args) {
        return type;
    }
    if (type->kind == JOG_TY_TYPE_PARAM) {
        for (i = 0; i < count; i++) {
            if (params[i] == type) {
                return args[i] ? args[i] : type;
            }
        }
        return type;
    }
    if (!jog_generic_has_param(type) || !types || !arena) {
        return type;
    }
    switch (type->kind) {
    case JOG_TY_PTR:
    case JOG_TY_SLICE:
    case JOG_TY_OPTION: {
        JogType *elem = jog_generic_subst(types, arena, type->as.elem.elem, params, args, count);
        if (elem == type->as.elem.elem) {
            return type;
        }
        if (!elem) {
            return NULL;
        }
        if (type->kind == JOG_TY_PTR) {
            return jog_type_ptr(types, elem, type->is_const);
        }
        if (type->kind == JOG_TY_SLICE) {
            return jog_type_slice(types, elem, type->is_const);
        }
        return jog_type_option(types, elem);
    }
    case JOG_TY_ARRAY: {
        JogType *elem = jog_generic_subst(types, arena, type->as.array.elem, params, args, count);
        if (elem == type->as.array.elem) {
            return type;
        }
        if (!elem) {
            return NULL;
        }
        return jog_type_array(types, elem, type->as.array.len);
    }
    case JOG_TY_TUPLE: {
        int changed = 0;
        JogType **elems = dup_subst(arena, type->as.tuple.elems, type->as.tuple.count, types, params, args, count, &changed);
        if (type->as.tuple.count > 0 && !elems) {
            return NULL;
        }
        if (!changed) {
            return type;
        }
        return jog_type_tuple(types, elems, type->as.tuple.count);
    }
    case JOG_TY_FN: {
        int changed_p = 0;
        int changed_c = 0;
        JogType **fn_params =
            dup_subst(arena, type->as.fn.params, type->as.fn.param_count, types, params, args, count, &changed_p);
        JogType **contexts =
            dup_subst(arena, type->as.fn.contexts, type->as.fn.context_count, types, params, args, count, &changed_c);
        JogType *ret = jog_generic_subst(types, arena, type->as.fn.ret, params, args, count);
        if ((type->as.fn.param_count > 0 && !fn_params) || (type->as.fn.context_count > 0 && !contexts) || !ret) {
            return NULL;
        }
        if (!changed_p && !changed_c && ret == type->as.fn.ret) {
            return type;
        }
        return jog_type_fn(types, fn_params, type->as.fn.param_count, ret, contexts, type->as.fn.context_count,
                           type->as.fn.variadic, type->as.fn.escaping, type->as.fn.is_extern);
    }
    case JOG_TY_STRUCT:
    case JOG_TY_ENUM: {
        int changed = 0;
        JogType **next;
        JogSymbol *sym;
        if (type->as.nominal.type_arg_count == 0) {
            return type;
        }
        next = dup_subst(arena, type->as.nominal.type_args, type->as.nominal.type_arg_count, types, params, args, count,
                         &changed);
        if (!next) {
            return NULL;
        }
        if (!changed) {
            return type;
        }
        sym = type->as.nominal.decl ? type->as.nominal.decl->symbol : NULL;
        if (!sym || sym->type_param_count != type->as.nominal.type_arg_count) {
            return NULL;
        }
        if (type->kind == JOG_TY_STRUCT) {
            return jog_generic_struct_instance(types, arena, sym, next, type->as.nominal.type_arg_count);
        }
        return jog_generic_enum_instance(types, arena, sym, next, type->as.nominal.type_arg_count);
    }
    case JOG_TY_INTERFACE:
    case JOG_TY_DISTINCT:
        return type;
    default:
        return type;
    }
}

JogType *jog_generic_lookup_param(const CheckJob *job, const char *name) {
    JogSymbol *sym;
    size_t i;
    if (!job || !name) {
        return NULL;
    }
    sym = job->symbol;
    if (!sym || !sym->type_params) {
        return NULL;
    }
    for (i = 0; i < sym->type_param_count; i++) {
        JogType *param = sym->type_params[i];
        if (param && param->as.param.name && strcmp(param->as.param.name, name) == 0) {
            return param;
        }
    }
    return NULL;
}

static bool bind_param_list(Check *ck, CheckJob *job, JogNode *owner, JogList list, bool *bad) {
    JogSymbol *sym;
    size_t n;
    size_t i;
    JogType **params;
    n = list.count;
    if (n == 0) {
        return true;
    }
    sym = job->symbol ? job->symbol : (owner ? owner->symbol : NULL);
    params = ct_arena_alloc(ck->program->arena, n * sizeof(*params), _Alignof(JogType *));
    if (!params) {
        jog_check_oom(ck);
        return false;
    }
    memset(params, 0, n * sizeof(*params));
    for (i = 0; i < n && !ck->stop; i++) {
        JogNode *param = list_at(list, i);
        JogType *type;
        size_t prev;
        if (!param || param->kind != JOG_NODE_TYPE_PARAM || !param->as.type_param.name ||
            param->as.type_param.name[0] == '\0') {
            jog_check_fail(ck, param ? param->pos : owner->pos, "expected a type parameter");
            if (bad) {
                *bad = true;
            }
            continue;
        }
        for (prev = 0; prev < i; prev++) {
            JogType *earlier = params[prev];
            if (earlier && earlier->as.param.name && strcmp(earlier->as.param.name, param->as.type_param.name) == 0) {
                jog_check_fail(ck, param->pos, "duplicate type parameter '%s'", param->as.type_param.name);
                if (bad) {
                    *bad = true;
                }
            }
        }
        {
            JogType *constraint = NULL;
            int constraint_bad = 0;
            if (!jog_constraint_bind(ck, job, param, &constraint, &constraint_bad)) {
                return false;
            }
            if (constraint_bad && bad) {
                *bad = true;
            }
            type = jog_type_new_param(ck->program->types, param->as.type_param.name, param, constraint);
        }
        if (!type) {
            jog_check_oom(ck);
            return false;
        }
        param->type = type;
        params[i] = type;
    }
    if (sym) {
        sym->type_params = params;
        sym->type_param_count = n;
    }
    return !ck->stop;
}

bool jog_generic_bind_params(Check *ck, CheckJob *job, bool *bad) {
    JogNode *fn;
    if (!ck || !job || !job->decl || job->decl->kind != JOG_NODE_FN || ck->stop) {
        return !ck || !ck->stop;
    }
    fn = job->decl;
    if (fn->as.fn.type_params.count == 0) {
        return true;
    }
    if (fn->as.fn.is_extern) {
        jog_check_fail(ck, fn->pos, "an extern function cannot have type parameters");
        if (bad) {
            *bad = true;
        }
    }
    if (has_attr(fn, "export")) {
        jog_check_fail(ck, fn->pos, "an exported function cannot have type parameters");
        if (bad) {
            *bad = true;
        }
    }
    if (has_attr(fn, "test")) {
        jog_check_fail(ck, fn->pos, "@test functions cannot have type parameters");
        if (bad) {
            *bad = true;
        }
    }
    if (!fn->as.fn.receiver && fn->as.fn.name && strcmp(fn->as.fn.name, "main") == 0 && job->package == ck->program->root) {
        jog_check_fail(ck, fn->pos, "fn main cannot have type parameters");
        if (bad) {
            *bad = true;
        }
    }
    return bind_param_list(ck, job, fn, fn->as.fn.type_params, bad);
}

bool jog_generic_bind_struct_params(Check *ck, CheckJob *job, bool *bad) {
    JogNode *node;
    if (!ck || !job || !job->decl || ck->stop) {
        return !ck || !ck->stop;
    }
    if (job->decl->kind != JOG_NODE_STRUCT && job->decl->kind != JOG_NODE_ENUM) {
        return true;
    }
    node = job->decl;
    return bind_param_list(ck, job, node, node->as.aggregate.type_params, bad);
}

bool jog_generic_bind_alias_params(Check *ck, CheckJob *job, bool *bad) {
    JogNode *node;
    if (!ck || !job || !job->decl || job->decl->kind != JOG_NODE_TYPE_DECL || ck->stop) {
        return !ck || !ck->stop;
    }
    node = job->decl;
    return bind_param_list(ck, job, node, node->as.type_decl.type_params, bad);
}

static JogNode *receiver_nominal_node(JogNode *recv) {
    while (recv && (recv->kind == JOG_NODE_TYPE_PTR || recv->kind == JOG_NODE_TYPE_SLICE || recv->kind == JOG_NODE_TYPE_OPTION)) {
        recv = recv->as.type_elem.elem;
    }
    if (recv && recv->kind == JOG_NODE_TYPE_NAME) {
        return recv;
    }
    return NULL;
}

/* A receiver type argument is a bare name. List<[]i32> is not a binder. */
static const char *receiver_binder_name(const JogNode *arg) {
    if (!arg || arg->kind != JOG_NODE_TYPE_NAME) {
        return NULL;
    }
    if (arg->as.type_name.args.count != 0) {
        return NULL;
    }
    if (arg->as.type_name.path.count != 1 || !arg->as.type_name.path.items || !arg->as.type_name.path.items[0]) {
        return NULL;
    }
    if (arg->as.type_name.path.items[0][0] == '\0') {
        return NULL;
    }
    return arg->as.type_name.path.items[0];
}

bool jog_generic_bind_receiver_params(Check *ck, CheckJob *job, JogSymbol *owner, size_t expect, bool *bad,
                                      int *skip_recv) {
    JogNode *fn;
    JogNode *recv;
    JogSymbol *sym;
    JogList args;
    size_t n;
    size_t own;
    size_t i;
    JogType **recv_params;
    JogType **combined;
    if (skip_recv) {
        *skip_recv = 0;
    }
    if (!ck || !job || !job->decl || job->decl->kind != JOG_NODE_FN || ck->stop) {
        return !ck || !ck->stop;
    }
    fn = job->decl;
    sym = job->symbol ? job->symbol : fn->symbol;
    own = sym ? sym->type_param_count : 0;
    recv = receiver_nominal_node(fn->as.fn.receiver);
    args.count = 0;
    args.items = NULL;
    if (recv) {
        args = recv->as.type_name.args;
    }
    n = args.count;
    if (!recv || n != expect) {
        jog_check_fail(ck, fn->pos, "expected %u type parameters on the receiver, found %u", (unsigned)expect, (unsigned)n);
        if (bad) {
            *bad = true;
        }
        if (skip_recv) {
            *skip_recv = 1;
        }
    }
    if (n == 0) {
        return !ck->stop;
    }
    /* The struct type is published before its parameters are bound, so wait
       until that job has finished and the constraints are stored. */
    if (owner && (!owner->type_params || owner->type_param_count < n)) {
        if (!jog_check_wait_finished(ck, job, owner)) {
            return false;
        }
    }
    recv_params = ct_arena_alloc(ck->program->arena, n * sizeof(*recv_params), _Alignof(JogType *));
    if (!recv_params) {
        jog_check_oom(ck);
        return false;
    }
    memset(recv_params, 0, n * sizeof(*recv_params));
    for (i = 0; i < n && !ck->stop; i++) {
        JogNode *arg = list_at(args, i);
        const char *name = receiver_binder_name(arg);
        JogType *type = NULL;
        size_t prev;
        if (!name) {
            jog_check_fail(ck, arg ? arg->pos : fn->pos, "expected a type parameter");
            if (bad) {
                *bad = true;
            }
            if (skip_recv) {
                *skip_recv = 1;
            }
            continue;
        }
        for (prev = 0; prev < i; prev++) {
            JogType *earlier = recv_params[prev];
            if (earlier && earlier->as.param.name && strcmp(earlier->as.param.name, name) == 0) {
                jog_check_fail(ck, arg->pos, "duplicate type parameter '%s'", name);
                if (bad) {
                    *bad = true;
                }
            }
        }
        if (sym && sym->type_params) {
            for (prev = 0; prev < own; prev++) {
                JogType *earlier = sym->type_params[prev];
                if (earlier && earlier->as.param.name && strcmp(earlier->as.param.name, name) == 0) {
                    jog_check_fail(ck, arg->pos, "duplicate type parameter '%s'", name);
                    if (bad) {
                        *bad = true;
                    }
                }
            }
        }
        {
            JogType *constraint = NULL;
            if (owner && owner->type_params && i < owner->type_param_count) {
                JogType *src = owner->type_params[i];
                if (src && src->kind == JOG_TY_TYPE_PARAM) {
                    constraint = src->as.param.constraint;
                }
            }
            type = jog_type_new_param(ck->program->types, name, arg, constraint);
        }
        if (!type) {
            jog_check_oom(ck);
            return false;
        }
        arg->type = type;
        recv_params[i] = type;
    }
    if (ck->stop) {
        return false;
    }
    if (n > 0 && own == 0) {
        if (has_attr(fn, "export")) {
            jog_check_fail(ck, fn->pos, "an exported function cannot have type parameters");
            if (bad) {
                *bad = true;
            }
        }
        if (has_attr(fn, "test")) {
            jog_check_fail(ck, fn->pos, "@test functions cannot have type parameters");
            if (bad) {
                *bad = true;
            }
        }
    }
    if (!sym) {
        jog_check_fail(ck, fn->pos, "missing receiver");
        if (bad) {
            *bad = true;
        }
        return !ck->stop;
    }
    combined = ct_arena_alloc(ck->program->arena, (n + own) * sizeof(*combined), _Alignof(JogType *));
    if (!combined) {
        jog_check_oom(ck);
        return false;
    }
    memset(combined, 0, (n + own) * sizeof(*combined));
    for (i = 0; i < n; i++) {
        combined[i] = recv_params[i];
    }
    for (i = 0; i < own; i++) {
        combined[n + i] = sym->type_params ? sym->type_params[i] : NULL;
    }
    sym->type_params = combined;
    sym->type_param_count = n + own;
    return !ck->stop;
}

size_t jog_generic_method_recv_params(const JogSymbol *sym) {
    JogNode *fn;
    JogType *on;
    size_t n;
    size_t i;
    if (!sym || !sym->type_params || !sym->decl || sym->decl->kind != JOG_NODE_FN) {
        return 0;
    }
    fn = sym->decl;
    if (!fn->as.fn.receiver) {
        return 0;
    }
    on = fn->as.fn.receiver->type;
    if (on && on->kind == JOG_TY_PTR && on->as.elem.elem) {
        on = on->as.elem.elem;
    }
    if (!on || (on->kind != JOG_TY_STRUCT && on->kind != JOG_TY_ENUM)) {
        return 0;
    }
    n = on->as.nominal.type_arg_count;
    if (n == 0 || n > sym->type_param_count || !on->as.nominal.type_args) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (on->as.nominal.type_args[i] != sym->type_params[i]) {
            return 0;
        }
    }
    return n;
}

static void reject_defaults(Check *ck, JogList params, bool *bad) {
    size_t i;
    for (i = 0; i < params.count && !ck->stop; i++) {
        JogNode *param = list_at(params, i);
        if (!param || param->kind != JOG_NODE_PARAM || !param->as.param.default_value) {
            continue;
        }
        if (jog_generic_has_param(param->type)) {
            jog_check_fail(ck, param->pos, "a type parameter cannot have a default value yet");
            if (bad) {
                *bad = true;
            }
        }
    }
}

void jog_generic_reject_param_defaults(Check *ck, JogNode *fn, bool *bad) {
    if (!ck || !fn || fn->kind != JOG_NODE_FN || ck->stop) {
        return;
    }
    if (!fn->symbol || fn->symbol->type_param_count == 0) {
        return;
    }
    reject_defaults(ck, fn->as.fn.params, bad);
    reject_defaults(ck, fn->as.fn.contexts, bad);
}

static size_t param_index(JogType *const *params, size_t count, const JogType *param) {
    size_t i;
    for (i = 0; i < count; i++) {
        if (params[i] == param) {
            return i;
        }
    }
    return count;
}

static int bind_solved(Check *ck, JogPos pos, const char *name, Solved *solved, JogType *arg) {
    if (!arg || arg->kind == JOG_TY_ERROR) {
        return 1;
    }
    if (!name || name[0] == '\0') {
        name = "?";
    }
    if (arg->kind == JOG_TY_UNTYPED_INT) {
        if (solved->soft_float) {
            jog_check_fail(ck, pos, "conflicting types for type parameter '%s'", name);
            return 0;
        }
        if (solved->type) {
            if (solved->type->kind == JOG_TY_INT) {
                return 1;
            }
            jog_check_fail(ck, pos, "conflicting types for type parameter '%s'", name);
            return 0;
        }
        solved->soft = 1;
        return 1;
    }
    if (arg->kind == JOG_TY_UNTYPED_FLOAT) {
        if (solved->soft) {
            jog_check_fail(ck, pos, "conflicting types for type parameter '%s'", name);
            return 0;
        }
        if (solved->type) {
            if (solved->type->kind == JOG_TY_FLOAT) {
                return 1;
            }
            jog_check_fail(ck, pos, "conflicting types for type parameter '%s'", name);
            return 0;
        }
        solved->soft_float = 1;
        return 1;
    }
    if (solved->type) {
        if (solved->type == arg) {
            return 1;
        }
        jog_check_fail(ck, pos, "conflicting types for type parameter '%s'", name);
        return 0;
    }
    if ((solved->soft && arg->kind != JOG_TY_INT) || (solved->soft_float && arg->kind != JOG_TY_FLOAT)) {
        jog_check_fail(ck, pos, "conflicting types for type parameter '%s'", name);
        return 0;
    }
    solved->type = arg;
    return 1;
}

static int unify(Check *ck, JogPos pos, JogType *param, JogType *arg, Solved *solved, JogType *const *params, size_t nparam);

static int unify_parts(Check *ck, JogPos pos, JogType *const *left, JogType *const *right, size_t n, Solved *solved,
                       JogType *const *params, size_t nparam) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (!unify(ck, pos, left ? left[i] : NULL, right ? right[i] : NULL, solved, params, nparam)) {
            return 0;
        }
    }
    return 1;
}

static int unify(Check *ck, JogPos pos, JogType *param, JogType *arg, Solved *solved, JogType *const *params, size_t nparam) {
    size_t index;
    char want[160];
    char got[160];
    if (!param || (arg && arg->kind == JOG_TY_ERROR)) {
        return 1;
    }
    if (param->kind == JOG_TY_TYPE_PARAM) {
        index = param_index(params, nparam, param);
        if (index < nparam) {
            const char *name = param->as.param.name;
            return bind_solved(ck, pos, name, &solved[index], arg);
        }
    }
    if (!arg) {
        return 0;
    }
    if (arg->kind == JOG_TY_UNTYPED_INT && param->kind == JOG_TY_INT) {
        return 1;
    }
    if (arg->kind == JOG_TY_UNTYPED_FLOAT && param->kind == JOG_TY_FLOAT) {
        return 1;
    }
    if (param->kind == JOG_TY_PTR || param->kind == JOG_TY_SLICE) {
        if (arg->kind != param->kind || arg->is_const != param->is_const) {
            goto mismatch;
        }
        return unify(ck, pos, param->as.elem.elem, arg->as.elem.elem, solved, params, nparam);
    }
    if (param->kind == JOG_TY_OPTION) {
        if (arg->kind != JOG_TY_OPTION) {
            goto mismatch;
        }
        return unify(ck, pos, param->as.elem.elem, arg->as.elem.elem, solved, params, nparam);
    }
    if (param->kind == JOG_TY_ARRAY) {
        if (arg->kind != JOG_TY_ARRAY || arg->as.array.len != param->as.array.len) {
            goto mismatch;
        }
        return unify(ck, pos, param->as.array.elem, arg->as.array.elem, solved, params, nparam);
    }
    if (param->kind == JOG_TY_TUPLE) {
        if (arg->kind != JOG_TY_TUPLE || arg->as.tuple.count != param->as.tuple.count) {
            goto mismatch;
        }
        return unify_parts(ck, pos, param->as.tuple.elems, arg->as.tuple.elems, param->as.tuple.count, solved, params, nparam);
    }
    if (param->kind == JOG_TY_FN) {
        if (arg->kind != JOG_TY_FN || arg->as.fn.param_count != param->as.fn.param_count ||
            arg->as.fn.context_count != param->as.fn.context_count || arg->as.fn.variadic != param->as.fn.variadic) {
            goto mismatch;
        }
        if (!unify_parts(ck, pos, param->as.fn.params, arg->as.fn.params, param->as.fn.param_count, solved, params, nparam)) {
            return 0;
        }
        if (!unify_parts(ck, pos, param->as.fn.contexts, arg->as.fn.contexts, param->as.fn.context_count, solved, params,
                         nparam)) {
            return 0;
        }
        return unify(ck, pos, param->as.fn.ret, arg->as.fn.ret, solved, params, nparam);
    }
    if ((param->kind == JOG_TY_STRUCT || param->kind == JOG_TY_ENUM || param->kind == JOG_TY_INTERFACE ||
         param->kind == JOG_TY_DISTINCT) &&
        arg->kind == param->kind && param->as.nominal.decl && param->as.nominal.decl == arg->as.nominal.decl &&
        param->as.nominal.type_arg_count > 0 && param->as.nominal.type_arg_count == arg->as.nominal.type_arg_count) {
        return unify_parts(ck, pos, param->as.nominal.type_args, arg->as.nominal.type_args, param->as.nominal.type_arg_count,
                           solved, params, nparam);
    }
    if (param == arg) {
        return 1;
    }
mismatch:
    type_text(param, want, sizeof(want));
    type_text(arg, got, sizeof(got));
    jog_check_fail(ck, pos, "mismatched types %s and %s", want, got);
    return 0;
}

static int param_named(JogNode *decl, const char *name, size_t count) {
    size_t i;
    if (!decl || !name) {
        return -1;
    }
    for (i = 0; i < count && i < decl->as.fn.params.count; i++) {
        JogNode *param = list_at(decl->as.fn.params, i);
        if (param && param->kind == JOG_NODE_PARAM && param->as.param.name && strcmp(param->as.param.name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static JogType *probe_arg(void *env, JogNode *value, JogGenericCheckExpr check_expr, int *untyped) {
    *untyped = 0;
    if (!value) {
        return NULL;
    }
    if (soft_kind(value) != 0) {
        *untyped = soft_kind(value);
        return NULL;
    }
    if (value->kind == JOG_NODE_NULL_LIT) {
        return NULL;
    }
    return check_expr(env, value, NULL);
}

static JogType *finish_solved(Check *ck, JogPos pos, Solved *solved, JogType *const *params, size_t n) {
    JogType *i64;
    JogType *f64;
    size_t i;
    i64 = jog_type_builtin(ck->program->types, JOG_BUILTIN_I64);
    f64 = jog_type_builtin(ck->program->types, JOG_BUILTIN_F64);
    if (!i64 || !f64) {
        jog_check_oom(ck);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        const char *name = params[i] && params[i]->as.param.name ? params[i]->as.param.name : "?";
        if (!solved[i].type) {
            if (solved[i].soft && solved[i].soft_float) {
                jog_check_fail(ck, pos, "conflicting types for type parameter '%s'", name);
                return jog_check_error_type(ck);
            }
            if (solved[i].soft) {
                solved[i].type = i64;
            } else if (solved[i].soft_float) {
                solved[i].type = f64;
            } else {
                jog_check_fail(ck, pos, "cannot infer type parameter '%s'", name);
                return jog_check_error_type(ck);
            }
        }
        if (solved[i].type->kind == JOG_TY_ERROR) {
            return solved[i].type;
        }
    }
    return NULL;
}

JogType *jog_check_generic_instantiate(Check *ck, CheckJob *job, void *env, JogNode *call, JogNode *callee,
                                      JogType *fn_type, JogGenericCheckExpr check_expr) {
    JogSymbol *sym;
    JogNode *decl;
    size_t n;
    size_t i;
    JogType **args;
    JogType *built;
    JogList written;
    Solved stack_solved[16];
    Solved *solved;
    if (!ck || !call || !callee || ck->stop) {
        return NULL;
    }
    sym = callee->symbol;
    if (!sym || sym->type_param_count == 0 || !sym->type_params) {
        return fn_type;
    }
    if (!fn_type || fn_type->kind != JOG_TY_FN) {
        return jog_check_error_type(ck);
    }
    n = sym->type_param_count;
    decl = sym->decl && sym->decl->kind == JOG_NODE_FN ? sym->decl : NULL;
    written.count = 0;
    written.items = NULL;
    if (callee->kind == JOG_NODE_IDENT) {
        written = callee->as.ident.type_args;
    }
    args = ct_arena_alloc(ck->program->arena, n * sizeof(*args), _Alignof(JogType *));
    if (!args) {
        jog_check_oom(ck);
        return NULL;
    }
    memset(args, 0, n * sizeof(*args));
    if (written.count > 0) {
        if (written.count != n) {
            jog_check_fail(ck, callee->pos, "expected %u type arguments, found %u", (unsigned)n, (unsigned)written.count);
            return jog_check_error_type(ck);
        }
        for (i = 0; i < n && !ck->stop; i++) {
            JogNode *arg = list_at(written, i);
            JogType *type = arg ? jog_check_resolve_type(ck, job, arg) : NULL;
            if (!type) {
                return NULL;
            }
            if (type->kind == JOG_TY_ERROR) {
                return type;
            }
            args[i] = type;
        }
        if (ck->stop) {
            return NULL;
        }
    } else {
        size_t nvalue = fn_type->as.fn.param_count;
        size_t positional = 0;
        int seen_named = 0;
        if (n > 16) {
            solved = ct_arena_alloc(ck->program->arena, n * sizeof(*solved), _Alignof(Solved));
            if (!solved) {
                jog_check_oom(ck);
                return NULL;
            }
        } else {
            solved = stack_solved;
        }
        memset(solved, 0, n * sizeof(*solved));
        for (i = 0; i < call->as.call.args.count && !ck->stop; i++) {
            JogNode *arg = list_at(call->as.call.args, i);
            JogNode *value;
            int index;
            int untyped = 0;
            JogType *got;
            if (!arg || arg->kind != JOG_NODE_ARG) {
                continue;
            }
            value = arg->as.arg.value;
            if (arg->as.arg.name) {
                seen_named = 1;
                index = param_named(decl, arg->as.arg.name, nvalue);
                if (index < 0) {
                    continue;
                }
            } else {
                if (seen_named || positional >= nvalue) {
                    continue;
                }
                index = (int)positional;
                positional++;
            }
            if (soft_kind(value) != 0) {
                got = NULL;
                untyped = soft_kind(value);
            } else if (value && value->kind == JOG_NODE_NULL_LIT) {
                got = NULL;
            } else {
                got = probe_arg(env, value, check_expr, &untyped);
                if (ck->stop) {
                    return NULL;
                }
            }
            if (untyped) {
                JogType *untyped_type =
                    jog_type_builtin(ck->program->types, untyped == 2 ? JOG_BUILTIN_UNTYPED_FLOAT : JOG_BUILTIN_UNTYPED_INT);
                if (!unify(ck, arg->pos, fn_type->as.fn.params[index], untyped_type, solved, sym->type_params, n)) {
                    return jog_check_error_type(ck);
                }
            } else if (got && got->kind != JOG_TY_ERROR) {
                if (!unify(ck, arg->pos, fn_type->as.fn.params[index], got, solved, sym->type_params, n)) {
                    return jog_check_error_type(ck);
                }
            } else if (got && got->kind == JOG_TY_ERROR) {
                return got;
            }
        }
        if (ck->stop) {
            return NULL;
        }
        {
            JogType *failed = finish_solved(ck, call->pos, solved, sym->type_params, n);
            if (ck->stop) {
                return NULL;
            }
            if (failed) {
                return failed;
            }
            for (i = 0; i < n; i++) {
                args[i] = solved[i].type;
            }
        }
    }
    {
        JogType *constraint_err = jog_constraint_check_args(ck, job, sym, args, n, call->pos);
        if (ck->stop) {
            return NULL;
        }
        if (constraint_err) {
            return constraint_err;
        }
    }
    built = jog_generic_subst(ck->program->types, ck->program->arena, fn_type, sym->type_params, args, n);
    if (!built) {
        jog_check_oom(ck);
        return NULL;
    }
    call->solved_type_args = args;
    call->solved_type_arg_count = n;
    return built;
}

static JogList member_type_args(JogNode *callee) {
    JogList empty;
    empty.items = NULL;
    empty.count = 0;
    if (!callee) {
        return empty;
    }
    if (callee->kind == JOG_NODE_IDENT) {
        return callee->as.ident.type_args;
    }
    if (callee->kind == JOG_NODE_MEMBER) {
        return callee->as.member.type_args;
    }
    return empty;
}

static int method_value_slot(JogNode *decl, JogNode *arg, size_t *positional, int *seen_named, int implicit, size_t nparam) {
    int slot;
    if (!arg || arg->kind != JOG_NODE_ARG) {
        return -1;
    }
    if (arg->as.arg.name) {
        int user;
        *seen_named = 1;
        user = param_named(decl, arg->as.arg.name, decl && decl->kind == JOG_NODE_FN ? decl->as.fn.params.count : 0);
        if (user < 0) {
            return -1;
        }
        slot = user + 1;
    } else {
        if (*seen_named) {
            return -1;
        }
        slot = implicit ? (int)(*positional + 1) : (int)(*positional);
        (*positional)++;
    }
    if (slot < 0 || (size_t)slot >= nparam) {
        return -1;
    }
    return slot;
}

JogType *jog_check_generic_method(Check *ck, CheckJob *job, void *env, JogNode *call, JogNode *callee, JogType *recv,
                                 int implicit, JogGenericCheckExpr check_expr) {
    JogSymbol *sym;
    JogNode *decl;
    JogType *fn_type;
    size_t n;
    size_t recv_n;
    size_t own_n;
    size_t i;
    JogType **args;
    JogType *built;
    JogList written;
    Solved stack_solved[16];
    Solved *solved;
    if (!ck || !call || !callee || ck->stop) {
        return NULL;
    }
    sym = callee->symbol;
    fn_type = callee->type && callee->type->kind == JOG_TY_FN ? callee->type : NULL;
    if (!fn_type && sym && sym->decl && sym->decl->kind == JOG_NODE_FN && sym->decl->type &&
        sym->decl->type->kind == JOG_TY_FN) {
        fn_type = sym->decl->type;
    }
    if (!sym || sym->type_param_count == 0 || !sym->type_params) {
        return fn_type;
    }
    if (!fn_type) {
        return jog_check_error_type(ck);
    }
    n = sym->type_param_count;
    recv_n = jog_generic_method_recv_params(sym);
    if (recv_n > n) {
        recv_n = 0;
    }
    own_n = n - recv_n;
    decl = sym->decl && sym->decl->kind == JOG_NODE_FN ? sym->decl : NULL;
    args = ct_arena_alloc(ck->program->arena, n * sizeof(*args), _Alignof(JogType *));
    if (!args) {
        jog_check_oom(ck);
        return NULL;
    }
    memset(args, 0, n * sizeof(*args));
    if (recv_n > 0) {
        if (!recv || (recv->kind != JOG_TY_STRUCT && recv->kind != JOG_TY_ENUM) ||
            recv->as.nominal.type_arg_count != recv_n || !recv->as.nominal.type_args) {
            jog_check_fail(ck, call->pos, "generic type was not instantiated");
            return jog_check_error_type(ck);
        }
        for (i = 0; i < recv_n; i++) {
            JogType *arg = recv->as.nominal.type_args[i];
            if (!arg || arg->kind == JOG_TY_ERROR) {
                return arg && arg->kind == JOG_TY_ERROR ? arg : jog_check_error_type(ck);
            }
            args[i] = arg;
        }
    }
    written = member_type_args(callee);
    if (written.count > 0 || own_n == 0) {
        if (written.count != own_n) {
            jog_check_fail(ck, callee->pos, "expected %u type arguments, found %u", (unsigned)own_n,
                           (unsigned)written.count);
            return jog_check_error_type(ck);
        }
        for (i = 0; i < own_n && !ck->stop; i++) {
            JogNode *arg = list_at(written, i);
            JogType *type = arg ? jog_check_resolve_type(ck, job, arg) : NULL;
            if (!type) {
                return NULL;
            }
            if (type->kind == JOG_TY_ERROR) {
                return type;
            }
            args[recv_n + i] = type;
        }
        if (ck->stop) {
            return NULL;
        }
    } else {
        size_t nparam = fn_type->as.fn.param_count;
        size_t positional = 0;
        int seen_named = 0;
        if (n > 16) {
            solved = ct_arena_alloc(ck->program->arena, n * sizeof(*solved), _Alignof(Solved));
            if (!solved) {
                jog_check_oom(ck);
                return NULL;
            }
        } else {
            solved = stack_solved;
        }
        memset(solved, 0, n * sizeof(*solved));
        for (i = 0; i < recv_n; i++) {
            solved[i].type = args[i];
        }
        for (i = 0; i < call->as.call.args.count && !ck->stop; i++) {
            JogNode *arg = list_at(call->as.call.args, i);
            JogNode *value;
            int slot;
            int untyped = 0;
            JogType *got;
            slot = method_value_slot(decl, arg, &positional, &seen_named, implicit, nparam);
            if (slot < 0 || !arg) {
                continue;
            }
            value = arg->as.arg.value;
            if (soft_kind(value) != 0) {
                got = NULL;
                untyped = soft_kind(value);
            } else if (value && value->kind == JOG_NODE_NULL_LIT) {
                got = NULL;
            } else {
                got = probe_arg(env, value, check_expr, &untyped);
                if (ck->stop) {
                    return NULL;
                }
            }
            if (untyped) {
                JogType *untyped_type =
                    jog_type_builtin(ck->program->types, untyped == 2 ? JOG_BUILTIN_UNTYPED_FLOAT : JOG_BUILTIN_UNTYPED_INT);
                if (!unify(ck, arg->pos, fn_type->as.fn.params[slot], untyped_type, solved, sym->type_params, n)) {
                    return jog_check_error_type(ck);
                }
            } else if (got && got->kind != JOG_TY_ERROR) {
                if (!unify(ck, arg->pos, fn_type->as.fn.params[slot], got, solved, sym->type_params, n)) {
                    return jog_check_error_type(ck);
                }
            } else if (got && got->kind == JOG_TY_ERROR) {
                return got;
            }
        }
        if (ck->stop) {
            return NULL;
        }
        {
            JogType *failed = finish_solved(ck, call->pos, solved, sym->type_params, n);
            if (ck->stop) {
                return NULL;
            }
            if (failed) {
                return failed;
            }
            for (i = 0; i < n; i++) {
                args[i] = solved[i].type;
            }
        }
    }
    {
        JogType *constraint_err = jog_constraint_check_args(ck, job, sym, args, n, call->pos);
        if (ck->stop) {
            return NULL;
        }
        if (constraint_err) {
            return constraint_err;
        }
    }
    built = jog_generic_subst(ck->program->types, ck->program->arena, fn_type, sym->type_params, args, n);
    if (!built) {
        jog_check_oom(ck);
        return NULL;
    }
    call->solved_type_args = args;
    call->solved_type_arg_count = n;
    return built;
}

/* The unfinished-struct placeholder: one nameless field. An empty struct
   has no fields, so the two stay distinct. */
static int nominal_hold(const JogType *type) {
    if (!type || type->kind != JOG_TY_STRUCT || type->as.nominal.field_count != 1 || !type->as.nominal.fields) {
        return 0;
    }
    return type->as.nominal.fields[0].name == NULL && type->as.nominal.fields[0].type == NULL;
}

static int template_ready(const JogType *type) {
    return type && type->kind == JOG_TY_STRUCT && !nominal_hold(type);
}

static int same_args(JogType *const *left, JogType *const *right, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if ((left ? left[i] : NULL) != (right ? right[i] : NULL)) {
            return 0;
        }
    }
    return 1;
}

static JogType *find_nominal(JogSymbol *sym, JogType *const *args, size_t n) {
    size_t i;
    if (!sym) {
        return NULL;
    }
    for (i = 0; i < sym->nominal_inst_count; i++) {
        JogType *inst = sym->nominal_insts ? sym->nominal_insts[i] : NULL;
        if (inst && inst->as.nominal.type_arg_count == n && same_args(inst->as.nominal.type_args, args, n)) {
            return inst;
        }
    }
    return NULL;
}

static int remember_nominal(CtArena *arena, JogSymbol *sym, JogType *inst) {
    JogType **grown;
    size_t cap;
    if (sym->nominal_inst_count == sym->nominal_inst_cap) {
        cap = sym->nominal_inst_cap ? sym->nominal_inst_cap * 2 : 4;
        grown = ct_arena_alloc(arena, cap * sizeof(*grown), _Alignof(JogType *));
        if (!grown) {
            return 0;
        }
        if (sym->nominal_inst_count > 0 && sym->nominal_insts) {
            memcpy(grown, sym->nominal_insts, sym->nominal_inst_count * sizeof(*grown));
        }
        sym->nominal_insts = grown;
        sym->nominal_inst_cap = cap;
    }
    sym->nominal_insts[sym->nominal_inst_count++] = inst;
    return 1;
}

static int hold_nominal(CtArena *arena, JogType *inst) {
    JogField *hold = ct_arena_alloc(arena, sizeof(*hold), _Alignof(JogField));
    if (!hold) {
        return 0;
    }
    memset(hold, 0, sizeof(*hold));
    inst->as.nominal.fields = hold;
    inst->as.nominal.field_count = 1;
    return 1;
}

/* Clear the hold before substituting so a pointer back to this instance
   does not fill it again. */
static int fill_nominal(JogTypes *types, CtArena *arena, JogSymbol *sym, JogType *inst) {
    JogType *tmpl;
    size_t n;
    size_t i;
    JogField *fields;
    if (!sym || !inst || !types || !arena) {
        return 0;
    }
    tmpl = sym->type;
    if (!template_ready(tmpl) || !sym->type_params) {
        return 1;
    }
    inst->as.nominal.fields = NULL;
    inst->as.nominal.field_count = 0;
    n = tmpl->as.nominal.field_count;
    fields = NULL;
    if (n > 0) {
        fields = ct_arena_alloc(arena, n * sizeof(*fields), _Alignof(JogField));
        if (!fields) {
            return 0;
        }
        memset(fields, 0, n * sizeof(*fields));
        for (i = 0; i < n; i++) {
            JogField *src = tmpl->as.nominal.fields ? &tmpl->as.nominal.fields[i] : NULL;
            JogType *field_type;
            if (!src) {
                return 0;
            }
            field_type = jog_generic_subst(types, arena, src->type, sym->type_params, inst->as.nominal.type_args,
                                           inst->as.nominal.type_arg_count);
            if (!field_type) {
                return 0;
            }
            fields[i] = *src;
            fields[i].type = field_type;
            fields[i].offset = 0;
        }
    }
    inst->as.nominal.fields = fields;
    inst->as.nominal.field_count = n;
    if (!jog_generic_has_param(inst)) {
        (void)jog_type_layout(types, inst);
    }
    return 1;
}

JogType *jog_generic_struct_instance(JogTypes *types, CtArena *arena, JogSymbol *sym, JogType *const *args,
                                    size_t count) {
    JogType *found;
    JogType *inst;
    JogType **copied;
    JogType *tmpl;
    size_t i;
    if (!types || !arena || !sym || (count > 0 && !args) || sym->type_param_count != count) {
        return NULL;
    }
    found = find_nominal(sym, args, count);
    if (found) {
        if (nominal_hold(found) && template_ready(sym->type)) {
            if (!fill_nominal(types, arena, sym, found)) {
                return NULL;
            }
        }
        return found;
    }
    tmpl = sym->type;
    if (!tmpl || tmpl->kind != JOG_TY_STRUCT) {
        return NULL;
    }
    copied = NULL;
    if (count > 0) {
        copied = ct_arena_alloc(arena, count * sizeof(*copied), _Alignof(JogType *));
        if (!copied) {
            return NULL;
        }
        for (i = 0; i < count; i++) {
            copied[i] = args[i];
        }
    }
    inst = jog_type_new_nominal(types, JOG_TY_STRUCT, tmpl->as.nominal.name, tmpl->as.nominal.package, tmpl->as.nominal.decl);
    if (!inst) {
        return NULL;
    }
    inst->as.nominal.type_args = copied;
    inst->as.nominal.type_arg_count = count;
    if (!remember_nominal(arena, sym, inst)) {
        return NULL;
    }
    if (!template_ready(tmpl)) {
        if (!hold_nominal(arena, inst)) {
            return NULL;
        }
        return inst;
    }
    if (!fill_nominal(types, arena, sym, inst)) {
        return NULL;
    }
    return inst;
}

bool jog_generic_struct_finish(JogTypes *types, CtArena *arena, JogSymbol *sym) {
    size_t i;
    if (!sym) {
        return true;
    }
    for (i = 0; i < sym->nominal_inst_count; i++) {
        JogType *inst = sym->nominal_insts ? sym->nominal_insts[i] : NULL;
        if (inst && nominal_hold(inst)) {
            if (!fill_nominal(types, arena, sym, inst)) {
                return false;
            }
        }
    }
    return true;
}

JogType *jog_generic_struct_apply(Check *ck, CheckJob *job, JogSymbol *sym, JogList args, JogPos pos) {
    size_t n;
    size_t i;
    JogType **solved;
    JogType *inst;
    const char *name;
    if (!ck || !sym || ck->stop) {
        return NULL;
    }
    name = sym->name && sym->name[0] != '\0' ? sym->name : "?";
    n = sym->type_param_count;
    if (args.count != n) {
        if (args.count == 0) {
            jog_check_fail(ck, pos, "missing type arguments for '%s'", name);
        } else {
            jog_check_fail(ck, pos, "expected %u type arguments, found %u", (unsigned)n, (unsigned)args.count);
        }
        return jog_check_error_type(ck);
    }
    solved = ct_arena_alloc(ck->program->arena, n * sizeof(*solved), _Alignof(JogType *));
    if (!solved) {
        jog_check_oom(ck);
        return NULL;
    }
    memset(solved, 0, n * sizeof(*solved));
    for (i = 0; i < n && !ck->stop; i++) {
        JogNode *arg = list_at(args, i);
        JogType *type = arg ? jog_check_resolve_type(ck, job, arg) : NULL;
        if (!arg) {
            jog_check_fail(ck, pos, "missing type");
            return jog_check_error_type(ck);
        }
        if (!type) {
            return NULL;
        }
        if (type->kind == JOG_TY_ERROR) {
            return type;
        }
        solved[i] = type;
    }
    if (ck->stop) {
        return NULL;
    }
    {
        JogType *constraint_err = jog_constraint_check_args(ck, job, sym, solved, n, pos);
        if (ck->stop) {
            return NULL;
        }
        if (constraint_err) {
            return constraint_err;
        }
    }
    inst = jog_generic_struct_instance(ck->program->types, ck->program->arena, sym, solved, n);
    if (!inst) {
        jog_check_oom(ck);
        return NULL;
    }
    /* A hold means the template fields are not known yet. The struct
       checker reports real value recursion after those fields exist. */
    if (!nominal_hold(inst) && !jog_generic_has_param(inst) && !inst->has_layout) {
        jog_check_fail(ck, pos, "struct '%s' recursively contains itself", name);
        return jog_check_error_type(ck);
    }
    return inst;
}

static int enum_hold(const JogType *type) {
    if (!type || type->kind != JOG_TY_ENUM || type->as.nominal.variant_count != 1 || !type->as.nominal.variants) {
        return 0;
    }
    return type->as.nominal.variants[0].name == NULL && type->as.nominal.variants[0].decl == NULL;
}

static int enum_ready(const JogType *type) {
    return type && type->kind == JOG_TY_ENUM && type->as.nominal.variants && !enum_hold(type);
}

static int enum_contains(const JogType *self, const JogType *type, int depth) {
    size_t i;
    if (!type || depth > 32) {
        return 1;
    }
    if (type == self) {
        return 1;
    }
    switch (type->kind) {
    case JOG_TY_PTR:
    case JOG_TY_SLICE:
    case JOG_TY_FN:
    case JOG_TY_INTERFACE:
        return 0;
    case JOG_TY_ARRAY:
        return enum_contains(self, type->as.array.elem, depth + 1);
    case JOG_TY_OPTION:
        return enum_contains(self, type->as.elem.elem, depth + 1);
    case JOG_TY_DISTINCT:
        return enum_contains(self, type->as.nominal.base, depth + 1);
    case JOG_TY_TUPLE:
        for (i = 0; i < type->as.tuple.count; i++) {
            if (type->as.tuple.elems && enum_contains(self, type->as.tuple.elems[i], depth + 1)) {
                return 1;
            }
        }
        return 0;
    case JOG_TY_STRUCT:
        for (i = 0; i < type->as.nominal.field_count; i++) {
            if (type->as.nominal.fields && enum_contains(self, type->as.nominal.fields[i].type, depth + 1)) {
                return 1;
            }
        }
        return 0;
    case JOG_TY_ENUM:
        for (i = 0; i < type->as.nominal.variant_count; i++) {
            size_t f;
            const JogVariant *variant = type->as.nominal.variants ? &type->as.nominal.variants[i] : NULL;
            if (!variant) {
                return 1;
            }
            for (f = 0; f < variant->field_count; f++) {
                if (variant->fields && enum_contains(self, variant->fields[f].type, depth + 1)) {
                    return 1;
                }
            }
        }
        return 0;
    default:
        return 0;
    }
}

static int hold_enum(CtArena *arena, JogType *inst) {
    JogVariant *hold = ct_arena_alloc(arena, sizeof(*hold), _Alignof(JogVariant));
    if (!hold) {
        return 0;
    }
    memset(hold, 0, sizeof(*hold));
    inst->as.nominal.variants = hold;
    inst->as.nominal.variant_count = 1;
    return 1;
}

static int enum_value_cycle(const JogType *inst) {
    size_t i;
    if (!inst) {
        return 0;
    }
    for (i = 0; i < inst->as.nominal.variant_count; i++) {
        size_t f;
        const JogVariant *variant = inst->as.nominal.variants ? &inst->as.nominal.variants[i] : NULL;
        if (!variant) {
            continue;
        }
        for (f = 0; f < variant->field_count; f++) {
            if (variant->fields && enum_contains(inst, variant->fields[f].type, 0)) {
                return 1;
            }
        }
    }
    return 0;
}

static int fill_enum(JogTypes *types, CtArena *arena, JogSymbol *sym, JogType *inst) {
    JogType *tmpl;
    size_t n;
    size_t i;
    JogVariant *variants;
    if (!sym || !inst || !types || !arena) {
        return 0;
    }
    tmpl = sym->type;
    if (!enum_ready(tmpl) || !sym->type_params) {
        return 1;
    }
    inst->as.nominal.variants = NULL;
    inst->as.nominal.variant_count = 0;
    inst->as.nominal.base = tmpl->as.nominal.base;
    n = tmpl->as.nominal.variant_count;
    variants = NULL;
    if (n > 0) {
        variants = ct_arena_alloc(arena, n * sizeof(*variants), _Alignof(JogVariant));
        if (!variants) {
            return 0;
        }
        memset(variants, 0, n * sizeof(*variants));
        for (i = 0; i < n; i++) {
            JogVariant *src = tmpl->as.nominal.variants ? &tmpl->as.nominal.variants[i] : NULL;
            JogField *fields = NULL;
            size_t f;
            if (!src) {
                return 0;
            }
            if (src->field_count > 0) {
                fields = ct_arena_alloc(arena, src->field_count * sizeof(*fields), _Alignof(JogField));
                if (!fields) {
                    return 0;
                }
                memset(fields, 0, src->field_count * sizeof(*fields));
                for (f = 0; f < src->field_count; f++) {
                    JogType *field_type;
                    if (!src->fields) {
                        return 0;
                    }
                    field_type = jog_generic_subst(types, arena, src->fields[f].type, sym->type_params,
                                                   inst->as.nominal.type_args, inst->as.nominal.type_arg_count);
                    if (!field_type) {
                        return 0;
                    }
                    fields[f] = src->fields[f];
                    fields[f].type = field_type;
                    fields[f].offset = 0;
                }
            }
            variants[i] = *src;
            variants[i].fields = fields;
        }
    }
    inst->as.nominal.variants = variants;
    inst->as.nominal.variant_count = n;
    if (!jog_generic_has_param(inst) && !enum_value_cycle(inst)) {
        (void)jog_type_layout(types, inst);
    }
    return 1;
}

JogType *jog_generic_enum_instance(JogTypes *types, CtArena *arena, JogSymbol *sym, JogType *const *args,
                                  size_t count) {
    JogType *found;
    JogType *inst;
    JogType **copied;
    JogType *tmpl;
    size_t i;
    if (!types || !arena || !sym || (count > 0 && !args) || sym->type_param_count != count) {
        return NULL;
    }
    found = find_nominal(sym, args, count);
    if (found) {
        if (enum_hold(found) && enum_ready(sym->type)) {
            if (!fill_enum(types, arena, sym, found)) {
                return NULL;
            }
        }
        return found;
    }
    tmpl = sym->type;
    if (!tmpl || tmpl->kind != JOG_TY_ENUM) {
        return NULL;
    }
    copied = NULL;
    if (count > 0) {
        copied = ct_arena_alloc(arena, count * sizeof(*copied), _Alignof(JogType *));
        if (!copied) {
            return NULL;
        }
        for (i = 0; i < count; i++) {
            copied[i] = args[i];
        }
    }
    inst = jog_type_new_nominal(types, JOG_TY_ENUM, tmpl->as.nominal.name, tmpl->as.nominal.package, tmpl->as.nominal.decl);
    if (!inst) {
        return NULL;
    }
    inst->as.nominal.type_args = copied;
    inst->as.nominal.type_arg_count = count;
    inst->as.nominal.base = tmpl->as.nominal.base;
    if (!remember_nominal(arena, sym, inst)) {
        return NULL;
    }
    if (!enum_ready(tmpl)) {
        if (!hold_enum(arena, inst)) {
            return NULL;
        }
        return inst;
    }
    if (!fill_enum(types, arena, sym, inst)) {
        return NULL;
    }
    return inst;
}

bool jog_generic_enum_finish(JogTypes *types, CtArena *arena, JogSymbol *sym) {
    size_t i;
    if (!sym) {
        return true;
    }
    for (i = 0; i < sym->nominal_inst_count; i++) {
        JogType *inst = sym->nominal_insts ? sym->nominal_insts[i] : NULL;
        if (inst && enum_hold(inst)) {
            if (!fill_enum(types, arena, sym, inst)) {
                return false;
            }
        }
    }
    return true;
}

JogType *jog_generic_enum_apply(Check *ck, CheckJob *job, JogSymbol *sym, JogList args, JogPos pos) {
    size_t n;
    size_t i;
    JogType **solved;
    JogType *inst;
    const char *name;
    if (!ck || !sym || ck->stop) {
        return NULL;
    }
    name = sym->name && sym->name[0] != '\0' ? sym->name : "?";
    n = sym->type_param_count;
    if (args.count != n) {
        if (args.count == 0) {
            jog_check_fail(ck, pos, "missing type arguments for '%s'", name);
        } else {
            jog_check_fail(ck, pos, "expected %u type arguments, found %u", (unsigned)n, (unsigned)args.count);
        }
        return jog_check_error_type(ck);
    }
    solved = ct_arena_alloc(ck->program->arena, n * sizeof(*solved), _Alignof(JogType *));
    if (!solved) {
        jog_check_oom(ck);
        return NULL;
    }
    memset(solved, 0, n * sizeof(*solved));
    for (i = 0; i < n && !ck->stop; i++) {
        JogNode *arg = list_at(args, i);
        JogType *type = arg ? jog_check_resolve_type(ck, job, arg) : NULL;
        if (!arg) {
            jog_check_fail(ck, pos, "missing type");
            return jog_check_error_type(ck);
        }
        if (!type) {
            return NULL;
        }
        if (type->kind == JOG_TY_ERROR) {
            return type;
        }
        solved[i] = type;
    }
    if (ck->stop) {
        return NULL;
    }
    {
        JogType *constraint_err = jog_constraint_check_args(ck, job, sym, solved, n, pos);
        if (ck->stop) {
            return NULL;
        }
        if (constraint_err) {
            return constraint_err;
        }
    }
    inst = jog_generic_enum_instance(ck->program->types, ck->program->arena, sym, solved, n);
    if (!inst) {
        jog_check_oom(ck);
        return NULL;
    }
    if (!enum_hold(inst) && !jog_generic_has_param(inst) && !inst->has_layout) {
        jog_check_fail(ck, pos, "enum '%s' recursively contains itself", name);
        return jog_check_error_type(ck);
    }
    return inst;
}

/* The alias is a name. symbol->type is the target with the alias's own
   type parameters still in it. Substituting the written arguments yields
   one type for that list; a parameter that remains belongs to an outer
   generic and stays open. */
JogType *jog_generic_alias_apply(Check *ck, CheckJob *job, JogSymbol *sym, JogList args, JogPos pos) {
    size_t n;
    size_t i;
    JogType **solved;
    JogType *inst;
    const char *name;
    if (!ck || !sym || ck->stop) {
        return NULL;
    }
    name = sym->name && sym->name[0] != '\0' ? sym->name : "?";
    n = sym->type_param_count;
    if (args.count != n) {
        if (args.count == 0) {
            jog_check_fail(ck, pos, "missing type arguments for '%s'", name);
        } else {
            jog_check_fail(ck, pos, "expected %u type arguments, found %u", (unsigned)n, (unsigned)args.count);
        }
        return jog_check_error_type(ck);
    }
    solved = ct_arena_alloc(ck->program->arena, n * sizeof(*solved), _Alignof(JogType *));
    if (!solved) {
        jog_check_oom(ck);
        return NULL;
    }
    memset(solved, 0, n * sizeof(*solved));
    for (i = 0; i < n && !ck->stop; i++) {
        JogNode *arg = list_at(args, i);
        JogType *type = arg ? jog_check_resolve_type(ck, job, arg) : NULL;
        if (!arg) {
            jog_check_fail(ck, pos, "missing type");
            return jog_check_error_type(ck);
        }
        if (!type) {
            return NULL;
        }
        if (type->kind == JOG_TY_ERROR) {
            return type;
        }
        solved[i] = type;
    }
    if (ck->stop) {
        return NULL;
    }
    if (!sym->type || !sym->type_params) {
        jog_check_fail(ck, pos, "'%s' has no type", name);
        return jog_check_error_type(ck);
    }
    {
        JogType *constraint_err = jog_constraint_check_args(ck, job, sym, solved, n, pos);
        if (ck->stop) {
            return NULL;
        }
        if (constraint_err) {
            return constraint_err;
        }
    }
    inst = jog_generic_subst(ck->program->types, ck->program->arena, sym->type, sym->type_params, solved, n);
    if (!inst) {
        jog_check_oom(ck);
        return NULL;
    }
    return inst;
}
