#include "prelude.h"

#include "lexer.h"
#include "phases.h"
#include "resolve_universe.h"
#include "scalar_op.h"

#include "file.h"
#include "fs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The core prelude is not a package. scalar.jog is parsed here and its
   names are bound onto the builtins the backend already uses. A distinct
   u32 over [4]byte would not be the integer those builtins are. */

enum {
    PRE_INT = 0,
    PRE_FLOAT,
    PRE_BOOL,
    PRE_BYTES,
    PRE_STR,
    PRE_RUNE
};

static const struct {
    const char *name;
    JogBuiltin which;
    int form;
} k_scalar[] = {
    {"i8", JOG_BUILTIN_I8, PRE_INT},
    {"i16", JOG_BUILTIN_I16, PRE_INT},
    {"i32", JOG_BUILTIN_I32, PRE_INT},
    {"i64", JOG_BUILTIN_I64, PRE_INT},
    {"u8", JOG_BUILTIN_U8, PRE_INT},
    {"u16", JOG_BUILTIN_U16, PRE_INT},
    {"u32", JOG_BUILTIN_U32, PRE_INT},
    {"u64", JOG_BUILTIN_U64, PRE_INT},
    {"isize", JOG_BUILTIN_ISIZE, PRE_INT},
    {"usize", JOG_BUILTIN_USIZE, PRE_INT},
    {"f32", JOG_BUILTIN_F32, PRE_FLOAT},
    {"f64", JOG_BUILTIN_F64, PRE_FLOAT},
    {"bool", JOG_BUILTIN_BOOL, PRE_BOOL},
    {"bytes", JOG_BUILTIN_BYTES, PRE_BYTES},
    {"str", JOG_BUILTIN_CONST_BYTES, PRE_STR},
    {"rune", JOG_BUILTIN_U32, PRE_RUNE},
};

typedef struct Prelude {
    CtArena *arena;
    CtDiag *diag;
    CtIntern *intern;
    JogTypes *types;
    JogPackageScope *scope;
    int failed;
} Prelude;

static CtPos cpos(JogPos pos) {
    CtPos out;
    out.file = pos.file;
    out.line = pos.line;
    out.column = pos.column;
    return out;
}

static void pre_fail(Prelude *p, JogPos pos, const char *msg) {
    ct_diag_emit(p->diag, CT_SEVERITY_ERROR, cpos(pos), "%s", msg ? msg : "error");
    p->failed = 1;
}

static void pre_fail_name(Prelude *p, JogPos pos, const char *fmt, const char *name) {
    char buf[256];
    snprintf(buf, sizeof(buf), fmt, name && name[0] != '\0' ? name : "?");
    pre_fail(p, pos, buf);
}

