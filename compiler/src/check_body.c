#include "check_body.h"

#include "check_constraint.h"
#include "check_context.h"
#include "check_enum.h"
#include "check_error.h"
#include "check_generic.h"
#include "check_interface.h"
#include "check_memory.h"
#include "check_package.h"
#include "check_string.h"
#include "check_struct.h"
#include "check_coro.h"
#include "check_chan_for.h"
#include "check_comptime.h"
#include "const_eval.h"
#include "float_lit.h"
#include "resolve_package.h"
#include "str.h"

#include <stdio.h>
#include <string.h>

/* Milestone 2 expressions and statements.
   c"..." is *const i8, which is *const c.char on the V1 host
   (type char = i8) without loading std.c.
   for i in a..b binds i as a constant of the bounds' integer type.
   An untyped float stays soft until a typed float wins, otherwise f64.
   A character literal is the Unicode code point as an untyped integer.
   for x in xs, and for i, x in xs, walk an array, a slice, str, bytes,
   or bytes[N]. x is a constant element and i is a constant usize.
   A channel is not a collection. for v in ch receives until the channel is closed.
   A lambda checks where it is written. A named function may be used as a
   value. A non-escaping lambda may name an outer local; it is captured
   by reference. A unit lambda discards a non-unit tail.
   A lambda matched to a function type with context parameters takes those
   parameters itself. Calls in its body resolve them before outer contexts.
   Struct fields, literals, and methods are checked in check_struct.c.
   Pointers, arrays, slices, and bytes are checked in check_memory.c.
   Tuples, error?, defer, and panic are checked in check_error.c.
   size(T) and align(T) are builtin usize constants of T's layout when that
   name is not bound. align(T) is the alignment field jog_type_layout stored.
   Enums, when, and Option operators are checked in check_enum.c.
   with clauses, context bindings, and call resolution are checked in
   check_context.c. Unconstrained function type parameters are checked in
   check_generic.c. */

typedef struct AssignSet {
    JogSymbol *symbol;
    struct AssignSet *next;
} AssignSet;

typedef struct Loop {
    AssignSet *breaks;
    bool any_break;
    bool forever;
    struct Loop *parent;
} Loop;

typedef struct Env {
    Check *ck;
    CheckJob *job;
    JogBlockScope *scope;
    JogType *ret;
    AssignSet *assigned;
    bool reachable;
    Loop *loop;
    /* Innermost lambda scope, or NULL outside a lambda. A local bound
       outside this scope is a capture. */
    JogBlockScope *lambda_scope;
} Env;

static JogType *check_expr(Env *env, JogNode *node, JogType *target);
static JogType *check_expr_raw(Env *env, JogNode *node, JogType *target);
static void check_stmt(Env *env, JogNode *stmt);
static void check_stmt_raw(Env *env, JogNode *stmt);
static void check_block(Env *env, JogNode *block);
static JogType *check_if(Env *env, JogNode *node, JogType *target, bool as_value);
static JogType *check_lambda(Env *env, JogNode *node, JogType *target);
static int push_lambda_contexts(Env *env, JogNode *node, JogType *fn_target);

/* Pin the active body so jog_check_expr reuses its definite-assignment set. */
static JogType *check_expr(Env *env, JogNode *node, JogType *target) {
    Env *saved = NULL;
    JogType *got;
    if (env && env->job) {
        saved = (Env *)env->job->body_env;
        env->job->body_env = env;
    }
    got = check_expr_raw(env, node, target);
    if (env && env->job) {
        env->job->body_env = saved;
    }
    return got;
}

static void check_stmt(Env *env, JogNode *stmt) {
    Env *saved = NULL;
    if (env && env->job) {
        saved = (Env *)env->job->body_env;
        env->job->body_env = env;
    }
    check_stmt_raw(env, stmt);
    if (env && env->job) {
        env->job->body_env = saved;
    }
}

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static bool is_discard(const char *name) {
    return !name || name[0] == '\0' || (name[0] == '_' && name[1] == '\0');
}

static bool is_error(const JogType *type) {
    return type && type->kind == JOG_TY_ERROR;
}

static bool same_type(const JogType *a, const JogType *b) {
    if (!a || !b) {
        return false;
    }
    if (a->kind == JOG_TY_ERROR || b->kind == JOG_TY_ERROR) {
        return true;
    }
    return a == b;
}

/* bytes converts to const bytes, which is str. The header is the same
   pointer and length. The reverse is not implicit. []T converts to
   []const T the same way when the element type matches. []u8 and
   []const u8 convert to str the same way; bytes and []u8 stay different
   types. */
static bool bytes_to_const(const JogType *got, const JogType *want) {
    return got && want && got->kind == JOG_TY_BYTES && want->kind == JOG_TY_BYTES && !got->is_const && want->is_const;
}

static bool is_u8_type(const JogType *type) {
    return type && type->kind == JOG_TY_INT && !type->as.integer.is_signed && type->as.integer.bits == 8 &&
           !type->as.integer.is_size;
}

static bool u8_slice_to_str(const JogType *got, const JogType *want) {
    const JogType *elem;
    if (!got || !want || got->kind != JOG_TY_SLICE || want->kind != JOG_TY_BYTES || !want->is_const) {
        return false;
    }
    elem = got->as.elem.elem;
    if (elem && elem->kind == JOG_TY_DISTINCT) {
        elem = jog_type_underlying((JogType *)elem);
    }
    return is_u8_type(elem);
}

static bool slice_to_const(const JogType *got, const JogType *want) {
    return got && want && got->kind == JOG_TY_SLICE && want->kind == JOG_TY_SLICE && !got->is_const && want->is_const &&
           got->as.elem.elem && got->as.elem.elem == want->as.elem.elem;
}

/* bytes[N] as T, and T as bytes[N], when N is size(T). Unchecked reinterpret. */
static bool bytes_n_reinterpret(const JogType *got, const JogType *want) {
    if (!got || !want || !got->has_layout || !want->has_layout || got->size == 0 || got->size != want->size) {
        return false;
    }
    return got->kind == JOG_TY_BYTES_N || want->kind == JOG_TY_BYTES_N;
}

static bool is_arith_op(JogTokenKind op) {
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
        return true;
    default:
        return false;
    }
}

static bool is_rel_op(JogTokenKind op) {
    return op == JOG_TOK_LT || op == JOG_TOK_LE || op == JOG_TOK_GT || op == JOG_TOK_GE;
}

