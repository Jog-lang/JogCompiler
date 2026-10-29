#include "check_memory.h"

#include "check_generic.h"
#include "str.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Places are shallow. A val binding and a *const / []const / const bytes
   view are const. A var []T still has mutable elements. A const struct
   pointer does not make a pointer field's target const. */

typedef enum Place {
    PLACE_NO = 0,
    PLACE_MUT,
    PLACE_CONST
} Place;

static bool is_error(const JogType *type) {
    return type && type->kind == JOG_TY_ERROR;
}

static bool is_int(const JogType *type) {
    return type && type->kind == JOG_TY_INT;
}

/* A type parameter has no number in the template. It stands for a concrete
   type, so *T can be indexed inside a generic. An open Slot<K, V> is the
   same: the instance is laid out after substitution. *void still has no
   size. */
static bool has_size(const JogType *type) {
    if (!type) {
        return false;
    }
    if (type->kind == JOG_TY_TYPE_PARAM || jog_generic_has_param(type)) {
        return true;
    }
    return type->has_layout && type->size > 0;
}

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static void type_text(Check *ck, const JogType *type, char *buf, size_t cap) {
    CtSb sb;
    const char *text;
    (void)ck;
    if (cap == 0) {
        return;
    }
    buf[0] = '\0';
    if (!type) {
        snprintf(buf, cap, "<null type>");
        return;
    }
    ct_sb_init(&sb);
    jog_type_print(type, &sb);
    text = ct_sb_cstr(&sb);
    snprintf(buf, cap, "%s", text ? text : "?");
    ct_sb_free(&sb);
}

static JogType *poison(Check *ck, JogNode *node) {
    JogType *error = jog_check_error_type(ck);
    if (node && error) {
        node->type = error;
    }
    return error;
}

static JogType *builtin(Check *ck, JogBuiltin which) {
    JogType *type = jog_type_builtin(ck->program->types, which);
    if (!type) {
        jog_check_oom(ck);
    }
    return type;
}

static JogType *ptr_type(Check *ck, JogType *elem, bool is_const) {
    JogType *type;
    if (!elem) {
        return NULL;
    }
    type = jog_type_ptr(ck->program->types, elem, is_const);
    if (!type) {
        jog_check_oom(ck);
    }
    return type;
}

static JogType *finish(JogNode *node, JogType *type) {
    if (node && type) {
        node->type = type;
    }
    return type;
}