static int scalar_index(const char *name) {
    size_t i;
    if (!name) {
        return -1;
    }
    for (i = 0; i < sizeof(k_scalar) / sizeof(k_scalar[0]); i++) {
        if (strcmp(k_scalar[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

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

/* 1 when node is [N]byte or [ptr]byte. *is_ptr is set for the ptr spelling.
   A numeric length is written to *len. ptr is not a general constant. */
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

static int base_matches(const JogNode *target, int pointer_sized, uint64_t bit_width, uint32_t ptr_size) {
    int is_ptr = 0;
    uint64_t len = 0;
    uint64_t bytes;
    if (bit_width == 0 || bit_width % 8 != 0) {
        return 0;
    }
    bytes = bit_width / 8;
    if (pointer_sized) {
        if (!byte_array(target, &is_ptr, &len) || !is_ptr) {
            return 0;
        }
        return bytes == ptr_size;
    }
    if (bytes == 1) {
        return is_byte_name(target);
    }
    if (!byte_array(target, &is_ptr, &len) || is_ptr) {
        return 0;
    }
    return len == bytes;
}

typedef struct Intrinsic {
    int kind; /* PRE_INT, PRE_FLOAT, PRE_BOOL, or -1 */
    int has_signed;
    int is_signed;
    int has_bits;
    int bits_ptr;
    uint64_t bits;
} Intrinsic;

static const JogNode *arg_value(const JogNode *arg) {
    if (!arg || arg->kind != JOG_NODE_ARG) {
        return NULL;
    }
    return arg->as.arg.value;
}

static int ident_is(const JogNode *node, const char *name) {
    return node && node->kind == JOG_NODE_IDENT && node->as.ident.name && strcmp(node->as.ident.name, name) == 0;
}

/* 1 when the attribute is @intrinsic(...). 0 when the shape is wrong. */
static int read_intrinsic(const JogNode *attr, Intrinsic *out) {
    size_t i;
    int positionals = 0;
    memset(out, 0, sizeof(*out));
    out->kind = -1;
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
                out->kind = PRE_INT;
            } else if (ident_is(value, "float")) {
                out->kind = PRE_FLOAT;
            } else if (ident_is(value, "bool")) {
                out->kind = PRE_BOOL;
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
            if (value->kind == JOG_NODE_INT_LIT &&
                parse_dec(value->as.literal.text, value->as.literal.len, &out->bits)) {
                continue;
            }
            return 0;
        }
        return 0;
    }
    return out->kind >= 0;
}

static const JogNode *only_intrinsic(Prelude *p, const JogNode *node, const char *name) {
    const JogNode *attr;
    if (node->attrs.count != 1 || !node->attrs.items) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has a malformed @intrinsic", name);
        return NULL;
    }
    attr = node->attrs.items[0];
    if (!attr || attr->kind != JOG_NODE_ATTRIBUTE || !attr->as.attribute.name ||
        strcmp(attr->as.attribute.name, "intrinsic") != 0) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has a malformed @intrinsic", name);
        return NULL;
    }
    return attr;
}

static int bind_type(Prelude *p, const char *name, JogType *type, JogNode *decl) {
    JogSymbol *symbol;
    if (!name || !type) {
        pre_fail(p, decl ? decl->pos : (JogPos){0}, "out of memory");
        return 0;
    }
    if (jog_package_scope_lookup(p->scope, name)) {
        pre_fail_name(p, decl ? decl->pos : (JogPos){0}, "duplicate prelude declaration '%s'", name);
        return 0;
    }
    symbol = jog_symbol_new(p->arena, JOG_SYM_TYPE, name, decl, type, NULL);
    if (!symbol || !jog_package_scope_bind(p->scope, symbol)) {
        pre_fail(p, decl ? decl->pos : (JogPos){0}, "out of memory");
        return 0;
    }
    return 1;
}

static int slice_of_byte(const JogNode *target, int is_const) {
    return target && target->kind == JOG_NODE_TYPE_SLICE && target->as.type_elem.is_const == is_const &&
           is_byte_name(target->as.type_elem.elem);
}

static JogType *named_type(const JogPackageScope *scope, const char *name) {
    JogSymbol *symbol = name ? jog_package_scope_lookup(scope, name) : NULL;
    if (!symbol || symbol->kind != JOG_SYM_TYPE) {
        return NULL;
    }
    return symbol->type;
}

static int bind_intrinsic(Prelude *p, JogNode *node, const char *name, int index) {
    const JogNode *attr = only_intrinsic(p, node, name);
    Intrinsic spec;
    JogType *builtin;
    int pointer_sized = 0;
    uint64_t width = 0;
    uint32_t ptr_size;
    if (!attr) {
        return 1;
    }
    if (!read_intrinsic(attr, &spec)) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has a malformed @intrinsic", name);
        return 1;
    }
    builtin = jog_type_builtin(p->types, k_scalar[index].which);
    ptr_size = jog_types_ptr_size(p->types);
    if (!builtin) {
        pre_fail(p, node->pos, "out of memory");
        return 0;
    }
    if (spec.kind != k_scalar[index].form) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong kind", name);
        return 1;
    }
    if (k_scalar[index].form == PRE_BOOL) {
        if (spec.has_signed || spec.has_bits || !is_byte_name(node->as.type_decl.target) || builtin->kind != JOG_TY_BOOL ||
            builtin->size != 1) {
            if (spec.has_signed || spec.has_bits) {
                pre_fail_name(p, node->pos, "prelude declaration '%s' has a malformed @intrinsic", name);
            } else if (builtin->kind != JOG_TY_BOOL) {
                pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong kind", name);
            } else {
                pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong base", name);
            }
        } else if (!bind_type(p, name, builtin, NULL)) {
            return 0;
        }
        return 1;
    }
    if (k_scalar[index].form == PRE_FLOAT) {
        if (spec.has_signed || !spec.has_bits || spec.bits_ptr) {
            pre_fail_name(p, node->pos, "prelude declaration '%s' has a malformed @intrinsic", name);
            return 1;
        }
        if (builtin->kind != JOG_TY_FLOAT || (uint64_t)builtin->as.flt.bits != spec.bits) {
            pre_fail_name(p, node->pos, builtin->kind != JOG_TY_FLOAT ? "prelude declaration '%s' has the wrong kind"
                                                                       : "prelude declaration '%s' has the wrong width",
                          name);
            return 1;
        }
        if (!base_matches(node->as.type_decl.target, 0, spec.bits, ptr_size)) {
            pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong base", name);
            return 1;
        }
        return bind_type(p, name, builtin, NULL);
    }
    if (!spec.has_signed || !spec.has_bits) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has a malformed @intrinsic", name);
        return 1;
    }
    if (spec.bits_ptr) {
        pointer_sized = 1;
        width = (uint64_t)ptr_size * 8u;
    } else {
        width = spec.bits;
    }
    if (builtin->kind != JOG_TY_INT) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong kind", name);
        return 1;
    }
    if ((int)builtin->as.integer.is_signed != spec.is_signed) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong signedness", name);
        return 1;
    }
    if ((int)builtin->as.integer.is_size != pointer_sized || (uint64_t)builtin->as.integer.bits != width) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong width", name);
        return 1;
    }
    if (!base_matches(node->as.type_decl.target, pointer_sized, width, ptr_size)) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong base", name);
        return 1;
    }
    return bind_type(p, name, builtin, NULL);
}

static int bind_bytes(Prelude *p, JogNode *node, const char *name) {
    JogType *byte;
    JogType *slice;
    JogType *builtin;
    if (node->attrs.count != 0) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has a malformed @intrinsic", name);
        return 1;
    }
    byte = named_type(p->scope, "byte");
    builtin = jog_type_builtin(p->types, JOG_BUILTIN_BYTES);
    slice = byte ? jog_type_slice(p->types, byte, false) : NULL;
    if (!slice_of_byte(node->as.type_decl.target, 0) || !slice || slice != builtin) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong base", name);
        return 1;
    }
    return bind_type(p, name, builtin, NULL);
}