static bool is_eq_op(JogTokenKind op) {
    return op == JOG_TOK_EQ || op == JOG_TOK_NEQ;
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

static void write_type(Check *ck, const JogType *type, char *buf, size_t cap) {
    CtSb sb;
    const char *text;
    if (cap == 0) {
        return;
    }
    buf[0] = '\0';
    if (!type) {
        snprintf(buf, cap, "<null type>");
        return;
    }
    if (ck && type == jog_type_builtin(ck->program->types, JOG_BUILTIN_CONST_BYTES)) {
        snprintf(buf, cap, "str");
        return;
    }
    ct_sb_init(&sb);
    jog_type_print(type, &sb);
    text = ct_sb_cstr(&sb);
    snprintf(buf, cap, "%s", text ? text : "?");
    ct_sb_free(&sb);
}

static void fail_mismatch(Check *ck, JogPos pos, const JogType *want, const JogType *got) {
    char a[160];
    char b[160];
    write_type(ck, want, a, sizeof(a));
    write_type(ck, got, b, sizeof(b));
    jog_check_fail(ck, pos, "type mismatch: expected %s, got %s", a, b);
}

static void fail_types(Check *ck, JogPos pos, const char *what, const JogType *left, const JogType *right) {
    char a[160];
    char b[160];
    write_type(ck, left, a, sizeof(a));
    write_type(ck, right, b, sizeof(b));
    jog_check_fail(ck, pos, "%s %s and %s", what, a, b);
}

static void fail_op(Check *ck, JogPos pos, JogTokenKind op, const JogType *type) {
    char a[160];
    write_type(ck, type, a, sizeof(a));
    jog_check_fail(ck, pos, "operator '%s' does not apply to %s", jog_token_spelling(op), a);
}

static JogType *builtin(Check *ck, JogBuiltin which) {
    JogType *type = jog_type_builtin(ck->program->types, which);
    if (!type) {
        jog_check_oom(ck);
    }
    return type;
}

static bool is_str_type(Check *ck, const JogType *type) {
    return type && type == jog_type_builtin(ck->program->types, JOG_BUILTIN_CONST_BYTES);
}

static JogType *cstring_type(Check *ck) {
    JogType *elem = builtin(ck, JOG_BUILTIN_I8);
    JogType *type;
    if (!elem) {
        return NULL;
    }
    type = jog_type_ptr(ck->program->types, elem, true);
    if (!type) {
        jog_check_oom(ck);
    }
    return type;
}

bool jog_check_type_has_zero(const JogType *type) {
    size_t i;
    if (!type) {
        return false;
    }
    switch (type->kind) {
    case JOG_TY_ERROR:
    case JOG_TY_UNIT:
    case JOG_TY_BOOL:
    case JOG_TY_INT:
    case JOG_TY_FLOAT:
    case JOG_TY_UNTYPED_INT:
    case JOG_TY_UNTYPED_FLOAT:
    case JOG_TY_BYTES_N:
    case JOG_TY_BYTES:
    case JOG_TY_SLICE:
    case JOG_TY_OPTION:
    case JOG_TY_NULL:
        return true;
    case JOG_TY_ARRAY:
        return jog_check_type_has_zero(type->as.array.elem);
    case JOG_TY_TUPLE:
        for (i = 0; i < type->as.tuple.count; i++) {
            if (!type->as.tuple.elems || !jog_check_type_has_zero(type->as.tuple.elems[i])) {
                return false;
            }
        }
        return true;
    case JOG_TY_DISTINCT:
        return jog_check_type_has_zero(type->as.nominal.base);
    case JOG_TY_STRUCT:
        return jog_struct_has_zero(type);
    case JOG_TY_ENUM:
        return jog_enum_has_zero(type);
    case JOG_TY_PTR:
    case JOG_TY_FN:
    case JOG_TY_INTERFACE:
    case JOG_TY_TYPE_PARAM:
        return false;
    }
    return false;
}

static bool is_int_lit_tree(const JogNode *node) {
    JogTokenKind op;
    if (!node) {
        return false;
    }
    if (node->kind == JOG_NODE_INT_LIT) {
        return true;
    }
    if (node->kind == JOG_NODE_UNARY) {
        op = node->as.unary.op;
        return (op == JOG_TOK_MINUS || op == JOG_TOK_TILDE) && is_int_lit_tree(node->as.unary.operand);
    }
    if (node->kind != JOG_NODE_BINARY || !is_arith_op(node->as.binary.op)) {
        return false;
    }
    return is_int_lit_tree(node->as.binary.lhs) && is_int_lit_tree(node->as.binary.rhs);
}

static bool is_bool_lit_tree(const JogNode *node) {
    JogTokenKind op;
    if (!node) {
        return false;
    }
    if (node->kind == JOG_NODE_BOOL_LIT) {
        return true;
    }
    if (node->kind == JOG_NODE_UNARY && node->as.unary.op == JOG_TOK_BANG) {
        return is_bool_lit_tree(node->as.unary.operand);
    }
    if (node->kind != JOG_NODE_BINARY) {
        return false;
    }
    op = node->as.binary.op;
    if (op == JOG_TOK_AMPAMP || op == JOG_TOK_PIPEPIPE) {
        return is_bool_lit_tree(node->as.binary.lhs) && is_bool_lit_tree(node->as.binary.rhs);
    }
    if (is_eq_op(op)) {
        if (is_bool_lit_tree(node->as.binary.lhs) && is_bool_lit_tree(node->as.binary.rhs)) {
            return true;
        }
        return is_int_lit_tree(node->as.binary.lhs) && is_int_lit_tree(node->as.binary.rhs);
    }
    if (is_rel_op(op)) {
        return is_int_lit_tree(node->as.binary.lhs) && is_int_lit_tree(node->as.binary.rhs);
    }
    return false;
}

static JogType *fold_const(Check *ck, JogNode *node, JogType *target) {
    JogConst value;
    JogType *error;
    memset(&value, 0, sizeof(value));
    if (!jog_const_eval(ck->program->arena, ck->program->diag, ck->program->types, node, target, &value) || !value.type) {
        ck->failed = true;
        error = jog_check_error_type(ck);
        if (node && error) {
            node->type = error;
        }
        return error;
    }
    node->type = value.type;
    return value.type;
}

static const char *float_lit_msg(JogFloatLitStatus st) {
    if (st == JOG_FLOAT_LIT_DIV0) {
        return "division by zero";
    }
    if (st == JOG_FLOAT_LIT_BAD) {
        return "invalid float literal";
    }
    return "constant does not fit";
}

static unsigned float_bits_of(const JogType *type) {
    if (type && type->kind == JOG_TY_FLOAT && type->as.flt.bits == 32) {
        return 32;
    }
    return 64;
}

static int finite_d(double value) {
    return value == value && (value == 0.0 || value + value != value);
}

static JogType *fail_float(Env *env, JogNode *node, JogFloatLitStatus st) {
    JogType *error;
    jog_check_fail(env->ck, node ? node->pos : (JogPos){0}, "%s", float_lit_msg(st));
    error = jog_check_error_type(env->ck);
    if (node && error) {
        node->type = error;
    }
    return error;
}

static JogType *type_float_tree(Env *env, JogNode *node, JogType *target) {
    JogType *want;
    double value;
    JogFloatLitStatus st;
    unsigned bits;
    if (target && target->kind == JOG_TY_FLOAT) {
        want = target;
        bits = float_bits_of(target);
    } else {
        want = builtin(env->ck, JOG_BUILTIN_F64);
        bits = 64;
    }
    if (!want) {
        return NULL;
    }
    st = jog_float_lit_eval(node, bits, &value);
    if (st != JOG_FLOAT_LIT_OK) {
        return fail_float(env, node, st);
    }
    (void)value;
    node->type = want;
    return want;
}

static int code_fits(const JogType *type, uint32_t cp) {
    unsigned bits;
    if (!type || type->kind != JOG_TY_INT || type->as.integer.bits == 0 || type->as.integer.bits > 64) {
        return 0;
    }
    bits = type->as.integer.bits;
    if (bits >= 32) {
        return !type->as.integer.is_signed || cp <= 0x7FFFFFFFu;
    }
    if (type->as.integer.is_signed) {
        return cp < (1u << (bits - 1));
    }
    return cp < (1u << bits);
}

static JogType *type_char(Env *env, JogNode *node, JogType *target) {
    uint32_t cp = 0;
    JogType *want;
    JogType *error;
    if (!node || !jog_char_codepoint(node->as.literal.text, node->as.literal.len, &cp)) {
        jog_check_fail(env->ck, node ? node->pos : (JogPos){0}, "invalid char literal");
        error = jog_check_error_type(env->ck);
        if (node) {
            node->type = error;
        }
        return error;
    }
    if (target && target->kind == JOG_TY_INT) {
        if (!code_fits(target, cp)) {
            jog_check_fail(env->ck, node->pos, "integer constant does not fit");
            error = jog_check_error_type(env->ck);
            node->type = error;
            return error;
        }
        node->type = target;
        return target;
    }
    if (target && !is_error(target) && target->kind != JOG_TY_UNTYPED_INT) {
        jog_check_fail(env->ck, node->pos, "constant does not fit");
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    want = builtin(env->ck, JOG_BUILTIN_I64);
    if (!want) {
        return NULL;
    }
    node->type = want;
    return want;
}

static JogType *adapt_float(Env *env, JogNode *node, JogType *got, JogType *want) {
    if (!got) {
        return NULL;
    }
    if (got->kind != JOG_TY_UNTYPED_FLOAT) {
        return got;
    }
    if (want && want->kind == JOG_TY_FLOAT) {
        return type_float_tree(env, node, want);
    }
    return type_float_tree(env, node, NULL);
}

static int float_num_op(JogTokenKind op) {
    return op == JOG_TOK_PLUS || op == JOG_TOK_MINUS || op == JOG_TOK_STAR || op == JOG_TOK_SLASH || op == JOG_TOK_PERCENT ||
           is_rel_op(op) || is_eq_op(op);
}

static int float_fits_int(const JogType *type, double value) {
    unsigned bits;
    double limit;
    if (!type || type->kind != JOG_TY_INT || !finite_d(value) || type->as.integer.bits == 0 || type->as.integer.bits > 64) {
        return 0;
    }
    bits = type->as.integer.bits;
    if (!type->as.integer.is_signed) {
        if (value < 0.0) {
            return 0;
        }
        if (bits >= 64) {
            return value < 18446744073709551616.0;
        }
        limit = (double)((unsigned long long)1 << bits);
        return value < limit;
    }
    if (bits >= 64) {
        return value >= -9223372036854775808.0 && value < 9223372036854775808.0;
    }
    limit = (double)((unsigned long long)1 << (bits - 1));
    return value >= -limit && value < limit;
}

static bool assign_has(const AssignSet *set, const JogSymbol *symbol) {
    const AssignSet *it;
    for (it = set; it; it = it->next) {
        if (it->symbol == symbol) {
            return true;
        }
    }
    return false;
}

static AssignSet *assign_add(Check *ck, AssignSet *set, JogSymbol *symbol) {
    AssignSet *cell;
    if (!symbol || assign_has(set, symbol)) {
        return set;
    }
    cell = ct_arena_alloc(ck->program->arena, sizeof(*cell), _Alignof(AssignSet));
    if (!cell) {
        jog_check_oom(ck);
        return set;
    }
    cell->symbol = symbol;
    cell->next = set;
    return cell;
}

static AssignSet *assign_intersect(Check *ck, AssignSet *a, AssignSet *b) {
    AssignSet *out = NULL;
    AssignSet *it;
    for (it = a; it && !ck->stop; it = it->next) {
        if (assign_has(b, it->symbol)) {
            out = assign_add(ck, out, it->symbol);
        }
    }
    return out;
}

static JogBlockScope *new_scope(Env *env, JogBlockScope *parent) {
    JogPackageScope *package = NULL;
    JogBlockScope *scope;
    if (env->job && env->job->package) {
        package = env->job->package->scope;
    }
    scope = jog_block_scope_new(env->ck->program->arena, parent, package);
    if (!scope) {
        jog_check_oom(env->ck);
    }
    return scope;
}

static JogSymbol *lookup_name(Env *env, const char *name) {
    const JogBlockScope *block;
    if (!name || is_discard(name)) {
        return NULL;
    }
    for (block = env->scope; block; block = block->parent) {
        JogSymbol *found = jog_scope_map_lookup(block->bindings, name);
        if (found) {
            return found;
        }
    }
    if (!env->job) {
        return NULL;
    }
    return jog_check_lookup_file(env->job, name);
}

static JogSymbol *bind_local(Env *env, JogNode *at, JogSymbolKind kind, const char *name, JogType *type) {
    JogSymbol *symbol;
    JogPackage *package;
    if (!env->scope || !name || is_discard(name)) {
        return NULL;
    }
    package = env->job ? env->job->package : NULL;
    symbol = jog_symbol_new(env->ck->program->arena, kind, name, at, type, package);
    if (!symbol) {
        jog_check_oom(env->ck);
        return NULL;
    }
    if (!jog_block_scope_bind(env->scope, symbol)) {
        jog_check_fail(env->ck, at ? at->pos : (JogPos){0}, "duplicate declaration of '%s'", name);
        return NULL;
    }
    return symbol;
}

static void read_var(Env *env, JogNode *node, JogSymbol *symbol) {
    if (!symbol || symbol->kind != JOG_SYM_VARIABLE) {
        return;
    }
    if (jog_check_type_has_zero(symbol->type) || assign_has(env->assigned, symbol)) {
        return;
    }
    jog_check_fail(env->ck, node->pos, "variable '%s' is used before it is assigned", symbol->name ? symbol->name : "?");
}

static bool is_value_kind(JogSymbolKind kind) {
    switch (kind) {
    case JOG_SYM_FUNCTION:
    case JOG_SYM_VARIABLE:
    case JOG_SYM_CONSTANT:
    case JOG_SYM_PARAMETER:
    case JOG_SYM_CONTEXT:
        return true;
    default:
        return false;
    }
}

static JogType *finish_value(Env *env, JogNode *node, JogSymbol *symbol) {
    JogType *type;
    if (!jog_check_wait(env->ck, env->job, symbol)) {
        return NULL;
    }
    if (!is_value_kind(symbol->kind)) {
        jog_check_fail(env->ck, node->pos, "'%s' is not a value", symbol->name ? symbol->name : "?");
        type = jog_check_error_type(env->ck);
        node->type = type;
        return type;
    }
    type = symbol->type;
    if (!type) {
        jog_check_fail(env->ck, node->pos, "'%s' has no type", symbol->name ? symbol->name : "?");
        type = jog_check_error_type(env->ck);
        node->type = type;
        return type;
    }
    read_var(env, node, symbol);
    node->symbol = symbol;
    node->type = type;
    return type;
}

static JogType *check_ident(Env *env, JogNode *node) {
    JogSymbol *symbol;
    JogType *error;
    symbol = lookup_name(env, node->as.ident.name);
    if (!symbol) {
        jog_check_fail(env->ck, node->pos, "undefined name '%s'", node->as.ident.name ? node->as.ident.name : "?");
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    node->symbol = symbol;
    if (node->as.ident.type_args.count > 0) {
        if (!jog_check_wait(env->ck, env->job, symbol)) {
            return NULL;
        }
        if (symbol->kind != JOG_SYM_FUNCTION || symbol->type_param_count == 0) {
            jog_check_fail(env->ck, node->pos, "type arguments are only allowed on a generic function");
            error = jog_check_error_type(env->ck);
            node->type = error;
            return error;
        }
    }
    return finish_value(env, node, symbol);
}

/* x?.f is Some(v.f) or None. A struct field, including through one pointer,
   matches jog_check_enum_safe. .len and .ptr of str, bytes, or a slice are
   the view fields. The base is checked once. */
static JogType *check_safe_member(Env *env, JogNode *node) {
    JogType *base;
    JogType *inner;
    JogType *owner;
    JogType *error;
    JogType *field_ty = NULL;
    JogType *result;
    const char *name;
    const JogField *field = NULL;
    size_t i;
    name = node->as.member.name;
    if (!name) {
        jog_check_fail(env->ck, node->pos, "operator '?.' needs a field");
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    base = check_expr(env, node->as.member.base, NULL);
    if (!base || env->ck->stop) {
        return base;
    }
    if (is_error(base)) {
        node->type = base;
        return base;
    }
    base = jog_type_underlying(base);
    if (!base || base->kind != JOG_TY_OPTION) {
        jog_check_fail(env->ck, node->pos, "operator '?.' needs an option");
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    inner = jog_type_underlying(base->as.elem.elem);
    owner = inner && inner->kind == JOG_TY_PTR ? jog_type_underlying(inner->as.elem.elem) : inner;
    if (owner && owner->kind == JOG_TY_STRUCT && owner->has_layout) {
        for (i = 0; i < owner->as.nominal.field_count; i++) {
            const JogField *item = &owner->as.nominal.fields[i];
            if (item->name && strcmp(item->name, name) == 0) {
                field = item;
                break;
            }
        }
        if (!field) {
            jog_check_fail(env->ck, node->pos, "unknown field '%s'", name);
            error = jog_check_error_type(env->ck);
            node->type = error;
            return error;
        }
        if (field->decl && field->decl->symbol &&
            !jog_symbol_visible_from(field->decl->symbol, env->job ? env->job->package : NULL,
                                     env->job && env->job->decl ? env->job->decl->pos.file : NULL)) {
            jog_check_fail(env->ck, node->pos, "field '%s' is not visible", name);
            error = jog_check_error_type(env->ck);
            node->type = error;
            return error;
        }
        field_ty = field->type;
        node->symbol = field->decl ? field->decl->symbol : NULL;
    } else if (inner && (inner->kind == JOG_TY_BYTES || inner->kind == JOG_TY_SLICE) &&
               (strcmp(name, "len") == 0 || strcmp(name, "ptr") == 0)) {
        if (strcmp(name, "len") == 0) {
            field_ty = builtin(env->ck, JOG_BUILTIN_USIZE);
        } else {
            JogType *elem = inner->kind == JOG_TY_BYTES ? builtin(env->ck, JOG_BUILTIN_U8) : inner->as.elem.elem;
            if (!elem) {
                return NULL;
            }
            field_ty = jog_type_ptr(env->ck->program->types, elem, inner->is_const);
            if (!field_ty) {
                jog_check_oom(env->ck);
                return NULL;
            }
        }
    } else {
        jog_check_fail(env->ck, node->pos, "operator '?.' needs a struct field");
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    if (!field_ty) {
        return NULL;
    }
    result = jog_type_option(env->ck->program->types, field_ty);
    if (!result) {
        jog_check_oom(env->ck);
        return NULL;
    }
    node->type = result;
    return result;
}

static JogType *check_member(Env *env, JogNode *node) {
    JogNode *base;
    JogSymbol *pkg_sym;
    JogSymbol *mem;
    JogPackage *pkg;
    JogType *error;
    JogType *field = NULL;
    int struct_member;
    struct_member = jog_check_struct_member(env->ck, env->job, env->scope, node, &field);
    if (struct_member < 0) {
        return NULL;
    }
    if (struct_member > 0) {
        return field;
    }
    if (!node->as.member.name) {
        int tuple_field = jog_check_error_field(env->ck, env->job, env->scope, node, &field);
        if (tuple_field < 0 || env->ck->stop) {
            return NULL;
        }
        if (tuple_field > 0) {
            return field;
        }
    }
    base = node->as.member.base;
    if (node->as.member.safe) {
        jog_check_fail(env->ck, node->pos, "operator '?.' is not checked yet");
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    if (!node->as.member.name) {
        jog_check_fail(env->ck, node->pos, "field access is not checked yet");
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    if (!base || base->kind != JOG_NODE_IDENT) {
        int view;
        if (base) {
            (void)check_expr(env, base, NULL);
        }
        if (env->ck->stop) {
            return NULL;
        }
        view = jog_check_package_view(env->ck, env->job, node, &field);
        if (view < 0 || env->ck->stop) {
            return NULL;
        }
        if (view > 0) {
            return field;
        }
        jog_check_fail(env->ck, node->pos, "field access is not checked yet");
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    pkg_sym = lookup_name(env, base->as.ident.name);
    if (!pkg_sym) {
        jog_check_fail(env->ck, base->pos, "undefined name '%s'", base->as.ident.name ? base->as.ident.name : "?");
        error = jog_check_error_type(env->ck);
        base->type = error;
        node->type = error;
        return error;
    }
    base->symbol = pkg_sym;
    if (pkg_sym->kind != JOG_SYM_PACKAGE || !pkg_sym->package || !pkg_sym->package->scope) {
        int view;
        (void)finish_value(env, base, pkg_sym);
        if (env->ck->stop) {
            return NULL;
        }
        view = jog_check_package_view(env->ck, env->job, node, &field);
        if (view < 0 || env->ck->stop) {
            return NULL;
        }
        if (view > 0) {
            return field;
        }
        jog_check_fail(env->ck, node->pos, "field access is not checked yet");
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    pkg = pkg_sym->package;
    mem = jog_package_scope_lookup(pkg->scope, node->as.member.name);
    if (!mem) {
        jog_check_fail(env->ck, node->pos, "undefined name '%s'", node->as.member.name);
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    if (!jog_symbol_visible_from(mem, env->job ? env->job->package : NULL,
                                 env->job && env->job->decl ? env->job->decl->pos.file : NULL)) {
        jog_check_fail(env->ck, node->pos, "name '%s' is not visible", node->as.member.name);
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    node->symbol = mem;
    return finish_value(env, node, mem);
}

static JogType *adapt_int(Env *env, JogNode *node, JogType *got, JogType *want) {
    if (!got) {
        return NULL;
    }
    if (got->kind != JOG_TY_UNTYPED_INT) {
        return got;
    }
    if (want && want->kind == JOG_TY_INT) {
        return fold_const(env->ck, node, want);
    }
    return fold_const(env->ck, node, NULL);
}

static JogType *check_unary(Env *env, JogNode *node) {
    JogTokenKind op = node->as.unary.op;
    JogType *operand;
    JogType *error;
    JogType *boolean;
    if (op == JOG_TOK_AMP || op == JOG_TOK_STAR) {
        return jog_check_memory_unary(env->ck, env->job, env->scope, node);
    }
    if (op == JOG_TOK_BANG) {
        boolean = builtin(env->ck, JOG_BUILTIN_BOOL);
        if (!boolean) {
            return NULL;
        }
        operand = check_expr(env, node->as.unary.operand, boolean);
        if (!operand) {
            return NULL;
        }
        if (!is_error(operand) && operand->kind != JOG_TY_BOOL) {
            JogType *constrained = NULL;
            int cr = jog_constraint_unary(env->ck, node, operand, &constrained);
            if (cr < 0 || env->ck->stop) {
                return NULL;
            }
            if (cr > 0) {
                return constrained;
            }
            fail_op(env->ck, node->pos, op, operand);
            error = jog_check_error_type(env->ck);
            node->type = error;
            return error;
        }
        node->type = is_error(operand) ? operand : boolean;
        return node->type;
    }
    if (op != JOG_TOK_MINUS && op != JOG_TOK_TILDE) {
        jog_check_fail(env->ck, node->pos, "operator '%s' is not checked yet", jog_token_spelling(op));
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    if (is_int_lit_tree(node->as.unary.operand)) {
        operand = builtin(env->ck, JOG_BUILTIN_UNTYPED_INT);
    } else if (jog_float_lit_tree(node->as.unary.operand)) {
        operand = builtin(env->ck, JOG_BUILTIN_UNTYPED_FLOAT);
        if (operand) {
            node->as.unary.operand->type = operand;
        }
    } else {
        operand = check_expr(env, node->as.unary.operand, NULL);
    }
    if (!operand || env->ck->stop) {
        return operand;
    }
    operand = adapt_int(env, node->as.unary.operand, operand, NULL);
    if (operand && operand->kind == JOG_TY_UNTYPED_FLOAT) {
        operand = adapt_float(env, node->as.unary.operand, operand, NULL);
    }
    if (!operand || is_error(operand)) {
        node->type = operand;
        return operand;
    }
    if (operand->kind == JOG_TY_FLOAT) {
        if (op != JOG_TOK_MINUS) {
            fail_op(env->ck, node->pos, op, operand);
            error = jog_check_error_type(env->ck);
            node->type = error;
            return error;
        }
        node->type = operand;
        return operand;
    }
    if (operand->kind != JOG_TY_INT) {
        JogType *constrained = NULL;
        int cr = jog_constraint_unary(env->ck, node, operand, &constrained);
        if (cr < 0 || env->ck->stop) {
            return NULL;
        }
        if (cr > 0) {
            return constrained;
        }
        fail_op(env->ck, node->pos, op, operand);
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    node->type = operand;
    return operand;
}

static JogType *operand_type(Env *env, JogNode *node) {
    JogType *untyped;
    if (is_int_lit_tree(node)) {
        untyped = builtin(env->ck, JOG_BUILTIN_UNTYPED_INT);
        if (untyped) {
            node->type = untyped;
        }
        return untyped;
    }
    if (jog_float_lit_tree(node)) {
        untyped = builtin(env->ck, JOG_BUILTIN_UNTYPED_FLOAT);
        if (untyped) {
            node->type = untyped;
        }
        return untyped;
    }
    if (is_bool_lit_tree(node)) {
        return fold_const(env->ck, node, NULL);
    }
    return check_expr(env, node, NULL);
}

/* == and != on an enum when every payload is bool, an integer, a float,
   bytes or str, or another comparable enum. A struct, slice, pointer, or
   option payload is not comparable. Tag 0 is unrelated: it is the zero
   value, checked by jog_enum_has_zero. */
static bool enum_eq_ok(const JogType *type, int depth) {
    size_t i;
    if (!type || depth > 8 || type->kind != JOG_TY_ENUM) {
        return false;
    }
    for (i = 0; i < type->as.nominal.variant_count; i++) {
        const JogVariant *variant = type->as.nominal.variants ? &type->as.nominal.variants[i] : NULL;
        size_t f;
        if (!variant) {
            return false;
        }
        for (f = 0; f < variant->field_count; f++) {
            JogType *field = variant->fields ? variant->fields[f].type : NULL;
            JogType *underlying;
            if (!field) {
                return false;
            }
            underlying = jog_type_underlying(field);
            if (!underlying || underlying->kind == JOG_TY_UNIT) {
                continue;
            }
            if (underlying->kind == JOG_TY_BOOL || underlying->kind == JOG_TY_INT || underlying->kind == JOG_TY_FLOAT ||
                underlying->kind == JOG_TY_BYTES) {
                continue;
            }
            if (underlying->kind == JOG_TY_ENUM && enum_eq_ok(underlying, depth + 1)) {
                continue;
            }
            return false;
        }
    }
    return true;
}

static JogType *check_binary(Env *env, JogNode *node) {
    JogTokenKind op = node->as.binary.op;
    JogNode *lhs = node->as.binary.lhs;
    JogNode *rhs = node->as.binary.rhs;
    JogType *lt;
    JogType *rt;
    JogType *error;
    JogType *boolean;
    bool compare;
    bool logic;
    if (op == JOG_TOK_DOTDOT || op == JOG_TOK_CMP3 || op == JOG_TOK_ELVIS) {
        jog_check_fail(env->ck, node->pos, "operator '%s' is not checked yet", jog_token_spelling(op));
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    compare = is_eq_op(op) || is_rel_op(op);
    logic = op == JOG_TOK_AMPAMP || op == JOG_TOK_PIPEPIPE;
    if (!compare && !logic && !is_arith_op(op)) {
        jog_check_fail(env->ck, node->pos, "operator '%s' is not checked yet", jog_token_spelling(op));
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    if (op == JOG_TOK_AMPAMP) {
        JogIfaceNarrow narrow;
        lt = operand_type(env, lhs);
        if (!lt || env->ck->stop) {
            return NULL;
        }
        jog_iface_narrow_enter(env->ck, lhs, &narrow);
        rt = operand_type(env, rhs);
        jog_iface_narrow_leave(&narrow);
    } else {
        lt = operand_type(env, lhs);
        rt = operand_type(env, rhs);
    }
    if (!lt || !rt || env->ck->stop) {
        return NULL;
    }
    if (is_error(lt) || is_error(rt)) {
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    if (lt->kind == JOG_TY_UNTYPED_INT && rt->kind == JOG_TY_INT) {
        lt = adapt_int(env, lhs, lt, rt);
    } else if (rt->kind == JOG_TY_UNTYPED_INT && lt->kind == JOG_TY_INT) {
        rt = adapt_int(env, rhs, rt, lt);
    } else {
        if (lt->kind == JOG_TY_UNTYPED_INT) {
            lt = adapt_int(env, lhs, lt, NULL);
        }
        if (rt && rt->kind == JOG_TY_UNTYPED_INT) {
            rt = adapt_int(env, rhs, rt, lt && lt->kind == JOG_TY_INT ? lt : NULL);
        }
    }
    if (lt && rt && !is_error(lt) && !is_error(rt)) {
        if (lt->kind == JOG_TY_UNTYPED_FLOAT && rt->kind == JOG_TY_FLOAT) {
            lt = adapt_float(env, lhs, lt, rt);
        } else if (rt->kind == JOG_TY_UNTYPED_FLOAT && lt->kind == JOG_TY_FLOAT) {
            rt = adapt_float(env, rhs, rt, lt);
        } else {
            if (lt->kind == JOG_TY_UNTYPED_FLOAT) {
                lt = adapt_float(env, lhs, lt, NULL);
            }
            if (rt && rt->kind == JOG_TY_UNTYPED_FLOAT) {
                rt = adapt_float(env, rhs, rt, lt && lt->kind == JOG_TY_FLOAT ? lt : NULL);
            }
        }
    }
    if (!lt || !rt || env->ck->stop) {
        return NULL;
    }
    if (is_error(lt) || is_error(rt)) {
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    boolean = builtin(env->ck, JOG_BUILTIN_BOOL);
    if (!boolean) {
        return NULL;
    }
    if (logic) {
        if (lt->kind != JOG_TY_BOOL || rt->kind != JOG_TY_BOOL) {
            fail_op(env->ck, node->pos, op, lt->kind != JOG_TY_BOOL ? lt : rt);
            error = jog_check_error_type(env->ck);
            node->type = error;
            return error;
        }
        node->type = boolean;
        return boolean;
    }
    if (lt->kind == JOG_TY_PTR || rt->kind == JOG_TY_PTR) {
        JogType *mem_ty = NULL;
        int mem = jog_check_memory_binary(env->ck, node, lt, rt, &mem_ty);
        if (mem < 0 || env->ck->stop) {
            return NULL;
        }
        if (mem > 0) {
            return mem_ty;
        }
    }
    if (is_eq_op(op)) {
        JogType *cmp = NULL;
        int err_cmp = jog_check_error_compare(env->ck, node, lt, rt, &cmp);
        if (err_cmp < 0 || env->ck->stop) {
            return NULL;
        }
        if (err_cmp > 0) {
            return cmp;
        }
    }
    /* A char literal adapts to the integer it is compared or combined with,
       the same way an assignment `val x: u8 = 'a'` already does. */
    if (lhs && lhs->kind == JOG_NODE_CHAR_LIT && rt->kind == JOG_TY_INT && lt->kind == JOG_TY_INT && lt != rt) {
        lt = type_char(env, lhs, rt);
    }
    if (rhs && rhs->kind == JOG_NODE_CHAR_LIT && lt && lt->kind == JOG_TY_INT && rt->kind == JOG_TY_INT && rt != lt) {
        rt = type_char(env, rhs, lt);
    }
    if (!lt || !rt || env->ck->stop) {
        return NULL;
    }
    if (is_error(lt) || is_error(rt)) {
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    if (!same_type(lt, rt)) {
        fail_types(env->ck, node->pos, "mismatched types", lt, rt);
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    if (compare && (lt->kind == JOG_TY_BOOL || is_str_type(env->ck, lt))) {
        if (is_rel_op(op)) {
            fail_op(env->ck, node->pos, op, lt);
            error = jog_check_error_type(env->ck);
            node->type = error;
            return error;
        }
        node->type = boolean;
        return boolean;
    }
    if (is_eq_op(op)) {
        JogType *lu = jog_type_underlying(lt);
        JogType *ru = jog_type_underlying(rt);
        if (lu && ru && lu == ru && lu->kind == JOG_TY_ENUM && enum_eq_ok(lu, 0)) {
            node->type = boolean;
            return boolean;
        }
    }
    if (lt->kind == JOG_TY_INT && (compare || is_arith_op(op))) {
        node->type = compare ? boolean : lt;
        return node->type;
    }
    if (lt->kind == JOG_TY_FLOAT && rt->kind == JOG_TY_FLOAT && float_num_op(op)) {
        node->type = compare ? boolean : lt;
        return node->type;
    }
    {
        JogType *constrained = NULL;
        int cr = jog_constraint_binary(env->ck, node, lt, rt, &constrained);
        if (cr < 0 || env->ck->stop) {
            return NULL;
        }
        if (cr > 0) {
            return constrained;
        }
    }
    fail_op(env->ck, node->pos, op, lt);
    error = jog_check_error_type(env->ck);
    node->type = error;
    return error;
}

static JogType *expect_type(Env *env, JogNode *node, JogType *got, JogType *want) {
    JogType *error;
    if (!got || env->ck->stop) {
        return got;
    }
    if (!want || is_error(want) || is_error(got) || same_type(got, want)) {
        return is_error(got) ? got : (want && !is_error(want) ? want : got);
    }
    if (got->kind == JOG_TY_UNTYPED_INT && want->kind == JOG_TY_INT) {
        return adapt_int(env, node, got, want);
    }
    if (got->kind == JOG_TY_UNTYPED_FLOAT && want->kind == JOG_TY_FLOAT) {
        return adapt_float(env, node, got, want);
    }
    {
        JogType *replaced = NULL;
        int iface = jog_iface_expect(env->ck, node, got, want, &replaced);
        if (iface < 0) {
            return NULL;
        }
        if (iface > 0) {
            return replaced;
        }
    }
    if (bytes_to_const(got, want) || slice_to_const(got, want) || u8_slice_to_str(got, want)) {
        node->type = want;
        return want;
    }
    if (want->kind == JOG_TY_OPTION && want->as.elem.elem && same_type(got, want->as.elem.elem)) {
        node->type = want;
        return want;
    }
    fail_mismatch(env->ck, node->pos, want, got);
    error = jog_check_error_type(env->ck);
    node->type = error;
    return error;
}

static const char *param_name_at(JogNode *callee, size_t index) {
    JogNode *fn;
    JogNode *param;
    if (!callee) {
        return NULL;
    }
    fn = NULL;
    if (callee->kind == JOG_NODE_IDENT && callee->symbol && callee->symbol->decl && callee->symbol->decl->kind == JOG_NODE_FN) {
        fn = callee->symbol->decl;
    } else if (callee->kind == JOG_NODE_MEMBER && callee->symbol && callee->symbol->decl &&
               callee->symbol->decl->kind == JOG_NODE_FN) {
        fn = callee->symbol->decl;
    }
    if (!fn || index >= fn->as.fn.params.count) {
        return NULL;
    }
    param = list_at(fn->as.fn.params, index);
    if (!param || param->kind != JOG_NODE_PARAM) {
        return NULL;
    }
    return param->as.param.name;
}

static JogNode *param_decl(JogNode *callee, size_t index) {
    JogNode *fn = NULL;
    if (callee && callee->symbol && callee->symbol->decl && callee->symbol->decl->kind == JOG_NODE_FN) {
        fn = callee->symbol->decl;
    }
    if (!fn) {
        return NULL;
    }
    return list_at(fn->as.fn.params, index);
}

static int param_index_named(JogNode *callee, const char *name, size_t count) {
    size_t i;
    if (!name) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        const char *param = param_name_at(callee, i);
        if (param && strcmp(param, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Index of a pack in the function type, or -1. The local's type is the
   slice; extra call arguments are elements of that slice. A receiver, when
   the type has one, is param 0 and is not the pack. */
static int pack_slot(JogNode *callee, size_t nparam) {
    JogNode *decl;
    JogNode *param;
    size_t ast_n;
    if (!callee || !callee->symbol || !callee->symbol->decl || callee->symbol->decl->kind != JOG_NODE_FN || nparam == 0) {
        return -1;
    }
    decl = callee->symbol->decl;
    ast_n = decl->as.fn.params.count;
    if (ast_n == 0) {
        return -1;
    }
    param = list_at(decl->as.fn.params, ast_n - 1);
    if (!param || param->kind != JOG_NODE_PARAM || !param->as.param.pack) {
        return -1;
    }
    if (decl->as.fn.receiver) {
        if (nparam != ast_n + 1) {
            return -1;
        }
    } else if (nparam != ast_n) {
        return -1;
    }
    return (int)nparam - 1;
}

static JogType *pack_elem_type(const JogType *fn, int pack) {
    const JogType *slot;
    if (!fn || fn->kind != JOG_TY_FN || pack < 0 || (size_t)pack >= fn->as.fn.param_count || !fn->as.fn.params) {
        return NULL;
    }
    slot = fn->as.fn.params[pack];
    if (!slot || slot->kind != JOG_TY_SLICE) {
        return NULL;
    }
    return slot->as.elem.elem;
}

/* void is the universe incomplete struct. jog_type_layout must not see it:
   an empty struct would become size 0, and void has no size. */
static bool is_universe_void(const JogType *type) {
    const char *name;
    if (!type || type->kind != JOG_TY_STRUCT || type->has_layout) {
        return false;
    }
    if (type->as.nominal.field_count != 0 || type->as.nominal.package != NULL || type->as.nominal.decl != NULL) {
        return false;
    }
    name = type->as.nominal.name;
    return name != NULL && strcmp(name, "void") == 0;
}

/* List<i32> inside size(List<i32>) or align(List<i32>). A generic alias is its target.
   what is the builtin name ("size" or "align") used when the symbol is not a type. */
static JogType *named_type_use(Env *env, JogSymbol *sym, JogList args, JogPos pos, const char *what) {
    if (!sym) {
        return jog_check_error_type(env->ck);
    }
    if (!jog_check_wait(env->ck, env->job, sym)) {
        return NULL;
    }
    if (sym->decl && sym->decl->kind == JOG_NODE_TYPE_DECL && sym->type_param_count > 0 &&
        !sym->decl->as.type_decl.distinct) {
        return jog_generic_alias_apply(env->ck, env->job, sym, args, pos);
    }
    if (sym->decl && sym->decl->kind == JOG_NODE_ENUM && sym->type_param_count > 0) {
        return jog_generic_enum_apply(env->ck, env->job, sym, args, pos);
    }
    if (sym->decl && sym->decl->kind == JOG_NODE_STRUCT && sym->type_param_count > 0) {
        return jog_generic_struct_apply(env->ck, env->job, sym, args, pos);
    }
    if (args.count > 0) {
        jog_check_fail(env->ck, pos, "type arguments are not checked yet");
        return jog_check_error_type(env->ck);
    }
    if (!sym->type) {
        jog_check_fail(env->ck, pos, "%s expects a type", what);
        return jog_check_error_type(env->ck);
    }
    return sym->type;
}

/* The argument of size(T) or align(T). A type parameter is the template's T, not a value.
   0 is not returned: a missing type is an error type or NULL when stopped.
   what is the builtin name used in errors. */
static JogType *size_type_arg(Env *env, JogNode *node, const char *what) {
    JogType *error;
    error = jog_check_error_type(env->ck);
    if (!node) {
        jog_check_fail(env->ck, (JogPos){0}, "%s expects a type", what);
        return error;
    }
    if (node->kind == JOG_NODE_IDENT) {
        const char *name = node->as.ident.name;
        JogType *param;
        JogSymbol *sym;
        JogType *type;
        if (!name) {
            jog_check_fail(env->ck, node->pos, "%s expects a type", what);
            node->type = error;
            return error;
        }
        param = jog_generic_lookup_param(env->job, name);
        if (param) {
            if (node->as.ident.type_args.count > 0) {
                jog_check_fail(env->ck, node->pos, "type arguments are not checked yet");
                node->type = error;
                return error;
            }
            node->type = param;
            return param;
        }
        sym = lookup_name(env, name);
        if (!sym) {
            jog_check_fail(env->ck, node->pos, "undefined name '%s'", name);
            node->type = error;
            return error;
        }
        if (sym->kind != JOG_SYM_TYPE) {
            jog_check_fail(env->ck, node->pos, "%s expects a type", what);
            node->symbol = sym;
            node->type = error;
            return error;
        }
        node->symbol = sym;
        type = named_type_use(env, sym, node->as.ident.type_args, node->pos, what);
        if (!type) {
            return NULL;
        }
        node->type = type;
        return type;
    }
    if (node->kind == JOG_NODE_MEMBER && !node->as.member.safe && node->as.member.name && node->as.member.base &&
        node->as.member.base->kind == JOG_NODE_IDENT) {
        JogNode *base = node->as.member.base;
        JogSymbol *pkg_sym = lookup_name(env, base->as.ident.name);
        JogSymbol *mem;
        JogType *type;
        const char *file;
        if (pkg_sym && pkg_sym->kind == JOG_SYM_PACKAGE && pkg_sym->package && pkg_sym->package->scope) {
            base->symbol = pkg_sym;
            mem = jog_package_scope_lookup(pkg_sym->package->scope, node->as.member.name);
            if (!mem) {
                jog_check_fail(env->ck, node->pos, "undefined name '%s'", node->as.member.name);
                node->type = error;
                return error;
            }
            file = env->job && env->job->decl ? env->job->decl->pos.file : NULL;
            if (!jog_symbol_visible_from(mem, env->job ? env->job->package : NULL, file)) {
                jog_check_fail(env->ck, node->pos, "name '%s' is not visible", node->as.member.name);
                node->type = error;
                return error;
            }
            if (mem->kind != JOG_SYM_TYPE) {
                jog_check_fail(env->ck, node->pos, "%s expects a type", what);
                node->symbol = mem;
                node->type = error;
                return error;
            }
            node->symbol = mem;
            type = named_type_use(env, mem, node->as.member.type_args, node->pos, what);
            if (!type) {
                return NULL;
            }
            node->type = type;
            return type;
        }
    }
    if (node->kind == JOG_NODE_UNARY && node->as.unary.op == JOG_TOK_STAR) {
        JogType *elem = size_type_arg(env, node->as.unary.operand, what);
        JogType *built;
        if (!elem) {
            return NULL;
        }
        if (elem->kind == JOG_TY_ERROR) {
            node->type = elem;
            return elem;
        }
        built = jog_type_ptr(env->ck->program->types, elem, false);
        if (!built) {
            jog_check_oom(env->ck);
            return NULL;
        }
        node->type = built;
        return built;
    }
    if (node->kind == JOG_NODE_TYPE_OPTION) {
        JogType *elem = size_type_arg(env, node->as.type_elem.elem, what);
        JogType *built;
        if (!elem) {
            return NULL;
        }
        if (elem->kind == JOG_TY_ERROR) {
            node->type = elem;
            return elem;
        }
        built = jog_type_option(env->ck->program->types, elem);
        if (!built) {
            jog_check_oom(env->ck);
            return NULL;
        }
        node->type = built;
        return built;
    }
    if (node->kind == JOG_NODE_TYPE_NAME || node->kind == JOG_NODE_TYPE_PTR || node->kind == JOG_NODE_TYPE_SLICE ||
        node->kind == JOG_NODE_TYPE_ARRAY || node->kind == JOG_NODE_TYPE_BYTES || node->kind == JOG_NODE_TYPE_FN ||
        node->kind == JOG_NODE_TYPE_TUPLE || node->kind == JOG_NODE_TYPE_UNIT) {
        JogType *type = jog_check_resolve_type(env->ck, env->job, node);
        if (!type) {
            return NULL;
        }
        node->type = type;
        return type;
    }
    jog_check_fail(env->ck, node->pos, "%s expects a type", what);
    node->type = error;
    return error;
}

/* wrappingAdd, wrappingSub, and wrappingMul on an integer. Checked
   arithmetic panics; these wrap modulo 2^n. 1 when this call is one of
   them, 0 when the name is something else, -1 when the checker stopped. */
static bool wrapping_name(const char *name) {
    return name && (strcmp(name, "wrappingAdd") == 0 || strcmp(name, "wrappingSub") == 0 ||
                    strcmp(name, "wrappingMul") == 0);
}

static int check_wrapping_call(Env *env, JogNode *call) {
    JogNode *callee;
    JogNode *base;
    JogNode *arg;
    JogNode *value;
    JogType *ty;
    JogType *got;
    JogType *error;
    const char *name;
    if (!env || !call || call->kind != JOG_NODE_CALL) {
        return 0;
    }
    callee = call->as.call.callee;
    if (!callee || callee->kind != JOG_NODE_MEMBER || callee->as.member.safe) {
        return 0;
    }
    name = callee->as.member.name;
    if (!wrapping_name(name)) {
        return 0;
    }
    base = callee->as.member.base;
    ty = base && base->type ? jog_type_underlying(base->type) : NULL;
    if (!ty || ty->kind != JOG_TY_INT) {
        return 0;
    }
    error = jog_check_error_type(env->ck);
    if (call->as.call.trailing || call->as.call.contexts.count > 0 || call->as.call.args.count != 1) {
        jog_check_fail(env->ck, call->pos, "%s expects 1 argument", name);
        call->type = error;
        return 1;
    }
    arg = list_at(call->as.call.args, 0);
    if (arg && arg->kind == JOG_NODE_ARG && arg->as.arg.name) {
        jog_check_fail(env->ck, call->pos, "%s expects 1 argument", name);
        call->type = error;
        return 1;
    }
    value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : arg;
    got = check_expr(env, value, ty);
    if (env->ck->stop) {
        return -1;
    }
    if (!got || is_error(got)) {
        call->type = error;
        return 1;
    }
    if (!same_type(got, ty)) {
        jog_check_fail(env->ck, call->pos, "%s expects 1 argument", name);
        call->type = error;
        return 1;
    }
    call->type = ty;
    return 1;
}

/* 1 when call is the builtin named by which ("size" or "align"). 0 when
   that name is bound, or this is not that call. -1 when the checker has stopped.
   Every V1 type jog_type_layout accepts has a layout, including size 0. void is
   the incomplete exception. A type parameter is usize here; the constant is
   the concrete type's layout after monomorphization.
   noun is the word in "has no <noun>": "size" or "alignment". */
static int check_layout_call(Env *env, JogNode *call, const char *which, const char *noun) {
    JogNode *callee;
    JogNode *arg;
    JogNode *value;
    JogType *type;
    JogType *usize;
    JogType *error;
    const char *name;
    char text[160];
    if (!env || !call || call->kind != JOG_NODE_CALL || !which || !noun) {
        return 0;
    }
    callee = call->as.call.callee;
    if (!callee || callee->kind != JOG_NODE_IDENT || callee->as.ident.type_args.count > 0) {
        return 0;
    }
    name = callee->as.ident.name;
    if (!name || strcmp(name, which) != 0) {
        return 0;
    }
    if (lookup_name(env, name)) {
        return 0;
    }
    error = jog_check_error_type(env->ck);
    if (call->as.call.trailing || call->as.call.contexts.count > 0 || call->as.call.args.count != 1) {
        jog_check_fail(env->ck, call->pos, "%s expects 1 argument", which);
        call->type = error;
        return env->ck->stop ? -1 : 1;
    }
    arg = list_at(call->as.call.args, 0);
    if (!arg || arg->kind != JOG_NODE_ARG || arg->as.arg.name) {
        jog_check_fail(env->ck, arg ? arg->pos : call->pos, "%s expects 1 argument", which);
        call->type = error;
        return env->ck->stop ? -1 : 1;
    }
    value = arg->as.arg.value;
    type = size_type_arg(env, value, which);
    if (!type || env->ck->stop) {
        return -1;
    }
    if (type->kind == JOG_TY_ERROR) {
        call->type = type;
        return 1;
    }
    /* Open in the template. Instantiation substitutes, then lowering reads
       the concrete layout. Do not reject T because it is a type parameter. */
    if (jog_generic_has_param(type)) {
        usize = builtin(env->ck, JOG_BUILTIN_USIZE);
        if (!usize) {
            return -1;
        }
        call->type = usize;
        return 1;
    }
    if (is_universe_void(type) || (!type->has_layout && !jog_type_layout(env->ck->program->types, type)) ||
        !type->has_layout) {
        write_type(env->ck, type, text, sizeof(text));
        jog_check_fail(env->ck, value ? value->pos : call->pos, "type '%s' has no %s", text, noun);
        call->type = error;
        return env->ck->stop ? -1 : 1;
    }
    usize = builtin(env->ck, JOG_BUILTIN_USIZE);
    if (!usize) {
        return -1;
    }
    call->type = usize;
    return 1;
}

static int check_size_call(Env *env, JogNode *call) {
    return check_layout_call(env, call, "size", "size");
}

static int check_align_call(Env *env, JogNode *call) {
    return check_layout_call(env, call, "align", "alignment");
}

static JogType *generic_probe(void *env, JogNode *node, JogType *target) {
    return check_expr((Env *)env, node, target);
}

static JogType *check_call(Env *env, JogNode *node) {
    JogType *callee;
    JogType *fn;
    JogType *error;
    JogType *special = NULL;
    JogNode *target;
    size_t count;
    size_t i;
    size_t positional = 0;
    bool seen_named = false;
    bool variadic;
    int pack;
    int saw_elems;
    unsigned char *filled;
    int struct_call;
    target = node->as.call.callee;
    {
        int coro = jog_check_coro_call(env->ck, env->job, env->scope, node);
        if (coro < 0) {
            return NULL;
        }
        if (coro > 0) {
            return node->type;
        }
    }
    struct_call = jog_check_struct_call(env->ck, env->job, env->scope, node, &special);
    if (struct_call < 0) {
        return NULL;
    }
    if (struct_call > 0) {
        return special;
    }
    {
        int coro = jog_check_coro_method(env->ck, env->job, env->scope, node);
        if (coro < 0) {
            return NULL;
        }
        if (coro > 0) {
            return node->type;
        }
    }
    {
        int mem_call = jog_check_memory_call(env->ck, env->job, env->scope, node);
        if (mem_call < 0) {
            return NULL;
        }
        if (mem_call > 0) {
            return node->type;
        }
    }
    {
        int str_call = jog_check_string_call(env->ck, env->job, env->scope, node);
        if (str_call < 0) {
            return NULL;
        }
        if (str_call > 0) {
            return node->type;
        }
    }
    {
        int wrapped = check_wrapping_call(env, node);
        if (wrapped < 0) {
            return NULL;
        }
        if (wrapped > 0) {
            return node->type;
        }
    }
    if (target && target->kind == JOG_NODE_MEMBER && target->as.member.name && target->as.member.base &&
        target->as.member.base->type) {
        JogType *outer = jog_type_underlying(target->as.member.base->type);
        if (outer && outer->kind == JOG_TY_PTR) {
            outer = jog_type_underlying(outer->as.elem.elem);
        }
        if (outer && (outer->kind == JOG_TY_STRUCT || outer->kind == JOG_TY_ENUM || outer->kind == JOG_TY_DISTINCT)) {
            error = jog_check_error_type(env->ck);
            jog_check_fail(env->ck, node->pos, "undefined method '%s'", target->as.member.name);
            node->type = error;
            return error;
        }
    }
    callee = check_expr(env, target, NULL);
    if (env->ck->stop) {
        return NULL;
    }
    error = jog_check_error_type(env->ck);
    if (!callee || is_error(callee)) {
        node->type = error;
        return error;
    }
    if (callee->kind != JOG_TY_FN) {
        char buf[160];
        write_type(env->ck, callee, buf, sizeof(buf));
        jog_check_fail(env->ck, node->pos, "cannot call a value of type %s", buf);
        node->type = error;
        return error;
    }
    fn = callee;
    if (target && target->symbol && target->symbol->type_param_count > 0) {
        JogType *inst = jog_check_generic_instantiate(env->ck, env->job, env, node, target, fn, generic_probe);
        if (!inst) {
            return NULL;
        }
        if (inst->kind == JOG_TY_ERROR) {
            node->type = inst;
            return inst;
        }
        fn = inst;
        target->type = inst;
    }
    node->symbol = target ? target->symbol : NULL;
    count = fn->as.fn.param_count;
    variadic = fn->as.fn.variadic;
    pack = pack_slot(target, count);
    saw_elems = 0;
    filled = NULL;
    if (count > 0) {
        filled = ct_arena_alloc(env->ck->program->arena, count, 1);
        if (!filled) {
            jog_check_oom(env->ck);
            return NULL;
        }
        memset(filled, 0, count);
    }
    for (i = 0; i < node->as.call.args.count && !env->ck->stop; i++) {
        JogNode *arg = list_at(node->as.call.args, i);
        JogNode *value;
        JogType *want = NULL;
        int index;
        if (!arg || arg->kind != JOG_NODE_ARG) {
            jog_check_fail(env->ck, arg ? arg->pos : node->pos, "expected an argument");
            continue;
        }
        value = arg->as.arg.value;
        if (arg->as.arg.name) {
            seen_named = true;
            index = param_index_named(target, arg->as.arg.name, count);
            if (index < 0) {
                jog_check_fail(env->ck, arg->pos, "unknown parameter '%s'", arg->as.arg.name);
                if (value) {
                    (void)check_expr(env, value, NULL);
                }
                continue;
            }
            if (filled[index]) {
                jog_check_fail(env->ck, arg->pos, "duplicate argument '%s'", arg->as.arg.name);
                continue;
            }
            if (pack >= 0 && index == pack && saw_elems) {
                jog_check_fail(env->ck, arg->pos, "duplicate argument '%s'", arg->as.arg.name);
                continue;
            }
            filled[index] = 1;
            want = fn->as.fn.params[index];
        } else {
            if (seen_named) {
                jog_check_fail(env->ck, arg->pos, "positional argument after a named argument");
                if (value) {
                    (void)check_expr(env, value, NULL);
                }
                continue;
            }
            if (pack >= 0 && positional >= (size_t)pack) {
                JogType *elem = pack_elem_type(fn, pack);
                if (filled && filled[pack]) {
                    const char *pname = param_name_at(target, (size_t)pack);
                    jog_check_fail(env->ck, arg->pos, "duplicate argument '%s'", pname ? pname : "?");
                    if (value) {
                        (void)check_expr(env, value, NULL);
                    }
                    continue;
                }
                if (!elem) {
                    jog_check_fail(env->ck, arg->pos, "pack is not a slice");
                    if (value) {
                        (void)check_expr(env, value, NULL);
                    }
                    continue;
                }
                saw_elems = 1;
                want = elem;
                positional++;
            } else if (positional >= count) {
                if (!variadic) {
                    jog_check_fail(env->ck, arg->pos, "too many arguments");
                    if (value) {
                        (void)check_expr(env, value, NULL);
                    }
                    continue;
                }
                if (value) {
                    (void)check_expr(env, value, NULL);
                }
                positional++;
                continue;
            } else {
                index = (int)positional;
                positional++;
                filled[index] = 1;
                want = fn->as.fn.params[index];
            }
        }
        if (value) {
            if (node->solved_type_arg_count > 0 && value->type && value->type->kind != JOG_TY_UNTYPED_INT &&
                value->type->kind != JOG_TY_UNTYPED_FLOAT && value->type->kind != JOG_TY_ERROR) {
                arg->type = expect_type(env, value, value->type, want);
            } else {
                arg->type = check_expr(env, value, want);
            }
        }
    }
    if (pack >= 0 && filled) {
        filled[pack] = 1;
    }
    if (node->as.call.trailing && !env->ck->stop) {
        JogNode *lam = node->as.call.trailing;
        JogType *want = NULL;
        JogType *inner = NULL;
        if (count == 0 || !fn->as.fn.params) {
            jog_check_fail(env->ck, lam->pos, "trailing lambda needs a function parameter");
            (void)check_expr(env, lam, NULL);
        } else if (filled && filled[count - 1]) {
            jog_check_fail(env->ck, lam->pos, "trailing lambda does not fit");
            (void)check_expr(env, lam, NULL);
        } else {
            want = fn->as.fn.params[count - 1];
            inner = want ? jog_type_underlying(want) : NULL;
            if (!inner || inner->kind != JOG_TY_FN) {
                jog_check_fail(env->ck, lam->pos, "trailing lambda needs a function parameter");
                (void)check_expr(env, lam, NULL);
            } else {
                if (filled) {
                    filled[count - 1] = 1;
                }
                (void)check_expr(env, lam, want);
            }
        }
    }
    for (i = 0; i < count && !env->ck->stop; i++) {
        JogNode *decl;
        if (filled && filled[i]) {
            continue;
        }
        decl = param_decl(target, i);
        if (decl && decl->kind == JOG_NODE_PARAM && decl->as.param.default_value) {
            continue;
        }
        jog_check_fail(env->ck, node->pos, "missing argument '%s'", param_name_at(target, i) ? param_name_at(target, i) : "?");
    }
    if (env->ck->stop) {
        return NULL;
    }
    if (jog_check_context_resolve(env->ck, env->job, node) < 0) {
        return NULL;
    }
    node->type = fn->as.fn.ret ? fn->as.fn.ret : error;
    return node->type;
}

static JogType *check_cast(Env *env, JogNode *node) {
    JogType *dest;
    JogType *got;
    JogType *error;
    char a[160];
    char b[160];
    if (node->as.cast.op != JOG_TOK_KW_AS) {
        int asq = jog_check_interface_asq(env->ck, env->job, env->scope, node);
        if (asq < 0) {
            return NULL;
        }
        if (asq > 0) {
            return node->type;
        }
        jog_check_fail(env->ck, node->pos, "as? is not checked yet");
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    dest = jog_check_resolve_type(env->ck, env->job, node->as.cast.type);
    if (!dest || env->ck->stop) {
        return dest;
    }
    if (is_error(dest)) {
        node->type = dest;
        return dest;
    }
    if (is_int_lit_tree(node->as.cast.expr) && dest->kind == JOG_TY_INT) {
        got = fold_const(env->ck, node->as.cast.expr, dest);
        if (!got || is_error(got)) {
            node->type = got;
            return got;
        }
        node->type = dest;
        return dest;
    }
    if (jog_float_lit_tree(node->as.cast.expr) && (dest->kind == JOG_TY_FLOAT || dest->kind == JOG_TY_INT)) {
        if (dest->kind == JOG_TY_FLOAT) {
            got = type_float_tree(env, node->as.cast.expr, dest);
        } else {
            double value = 0.0;
            JogFloatLitStatus st = jog_float_lit_eval(node->as.cast.expr, 64, &value);
            if (st != JOG_FLOAT_LIT_OK) {
                return fail_float(env, node, st);
            }
            if (!float_fits_int(dest, value)) {
                jog_check_fail(env->ck, node->pos, "constant does not fit");
                error = jog_check_error_type(env->ck);
                node->type = error;
                return error;
            }
            got = type_float_tree(env, node->as.cast.expr, NULL);
        }
        if (!got || is_error(got)) {
            node->type = got;
            return got;
        }
        node->type = dest;
        return dest;
    }
    if (is_int_lit_tree(node->as.cast.expr) && dest->kind == JOG_TY_FLOAT) {
        got = fold_const(env->ck, node->as.cast.expr, NULL);
        if (!got || is_error(got)) {
            node->type = got;
            return got;
        }
        node->type = dest;
        return dest;
    }
    got = check_expr(env, node->as.cast.expr, NULL);
    if (!got || env->ck->stop) {
        return got;
    }
    if (is_error(got) || (got->kind == JOG_TY_INT && dest->kind == JOG_TY_INT) ||
        (got->kind == JOG_TY_FLOAT && (dest->kind == JOG_TY_FLOAT || dest->kind == JOG_TY_INT)) ||
        (got->kind == JOG_TY_INT && dest->kind == JOG_TY_FLOAT) || same_type(got, dest)) {
        node->type = is_error(got) ? got : dest;
        return node->type;
    }
    if (jog_check_context_cast(env->ck, node, got, dest)) {
        return node->type;
    }
    {
        int iface = jog_check_interface_as(env->ck, node, got, dest);
        if (iface < 0) {
            return NULL;
        }
        if (iface > 0) {
            return node->type;
        }
    }
    if (jog_check_package_cast(env->ck, got, dest)) {
        node->type = dest;
        return dest;
    }
    if (bytes_to_const(got, dest) || slice_to_const(got, dest) || u8_slice_to_str(got, dest) ||
        bytes_n_reinterpret(got, dest)) {
        node->type = dest;
        return dest;
    }
    write_type(env->ck, got, a, sizeof(a));
    write_type(env->ck, dest, b, sizeof(b));
    jog_check_fail(env->ck, node->pos, "cannot cast %s to %s", a, b);
    error = jog_check_error_type(env->ck);
    node->type = error;
    return error;
}

static JogNode *block_tail(JogNode *block) {
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

static JogType *check_block_value(Env *env, JogNode *block, JogType *target) {
    JogBlockScope *inner;
    Env nested;
    size_t i;
    size_t count;
    JogNode *tail;
    JogType *got;
    JogType *error;
    void *mark = NULL;
    if (!block || block->kind != JOG_NODE_BLOCK) {
        jog_check_fail(env->ck, block ? block->pos : (JogPos){0}, "expected a block");
        return jog_check_error_type(env->ck);
    }
    inner = new_scope(env, env->scope);
    if (!inner) {
        return NULL;
    }
    nested = *env;
    nested.scope = inner;
    mark = env->job ? jog_check_context_mark(env->job) : NULL;
    count = block->as.block.stmts.count;
    tail = block_tail(block);
    for (i = 0; i + 1 < count && !nested.ck->stop; i++) {
        check_stmt(&nested, list_at(block->as.block.stmts, i));
    }
    if (nested.ck->stop) {
        if (env->job) {
            jog_check_context_reset(env->job, mark);
        }
        return NULL;
    }
    if (!tail) {
        if (count > 0) {
            check_stmt(&nested, list_at(block->as.block.stmts, count - 1));
        }
        jog_check_fail(env->ck, block->pos, "if branch has no value");
        error = jog_check_error_type(env->ck);
        block->type = error;
        env->assigned = nested.assigned;
        env->reachable = nested.reachable;
        if (env->job) {
            jog_check_context_reset(env->job, mark);
        }
        return error;
    }
    if (!target && is_int_lit_tree(tail)) {
        got = builtin(env->ck, JOG_BUILTIN_UNTYPED_INT);
        if (got) {
            tail->type = got;
        }
    } else if (!target && jog_float_lit_tree(tail)) {
        got = builtin(env->ck, JOG_BUILTIN_UNTYPED_FLOAT);
        if (got) {
            tail->type = got;
        }
    } else {
        got = check_expr(&nested, tail, target);
    }
    env->assigned = nested.assigned;
    env->reachable = nested.reachable;
    block->type = got;
    if (env->job) {
        jog_check_context_reset(env->job, mark);
    }
    return got;
}

static JogType *check_if(Env *env, JogNode *node, JogType *target, bool as_value) {
    JogType *boolean;
    JogType *then_ty = NULL;
    JogType *else_ty = NULL;
    JogType *error;
    AssignSet *entry;
    bool entry_reach;
    AssignSet *then_set;
    bool then_reach;
    AssignSet *else_set;
    bool else_reach;
    JogNode *else_branch;
    JogIfaceNarrow narrow;
    boolean = builtin(env->ck, JOG_BUILTIN_BOOL);
    if (!boolean) {
        return NULL;
    }
    entry = env->assigned;
    entry_reach = env->reachable;
    (void)check_expr(env, node->as.if_.cond, boolean);
    if (env->ck->stop) {
        return NULL;
    }
    env->assigned = entry;
    env->reachable = entry_reach;
    jog_iface_narrow_enter(env->ck, node->as.if_.cond, &narrow);
    if (as_value) {
        then_ty = check_block_value(env, node->as.if_.then_block, target);
    } else if (node->as.if_.then_block) {
        check_block(env, node->as.if_.then_block);
        then_ty = jog_check_unit_type(env->ck);
    }
    jog_iface_narrow_leave(&narrow);
    if (env->ck->stop) {
        return NULL;
    }
    then_set = env->assigned;
    then_reach = env->reachable;
    else_branch = node->as.if_.else_branch;
    env->assigned = entry;
    env->reachable = entry_reach;
    if (else_branch && else_branch->kind == JOG_NODE_IF) {
        else_ty = check_if(env, else_branch, target, as_value);
    } else if (else_branch && else_branch->kind == JOG_NODE_BLOCK) {
        if (as_value) {
            else_ty = check_block_value(env, else_branch, target);
        } else {
            check_block(env, else_branch);
            else_ty = jog_check_unit_type(env->ck);
        }
    } else if (as_value) {
        jog_check_fail(env->ck, node->pos, "if expression requires else");
        else_ty = jog_check_error_type(env->ck);
    }
    if (env->ck->stop) {
        return NULL;
    }
    else_set = env->assigned;
    else_reach = env->reachable;
    if (!else_branch) {
        env->assigned = entry;
        env->reachable = entry_reach;
    } else if (then_reach && else_reach) {
        env->assigned = assign_intersect(env->ck, then_set, else_set);
        env->reachable = true;
    } else if (then_reach) {
        env->assigned = then_set;
        env->reachable = true;
    } else if (else_reach) {
        env->assigned = else_set;
        env->reachable = true;
    } else {
        env->assigned = entry;
        env->reachable = false;
    }
    if (!as_value) {
        node->type = jog_check_unit_type(env->ck);
        return node->type;
    }
    if (!then_ty || !else_ty) {
        return NULL;
    }
    if (is_error(then_ty) || is_error(else_ty)) {
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    if (then_ty->kind == JOG_TY_UNTYPED_INT || else_ty->kind == JOG_TY_UNTYPED_INT) {
        JogNode *then_tail = block_tail(node->as.if_.then_block);
        JogNode *else_tail = else_branch && else_branch->kind == JOG_NODE_BLOCK ? block_tail(else_branch) : NULL;
        if (then_ty->kind == JOG_TY_UNTYPED_INT && else_ty->kind == JOG_TY_INT && then_tail) {
            then_ty = adapt_int(env, then_tail, then_ty, else_ty);
        } else if (else_ty->kind == JOG_TY_UNTYPED_INT && then_ty->kind == JOG_TY_INT && else_tail) {
            else_ty = adapt_int(env, else_tail, else_ty, then_ty);
        } else {
            if (then_ty->kind == JOG_TY_UNTYPED_INT && then_tail) {
                then_ty = adapt_int(env, then_tail, then_ty, target);
            }
            if (else_ty && else_ty->kind == JOG_TY_UNTYPED_INT && else_tail) {
                else_ty = adapt_int(env, else_tail, else_ty, then_ty && then_ty->kind == JOG_TY_INT ? then_ty : target);
            }
        }
    }
    if (then_ty && else_ty && (then_ty->kind == JOG_TY_UNTYPED_FLOAT || else_ty->kind == JOG_TY_UNTYPED_FLOAT)) {
        JogNode *then_tail = block_tail(node->as.if_.then_block);
        JogNode *else_tail = else_branch && else_branch->kind == JOG_NODE_BLOCK ? block_tail(else_branch) : NULL;
        if (then_ty->kind == JOG_TY_UNTYPED_FLOAT && else_ty->kind == JOG_TY_FLOAT && then_tail) {
            then_ty = adapt_float(env, then_tail, then_ty, else_ty);
        } else if (else_ty->kind == JOG_TY_UNTYPED_FLOAT && then_ty->kind == JOG_TY_FLOAT && else_tail) {
            else_ty = adapt_float(env, else_tail, else_ty, then_ty);
        } else {
            if (then_ty->kind == JOG_TY_UNTYPED_FLOAT && then_tail) {
                then_ty = adapt_float(env, then_tail, then_ty, target && target->kind == JOG_TY_FLOAT ? target : NULL);
            }
            if (else_ty && else_ty->kind == JOG_TY_UNTYPED_FLOAT && else_tail) {
                else_ty = adapt_float(env, else_tail, else_ty,
                                      then_ty && then_ty->kind == JOG_TY_FLOAT ? then_ty
                                                                               : (target && target->kind == JOG_TY_FLOAT ? target : NULL));
            }
        }
    }
    if (!then_ty || !else_ty || env->ck->stop) {
        return NULL;
    }
    if (is_error(then_ty) || is_error(else_ty)) {
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    if (!same_type(then_ty, else_ty)) {
        fail_types(env->ck, node->pos, "if branches have types", then_ty, else_ty);
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    if (target && !same_type(then_ty, target)) {
        fail_mismatch(env->ck, node->pos, target, then_ty);
        error = jog_check_error_type(env->ck);
        node->type = error;
        return error;
    }
    node->type = then_ty;
    return then_ty;
}

static void note_assigned(Env *env, JogSymbol *symbol) {
    env->assigned = assign_add(env->ck, env->assigned, symbol);
}

static JogType *binding_type(Env *env, JogNode *node, JogType *annotated) {
    JogType *got = NULL;
    if (node->as.binding.init) {
        got = check_expr(env, node->as.binding.init, annotated && !is_error(annotated) ? annotated : NULL);
        if (env->ck->stop) {
            return NULL;
        }
    }
    if (annotated) {
        return annotated;
    }
    return got;
}

static void check_local_binding(Env *env, JogNode *node) {
    JogType *annotated = NULL;
    JogType *type;
    JogSymbol *symbol;
    const char *name;
    JogSymbolKind kind;
    JogType *error;
    if (node->as.binding.destructure && node->as.binding.names.count > 1) {
        jog_check_error_binding(env->ck, env->job, env->scope, node);
        return;
    }
    if (node->as.binding.names.count != 1) {
        jog_check_fail(env->ck, node->pos, "destructuring is not checked yet");
        return;
    }
    name = node->as.binding.names.items ? node->as.binding.names.items[0] : NULL;
    if (node->as.binding.type) {
        annotated = jog_check_resolve_type(env->ck, env->job, node->as.binding.type);
        if (!annotated || env->ck->stop) {
            return;
        }
    }
    if (node->kind == JOG_NODE_VAL && !node->as.binding.init) {
        jog_check_fail(env->ck, node->pos, "val requires an initializer");
    }
    if (!annotated && !node->as.binding.init) {
        jog_check_fail(env->ck, node->pos, "missing type");
        return;
    }
    type = binding_type(env, node, annotated);
    if (env->ck->stop) {
        return;
    }
    if (!type) {
        error = jog_check_error_type(env->ck);
        type = error;
    }
    if (type && type->kind == JOG_TY_NULL) {
        jog_check_fail(env->ck, node->pos, "null needs an option type");
        type = jog_check_error_type(env->ck);
    }
    kind = node->kind == JOG_NODE_VAL ? JOG_SYM_CONSTANT : JOG_SYM_VARIABLE;
    symbol = bind_local(env, node, kind, name, type);
    node->type = type;
    node->symbol = symbol;
    if (!symbol) {
        return;
    }
    if (node->as.binding.init || jog_check_type_has_zero(type)) {
        note_assigned(env, symbol);
    }
}

static bool assignable(const JogSymbol *symbol) {
    return symbol && (symbol->kind == JOG_SYM_VARIABLE || symbol->kind == JOG_SYM_PARAMETER);
}

static void check_assign(Env *env, JogNode *node) {
    JogNode *target = node->as.assign.target;
    JogTokenKind base;
    JogSymbol *symbol;
    JogType *dest;
    JogType *got;
    JogType *unit;
    if (target && target->kind == JOG_NODE_IDENT && is_discard(target->as.ident.name)) {
        (void)check_expr(env, node->as.assign.value, NULL);
        unit = jog_check_unit_type(env->ck);
        node->type = unit;
        return;
    }
    if (target && target->kind == JOG_NODE_MEMBER) {
        int view = jog_check_package_view_assign(env->ck, env->job, env->scope, node);
        if (view != 0) {
            return;
        }
        jog_check_field_assign(env->ck, env->job, env->scope, node);
        return;
    }
    if (target && (target->kind == JOG_NODE_INDEX ||
                   (target->kind == JOG_NODE_UNARY && target->as.unary.op == JOG_TOK_STAR))) {
        jog_check_memory_assign(env->ck, env->job, env->scope, node);
        return;
    }
    if (!target || target->kind != JOG_NODE_IDENT) {
        jog_check_fail(env->ck, node->pos, "assignment target is not checked yet");
        if (node->as.assign.value) {
            (void)check_expr(env, node->as.assign.value, NULL);
        }
        return;
    }
    symbol = lookup_name(env, target->as.ident.name);
    if (!symbol) {
        jog_check_fail(env->ck, target->pos, "undefined name '%s'", target->as.ident.name ? target->as.ident.name : "?");
        if (node->as.assign.value) {
            (void)check_expr(env, node->as.assign.value, NULL);
        }
        return;
    }
    target->symbol = symbol;
    if (!jog_check_wait(env->ck, env->job, symbol)) {
        return;
    }
    dest = symbol->type;
    target->type = dest;
    if (!assignable(symbol)) {
        jog_check_fail(env->ck, target->pos, "cannot assign to '%s'", symbol->name ? symbol->name : "?");
        if (node->as.assign.value) {
            (void)check_expr(env, node->as.assign.value, NULL);
        }
        return;
    }
    base = compound_base(node->as.assign.op);
    if ((base == JOG_TOK_PLUS || base == JOG_TOK_MINUS) && dest && dest->kind == JOG_TY_PTR) {
        read_var(env, target, symbol);
        if (jog_check_memory_ptr_compound(env->ck, env->job, env->scope, node->as.assign.value, dest) < 0 || env->ck->stop) {
            return;
        }
        note_assigned(env, symbol);
        node->type = jog_check_unit_type(env->ck);
        return;
    }
    if (base != JOG_TOK_EOF) {
        read_var(env, target, symbol);
        got = check_expr(env, node->as.assign.value, dest);
        if (!got || env->ck->stop) {
            return;
        }
        if (!is_error(got) && dest && !is_error(dest) && dest->kind != JOG_TY_INT) {
            if (dest->kind != JOG_TY_FLOAT || (base != JOG_TOK_PLUS && base != JOG_TOK_MINUS && base != JOG_TOK_STAR &&
                                               base != JOG_TOK_SLASH && base != JOG_TOK_PERCENT)) {
                fail_op(env->ck, node->pos, base, dest);
            }
        }
    } else if (node->as.assign.op == JOG_TOK_ASSIGN) {
        (void)check_expr(env, node->as.assign.value, dest);
    } else {
        jog_check_fail(env->ck, node->pos, "operator '%s' is not checked yet", jog_token_spelling(node->as.assign.op));
        return;
    }
    if (env->ck->stop) {
        return;
    }
    note_assigned(env, symbol);
    node->type = jog_check_unit_type(env->ck);
}

static void check_return(Env *env, JogNode *node) {
    JogType *ret = env->ret;
    if (env->job && env->job->in_go) {
        jog_check_coro_return(env->ck, env->job, env->scope, node);
        env->reachable = false;
        return;
    }
    JogType *unit;
    size_t count = node->as.ret.values.count;
    unit = jog_check_unit_type(env->ck);
    if (!ret) {
        ret = unit;
    }
    if (count > 1) {
        jog_check_error_return(env->ck, env->job, env->scope, node, ret);
        env->reachable = false;
        return;
    }
    if (count == 0) {
        if (ret && !is_error(ret) && ret->kind != JOG_TY_UNIT) {
            jog_check_fail(env->ck, node->pos, "missing return value");
        }
        node->type = unit;
        env->reachable = false;
        return;
    }
    if (ret && ret->kind == JOG_TY_UNIT && !is_error(ret)) {
        (void)check_expr(env, list_at(node->as.ret.values, 0), NULL);
        jog_check_fail(env->ck, node->pos, "unexpected return value");
        env->reachable = false;
        return;
    }
    (void)check_expr(env, list_at(node->as.ret.values, 0), ret);
    node->type = ret;
    env->reachable = false;
}

static bool is_range_expr(const JogNode *node) {
    return node && node->kind == JOG_NODE_BINARY && node->as.binary.op == JOG_TOK_DOTDOT;
}

/* Index and element symbols for a collection for. count is 1 (element)
   or 2 (index, element). A discarded name is NULL. */
static bool remember_for(Env *env, JogNode *node, JogSymbol *first, JogSymbol *second, size_t count) {
    JogSymbol **syms;
    if (count == 0) {
        node->bound_symbols = NULL;
        node->bound_symbol_count = 0;
        return true;
    }
    syms = ct_arena_alloc(env->ck->program->arena, count * sizeof(JogSymbol *), _Alignof(JogSymbol *));
    if (!syms) {
        jog_check_oom(env->ck);
        return false;
    }
    syms[0] = first;
    if (count > 1) {
        syms[1] = second;
    }
    node->bound_symbols = syms;
    node->bound_symbol_count = count;
    return true;
}

static void bind_iter_name(Env *env, JogNode *at, const char *name, JogType *type, JogSymbol **out) {
    *out = NULL;
    if (!name || is_discard(name) || !type || is_error(type)) {
        return;
    }
    *out = bind_local(env, at, JOG_SYM_CONSTANT, name, type);
    if (*out) {
        env->assigned = assign_add(env->ck, env->assigned, *out);
    }
}

/* Element of an array, slice, str, bytes, or bytes[N]. False when base
   is not one of those. bytes[1] is the element of every bytes form. */
static bool collection_elem(Env *env, JogType *base, JogType **elem) {
    *elem = NULL;
    if (!base || is_error(base)) {
        return false;
    }
    if (base->kind == JOG_TY_ARRAY) {
        *elem = base->as.array.elem;
        return *elem != NULL;
    }
    if (base->kind == JOG_TY_SLICE) {
        *elem = base->as.elem.elem;
        return *elem != NULL;
    }
    if (base->kind == JOG_TY_BYTES || base->kind == JOG_TY_BYTES_N) {
        *elem = jog_type_bytes_n(env->ck->program->types, 1);
        if (!*elem) {
            jog_check_oom(env->ck);
            return false;
        }
        return true;
    }
    return false;
}

static void check_collection(Env *env, JogNode *node, JogType **elem_out) {
    JogType *base;
    JogType *elem = NULL;
    JogNode *expr = node->as.for_.expr;
    *elem_out = NULL;
    if (!expr || is_range_expr(expr)) {
        jog_check_fail(env->ck, expr ? expr->pos : node->pos, "for-in over a collection is not checked yet");
        *elem_out = jog_check_error_type(env->ck);
        return;
    }
    base = check_expr(env, expr, NULL);
    if (!base || env->ck->stop) {
        return;
    }
    if (is_error(base)) {
        *elem_out = base;
        return;
    }
    if (!collection_elem(env, base, &elem)) {
        int ch;
        if (env->ck->stop) {
            return;
        }
        ch = jog_check_chan_for(env->ck, node, base, &elem);
        if (ch < 0 || env->ck->stop) {
            *elem_out = elem;
            return;
        }
        if (ch > 0) {
            *elem_out = elem;
            return;
        }
        jog_check_fail(env->ck, expr->pos, "for-in over a collection is not checked yet");
        *elem_out = jog_check_error_type(env->ck);
        return;
    }
    *elem_out = elem;
}

static void unify_range(Env *env, JogNode *range, JogType **out) {
    JogType *lt;
    JogType *rt;
    JogType *error;
    if (!range || range->kind != JOG_NODE_BINARY || range->as.binary.op != JOG_TOK_DOTDOT) {
        jog_check_fail(env->ck, range ? range->pos : (JogPos){0}, "for-in over a collection is not checked yet");
        *out = jog_check_error_type(env->ck);
        return;
    }
    lt = operand_type(env, range->as.binary.lhs);
    rt = operand_type(env, range->as.binary.rhs);
    if (!lt || !rt || env->ck->stop) {
        *out = NULL;
        return;
    }
    if (lt->kind == JOG_TY_UNTYPED_INT && rt->kind == JOG_TY_INT) {
        lt = adapt_int(env, range->as.binary.lhs, lt, rt);
    } else if (rt->kind == JOG_TY_UNTYPED_INT && lt->kind == JOG_TY_INT) {
        rt = adapt_int(env, range->as.binary.rhs, rt, lt);
    } else {
        if (lt->kind == JOG_TY_UNTYPED_INT) {
            lt = adapt_int(env, range->as.binary.lhs, lt, NULL);
        }
        if (rt && rt->kind == JOG_TY_UNTYPED_INT) {
            rt = adapt_int(env, range->as.binary.rhs, rt, lt && lt->kind == JOG_TY_INT ? lt : NULL);
        }
    }
    if (!lt || !rt || env->ck->stop) {
        *out = NULL;
        return;
    }
    if (is_error(lt) || is_error(rt)) {
        *out = jog_check_error_type(env->ck);
        return;
    }
    if (lt->kind != JOG_TY_INT || !same_type(lt, rt)) {
        if (lt->kind != JOG_TY_INT) {
            fail_op(env->ck, range->pos, JOG_TOK_DOTDOT, lt);
        } else {
            fail_types(env->ck, range->pos, "mismatched types", lt, rt);
        }
        error = jog_check_error_type(env->ck);
        range->type = error;
        *out = error;
        return;
    }
    range->type = lt;
    *out = lt;
}

static void check_for(Env *env, JogNode *node) {
    JogType *boolean;
    JogType *elem = NULL;
    AssignSet *entry;
    bool entry_reach;
    Loop loop;
    Env body;
    JogBlockScope *loop_scope;
    JogSymbol *symbol;
    bool collection = false;
    boolean = builtin(env->ck, JOG_BUILTIN_BOOL);
    if (!boolean) {
        return;
    }
    entry = env->assigned;
    entry_reach = env->reachable;
    memset(&loop, 0, sizeof(loop));
    loop.parent = env->loop;
    loop.forever = node->as.for_.form == JOG_FOR_FOREVER;
    if (node->as.for_.form == JOG_FOR_COND) {
        (void)check_expr(env, node->as.for_.expr, boolean);
    } else if (node->as.for_.form == JOG_FOR_IN) {
        if (!node->as.for_.second && is_range_expr(node->as.for_.expr)) {
            unify_range(env, node->as.for_.expr, &elem);
        } else {
            check_collection(env, node, &elem);
            collection = true;
        }
    }
    if (env->ck->stop) {
        return;
    }
    loop_scope = new_scope(env, env->scope);
    if (!loop_scope) {
        return;
    }
    body = *env;
    body.scope = loop_scope;
    body.loop = &loop;
    body.assigned = entry;
    body.reachable = true;
    if (node->as.for_.form == JOG_FOR_IN && elem && !is_error(elem)) {
        if (collection) {
            JogSymbol *index_sym = NULL;
            JogSymbol *elem_sym = NULL;
            if (node->as.for_.second) {
                JogType *usize;
                if (!is_discard(node->as.for_.first)) {
                    usize = builtin(env->ck, JOG_BUILTIN_USIZE);
                    if (!usize) {
                        return;
                    }
                    bind_iter_name(&body, node, node->as.for_.first, usize, &index_sym);
                }
                bind_iter_name(&body, node, node->as.for_.second, elem, &elem_sym);
                if (env->ck->stop || !remember_for(&body, node, index_sym, elem_sym, 2)) {
                    return;
                }
                node->symbol = index_sym ? index_sym : elem_sym;
            } else {
                bind_iter_name(&body, node, node->as.for_.first, elem, &elem_sym);
                if (env->ck->stop || !remember_for(&body, node, elem_sym, NULL, 1)) {
                    return;
                }
                node->symbol = elem_sym;
            }
        } else if (node->as.for_.first && !is_discard(node->as.for_.first)) {
            symbol = bind_local(&body, node, JOG_SYM_CONSTANT, node->as.for_.first, elem);
            node->symbol = symbol;
            if (symbol) {
                body.assigned = assign_add(env->ck, body.assigned, symbol);
            }
        }
    }
    if (node->as.for_.body) {
        check_block(&body, node->as.for_.body);
    }
    env->loop = loop.parent;
    if (loop.forever && loop.any_break) {
        env->assigned = loop.breaks;
        env->reachable = entry_reach;
    } else if (loop.forever) {
        env->assigned = entry;
        env->reachable = false;
    } else {
        env->assigned = entry;
        env->reachable = entry_reach;
    }
    node->type = jog_check_unit_type(env->ck);
}

static void check_block(Env *env, JogNode *block) {
    JogBlockScope *inner;
    Env nested;
    size_t i;
    void *mark = NULL;
    if (!block || block->kind != JOG_NODE_BLOCK) {
        jog_check_fail(env->ck, block ? block->pos : (JogPos){0}, "expected a block");
        return;
    }
    inner = new_scope(env, env->scope);
    if (!inner) {
        return;
    }
    nested = *env;
    nested.scope = inner;
    if (env->job) {
        mark = jog_check_context_mark(env->job);
    }
    for (i = 0; i < block->as.block.stmts.count && !nested.ck->stop; i++) {
        check_stmt(&nested, list_at(block->as.block.stmts, i));
    }
    if (env->job) {
        jog_check_context_reset(env->job, mark);
    }
    env->assigned = nested.assigned;
    env->reachable = nested.reachable;
    block->type = jog_check_unit_type(env->ck);
}

static void check_stmt_raw(Env *env, JogNode *stmt) {
    JogType *unit;
    if (!stmt || env->ck->stop) {
        return;
    }
    switch (stmt->kind) {
    case JOG_NODE_VAL:
    case JOG_NODE_VAR:
        check_local_binding(env, stmt);
        return;
    case JOG_NODE_CONTEXT:
        (void)jog_check_context_binding(env->ck, env->job, stmt);
        return;
    case JOG_NODE_ASSIGN:
        check_assign(env, stmt);
        return;
    case JOG_NODE_RETURN:
        check_return(env, stmt);
        return;
    case JOG_NODE_FOR:
        check_for(env, stmt);
        return;
    case JOG_NODE_BREAK:
        if (!env->loop) {
            jog_check_fail(env->ck, stmt->pos, "break outside of a loop");
        } else if (!env->loop->any_break) {
            env->loop->breaks = env->assigned;
            env->loop->any_break = true;
        } else {
            env->loop->breaks = assign_intersect(env->ck, env->loop->breaks, env->assigned);
        }
        env->reachable = false;
        return;
    case JOG_NODE_CONTINUE:
        if (!env->loop) {
            jog_check_fail(env->ck, stmt->pos, "continue outside of a loop");
        }
        env->reachable = false;
        return;
    case JOG_NODE_EXPR_STMT:
        if (stmt->as.expr_stmt.expr && stmt->as.expr_stmt.expr->kind == JOG_NODE_WHEN) {
            (void)jog_check_enum_when(env->ck, env->job, env->scope, stmt->as.expr_stmt.expr, NULL, false);
        } else if (stmt->as.expr_stmt.expr && stmt->as.expr_stmt.expr->kind == JOG_NODE_IF) {
            (void)check_if(env, stmt->as.expr_stmt.expr, NULL, false);
        } else {
            (void)check_expr(env, stmt->as.expr_stmt.expr, NULL);
        }
        unit = jog_check_unit_type(env->ck);
        stmt->type = unit;
        return;
    case JOG_NODE_DESTRUCTURE:
        jog_check_error_destructure(env->ck, env->job, env->scope, stmt);
        return;
    case JOG_NODE_DEFER:
    case JOG_NODE_ERRDEFER:
        jog_check_error_defer(env->ck, env->job, env->scope, stmt, env->ret);
        return;
    case JOG_NODE_WITH: {
        int pushed = jog_check_context_push_with(env->ck, env->job, stmt);
        void *mark;
        if (pushed < 0) {
            return;
        }
        mark = jog_check_context_mark(env->job);
        if (stmt->as.with.body) {
            if (stmt->as.with.body->kind == JOG_NODE_BLOCK) {
                check_block(env, stmt->as.with.body);
            } else {
                check_stmt(env, stmt->as.with.body);
            }
        }
        jog_check_context_reset(env->job, mark);
        if (pushed > 0) {
            jog_check_context_pop(env->job);
        }
        return;
    }
    case JOG_NODE_ATTR_BLOCK:
        if (jog_check_memory_attr(env->ck, stmt) < 0) {
            return;
        }
        if (stmt->as.attr_block.body) {
            if (stmt->as.attr_block.body->kind == JOG_NODE_BLOCK) {
                check_block(env, stmt->as.attr_block.body);
            } else {
                check_stmt(env, stmt->as.attr_block.body);
            }
        }
        stmt->type = jog_check_unit_type(env->ck);
        return;
    case JOG_NODE_BLOCK:
        check_block(env, stmt);
        return;
    default:
        jog_check_fail(env->ck, stmt->pos, "statement is not checked yet");
        return;
    }
}

/* One level only. i32 converts to i32?, and the literal is folded as i32.
   i32 does not convert to i32??, so an option element is left alone. */
static JogType *literal_target(JogType *target) {
    JogType *elem;
    if (!target || target->kind != JOG_TY_OPTION) {
        return target;
    }
    elem = target->as.elem.elem;
    if (!elem) {
        return target;
    }
    if (elem->kind == JOG_TY_INT || elem->kind == JOG_TY_FLOAT || elem->kind == JOG_TY_BOOL ||
        elem->kind == JOG_TY_UNTYPED_INT || elem->kind == JOG_TY_UNTYPED_FLOAT) {
        return elem;
    }
    return target;
}

static JogType *check_expr_raw(Env *env, JogNode *node, JogType *target) {
    JogType *got = NULL;
    if (!node || env->ck->stop) {
        return NULL;
    }
    if (is_bool_lit_tree(node)) {
        JogType *fold = literal_target(target);
        if (fold && !is_error(fold) && fold->kind != JOG_TY_BOOL) {
            got = fold_const(env->ck, node, fold);
        } else {
            got = fold_const(env->ck, node, NULL);
        }
        if (!got || env->ck->stop || is_error(got)) {
            if (got) {
                node->type = got;
            }
            return got;
        }
        node->type = got;
        return expect_type(env, node, got, target);
    }
    if (is_int_lit_tree(node)) {
        JogType *fold = literal_target(target);
        if (fold && fold->kind == JOG_TY_INT) {
            got = fold_const(env->ck, node, fold);
        } else if (fold && !is_error(fold) && fold->kind != JOG_TY_UNTYPED_INT) {
            got = fold_const(env->ck, node, fold);
        } else {
            got = fold_const(env->ck, node, NULL);
        }
        if (!got || env->ck->stop || is_error(got)) {
            if (got) {
                node->type = got;
            }
            return got;
        }
        node->type = got;
        return expect_type(env, node, got, target);
    }
    if (jog_float_lit_tree(node)) {
        JogType *fold = literal_target(target);
        if (fold && fold->kind == JOG_TY_FLOAT) {
            got = type_float_tree(env, node, fold);
        } else if (fold && !is_error(fold) && fold->kind != JOG_TY_UNTYPED_FLOAT) {
            jog_check_fail(env->ck, node->pos, "constant does not fit");
            got = jog_check_error_type(env->ck);
            node->type = got;
            return got;
        } else {
            got = type_float_tree(env, node, NULL);
        }
        if (!got || env->ck->stop || is_error(got)) {
            return got;
        }
        node->type = got;
        return expect_type(env, node, got, target);
    }
    switch (node->kind) {
    case JOG_NODE_IDENT:
        got = check_ident(env, node);
        break;
    case JOG_NODE_MEMBER:
        if (node->as.member.safe) {
            got = check_safe_member(env, node);
            break;
        }
        {
            int en = jog_check_enum_member(env->ck, env->job, env->scope, node, &got);
            if (en < 0 || env->ck->stop) {
                return NULL;
            }
            if (en > 0) {
                break;
            }
        }
        got = check_member(env, node);
        break;
    case JOG_NODE_UNARY:
        got = check_unary(env, node);
        break;
    case JOG_NODE_FORCE:
        got = jog_check_enum_force(env->ck, env->job, env->scope, node);
        break;
    case JOG_NODE_BINARY:
        if (node->as.binary.op == JOG_TOK_ELVIS) {
            got = jog_check_enum_elvis(env->ck, env->job, env->scope, node);
            break;
        }
        got = check_binary(env, node);
        break;
    case JOG_NODE_CALL: {
        int en = jog_check_enum_call(env->ck, env->job, env->scope, node, target, &got);
        int panic;
        if (en < 0 || env->ck->stop) {
            return NULL;
        }
        if (en > 0) {
            break;
        }
        panic = jog_check_error_call(env->ck, env->job, env->scope, node, target);
        if (panic < 0 || env->ck->stop) {
            return NULL;
        }
        if (panic > 0) {
            got = node->type;
            break;
        }
        {
            int sized = check_size_call(env, node);
            if (sized == 0) {
                sized = check_align_call(env, node);
            }
            if (sized < 0 || env->ck->stop) {
                return NULL;
            }
            if (sized > 0) {
                got = node->type;
                break;
            }
        }
        got = check_call(env, node);
        break;
    }
    case JOG_NODE_CAST: {
        int en = jog_check_enum_cast(env->ck, env->job, env->scope, node, &got);
        if (en < 0 || env->ck->stop) {
            return NULL;
        }
        if (en > 0) {
            break;
        }
        got = check_cast(env, node);
        break;
    }
    case JOG_NODE_IS: {
        int iface = jog_check_interface_is(env->ck, env->job, env->scope, node, &got);
        if (iface < 0 || env->ck->stop) {
            return NULL;
        }
        if (iface == 0) {
            got = jog_check_enum_is(env->ck, env->job, env->scope, node);
        }
        break;
    }
    case JOG_NODE_DOT_VARIANT:
        got = jog_check_enum_dot(env->ck, env->job, node, target);
        break;
    case JOG_NODE_WHEN:
        got = jog_check_enum_when(env->ck, env->job, env->scope, node, target, true);
        break;
    case JOG_NODE_IF:
        got = check_if(env, node, target, true);
        break;
    case JOG_NODE_BLOCK:
        got = check_block_value(env, node, target);
        if (got && got->kind == JOG_TY_UNTYPED_INT) {
            JogNode *tail = block_tail(node);
            if (tail) {
                got = adapt_int(env, tail, got, target && target->kind == JOG_TY_INT ? target : NULL);
            }
        }
        if (got && got->kind == JOG_TY_UNTYPED_FLOAT) {
            JogNode *tail = block_tail(node);
            if (tail) {
                got = adapt_float(env, tail, got, target && target->kind == JOG_TY_FLOAT ? target : NULL);
            }
        }
        break;
    case JOG_NODE_STRING_LIT: {
        JogType *str = builtin(env->ck, JOG_BUILTIN_CONST_BYTES);
        got = str;
        node->type = str;
        break;
    }
    case JOG_NODE_CSTRING_LIT:
        got = cstring_type(env->ck);
        node->type = got;
        break;
    case JOG_NODE_BOOL_LIT:
        got = builtin(env->ck, JOG_BUILTIN_BOOL);
        node->type = got;
        break;
    case JOG_NODE_NULL_LIT:
        got = jog_check_error_null(env->ck, node, target);
        break;
    case JOG_NODE_FLOAT_LIT:
        got = type_float_tree(env, node, target && target->kind == JOG_TY_FLOAT ? target : NULL);
        break;
    case JOG_NODE_CHAR_LIT:
        got = type_char(env, node, target);
        break;
    case JOG_NODE_INTERP:
        got = jog_check_string_interp(env->ck, env->job, env->scope, node);
        break;
    case JOG_NODE_INDEX:
        got = jog_check_memory_index(env->ck, env->job, env->scope, node);
        break;
    case JOG_NODE_SLICE:
        got = jog_check_memory_slice(env->ck, env->job, env->scope, node);
        break;
    case JOG_NODE_ARRAY_LIT:
        got = jog_check_memory_array_lit(env->ck, env->job, env->scope, node);
        break;
    case JOG_NODE_TUPLE:
        got = jog_check_error_tuple(env->ck, env->job, env->scope, node, target);
        break;
    case JOG_NODE_LAMBDA:
        got = check_lambda(env, node, target);
        break;
    case JOG_NODE_GO:
        got = jog_check_coro_go(env->ck, env->job, env->scope, node);
        break;
    case JOG_NODE_BUILTIN:
        got = jog_check_comptime(env->ck, env->job, env->scope, node, target);
        break;
    default:
        jog_check_fail(env->ck, node->pos, "expression is not checked yet");
        got = jog_check_error_type(env->ck);
        node->type = got;
        break;
    }
    if (!got || env->ck->stop) {
        return got;
    }
    if (is_error(got)) {
        node->type = got;
        return got;
    }
    got = expect_type(env, node, got, target);
    if (got) {
        node->type = got;
    }
    return got;
}

static void bind_params(Env *env, JogList params) {
    size_t i;
    for (i = 0; i < params.count && !env->ck->stop; i++) {
        JogNode *param = list_at(params, i);
        JogSymbol *symbol;
        if (!param || param->kind != JOG_NODE_PARAM) {
            continue;
        }
        if (is_discard(param->as.param.name)) {
            continue;
        }
        symbol = bind_local(env, param, JOG_SYM_PARAMETER, param->as.param.name, param->type);
        param->symbol = symbol;
        if (symbol) {
            note_assigned(env, symbol);
        }
        if (param->as.param.default_value && param->type) {
            (void)check_expr(env, param->as.param.default_value, param->type);
        }
    }
}

/* A return inside a nested lambda belongs to that lambda. */
static bool node_has_return(const JogNode *node) {
    size_t i;
    if (!node) {
        return false;
    }
    switch (node->kind) {
    case JOG_NODE_RETURN:
        return true;
    case JOG_NODE_LAMBDA:
        return false;
    case JOG_NODE_BLOCK:
        for (i = 0; i < node->as.block.stmts.count; i++) {
            if (node_has_return(list_at(node->as.block.stmts, i))) {
                return true;
            }
        }
        return false;
    case JOG_NODE_EXPR_STMT:
        return node_has_return(node->as.expr_stmt.expr);
    case JOG_NODE_IF:
        return node_has_return(node->as.if_.then_block) || node_has_return(node->as.if_.else_branch);
    case JOG_NODE_FOR:
        return node_has_return(node->as.for_.body);
    case JOG_NODE_WHEN:
        for (i = 0; i < node->as.when.arms.count; i++) {
            JogNode *arm = list_at(node->as.when.arms, i);
            if (arm && node_has_return(arm->as.when_arm.body)) {
                return true;
            }
        }
        return false;
    case JOG_NODE_DEFER:
    case JOG_NODE_ERRDEFER:
        return node_has_return(node->as.defer.body);
    case JOG_NODE_WITH:
        return node_has_return(node->as.with.body);
    case JOG_NODE_ATTR_BLOCK:
        return node_has_return(node->as.attr_block.body);
    default:
        return false;
    }
}

/* The target function type's context parameters become this lambda's own.
   Calls in the body resolve them before the enclosing function's contexts.
   The symbols stay on the node so lowering can bind the parameter locals. */
static int push_lambda_contexts(Env *env, JogNode *node, JogType *fn_target) {
    size_t n;
    size_t i;
    JogSymbol **symbols;
    JogType **types;
    JogPackage *package;
    if (!fn_target || fn_target->as.fn.context_count == 0) {
        return 1;
    }
    if (!env->job) {
        jog_check_fail(env->ck, node->pos, "a lambda cannot take a context parameter yet");
        return 0;
    }
    n = fn_target->as.fn.context_count;
    if (!fn_target->as.fn.contexts) {
        jog_check_fail(env->ck, node->pos, "missing context parameter");
        return 0;
    }
    symbols = ct_arena_alloc(env->ck->program->arena, n * sizeof(*symbols), _Alignof(JogSymbol *));
    types = ct_arena_alloc(env->ck->program->arena, n * sizeof(*types), _Alignof(JogType *));
    if (!symbols || !types) {
        jog_check_oom(env->ck);
        return 0;
    }
    memset(symbols, 0, n * sizeof(*symbols));
    memset(types, 0, n * sizeof(*types));
    package = env->job->package;
    for (i = 0; i < n && !env->ck->stop; i++) {
        char buf[32];
        const char *name;
        JogType *ty = fn_target->as.fn.contexts[i];
        if (!ty || is_error(ty)) {
            jog_check_fail(env->ck, node->pos, "missing context parameter");
            return 0;
        }
        snprintf(buf, sizeof(buf), "ctx%zu", i);
        name = ct_intern_cstr(env->ck->program->intern, buf);
        if (!name) {
            jog_check_oom(env->ck);
            return 0;
        }
        symbols[i] = jog_symbol_new(env->ck->program->arena, JOG_SYM_CONTEXT, name, node, ty, package);
        if (!symbols[i]) {
            jog_check_oom(env->ck);
            return 0;
        }
        types[i] = ty;
    }
    if (env->ck->stop) {
        return 0;
    }
    node->context_symbols = symbols;
    node->context_symbol_count = n;
    jog_check_context_push_params(env->ck, env->job, symbols, types, n);
    return !env->ck->stop;
}

/* The block value is the last expression. A unit lambda discards that
   tail, so () -> { xs.pop() } has type () -> (). return leaves this
   lambda only. */
static JogType *check_lambda(Env *env, JogNode *node, JogType *target) {
    JogType *fn_target = NULL;
    JogType *error;
    JogType *ret = NULL;
    JogType *annotated = NULL;
    JogType **params = NULL;
    JogType *built;
    JogType *tail_ty = NULL;
    Env nested;
    JogNode *body;
    JogNode *last = NULL;
    size_t nparam = 0;
    size_t i;
    size_t limit;
    bool tail = false;
    bool saved_go = false;
    void *mark = NULL;
    error = jog_check_error_type(env->ck);
    if (target && !is_error(target) && target->kind == JOG_TY_FN) {
        fn_target = target;
    }
    if (fn_target && fn_target->as.fn.variadic) {
        jog_check_fail(env->ck, node->pos, "a lambda cannot be variadic yet");
        node->type = error;
        return error;
    }
    body = node->as.lambda.body;
    if (!body) {
        jog_check_fail(env->ck, node->pos, "lambda has no body");
        node->type = error;
        return error;
    }
    nested = *env;
    nested.scope = new_scope(env, env->scope);
    if (!nested.scope) {
        return NULL;
    }
    nested.lambda_scope = nested.scope;
    nested.loop = NULL;
    nested.reachable = true;
    if (node->as.lambda.implicit_it) {
        JogSymbol *symbol;
        /* `{ body }` on () -> R is a lambda with no parameters. Implicit
           `it` is only the one-parameter case. */
        if (fn_target && fn_target->as.fn.param_count == 0) {
            node->as.lambda.implicit_it = false;
            nparam = 0;
        } else if (!fn_target || fn_target->as.fn.param_count != 1 || !fn_target->as.fn.params) {
            jog_check_fail(env->ck, node->pos, "implicit 'it' needs a function type with one parameter");
            node->type = error;
            return error;
        } else {
            nparam = 1;
            params = ct_arena_alloc(env->ck->program->arena, sizeof(*params), _Alignof(JogType *));
            if (!params) {
                jog_check_oom(env->ck);
                return NULL;
            }
            params[0] = fn_target->as.fn.params[0];
            symbol = bind_local(&nested, node, JOG_SYM_PARAMETER, "it", params[0]);
            node->symbol = symbol;
            if (symbol) {
                note_assigned(&nested, symbol);
            }
        }
    } else {
        nparam = node->as.lambda.params.count;
        if (fn_target && nparam != fn_target->as.fn.param_count) {
            jog_check_fail(env->ck, node->pos, "lambda parameter count does not match the function type");
            node->type = error;
            return error;
        }
        if (nparam > 0) {
            params = ct_arena_alloc(env->ck->program->arena, nparam * sizeof(*params), _Alignof(JogType *));
            if (!params) {
                jog_check_oom(env->ck);
                return NULL;
            }
        }
        for (i = 0; i < nparam && !env->ck->stop; i++) {
            JogNode *param = list_at(node->as.lambda.params, i);
            JogType *ty = NULL;
            JogType *want = NULL;
            if (!param || param->kind != JOG_NODE_PARAM) {
                jog_check_fail(env->ck, node->pos, "expected a parameter");
                node->type = error;
                return error;
            }
            if (fn_target && fn_target->as.fn.params) {
                want = fn_target->as.fn.params[i];
            }
            if (param->as.param.type) {
                ty = jog_check_resolve_type(env->ck, env->job, param->as.param.type);
                if (!ty || env->ck->stop) {
                    return NULL;
                }
            } else if (want) {
                ty = want;
            } else {
                jog_check_fail(env->ck, param->pos, "lambda parameter needs a type");
                node->type = error;
                return error;
            }
            if (want && ty && !same_type(ty, want)) {
                jog_check_fail(env->ck, param->pos, "lambda parameter type does not match the function type");
                node->type = error;
                return error;
            }
            param->type = ty;
            params[i] = ty;
        }
        if (env->ck->stop) {
            return NULL;
        }
        bind_params(&nested, node->as.lambda.params);
    }
    if (node->as.lambda.ret) {
        annotated = jog_check_resolve_type(env->ck, env->job, node->as.lambda.ret);
        if (!annotated || env->ck->stop) {
            return NULL;
        }
    }
    if (fn_target) {
        if (annotated && !same_type(annotated, fn_target->as.fn.ret)) {
            jog_check_fail(env->ck, node->pos, "lambda result type does not match the function type");
            node->type = error;
            return error;
        }
        ret = fn_target->as.fn.ret;
    } else {
        ret = annotated;
    }
    if (!ret && node_has_return(body)) {
        jog_check_fail(env->ck, node->pos, "lambda result needs a type");
        ret = error;
    }
    nested.ret = ret;
    if (env->job) {
        saved_go = env->job->in_go;
        env->job->in_go = false;
        mark = jog_check_context_mark(env->job);
        if (!push_lambda_contexts(env, node, fn_target)) {
            jog_check_context_reset(env->job, mark);
            env->job->in_go = saved_go;
            if (env->ck->stop) {
                return NULL;
            }
            node->type = error;
            return error;
        }
    } else if (fn_target && fn_target->as.fn.context_count > 0) {
        jog_check_fail(env->ck, node->pos, "a lambda cannot take a context parameter yet");
        node->type = error;
        return error;
    }
    if (node->as.lambda.expr_body) {
        if (ret && !is_error(ret) && ret->kind == JOG_TY_UNIT) {
            (void)check_expr(&nested, body, NULL);
        } else {
            tail_ty = check_expr(&nested, body, ret);
            if (!ret) {
                ret = tail_ty;
            }
        }
    } else if (body->kind != JOG_NODE_BLOCK) {
        jog_check_fail(env->ck, body->pos, "expected a block");
        ret = error;
    } else {
        limit = body->as.block.stmts.count;
        if (limit > 0) {
            last = list_at(body->as.block.stmts, limit - 1);
            tail = last && last->kind == JOG_NODE_EXPR_STMT;
            if (tail && ret && !is_error(ret) && ret->kind == JOG_TY_UNIT) {
                tail = false;
            }
            if (tail) {
                limit--;
            }
        }
        for (i = 0; i < limit && !nested.ck->stop; i++) {
            check_stmt(&nested, list_at(body->as.block.stmts, i));
        }
        if (tail && last && !nested.ck->stop) {
            tail_ty = check_expr(&nested, last->as.expr_stmt.expr, ret);
            if (!ret) {
                ret = tail_ty;
            }
        } else if (!ret) {
            ret = jog_type_builtin(env->ck->program->types, JOG_BUILTIN_UNIT);
        }
        if (nested.reachable && ret && !is_error(ret) && ret->kind != JOG_TY_UNIT && !tail && !nested.ck->stop) {
            jog_check_fail(env->ck, node->pos, "missing return");
        }
    }
    if (env->job) {
        jog_check_context_reset(env->job, mark);
        env->job->in_go = saved_go;
    }
    if (env->ck->stop) {
        return NULL;
    }
    if (!ret || is_error(ret)) {
        node->type = error;
        return error;
    }
    built = jog_type_fn(env->ck->program->types, params, nparam, ret,
                        fn_target ? fn_target->as.fn.contexts : NULL,
                        fn_target ? fn_target->as.fn.context_count : 0, false,
                        fn_target && fn_target->as.fn.escaping, false);
    if (!built) {
        jog_check_oom(env->ck);
        return NULL;
    }
    node->type = built;
    return built;
}

static void bind_context_params(Env *env, JogNode *fn) {
    size_t n;
    size_t i;
    JogSymbol **symbols;
    JogType **types;
    if (!env || !fn || !env->job) {
        return;
    }
    jog_check_context_begin(env->job);
    n = fn->as.fn.contexts.count;
    if (n == 0 || env->ck->stop) {
        return;
    }
    symbols = ct_arena_alloc(env->ck->program->arena, n * sizeof(*symbols), _Alignof(JogSymbol *));
    types = ct_arena_alloc(env->ck->program->arena, n * sizeof(*types), _Alignof(JogType *));
    if (!symbols || !types) {
        jog_check_oom(env->ck);
        return;
    }
    memset(symbols, 0, n * sizeof(*symbols));
    memset(types, 0, n * sizeof(*types));
    for (i = 0; i < n && !env->ck->stop; i++) {
        JogNode *param = list_at(fn->as.fn.contexts, i);
        if (!param || param->kind != JOG_NODE_PARAM) {
            continue;
        }
        types[i] = param->type;
        if (is_discard(param->as.param.name)) {
            continue;
        }
        symbols[i] = bind_local(env, param, JOG_SYM_CONTEXT, param->as.param.name, param->type);
        param->symbol = symbols[i];
        if (symbols[i]) {
            note_assigned(env, symbols[i]);
        }
    }
    if (!env->ck->stop) {
        jog_check_context_push_params(env->ck, env->job, symbols, types, n);
    }
}

static Env env_for(Check *ck, CheckJob *job, JogBlockScope *scope) {
    Env env;
    memset(&env, 0, sizeof(env));
    env.ck = ck;
    env.job = job;
    env.scope = scope;
    env.reachable = true;
    return env;
}

JogType *jog_check_expr(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType *target) {
    Env *active = job ? (Env *)job->body_env : NULL;
    Env env;
    if (active && active->ck == ck && active->scope == scope) {
        return check_expr(active, node, target);
    }
    env = env_for(ck, job, scope);
    return check_expr(&env, node, target);
}

void jog_check_fn_body(Check *ck, CheckJob *job) {
    JogNode *fn;
    Env env;
    JogBlockScope *fn_scope;
    JogType *ret;
    if (!ck || !job || !job->decl || job->decl->kind != JOG_NODE_FN || ck->stop) {
        return;
    }
    fn = job->decl;
    if (!fn->as.fn.body) {
        return;
    }
    if (fn->as.fn.is_extern) {
        jog_check_fail(ck, fn->pos, "an extern function cannot have a body");
        return;
    }
    env = env_for(ck, job, NULL);
    fn_scope = new_scope(&env, NULL);
    if (!fn_scope) {
        return;
    }
    env = env_for(ck, job, fn_scope);
    ret = fn->type && fn->type->kind == JOG_TY_FN ? fn->type->as.fn.ret : NULL;
    env.ret = ret;
    if (fn->as.fn.receiver) {
        jog_check_bind_self(ck, job, fn_scope);
    }
    bind_params(&env, fn->as.fn.params);
    bind_context_params(&env, fn);
    if (ck->stop) {
        return;
    }
    if (fn->as.fn.expr_body) {
        (void)check_expr(&env, fn->as.fn.body, ret);
        return;
    }
    check_block(&env, fn->as.fn.body);
    if (ck->stop) {
        return;
    }
    if (env.reachable && ret && !is_error(ret) && ret->kind != JOG_TY_UNIT) {
        jog_check_fail(ck, fn->pos, "missing return");
    }
}

JogSymbol *jog_check_body_lookup(CheckJob *job, const char *name) {
    Env *env = job ? (Env *)job->body_env : NULL;
    if (!env) {
        return NULL;
    }
    return lookup_name(env, name);
}

JogSymbol *jog_check_body_bind(CheckJob *job, JogNode *at, JogSymbolKind kind, const char *name, JogType *type) {
    Env *env = job ? (Env *)job->body_env : NULL;
    if (!env) {
        return NULL;
    }
    return bind_local(env, at, kind, name, type);
}

void jog_check_body_assigned(CheckJob *job, JogSymbol *symbol) {
    Env *env = job ? (Env *)job->body_env : NULL;
    if (!env || !symbol) {
        return;
    }
    note_assigned(env, symbol);
}

void jog_check_body_unreachable(CheckJob *job) {
    Env *env = job ? (Env *)job->body_env : NULL;
    if (!env) {
        return;
    }
    env->reachable = false;
}

JogBlockScope *jog_check_body_scope(CheckJob *job) {
    Env *env = job ? (Env *)job->body_env : NULL;
    if (!env) {
        return NULL;
    }
    return env->scope;
}

void jog_check_body_set_scope(CheckJob *job, JogBlockScope *scope) {
    Env *env = job ? (Env *)job->body_env : NULL;
    if (!env) {
        return;
    }
    env->scope = scope;
}

JogType *jog_check_when_arm(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *body, JogType *target, bool as_value,
                            bool *reaches) {
    Env *env = job ? (Env *)job->body_env : NULL;
    JogBlockScope *saved_scope;
    AssignSet *saved_assigned;
    bool saved_reachable;
    JogType *got = NULL;
    void *mark = NULL;
    if (reaches) {
        *reaches = true;
    }
    if (!env || env->ck != ck) {
        if (as_value) {
            got = jog_check_expr(ck, job, scope, body, target);
        }
        return as_value ? got : jog_check_unit_type(ck);
    }
    mark = jog_check_context_mark(job);
    saved_scope = env->scope;
    saved_assigned = env->assigned;
    saved_reachable = env->reachable;
    if (scope) {
        env->scope = scope;
    }
    if (!body) {
        jog_check_fail(ck, (JogPos){0}, "when arm needs a body");
        got = jog_check_error_type(ck);
    } else if (!as_value) {
        if (body->kind == JOG_NODE_BLOCK) {
            check_block(env, body);
        } else {
            (void)check_expr(env, body, NULL);
        }
        got = jog_check_unit_type(ck);
    } else if (body->kind == JOG_NODE_BLOCK) {
        got = check_block_value(env, body, target);
    } else {
        got = check_expr(env, body, target);
    }
    if (reaches) {
        *reaches = env->reachable;
    }
    jog_check_context_reset(job, mark);
    env->scope = saved_scope;
    env->assigned = saved_assigned;
    env->reachable = saved_reachable;
    return got;
}

void jog_check_body_stmt_isolated(CheckJob *job, JogNode *stmt) {
    Env *env = job ? (Env *)job->body_env : NULL;
    AssignSet *saved_assigned;
    bool saved_reachable;
    void *mark;
    if (!env || !stmt) {
        return;
    }
    saved_assigned = env->assigned;
    saved_reachable = env->reachable;
    mark = jog_check_context_mark(job);
    check_stmt(env, stmt);
    jog_check_context_reset(job, mark);
    env->assigned = saved_assigned;
    env->reachable = saved_reachable;
}
