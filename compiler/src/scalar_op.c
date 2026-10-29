#include "scalar_op.h"

#include <stdint.h>
#include <string.h>

static int parse_dec(const char *text, size_t len, uint64_t *out) {
    uint64_t value = 0;
    size_t i;
    if (!text || len == 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned digit;
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        digit = (unsigned)(text[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return 0;
        }
        value = value * 10 + digit;
    }
    *out = value;
    return 1;
}

static int is_byte_name(const JogNode *node) {
    const char *name;
    if (!node || node->kind != JOG_NODE_TYPE_NAME || node->as.type_name.is_const) {
        return 0;
    }
    if (node->as.type_name.path.count != 1 || !node->as.type_name.path.items) {
        return 0;
    }
    name = node->as.type_name.path.items[0];
    return name && strcmp(name, "byte") == 0;
}

static int ident_is(const JogNode *node, const char *name) {
    return node && node->kind == JOG_NODE_IDENT && node->as.ident.name && name && strcmp(node->as.ident.name, name) == 0;
}

static const JogNode *arg_value(const JogNode *arg) {
    if (!arg) {
        return NULL;
    }
    if (arg->kind == JOG_NODE_ARG) {
        return arg->as.arg.value;
    }
    return arg;
}

/* 1 when node is [N]byte or [ptr]byte. */
static int byte_array(const JogNode *node, int *is_ptr, uint64_t *len) {
    const JogNode *expr;
    if (!node || node->kind != JOG_NODE_TYPE_ARRAY || node->as.type_array.infer_len) {
        return 0;
    }
    if (!is_byte_name(node->as.type_array.elem)) {
        return 0;
    }
    expr = node->as.type_array.len;
    if (!expr) {
        return 0;
    }
    if (expr->kind == JOG_NODE_IDENT && expr->as.ident.name && strcmp(expr->as.ident.name, "ptr") == 0) {
        *is_ptr = 1;
        *len = 0;
        return 1;
    }
    if (expr->kind == JOG_NODE_INT_LIT) {
        *is_ptr = 0;
        return parse_dec(expr->as.literal.text, expr->as.literal.len, len);
    }
    return 0;
}

int jog_parse_intrinsic(const JogNode *attr, JogIntrinsicSpec *out) {
    size_t i;
    int positionals = 0;
    if (!out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (!attr || attr->kind != JOG_NODE_ATTRIBUTE || !attr->as.attribute.has_parens) {
        return 0;
    }
    if (!attr->as.attribute.name || strcmp(attr->as.attribute.name, "intrinsic") != 0) {
        return 0;
    }
    for (i = 0; i < attr->as.attribute.args.count; i++) {
        const JogNode *arg = attr->as.attribute.args.items ? attr->as.attribute.args.items[i] : NULL;
        const JogNode *value = arg_value(arg);
        if (!arg || arg->kind != JOG_NODE_ARG || !value) {
            return 0;
        }
        if (!arg->as.arg.name) {
            positionals++;
            if (positionals != 1) {
                return 0;
            }
            if (ident_is(value, "int")) {
                out->kind = JOG_INTRINSIC_INT;
            } else if (ident_is(value, "float")) {
                out->kind = JOG_INTRINSIC_FLOAT;
            } else if (ident_is(value, "bool")) {
                out->kind = JOG_INTRINSIC_BOOL;
            } else if (value->kind == JOG_NODE_IDENT && value->as.ident.name) {
                out->unknown = value->as.ident.name;
            } else {
                return 0;
            }
            continue;
        }
        if (strcmp(arg->as.arg.name, "signed") == 0) {
            if (out->has_signed || value->kind != JOG_NODE_BOOL_LIT) {
                return 0;
            }
            out->has_signed = 1;
            out->is_signed = value->as.literal.value ? 1 : 0;
            continue;
        }
        if (strcmp(arg->as.arg.name, "bits") == 0) {
            if (out->has_bits) {
                return 0;
            }
            out->has_bits = 1;
            if (ident_is(value, "ptr")) {
                out->bits_ptr = 1;
                continue;
            }
            if (value->kind == JOG_NODE_INT_LIT && parse_dec(value->as.literal.text, value->as.literal.len, &out->bits)) {
                continue;
            }
            return 0;
        }
        return 0;
    }
    return positionals == 1;
}

int jog_intrinsic_width_ok(const JogIntrinsicSpec *spec, uint32_t ptr_size, unsigned *bits, int *is_ptr) {
    if (bits) {
        *bits = 0;
    }
    if (is_ptr) {
        *is_ptr = 0;
    }
    if (!spec || spec->unknown || spec->kind == JOG_INTRINSIC_NONE) {
        return 0;
    }
    if (spec->kind == JOG_INTRINSIC_BOOL) {
        if (spec->has_signed || spec->has_bits) {
            return 0;
        }
        if (bits) {
            *bits = 8;
        }
        return 1;
    }
    if (spec->kind == JOG_INTRINSIC_FLOAT) {
        if (spec->has_signed || !spec->has_bits || spec->bits_ptr) {
            return 0;
        }
        if (spec->bits != 32 && spec->bits != 64) {
            return -1;
        }
        if (bits) {
            *bits = (unsigned)spec->bits;
        }
        return 1;
    }
    if (spec->kind != JOG_INTRINSIC_INT || !spec->has_signed || !spec->has_bits) {
        return 0;
    }
    if (spec->bits_ptr) {
        if (ptr_size != 4 && ptr_size != 8) {
            return -1;
        }
        if (is_ptr) {
            *is_ptr = 1;
        }
        if (bits) {
            *bits = ptr_size * 8u;
        }
        return 1;
    }
    if (spec->bits != 8 && spec->bits != 16 && spec->bits != 32 && spec->bits != 64) {
        return -1;
    }
    if (bits) {
        *bits = (unsigned)spec->bits;
    }
    return 1;
}

int jog_intrinsic_base_ok(const JogNode *target, unsigned bits, int is_ptr) {
    int got_ptr = 0;
    uint64_t len = 0;
    if (bits == 0 || bits % 8 != 0) {
        return 0;
    }
    if (is_ptr) {
        return byte_array(target, &got_ptr, &len) && got_ptr;
    }
    if (bits == 8) {
        return is_byte_name(target);
    }
    if (!byte_array(target, &got_ptr, &len) || got_ptr) {
        return 0;
    }
    return len == (uint64_t)bits / 8u;
}

const char *jog_op_method_name(JogTokenKind op) {
    switch (op) {
    case JOG_TOK_PLUS:
        return "op+";
    case JOG_TOK_MINUS:
        return "op-";
    case JOG_TOK_STAR:
        return "op*";
    case JOG_TOK_SLASH:
        return "op/";
    case JOG_TOK_PERCENT:
        return "op%";
    case JOG_TOK_AMP:
        return "op&";
    case JOG_TOK_PIPE:
        return "op|";
    case JOG_TOK_CARET:
        return "op^";
    case JOG_TOK_SHL:
        return "op<<";
    case JOG_TOK_SHR:
        return "op>>";
    case JOG_TOK_TILDE:
        return "op~";
    case JOG_TOK_BANG:
        return "op!";
    case JOG_TOK_EQ:
        return "op==";
    case JOG_TOK_NEQ:
        return "op!=";
    case JOG_TOK_LT:
        return "op<";
    case JOG_TOK_LE:
        return "op<=";
    case JOG_TOK_GT:
        return "op>";
    case JOG_TOK_GE:
        return "op>=";
    default:
        return NULL;
    }
}

const char *jog_op_intrinsic_name(JogTokenKind op, int unary) {
    switch (op) {
    case JOG_TOK_PLUS:
        return unary ? NULL : "add";
    case JOG_TOK_MINUS:
        return unary ? "neg" : "sub";
    case JOG_TOK_STAR:
        return unary ? NULL : "mul";
    case JOG_TOK_SLASH:
        return unary ? NULL : "div";
    case JOG_TOK_PERCENT:
        return unary ? NULL : "rem";
    case JOG_TOK_AMP:
        return unary ? NULL : "and";
    case JOG_TOK_PIPE:
        return unary ? NULL : "or";
    case JOG_TOK_CARET:
        return unary ? NULL : "xor";
    case JOG_TOK_SHL:
        return unary ? NULL : "shl";
    case JOG_TOK_SHR:
        return unary ? NULL : "shr";
    case JOG_TOK_TILDE:
        return unary ? "bitnot" : NULL;
    case JOG_TOK_BANG:
        return unary ? "not" : NULL;
    case JOG_TOK_EQ:
        return unary ? NULL : "eq";
    case JOG_TOK_NEQ:
        return unary ? NULL : "ne";
    case JOG_TOK_LT:
        return unary ? NULL : "lt";
    case JOG_TOK_LE:
        return unary ? NULL : "le";
    case JOG_TOK_GT:
        return unary ? NULL : "gt";
    case JOG_TOK_GE:
        return unary ? NULL : "ge";
    default:
        return NULL;
    }
}

static int name_is(const char *name, const char *expect) {
    return name && expect && strcmp(name, expect) == 0;
}

int jog_intrinsic_returns_bool(const char *intrinsic) {
    return name_is(intrinsic, "eq") || name_is(intrinsic, "ne") || name_is(intrinsic, "lt") || name_is(intrinsic, "le") ||
           name_is(intrinsic, "gt") || name_is(intrinsic, "ge") || name_is(intrinsic, "not");
}

int jog_intrinsic_allowed(int kind, const char *intrinsic) {
    int arith = name_is(intrinsic, "add") || name_is(intrinsic, "sub") || name_is(intrinsic, "mul") ||
                name_is(intrinsic, "div") || name_is(intrinsic, "rem") || name_is(intrinsic, "neg");
    int order = name_is(intrinsic, "eq") || name_is(intrinsic, "ne") || name_is(intrinsic, "lt") || name_is(intrinsic, "le") ||
                name_is(intrinsic, "gt") || name_is(intrinsic, "ge");
    int bits = name_is(intrinsic, "and") || name_is(intrinsic, "or") || name_is(intrinsic, "xor") ||
               name_is(intrinsic, "shl") || name_is(intrinsic, "shr") || name_is(intrinsic, "bitnot");
    if (kind == JOG_INTRINSIC_BOOL) {
        return name_is(intrinsic, "eq") || name_is(intrinsic, "ne") || name_is(intrinsic, "not");
    }
    if (kind == JOG_INTRINSIC_FLOAT) {
        return arith || order;
    }
    if (kind == JOG_INTRINSIC_INT) {
        return arith || order || bits;
    }
    return 0;
}

int jog_is_op_intrinsic_name(const char *name) {
    return jog_intrinsic_allowed(JOG_INTRINSIC_INT, name) || name_is(name, "not");
}

static const JogNode *list_node(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

int jog_fn_intrinsic_body(const JogNode *fn, const char **intrinsic_out) {
    const char *expect;
    const char *param_name = NULL;
    const JogNode *body;
    const JogNode *param;
    const JogNode *a0;
    const JogNode *a1;
    int unary;
    if (intrinsic_out) {
        *intrinsic_out = NULL;
    }
    if (!fn || fn->kind != JOG_NODE_FN || !fn->as.fn.expr_body || fn->as.fn.receiver_is_ptr || !fn->as.fn.receiver) {
        return 0;
    }
    if (fn->as.fn.params.count > 1) {
        return 0;
    }
    unary = fn->as.fn.params.count == 0;
    expect = jog_op_intrinsic_name(fn->as.fn.op, unary);
    if (!expect) {
        return 0;
    }
    body = fn->as.fn.body;
    if (!body || body->kind != JOG_NODE_BUILTIN || !body->as.builtin.name || strcmp(body->as.builtin.name, expect) != 0) {
        return 0;
    }
    if (!unary) {
        param = list_node(fn->as.fn.params, 0);
        if (!param || param->kind != JOG_NODE_PARAM || !param->as.param.name) {
            return 0;
        }
        param_name = param->as.param.name;
        if (body->as.builtin.args.count != 2) {
            return 0;
        }
        a0 = arg_value(list_node(body->as.builtin.args, 0));
        a1 = arg_value(list_node(body->as.builtin.args, 1));
        if (!ident_is(a0, "self") || !ident_is(a1, param_name)) {
            return 0;
        }
    } else {
        if (body->as.builtin.args.count != 1) {
            return 0;
        }
        a0 = arg_value(list_node(body->as.builtin.args, 0));
        if (!ident_is(a0, "self")) {
            return 0;
        }
    }
    if (intrinsic_out) {
        *intrinsic_out = expect;
    }
    return 1;
}

static const JogNode *intrinsic_attr(const JogNode *decl) {
    size_t i;
    const JogNode *found = NULL;
    if (!decl) {
        return NULL;
    }
    for (i = 0; i < decl->attrs.count; i++) {
        const JogNode *attr = decl->attrs.items ? decl->attrs.items[i] : NULL;
        if (!attr || attr->kind != JOG_NODE_ATTRIBUTE || !attr->as.attribute.name ||
            strcmp(attr->as.attribute.name, "intrinsic") != 0) {
            continue;
        }
        if (found) {
            return NULL;
        }
        found = attr;
    }
    return found;
}

int jog_type_apply_intrinsic(JogTypes *types, JogType *type, const JogNode *decl) {
    const JogNode *attr;
    JogIntrinsicSpec spec;
    unsigned bits = 0;
    int is_ptr = 0;
    int width;
    if (!types || !type || !decl || decl->kind != JOG_NODE_TYPE_DECL || !decl->as.type_decl.distinct) {
        return 0;
    }
    attr = intrinsic_attr(decl);
    if (!attr || !jog_parse_intrinsic(attr, &spec)) {
        return 0;
    }
    width = jog_intrinsic_width_ok(&spec, jog_types_ptr_size(types), &bits, &is_ptr);
    if (width != 1 || !jog_intrinsic_base_ok(decl->as.type_decl.target, bits, is_ptr)) {
        return 0;
    }
    jog_type_set_intrinsic(type, spec.kind, bits, spec.is_signed, is_ptr);
    return 1;
}