static int bind_str(Prelude *p, JogNode *node, const char *name) {
    JogType *byte;
    JogType *slice;
    JogType *builtin;
    JogType *type;
    if (node->attrs.count != 0) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has a malformed @intrinsic", name);
        return 1;
    }
    byte = named_type(p->scope, "byte");
    builtin = jog_type_builtin(p->types, JOG_BUILTIN_CONST_BYTES);
    slice = byte ? jog_type_slice(p->types, byte, true) : NULL;
    if (!slice_of_byte(node->as.type_decl.target, 1) || !slice || slice != builtin) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong base", name);
        return 1;
    }
    type = jog_type_new_nominal(p->types, JOG_TY_DISTINCT, name, NULL, node);
    if (!type) {
        pre_fail(p, node->pos, "out of memory");
        return 0;
    }
    type->as.nominal.base = builtin;
    if (!jog_type_layout(p->types, type)) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong base", name);
        return 1;
    }
    if (!bind_type(p, name, type, node)) {
        return 0;
    }
    jog_types_set_core_str(p->types, type);
    return 1;
}

static int bind_rune(Prelude *p, JogNode *node, const char *name) {
    JogType *base;
    JogType *type;
    const JogNode *target = node->as.type_decl.target;
    const char *base_name;
    if (node->attrs.count != 0) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has a malformed @intrinsic", name);
        return 1;
    }
    if (!target || target->kind != JOG_NODE_TYPE_NAME || target->as.type_name.path.count != 1 ||
        !target->as.type_name.path.items) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong base", name);
        return 1;
    }
    base_name = target->as.type_name.path.items[0];
    base = base_name ? named_type(p->scope, base_name) : NULL;
    if (!base || base != jog_type_builtin(p->types, JOG_BUILTIN_U32)) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong base", name);
        return 1;
    }
    type = jog_type_new_nominal(p->types, JOG_TY_DISTINCT, name, NULL, node);
    if (!type) {
        pre_fail(p, node->pos, "out of memory");
        return 0;
    }
    type->as.nominal.base = base;
    if (!jog_type_layout(p->types, type)) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong base", name);
        return 1;
    }
    return bind_type(p, name, type, node);
}

static const char *other_name(const JogNode *node) {
    if (!node) {
        return NULL;
    }
    switch (node->kind) {
    case JOG_NODE_TYPE_DECL:
        return node->as.type_decl.name;
    case JOG_NODE_FN:
        return node->as.fn.name;
    case JOG_NODE_STRUCT:
    case JOG_NODE_INTERFACE:
    case JOG_NODE_ENUM:
        return node->as.aggregate.name;
    case JOG_NODE_VAR:
    case JOG_NODE_VAL:
    case JOG_NODE_CONTEXT:
        if (node->as.binding.names.count > 0 && node->as.binding.names.items) {
            return node->as.binding.names.items[0];
        }
        return NULL;
    default:
        return NULL;
    }
}

static const char *one_type_name(const JogNode *node) {
    if (!node || node->kind != JOG_NODE_TYPE_NAME || node->as.type_name.is_const) {
        return NULL;
    }
    if (node->as.type_name.path.count != 1 || !node->as.type_name.path.items || node->as.type_name.args.count != 0) {
        return NULL;
    }
    return node->as.type_name.path.items[0];
}

/* 1 to keep going, 0 on out of memory. Operator methods are recorded on the
   builtin. They are not universe names. */