static Place place_of(const JogNode *node) {
    JogType *base_ty;
    if (!node) {
        return PLACE_NO;
    }
    if (node->kind == JOG_NODE_IDENT && node->symbol) {
        if (node->symbol->kind == JOG_SYM_VARIABLE || node->symbol->kind == JOG_SYM_PARAMETER) {
            return PLACE_MUT;
        }
        if (node->symbol->kind == JOG_SYM_CONSTANT) {
            return PLACE_CONST;
        }
        return PLACE_NO;
    }
    if (node->kind == JOG_NODE_MEMBER && node->symbol && node->symbol->kind == JOG_SYM_FIELD) {
        base_ty = node->as.member.base ? node->as.member.base->type : NULL;
        if (base_ty && base_ty->kind == JOG_TY_PTR) {
            return base_ty->is_const ? PLACE_CONST : PLACE_MUT;
        }
        return place_of(node->as.member.base);
    }
    if (node->kind == JOG_NODE_INDEX) {
        base_ty = node->as.index.base ? node->as.index.base->type : NULL;
        if (!base_ty) {
            return PLACE_NO;
        }
        if (base_ty->kind == JOG_TY_PTR || base_ty->kind == JOG_TY_SLICE || base_ty->kind == JOG_TY_BYTES) {
            return base_ty->is_const ? PLACE_CONST : PLACE_MUT;
        }
        if (base_ty->kind == JOG_TY_ARRAY || base_ty->kind == JOG_TY_BYTES_N) {
            return place_of(node->as.index.base);
        }
        return PLACE_NO;
    }
    if (node->kind == JOG_NODE_UNARY && node->as.unary.op == JOG_TOK_STAR) {
        base_ty = node->as.unary.operand ? node->as.unary.operand->type : NULL;
        if (base_ty && base_ty->kind == JOG_TY_PTR) {
            return base_ty->is_const ? PLACE_CONST : PLACE_MUT;
        }
    }
    return PLACE_NO;
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

static bool eq_op(JogTokenKind op) {
    return op == JOG_TOK_EQ || op == JOG_TOK_NEQ;
}

static bool is_error_option_type(const JogType *type) {
    const JogType *elem;
    if (!type || type->kind != JOG_TY_OPTION) {
        return false;
    }
    elem = type->as.elem.elem;
    while (elem && elem->kind == JOG_TY_DISTINCT) {
        elem = elem->as.nominal.base;
    }
    return elem && elem->kind == JOG_TY_INTERFACE && elem->as.nominal.name && strcmp(elem->as.nominal.name, "error") == 0;
}

static bool rel_op(JogTokenKind op) {
    return op == JOG_TOK_LT || op == JOG_TOK_LE || op == JOG_TOK_GT || op == JOG_TOK_GE;
}

static void need_size(Check *ck, JogPos pos) {
    jog_check_fail(ck, pos, "pointer arithmetic needs a size");
}

int jog_check_memory_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call) {
    JogNode *callee;
    JogSymbol *symbol;
    JogType *type;
    (void)job;
    (void)scope;
    if (!ck || !call || call->kind != JOG_NODE_CALL || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    callee = call->as.call.callee;
    if (!callee || callee->kind != JOG_NODE_IDENT || !callee->symbol) {
        return 0;
    }
    symbol = callee->symbol;
    type = symbol->type ? symbol->type : callee->type;
    if (symbol->kind != JOG_SYM_TYPE || !type || type->kind != JOG_TY_BYTES || type->is_const) {
        return 0;
    }
    if (call->as.call.trailing || call->as.call.contexts.count > 0) {
        return 0;
    }
    if (call->as.call.args.count > 0) {
        size_t i;
        jog_check_fail(ck, call->pos, "bytes() does not take arguments");
        for (i = 0; i < call->as.call.args.count && !ck->stop; i++) {
            JogNode *arg = list_at(call->as.call.args, i);
            if (arg && arg->kind == JOG_NODE_ARG && arg->as.arg.value) {
                (void)jog_check_expr(ck, job, scope, arg->as.arg.value, NULL);
            }
        }
        if (ck->stop) {
            return -1;
        }
        call->type = poison(ck, call);
        return 1;
    }
    call->type = type;
    return ck->stop ? -1 : 1;
}

JogType *jog_check_memory_unary(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node) {
    JogNode *operand;
    JogType *op_ty;
    JogTokenKind op;
    Place place;
    if (!ck || !node || ck->stop) {
        return NULL;
    }
    op = node->as.unary.op;
    operand = node->as.unary.operand;
    op_ty = operand ? jog_check_expr(ck, job, scope, operand, NULL) : NULL;
    if (!op_ty || ck->stop) {
        return op_ty;
    }
    if (is_error(op_ty)) {
        return finish(node, op_ty);
    }
    if (op == JOG_TOK_STAR) {
        if (op_ty->kind != JOG_TY_PTR || !op_ty->as.elem.elem) {
            char buf[160];
            type_text(ck, op_ty, buf, sizeof(buf));
            jog_check_fail(ck, node->pos, "operator '%s' does not apply to %s", jog_token_spelling(op), buf);
            return poison(ck, node);
        }
        return finish(node, op_ty->as.elem.elem);
    }
    if (op != JOG_TOK_AMP) {
        jog_check_fail(ck, node->pos, "operator '%s' is not checked yet", jog_token_spelling(op));
        return poison(ck, node);
    }
    place = place_of(operand);
    if (place == PLACE_NO) {
        jog_check_fail(ck, node->pos, "cannot take the address of this value");
        return poison(ck, node);
    }
    return finish(node, ptr_type(ck, op_ty, place == PLACE_CONST));
}

static JogType *check_bound(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *bound) {
    JogType *usize;
    JogType *got;
    if (!bound) {
        return NULL;
    }
    usize = builtin(ck, JOG_BUILTIN_USIZE);
    if (!usize) {
        return NULL;
    }
    got = jog_check_expr(ck, job, scope, bound, usize);
    if (!got || ck->stop) {
        return NULL;
    }
    return got;
}

JogType *jog_check_memory_index(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node) {
    JogNode *base_node;
    JogType *base;
    JogType *elem = NULL;
    JogType *index_ty;
    JogType *usize = NULL;
    bool pointer = false;
    char buf[160];
    if (!ck || !node || ck->stop) {
        return NULL;
    }
    base_node = node->as.index.base;
    base = base_node ? jog_check_expr(ck, job, scope, base_node, NULL) : NULL;
    if (!base || ck->stop) {
        return base;
    }
    if (is_error(base)) {
        if (node->as.index.index) {
            (void)jog_check_expr(ck, job, scope, node->as.index.index, NULL);
        }
        return finish(node, base);
    }
    if (base->kind == JOG_TY_DISTINCT) {
        JogType *view = jog_type_bytes_view(base);
        if (view) {
            base = view;
        }
    }
    if (base->kind == JOG_TY_PTR) {
        pointer = true;
        elem = base->as.elem.elem;
        if (!has_size(elem)) {
            need_size(ck, node->pos);
            if (node->as.index.index) {
                (void)jog_check_expr(ck, job, scope, node->as.index.index, NULL);
            }
            return poison(ck, node);
        }
    } else if (base->kind == JOG_TY_SLICE || base->kind == JOG_TY_ARRAY) {
        elem = base->kind == JOG_TY_SLICE ? base->as.elem.elem : base->as.array.elem;
        usize = builtin(ck, JOG_BUILTIN_USIZE);
    } else if (base->kind == JOG_TY_BYTES || base->kind == JOG_TY_BYTES_N) {
        elem = jog_type_bytes_n(ck->program->types, 1);
        if (!elem) {
            jog_check_oom(ck);
            return NULL;
        }
        usize = builtin(ck, JOG_BUILTIN_USIZE);
    } else {
        type_text(ck, base, buf, sizeof(buf));
        jog_check_fail(ck, node->pos, "cannot index %s", buf);
        if (node->as.index.index) {
            (void)jog_check_expr(ck, job, scope, node->as.index.index, NULL);
        }
        return poison(ck, node);
    }
    if (!elem || !node->as.index.index) {
        jog_check_fail(ck, node->pos, "missing index");
        return poison(ck, node);
    }
    if (pointer) {
        index_ty = jog_check_expr(ck, job, scope, node->as.index.index, NULL);
        if (!index_ty || ck->stop) {
            return NULL;
        }
        if (!is_error(index_ty) && !is_int(index_ty)) {
            type_text(ck, index_ty, buf, sizeof(buf));
            jog_check_fail(ck, node->as.index.index->pos, "pointer offset must be an integer, got %s", buf);
            return poison(ck, node);
        }
    } else {
        if (!usize) {
            return NULL;
        }
        index_ty = jog_check_expr(ck, job, scope, node->as.index.index, usize);
        if (!index_ty || ck->stop) {
            return NULL;
        }
    }
    if (is_error(index_ty)) {
        return finish(node, index_ty);
    }
    return finish(node, elem);
}

JogType *jog_check_memory_slice(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node) {
    JogNode *base_node;
    JogType *base;
    JogType *elem;
    JogType *result;
    Place place;
    bool view_const;
    int slice_is_str = 0;
    char buf[160];
    if (!ck || !node || ck->stop) {
        return NULL;
    }
    base_node = node->as.slice.base;
    base = base_node ? jog_check_expr(ck, job, scope, base_node, NULL) : NULL;
    if (!base || ck->stop) {
        return base;
    }
    if (is_error(base)) {
        return finish(node, base);
    }
    slice_is_str = jog_type_is_core_str(ck->program->types, base);
    {
        JogType *view = jog_type_bytes_view(base);
        if (view && base->kind == JOG_TY_DISTINCT) {
            base = view;
        }
    }
    if (base->kind == JOG_TY_PTR ||
        (base->kind != JOG_TY_SLICE && base->kind != JOG_TY_ARRAY && base->kind != JOG_TY_BYTES &&
         base->kind != JOG_TY_BYTES_N)) {
        type_text(ck, base, buf, sizeof(buf));
        jog_check_fail(ck, node->pos, "cannot slice %s", buf);
        return poison(ck, node);
    }
    if (check_bound(ck, job, scope, node->as.slice.lo) == NULL && node->as.slice.lo) {
        return ck->stop ? NULL : poison(ck, node);
    }
    if (ck->stop) {
        return NULL;
    }
    if (check_bound(ck, job, scope, node->as.slice.hi) == NULL && node->as.slice.hi) {
        return ck->stop ? NULL : poison(ck, node);
    }
    if (ck->stop) {
        return NULL;
    }
    if (base->kind == JOG_TY_SLICE) {
        result = base;
    } else if (base->kind == JOG_TY_BYTES) {
        result = slice_is_str ? jog_type_core_str(ck->program->types) : base;
        if (!result) {
            jog_check_oom(ck);
            return NULL;
        }
    } else if (base->kind == JOG_TY_BYTES_N) {
        place = place_of(base_node);
        view_const = place == PLACE_CONST;
        result = builtin(ck, view_const ? JOG_BUILTIN_CONST_BYTES : JOG_BUILTIN_BYTES);
    } else {
        elem = base->as.array.elem;
        place = place_of(base_node);
        view_const = place == PLACE_CONST;
        result = jog_type_slice(ck->program->types, elem, view_const);
        if (!result) {
            jog_check_oom(ck);
            return NULL;
        }
    }
    return finish(node, result);
}

static JogType *lit_elem_type(Check *ck, CheckJob *job, JogNode *type_node, JogType **result, bool *is_slice, uint64_t *len,
                              size_t count) {
    JogType *resolved;
    JogType *elem;
    *is_slice = false;
    *len = 0;
    *result = NULL;
    if (!type_node) {
        jog_check_fail(ck, (JogPos){0}, "array literal is missing a type");
        return NULL;
    }
    if (type_node->kind == JOG_NODE_TYPE_ARRAY && type_node->as.type_array.infer_len) {
        elem = type_node->as.type_array.elem ? jog_check_resolve_type(ck, job, type_node->as.type_array.elem) : NULL;
        if (!elem || ck->stop) {
            return NULL;
        }
        if (is_error(elem)) {
            return elem;
        }
        *len = (uint64_t)count;
        *result = jog_type_array(ck->program->types, elem, *len);
        if (!*result) {
            jog_check_oom(ck);
            return NULL;
        }
        type_node->type = *result;
        return elem;
    }
    resolved = jog_check_resolve_type(ck, job, type_node);
    if (!resolved || ck->stop) {
        return NULL;
    }
    if (is_error(resolved)) {
        return resolved;
    }
    if (resolved->kind == JOG_TY_BYTES) {
        JogType *byte = jog_type_bytes_n(ck->program->types, 1);
        if (!byte) {
            jog_check_oom(ck);
            return NULL;
        }
        *is_slice = true;
        *result = resolved;
        return byte;
    }
    if (resolved->kind == JOG_TY_BYTES_N) {
        JogType *byte = jog_type_bytes_n(ck->program->types, 1);
        if (!byte) {
            jog_check_oom(ck);
            return NULL;
        }
        *len = resolved->as.bytes_n.n;
        *result = resolved;
        return byte;
    }
    if (resolved->kind == JOG_TY_SLICE) {
        *is_slice = true;
        *result = resolved;
        return resolved->as.elem.elem;
    }
    if (resolved->kind == JOG_TY_ARRAY) {
        *len = resolved->as.array.len;
        *result = resolved;
        return resolved->as.array.elem;
    }
    jog_check_fail(ck, type_node->pos, "array literal type must be an array or a slice");
    return jog_check_error_type(ck);
}

JogType *jog_check_memory_array_lit(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node) {
    JogNode *type_node;
    JogType *elem;
    JogType *result = NULL;
    bool is_slice = false;
    bool named = false;
    bool lifetime = false;
    uint64_t len = 0;
    size_t i;
    size_t count;
    if (!ck || !node || ck->stop) {
        return NULL;
    }
    count = node->as.array_lit.elems.count;
    for (i = 0; i < count; i++) {
        JogNode *arg = list_at(node->as.array_lit.elems, i);
        if (arg && arg->kind == JOG_NODE_ARG && arg->as.arg.name) {
            named = true;
            jog_check_fail(ck, arg->pos, "array literals do not take named arguments");
        }
    }
    type_node = node->as.array_lit.type;
    elem = lit_elem_type(ck, job, type_node, &result, &is_slice, &len, count);
    if (!elem || ck->stop) {
        return elem && is_error(elem) ? finish(node, elem) : NULL;
    }
    if (is_error(elem)) {
        return finish(node, elem);
    }
    if (is_slice && !scope) {
        jog_check_fail(ck, node->pos, "slice literal in a global initializer does not outlive initialization");
        lifetime = true;
    }
    if (!is_slice && result && (result->kind == JOG_TY_ARRAY || result->kind == JOG_TY_BYTES_N)) {
        if (count == 0) {
            if (!jog_check_type_has_zero(result)) {
                jog_check_fail(ck, node->pos, "array literal has no zero value");
                lifetime = true;
            }
        } else if ((uint64_t)count != len) {
            jog_check_fail(ck, node->pos, "array literal length mismatch: expected %" PRIu64 ", got %" PRIu64, len,
                           (uint64_t)count);
            lifetime = true;
        }
    }
    for (i = 0; i < count && !ck->stop; i++) {
        JogNode *arg = list_at(node->as.array_lit.elems, i);
        JogNode *value;
        JogType *got;
        if (!arg || arg->kind != JOG_NODE_ARG) {
            continue;
        }
        value = arg->as.arg.value;
        if (!value || named) {
            if (value && named) {
                (void)jog_check_expr(ck, job, scope, value, NULL);
            }
            continue;
        }
        got = jog_check_expr(ck, job, scope, value, elem);
        if (!got || ck->stop) {
            return NULL;
        }
    }
    if (ck->stop) {
        return NULL;
    }
    if (lifetime || named || !result) {
        return poison(ck, node);
    }
    return finish(node, result);
}

int jog_check_memory_binary(Check *ck, JogNode *node, JogType *left, JogType *right, JogType **out) {
    bool lp;
    bool rp;
    JogTokenKind op;
    JogType *boolean;
    JogType *ptr;
    JogType *other;
    char a[160];
    char b[160];
    if (!out) {
        return -1;
    }
    *out = NULL;
    if (!ck || !node || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    lp = left && left->kind == JOG_TY_PTR;
    rp = right && right->kind == JOG_TY_PTR;
    if (!lp && !rp) {
        return 0;
    }
    op = node->as.binary.op;
    if (lp && rp) {
        if (left != right) {
            type_text(ck, left, a, sizeof(a));
            type_text(ck, right, b, sizeof(b));
            jog_check_fail(ck, node->pos, "mismatched types %s and %s", a, b);
            *out = poison(ck, node);
            return ck->stop ? -1 : 1;
        }
        if (eq_op(op) || rel_op(op)) {
            boolean = builtin(ck, JOG_BUILTIN_BOOL);
            if (!boolean) {
                return -1;
            }
            *out = finish(node, boolean);
            return 1;
        }
        if (op == JOG_TOK_MINUS) {
            if (!has_size(left->as.elem.elem)) {
                need_size(ck, node->pos);
                *out = poison(ck, node);
                return ck->stop ? -1 : 1;
            }
            *out = finish(node, builtin(ck, JOG_BUILTIN_ISIZE));
            return *out ? 1 : -1;
        }
        type_text(ck, left, a, sizeof(a));
        jog_check_fail(ck, node->pos, "operator '%s' does not apply to %s", jog_token_spelling(op), a);
        *out = poison(ck, node);
        return ck->stop ? -1 : 1;
    }
    ptr = lp ? left : right;
    other = lp ? right : left;
    /* error? == a sentinel pointer is an identity compare, not arithmetic. */
    if (eq_op(op) && ((lp && is_error_option_type(right)) || (rp && is_error_option_type(left)))) {
        return 0;
    }
    if ((op == JOG_TOK_PLUS || (op == JOG_TOK_MINUS && lp)) && is_int(other)) {
        if (!has_size(ptr->as.elem.elem)) {
            need_size(ck, node->pos);
            *out = poison(ck, node);
            return ck->stop ? -1 : 1;
        }
        *out = finish(node, ptr);
        return 1;
    }
    type_text(ck, ptr, a, sizeof(a));
    jog_check_fail(ck, node->pos, "operator '%s' does not apply to %s", jog_token_spelling(op), a);
    *out = poison(ck, node);
    return ck->stop ? -1 : 1;
}

int jog_check_memory_ptr_compound(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *rhs, JogType *ptr_ty) {
    JogType *got;
    char buf[160];
    if (!ck || ck->stop) {
        return -1;
    }
    got = rhs ? jog_check_expr(ck, job, scope, rhs, NULL) : NULL;
    if (!got || ck->stop) {
        return -1;
    }
    if (!is_error(got) && !is_int(got)) {
        type_text(ck, got, buf, sizeof(buf));
        jog_check_fail(ck, rhs ? rhs->pos : (JogPos){0}, "pointer offset must be an integer, got %s", buf);
    }
    if (ptr_ty && ptr_ty->kind == JOG_TY_PTR && !has_size(ptr_ty->as.elem.elem)) {
        need_size(ck, rhs ? rhs->pos : (JogPos){0});
    }
    return ck->stop ? -1 : 1;
}

void jog_check_memory_assign(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *assign) {
    JogNode *target;
    JogType *elem = NULL;
    JogType *unit;
    JogTokenKind base;
    Place place;
    if (!ck || !assign || ck->stop) {
        return;
    }
    target = assign->as.assign.target;
    unit = jog_check_unit_type(ck);
    if (target && target->kind == JOG_NODE_INDEX) {
        elem = jog_check_memory_index(ck, job, scope, target);
    } else if (target && target->kind == JOG_NODE_UNARY && target->as.unary.op == JOG_TOK_STAR) {
        elem = jog_check_memory_unary(ck, job, scope, target);
    } else {
        jog_check_fail(ck, assign->pos, "assignment target is not checked yet");
        if (assign->as.assign.value) {
            (void)jog_check_expr(ck, job, scope, assign->as.assign.value, NULL);
        }
        assign->type = unit;
        return;
    }
    if (!elem || ck->stop) {
        return;
    }
    if (!is_error(elem)) {
        place = place_of(target);
        if (place == PLACE_NO) {
            jog_check_fail(ck, target->pos, "cannot assign to this value");
        } else if (place == PLACE_CONST) {
            jog_check_fail(ck, target->pos, "cannot assign through a const view");
        }
    }
    base = compound_base(assign->as.assign.op);
    if (base != JOG_TOK_EOF) {
        if (!is_error(elem) && elem->kind == JOG_TY_PTR && (base == JOG_TOK_PLUS || base == JOG_TOK_MINUS)) {
            if (jog_check_memory_ptr_compound(ck, job, scope, assign->as.assign.value, elem) < 0) {
                return;
            }
        } else if (!is_error(elem) && is_int(elem)) {
            (void)jog_check_expr(ck, job, scope, assign->as.assign.value, elem);
        } else if (!is_error(elem)) {
            char buf[160];
            type_text(ck, elem, buf, sizeof(buf));
            jog_check_fail(ck, assign->pos, "operator '%s' does not apply to %s", jog_token_spelling(base), buf);
            if (assign->as.assign.value) {
                (void)jog_check_expr(ck, job, scope, assign->as.assign.value, NULL);
            }
        } else if (assign->as.assign.value) {
            (void)jog_check_expr(ck, job, scope, assign->as.assign.value, NULL);
        }
    } else if (assign->as.assign.op == JOG_TOK_ASSIGN) {
        (void)jog_check_expr(ck, job, scope, assign->as.assign.value, is_error(elem) ? NULL : elem);
    } else {
        jog_check_fail(ck, assign->pos, "operator '%s' is not checked yet", jog_token_spelling(assign->as.assign.op));
        return;
    }
    if (!ck->stop) {
        assign->type = unit;
    }
}

int jog_check_memory_attr(Check *ck, JogNode *node) {
    size_t i;
    if (!ck || !node || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    for (i = 0; i < node->attrs.count; i++) {
        JogNode *attr = list_at(node->attrs, i);
        const char *name;
        if (!attr || attr->kind != JOG_NODE_ATTRIBUTE) {
            continue;
        }
        name = attr->as.attribute.name;
        if (!name || strcmp(name, "unchecked") != 0) {
            jog_check_fail(ck, attr->pos.line ? attr->pos : node->pos, "attribute '%s' is not checked yet",
                           name ? name : "?");
        }
    }
    return ck->stop ? -1 : 1;
}