static int bind_op_method(Prelude *p, JogNode *node) {
    const char *intrinsic = NULL;
    const char *recv_name;
    const char *ret_name;
    const char *method;
    const char *param_type;
    JogType *on;
    JogType *ret;
    JogNode *param;
    JogMachine machine;
    int value_params;
    method = node && node->as.fn.name ? node->as.fn.name : "?";
    if (!node || node->attrs.count != 0 || node->as.fn.type_params.count != 0 || node->as.fn.contexts.count != 0 ||
        node->as.fn.receiver_is_ptr || !node->as.fn.receiver || node->as.fn.op == JOG_TOK_EOF) {
        pre_fail_name(p, node ? node->pos : (JogPos){0}, "unexpected declaration '%s' in the core prelude", method);
        return 1;
    }
    if (!jog_fn_intrinsic_body(node, &intrinsic)) {
        pre_fail_name(p, node->pos, "prelude operator '%s' is malformed", method);
        return 1;
    }
    recv_name = one_type_name(node->as.fn.receiver);
    on = recv_name ? named_type(p->scope, recv_name) : NULL;
    if (!on || !jog_type_machine(on, &machine) ||
        (on->kind != JOG_TY_INT && on->kind != JOG_TY_FLOAT && on->kind != JOG_TY_BOOL) ||
        !jog_intrinsic_allowed(machine.kind, intrinsic)) {
        pre_fail_name(p, node->pos, "prelude operator '%s' is malformed", method);
        return 1;
    }
    value_params = (int)node->as.fn.params.count;
    if (value_params == 1) {
        param = node->as.fn.params.items ? node->as.fn.params.items[0] : NULL;
        param_type = param && param->kind == JOG_NODE_PARAM ? one_type_name(param->as.param.type) : NULL;
        if (!param_type || !recv_name || strcmp(param_type, recv_name) != 0) {
            pre_fail_name(p, node->pos, "prelude operator '%s' is malformed", method);
            return 1;
        }
    }
    ret_name = one_type_name(node->as.fn.ret);
    if (jog_intrinsic_returns_bool(intrinsic)) {
        ret = named_type(p->scope, "bool");
        if (!ret || !ret_name || strcmp(ret_name, "bool") != 0) {
            pre_fail_name(p, node->pos, "prelude operator '%s' is malformed", method);
            return 1;
        }
    } else {
        ret = on;
        if (!ret_name || !recv_name || strcmp(ret_name, recv_name) != 0) {
            pre_fail_name(p, node->pos, "prelude operator '%s' is malformed", method);
            return 1;
        }
    }
    if (!jog_types_add_scalar_op(p->types, on, node->as.fn.name, value_params, ret)) {
        if (jog_types_scalar_op(p->types, on, node->as.fn.name, value_params)) {
            pre_fail_name(p, node->pos, "duplicate prelude declaration '%s'", method);
            return 1;
        }
        pre_fail(p, node->pos, "out of memory");
        return 0;
    }
    return 1;
}

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static int read_tag(const JogNode *node, int64_t *out) {
    int neg = 0;
    uint64_t value = 0;
    if (!node || !out) {
        return 0;
    }
    if (node->kind == JOG_NODE_UNARY && node->as.unary.op == JOG_TOK_MINUS) {
        neg = 1;
        node = node->as.unary.operand;
    }
    if (!node || node->kind != JOG_NODE_INT_LIT || !parse_dec(node->as.literal.text, node->as.literal.len, &value)) {
        return 0;
    }
    if (neg) {
        if (value > (uint64_t)INT64_MAX + 1ull) {
            return 0;
        }
        *out = value == (uint64_t)INT64_MAX + 1ull ? INT64_MIN : -(int64_t)value;
    } else {
        if (value > (uint64_t)INT64_MAX) {
            return 0;
        }
        *out = (int64_t)value;
    }
    return 1;
}

static int nominal_shape(const JogNode *node) {
    return node && node->attrs.count == 0;
}

/* void has no layout. An empty struct that is laid out would be size 0. */
static int bind_void_decl(Prelude *p, JogNode *node, const char *name) {
    JogType *type;
    if (!nominal_shape(node) || node->as.aggregate.type_params.count != 0 || node->as.aggregate.tag_type ||
        node->as.aggregate.members.count != 0) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong form", name);
        return 1;
    }
    type = jog_type_new_nominal(p->types, JOG_TY_STRUCT, name, NULL, NULL);
    if (!type) {
        pre_fail(p, node->pos, "out of memory");
        return 0;
    }
    return bind_type(p, name, type, NULL);
}

/* The name Option is the structural option constructor. The enum is not
   given variants or a layout, so *T? stays a pointer. */
static int bind_option(Prelude *p, JogNode *node, const char *name) {
    JogNode *param;
    JogNode *none;
    JogNode *some;
    JogNode *field;
    JogType *type;
    JogSymbol *symbol;
    const char *param_name;
    const char *field_type;
    if (!nominal_shape(node) || node->as.aggregate.type_params.count != 1 || node->as.aggregate.tag_type ||
        node->as.aggregate.members.count != 2) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong form", name);
        return 1;
    }
    param = list_at(node->as.aggregate.type_params, 0);
    none = list_at(node->as.aggregate.members, 0);
    some = list_at(node->as.aggregate.members, 1);
    param_name = param && param->kind == JOG_NODE_TYPE_PARAM ? param->as.type_param.name : NULL;
    if (!param_name || !none || none->kind != JOG_NODE_VARIANT || !none->as.variant.name ||
        strcmp(none->as.variant.name, "None") != 0 || none->as.variant.fields.count != 0 || none->as.variant.value ||
        !some || some->kind != JOG_NODE_VARIANT || !some->as.variant.name || strcmp(some->as.variant.name, "Some") != 0 ||
        some->as.variant.fields.count != 1 || some->as.variant.value) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong form", name);
        return 1;
    }
    field = list_at(some->as.variant.fields, 0);
    field_type = field && field->kind == JOG_NODE_PARAM ? one_type_name(field->as.param.type) : NULL;
    if (!field || !field->as.param.name || strcmp(field->as.param.name, "value") != 0 || !field_type ||
        strcmp(field_type, param_name) != 0) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong form", name);
        return 1;
    }
    type = jog_type_new_nominal(p->types, JOG_TY_ENUM, name, NULL, node);
    if (!type || !bind_type(p, name, type, node)) {
        if (!p->failed) {
            pre_fail(p, node->pos, "out of memory");
        }
        return 0;
    }
    symbol = jog_package_scope_lookup(p->scope, name);
    if (!symbol) {
        pre_fail(p, node->pos, "out of memory");
        return 0;
    }
    symbol->type_param_count = 1;
    node->symbol = symbol;
    return 1;
}

static int bind_ordering(Prelude *p, JogNode *node, const char *name) {
    static const struct {
        const char *name;
        int64_t tag;
    } k_variant[] = {
        {"Less", -1},
        {"Equal", 0},
        {"Greater", 1},
    };
    JogType *tag;
    JogType *type;
    JogVariant *variants;
    const char *tag_name;
    size_t i;
    if (!nominal_shape(node) || node->as.aggregate.type_params.count != 0 || node->as.aggregate.members.count != 3) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong form", name);
        return 1;
    }
    tag_name = one_type_name(node->as.aggregate.tag_type);
    tag = tag_name ? named_type(p->scope, tag_name) : NULL;
    if (!tag || tag != jog_type_builtin(p->types, JOG_BUILTIN_I8)) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong base", name);
        return 1;
    }
    variants = ct_arena_alloc(p->arena, sizeof(JogVariant) * 3, _Alignof(JogVariant));
    type = jog_type_new_nominal(p->types, JOG_TY_ENUM, name, NULL, node);
    if (!variants || !type) {
        pre_fail(p, node->pos, "out of memory");
        return 0;
    }
    memset(variants, 0, sizeof(JogVariant) * 3);
    for (i = 0; i < 3; i++) {
        JogNode *variant = list_at(node->as.aggregate.members, i);
        int64_t value = 0;
        if (!variant || variant->kind != JOG_NODE_VARIANT || !variant->as.variant.name ||
            strcmp(variant->as.variant.name, k_variant[i].name) != 0 || variant->as.variant.fields.count != 0 ||
            !read_tag(variant->as.variant.value, &value) || value != k_variant[i].tag) {
            pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong form", name);
            return 1;
        }
        variants[i].name = variant->as.variant.name;
        variants[i].tag = value;
        variants[i].decl = variant;
        variant->symbol = jog_symbol_new(p->arena, JOG_SYM_VARIANT, variant->as.variant.name, variant, type, NULL);
        if (!variant->symbol) {
            pre_fail(p, node->pos, "out of memory");
            return 0;
        }
        variant->type = type;
    }
    type->as.nominal.base = tag;
    type->as.nominal.variants = variants;
    type->as.nominal.variant_count = 3;
    if (!jog_type_layout(p->types, type)) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong base", name);
        return 1;
    }
    if (!bind_type(p, name, type, node)) {
        return 0;
    }
    node->symbol = jog_package_scope_lookup(p->scope, name);
    return 1;
}

/* decl stays NULL so this remains the universe error interface. The method
   is declared here; concrete message(): str still satisfies it. */
static int bind_error_iface(Prelude *p, JogNode *node, const char *name) {
    JogNode *method;
    JogType *type;
    const char *ret;
    if (!nominal_shape(node) || node->as.aggregate.type_params.count != 0 || node->as.aggregate.tag_type ||
        node->as.aggregate.members.count != 1) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong form", name);
        return 1;
    }
    method = list_at(node->as.aggregate.members, 0);
    ret = method && method->kind == JOG_NODE_FN ? one_type_name(method->as.fn.ret) : NULL;
    if (!method || !method->as.fn.name || strcmp(method->as.fn.name, "message") != 0 || method->as.fn.receiver ||
        method->as.fn.receiver_is_ptr || method->as.fn.op != JOG_TOK_EOF || method->as.fn.type_params.count != 0 ||
        method->as.fn.params.count != 0 || method->as.fn.contexts.count != 0 || method->as.fn.body || !ret ||
        strcmp(ret, "str") != 0) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong form", name);
        return 1;
    }
    type = jog_type_new_nominal(p->types, JOG_TY_INTERFACE, name, NULL, NULL);
    if (!type || !jog_type_layout(p->types, type)) {
        pre_fail(p, node->pos, "out of memory");
        return 0;
    }
    return bind_type(p, name, type, NULL);
}

static int bind_core_nominal(Prelude *p, JogNode *node) {
    const char *name = other_name(node);
    if (!node || !name) {
        pre_fail(p, node ? node->pos : (JogPos){0}, "unexpected declaration in the core prelude");
        return 1;
    }
    if (node->kind == JOG_NODE_STRUCT && strcmp(name, "void") == 0) {
        return bind_void_decl(p, node, name);
    }
    if (node->kind == JOG_NODE_ENUM && strcmp(name, "Option") == 0) {
        return bind_option(p, node, name);
    }
    if (node->kind == JOG_NODE_ENUM && strcmp(name, "Ordering") == 0) {
        return bind_ordering(p, node, name);
    }
    if (node->kind == JOG_NODE_INTERFACE && strcmp(name, "error") == 0) {
        return bind_error_iface(p, node, name);
    }
    pre_fail_name(p, node->pos, "unexpected declaration '%s' in the core prelude", name);
    return 1;
}

/* 1 to keep going, 0 on out of memory. */
static int bind_decl(Prelude *p, JogNode *node, int *seen) {
    const char *name;
    int index;
    int form;
    if (!node || node->kind != JOG_NODE_TYPE_DECL) {
        if (node && node->kind == JOG_NODE_FN) {
            return bind_op_method(p, node);
        }
        return bind_core_nominal(p, node);
    }
    name = node->as.type_decl.name;
    if (!name || name[0] == '\0') {
        pre_fail(p, node->pos, "prelude declaration has no name");
        return 1;
    }
    index = scalar_index(name);
    if (index < 0) {
        pre_fail_name(p, node->pos, "unexpected declaration '%s' in the core prelude", name);
        return 1;
    }
    if (seen[index]) {
        pre_fail_name(p, node->pos, "duplicate prelude declaration '%s'", name);
        return 1;
    }
    seen[index] = 1;
    if (node->as.type_decl.type_params.count > 0 || !node->as.type_decl.target) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong form", name);
        return 1;
    }
    form = k_scalar[index].form;
    if (form == PRE_BYTES) {
        if (node->as.type_decl.distinct) {
            pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong form", name);
            return 1;
        }
        return bind_bytes(p, node, name);
    }
    if (!node->as.type_decl.distinct) {
        pre_fail_name(p, node->pos, "prelude declaration '%s' has the wrong form", name);
        return 1;
    }
    if (form == PRE_STR) {
        return bind_str(p, node, name);
    }
    if (form == PRE_RUNE) {
        return bind_rune(p, node, name);
    }
    return bind_intrinsic(p, node, name, index);
}

static void report_missing(Prelude *p, const JogNode *file, const int *seen) {
    size_t i;
    JogPos pos = file ? file->pos : (JogPos){0};
    for (i = 0; i < sizeof(k_scalar) / sizeof(k_scalar[0]); i++) {
        if (!seen[i]) {
            pre_fail_name(p, pos, "missing prelude declaration '%s'", k_scalar[i].name);
        }
    }
}

bool jog_bind_prelude(CtArena *arena, CtDiag *diag, CtIntern *intern, JogTypes *types, JogPackageScope *scope,
                      const char *core_root) {
    Prelude p;
    CtFile file;
    CtDa tokens;
    const char *path;
    JogNode *root;
    size_t before;
    size_t i;
    int seen[sizeof(k_scalar) / sizeof(k_scalar[0])];
    int tokens_ready = 0;
    int ok = 0;
    CtPos nowhere = {0};
    memset(&p, 0, sizeof(p));
    memset(&file, 0, sizeof(file));
    memset(seen, 0, sizeof(seen));
    p.arena = arena;
    p.diag = diag;
    p.intern = intern;
    p.types = types;
    p.scope = scope;
    if (!arena || !diag || !intern || !types || !scope) {
        if (diag) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, nowhere, "core prelude was not found");
        }
        return false;
    }
    if (!core_root || core_root[0] == '\0' || !ct_fs_is_dir(core_root)) {
        pre_fail(&p, (JogPos){0}, "core prelude was not found");
        return false;
    }
    path = ct_path_join(arena, core_root, "scalar.jog");
    if (!path) {
        pre_fail(&p, (JogPos){0}, "out of memory");
        return false;
    }
    if (!ct_fs_is_file(path)) {
        pre_fail(&p, (JogPos){0}, "core prelude was not found");
        return false;
    }
    if (!ct_da_init(&tokens, sizeof(JogToken))) {
        pre_fail(&p, (JogPos){0}, "out of memory");
        return false;
    }
    tokens_ready = 1;
    before = ct_diag_error_count(diag);
    if (!ct_file_read(arena, diag, path, &file) || !file.data) {
        p.failed = 1;
        goto done;
    }
    if (!jog_lex(arena, diag, intern, &file, &tokens) || ct_diag_error_count(diag) != before) {
        p.failed = 1;
        goto done;
    }
    root = jog_parse(arena, diag, intern, &file, &tokens);
    if (!root || ct_diag_error_count(diag) != before) {
        p.failed = 1;
        goto done;
    }
    if (root->kind != JOG_NODE_FILE) {
        pre_fail(&p, root->pos, "unexpected declaration in the core prelude");
        goto done;
    }
    if (root->as.file.imports.count > 0) {
        JogNode *import = root->as.file.imports.items ? root->as.file.imports.items[0] : NULL;
        pre_fail(&p, import ? import->pos : root->pos, "unexpected import in the core prelude");
    }
    for (i = 0; i < root->as.file.decls.count; i++) {
        JogNode *decl = root->as.file.decls.items ? root->as.file.decls.items[i] : NULL;
        if (!bind_decl(&p, decl, seen)) {
            goto done;
        }
    }
    report_missing(&p, root, seen);
    if (!p.failed && !jog_bind_lang_items(arena, intern, types, scope)) {
        pre_fail(&p, root->pos, "out of memory");
    }
    ok = !p.failed;
done:
    if (tokens_ready) {
        ct_da_free(&tokens);
    }
    return ok;
}

static void scan_list(JogProgram *program, JogList list, int *failed);

static void scan_node(JogProgram *program, JogNode *node, int *failed) {
    size_t i;
    if (!node) {
        return;
    }
    if (node->attrs.items) {
        for (i = 0; i < node->attrs.count; i++) {
            JogNode *attr = node->attrs.items[i];
            JogIntrinsicSpec spec;
            unsigned bits = 0;
            int is_ptr = 0;
            int width;
            if (!attr || attr->kind != JOG_NODE_ATTRIBUTE || !attr->as.attribute.name ||
                strcmp(attr->as.attribute.name, "intrinsic") != 0) {
                continue;
            }
            if (node->kind != JOG_NODE_TYPE_DECL || !node->as.type_decl.distinct) {
                ct_diag_emit(program->diag, CT_SEVERITY_ERROR, cpos(attr->pos),
                             "@intrinsic is only allowed on a distinct type");
                *failed = 1;
                continue;
            }
            if (!jog_parse_intrinsic(attr, &spec)) {
                ct_diag_emit(program->diag, CT_SEVERITY_ERROR, cpos(attr->pos), "@intrinsic is malformed");
                *failed = 1;
                continue;
            }
            if (spec.unknown) {
                ct_diag_emit(program->diag, CT_SEVERITY_ERROR, cpos(attr->pos), "unknown intrinsic '%s'", spec.unknown);
                *failed = 1;
                continue;
            }
            width = jog_intrinsic_width_ok(&spec, program->types ? jog_types_ptr_size(program->types) : 0, &bits, &is_ptr);
            if (width == 0) {
                ct_diag_emit(program->diag, CT_SEVERITY_ERROR, cpos(attr->pos), "@intrinsic is malformed");
                *failed = 1;
                continue;
            }
            if (width < 0) {
                ct_diag_emit(program->diag, CT_SEVERITY_ERROR, cpos(attr->pos), "@intrinsic has the wrong width");
                *failed = 1;
                continue;
            }
            if (!jog_intrinsic_base_ok(node->as.type_decl.target, bits, is_ptr)) {
                ct_diag_emit(program->diag, CT_SEVERITY_ERROR, cpos(attr->pos),
                             "@intrinsic width does not match the base");
                *failed = 1;
            }
        }
    }
    if (node->kind == JOG_NODE_BUILTIN && node->as.builtin.name && strcmp(node->as.builtin.name, "intrinsic") == 0) {
        ct_diag_emit(program->diag, CT_SEVERITY_ERROR, cpos(node->pos), "@intrinsic is not a value");
        *failed = 1;
    }
    switch (node->kind) {
    case JOG_NODE_FILE:
        scan_list(program, node->as.file.imports, failed);
        scan_list(program, node->as.file.decls, failed);
        return;
    case JOG_NODE_FN:
        scan_node(program, node->as.fn.receiver, failed);
        scan_list(program, node->as.fn.type_params, failed);
        scan_list(program, node->as.fn.params, failed);
        scan_list(program, node->as.fn.contexts, failed);
        scan_node(program, node->as.fn.ret, failed);
        scan_node(program, node->as.fn.body, failed);
        return;
    case JOG_NODE_VAR:
    case JOG_NODE_VAL:
    case JOG_NODE_CONTEXT:
        scan_node(program, node->as.binding.type, failed);
        scan_node(program, node->as.binding.init, failed);
        return;
    case JOG_NODE_TYPE_DECL:
        scan_list(program, node->as.type_decl.type_params, failed);
        scan_node(program, node->as.type_decl.target, failed);
        return;
    case JOG_NODE_STRUCT:
    case JOG_NODE_INTERFACE:
    case JOG_NODE_ENUM:
        scan_list(program, node->as.aggregate.type_params, failed);
        scan_node(program, node->as.aggregate.tag_type, failed);
        scan_list(program, node->as.aggregate.members, failed);
        return;
    case JOG_NODE_EXTERN_BLOCK:
        scan_list(program, node->as.extern_block.fns, failed);
        return;
    case JOG_NODE_FIELD:
        scan_node(program, node->as.field.type, failed);
        scan_node(program, node->as.field.default_value, failed);
        return;
    case JOG_NODE_EMBED:
        scan_node(program, node->as.embed.type, failed);
        return;
    case JOG_NODE_TYPE_SET:
        scan_list(program, node->as.type_set.terms, failed);
        return;
    case JOG_NODE_VARIANT:
        scan_list(program, node->as.variant.fields, failed);
        scan_node(program, node->as.variant.value, failed);
        return;
    case JOG_NODE_PARAM:
        scan_node(program, node->as.param.type, failed);
        scan_node(program, node->as.param.default_value, failed);
        return;
    case JOG_NODE_TYPE_PARAM:
        scan_node(program, node->as.type_param.constraint, failed);
        return;
    case JOG_NODE_TYPE_NAME:
        scan_list(program, node->as.type_name.args, failed);
        return;
    case JOG_NODE_TYPE_PTR:
    case JOG_NODE_TYPE_SLICE:
    case JOG_NODE_TYPE_OPTION:
        scan_node(program, node->as.type_elem.elem, failed);
        return;
    case JOG_NODE_TYPE_ARRAY:
        scan_node(program, node->as.type_array.len, failed);
        scan_node(program, node->as.type_array.elem, failed);
        return;
    case JOG_NODE_TYPE_TUPLE:
        scan_list(program, node->as.type_tuple.elems, failed);
        return;
    case JOG_NODE_TYPE_FN:
        scan_list(program, node->as.type_fn.params, failed);
        scan_node(program, node->as.type_fn.ret, failed);
        scan_list(program, node->as.type_fn.contexts, failed);
        return;
    case JOG_NODE_BLOCK:
        scan_list(program, node->as.block.stmts, failed);
        return;
    case JOG_NODE_EXPR_STMT:
        scan_node(program, node->as.expr_stmt.expr, failed);
        return;
    case JOG_NODE_ASSIGN:
        scan_node(program, node->as.assign.target, failed);
        scan_node(program, node->as.assign.value, failed);
        return;
    case JOG_NODE_DESTRUCTURE:
        scan_list(program, node->as.destructure.targets, failed);
        scan_node(program, node->as.destructure.value, failed);
        return;
    case JOG_NODE_WITH:
        scan_list(program, node->as.with.values, failed);
        scan_node(program, node->as.with.body, failed);
        return;
    case JOG_NODE_DEFER:
    case JOG_NODE_ERRDEFER:
        scan_node(program, node->as.defer.body, failed);
        return;
    case JOG_NODE_RETURN:
        scan_list(program, node->as.ret.values, failed);
        return;
    case JOG_NODE_FOR:
        scan_node(program, node->as.for_.expr, failed);
        scan_node(program, node->as.for_.body, failed);
        return;
    case JOG_NODE_ATTR_BLOCK:
        scan_node(program, node->as.attr_block.body, failed);
        return;
    case JOG_NODE_INTERP:
        scan_list(program, node->as.interp.parts, failed);
        return;
    case JOG_NODE_IDENT:
    case JOG_NODE_DOT_VARIANT:
        scan_list(program, node->as.ident.type_args, failed);
        return;
    case JOG_NODE_TUPLE:
        scan_list(program, node->as.tuple.elems, failed);
        return;
    case JOG_NODE_UNARY:
    case JOG_NODE_FORCE:
        scan_node(program, node->as.unary.operand, failed);
        return;
    case JOG_NODE_BINARY:
        scan_node(program, node->as.binary.lhs, failed);
        scan_node(program, node->as.binary.rhs, failed);
        return;
    case JOG_NODE_CAST:
        scan_node(program, node->as.cast.expr, failed);
        scan_node(program, node->as.cast.type, failed);
        return;
    case JOG_NODE_IS:
        scan_node(program, node->as.is.expr, failed);
        scan_node(program, node->as.is.type, failed);
        return;
    case JOG_NODE_CALL:
        scan_node(program, node->as.call.callee, failed);
        scan_list(program, node->as.call.args, failed);
        scan_node(program, node->as.call.trailing, failed);
        scan_list(program, node->as.call.contexts, failed);
        return;
    case JOG_NODE_ARG:
        scan_node(program, node->as.arg.value, failed);
        return;
    case JOG_NODE_INDEX:
        scan_node(program, node->as.index.base, failed);
        scan_node(program, node->as.index.index, failed);
        return;
    case JOG_NODE_SLICE:
        scan_node(program, node->as.slice.base, failed);
        scan_node(program, node->as.slice.lo, failed);
        scan_node(program, node->as.slice.hi, failed);
        return;
    case JOG_NODE_MEMBER:
        scan_node(program, node->as.member.base, failed);
        scan_list(program, node->as.member.type_args, failed);
        return;
    case JOG_NODE_IF:
        scan_node(program, node->as.if_.cond, failed);
        scan_node(program, node->as.if_.then_block, failed);
        scan_node(program, node->as.if_.else_branch, failed);
        return;
    case JOG_NODE_WHEN:
        scan_node(program, node->as.when.subject, failed);
        scan_list(program, node->as.when.arms, failed);
        return;
    case JOG_NODE_WHEN_ARM:
        scan_list(program, node->as.when_arm.conds, failed);
        scan_node(program, node->as.when_arm.body, failed);
        return;
    case JOG_NODE_LAMBDA:
        scan_list(program, node->as.lambda.params, failed);
        scan_node(program, node->as.lambda.ret, failed);
        scan_node(program, node->as.lambda.body, failed);
        return;
    case JOG_NODE_GO:
        scan_list(program, node->as.go.contexts, failed);
        scan_list(program, node->as.go.args, failed);
        scan_node(program, node->as.go.body, failed);
        return;
    case JOG_NODE_ARRAY_LIT:
        scan_node(program, node->as.array_lit.type, failed);
        scan_list(program, node->as.array_lit.elems, failed);
        return;
    case JOG_NODE_BUILTIN:
        scan_list(program, node->as.builtin.args, failed);
        return;
    case JOG_NODE_PAT_VARIANT:
        scan_list(program, node->as.pat_variant.fields, failed);
        return;
    case JOG_NODE_PAT_FIELD:
        scan_node(program, node->as.pat_field.pattern, failed);
        return;
    case JOG_NODE_PAT_IN:
    case JOG_NODE_PAT_EXPR:
        scan_node(program, node->as.pat_expr.expr, failed);
        return;
    case JOG_NODE_PAT_IS:
        scan_node(program, node->as.pat_is.type, failed);
        return;
    default:
        return;
    }
}

static void scan_list(JogProgram *program, JogList list, int *failed) {
    size_t i;
    if (!list.items) {
        return;
    }
    for (i = 0; i < list.count; i++) {
        scan_node(program, list.items[i], failed);
    }
}

bool jog_reject_intrinsic(JogProgram *program) {
    size_t p;
    int failed = 0;
    if (!program || !program->diag) {
        return false;
    }
    for (p = 0; p < program->package_count; p++) {
        JogPackage *pkg = program->packages ? program->packages[p] : NULL;
        size_t f;
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count; f++) {
            JogSourceFile *source = pkg->files ? pkg->files[f] : NULL;
            if (source && source->ast) {
                scan_node(program, source->ast, &failed);
            }
        }
    }
    return !failed;
}
