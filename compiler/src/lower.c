#include "phases.h"

#include "check_struct.h"
#include "const_eval.h"
#include "float_lit.h"
#include "check_enum.h"
#include "lower_internal.h"
#include "lower_constraint.h"
#include "lower_context.h"
#include "lower_enum.h"
#include "lower_error.h"
#include "lower_generic.h"
#include "lower_interface.h"
#include "lower_memory.h"
#include "lower_package.h"
#include "lower_string.h"
#include "lower_coro.h"
#include "lower_chan_for.h"
#include "lower_comptime.h"
#include "lower_global.h"
#include "lower_struct.h"
#include "symbols.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Lowers a checked milestone-2 program to JIR.
   Top-level val and var become JirGlobals. An integer, bool, or float literal initializer
   the compiler can fold is the global's data image and does not run.
   A constant image, including one whose type contains a pointer, is that
   data too: pointer slots stay zero and a relocation names the bytes.
   A pointer-free val that is not one of those images is interpreted and
   stored as const data. Struct, enum, and tuple globals lower. Top-level
   val (a, b) is one global per name. Every other initializer runs in init_func, in
   the order the checker's
   waits finish those bindings: round-robin over source order, and an
   initializer runs on the visit where every global it names is already
   done. A use loads the global. main_func is still only a non-extern
   root function named main.
   defer is a cleanup stack. when is a SWITCH plus compare chains in
   lower_enum.c. Struct values, fields, and methods are lowered
   in lower_struct.c. Pointers, arrays, slices, and bytes are lowered in
   lower_memory.c. Tuples, error?, defer, and panic are lowered in
   lower_error.c. with blocks, context bindings, and hidden call
   arguments are lowered in lower_context.c. Unconstrained generic
   functions, methods, structs, enums, and aliases are monomorphized.
   A method instance is one function per concrete receiver and the method's
   own type arguments. An alias template is not a value and is not emitted.
   A generic method template is not emitted.
   Context parameters are appended to the JIR
   parameter list when the type has any. An ordinary Jog function does not
   gain an environment pointer. A variadic extern with no named parameter
   stays zero-parameter and variadic; calls do not pass jog_va.
   A pack `...T: name` is a real slice parameter. The caller builds a
   stack array of the elements and passes the slice header. Zero elements
   is a zero slice. Bare `...` stays the extern variadic.
   Integer and bool constant trees are the folded value, so a constant that
   only fits its result type is not a checked overflow of a narrower step. */

static CtPos as_pos(JogPos pos) {
    CtPos out;
    out.file = pos.file;
    out.line = pos.line;
    out.column = pos.column;
    return out;
}

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static void fail_at(Lower *L, JogPos pos, const char *fmt, ...) {
    char buf[512];
    va_list args;
    int wrote;
    L->failed = true;
    va_start(args, fmt);
    wrote = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (wrote < 0) {
        buf[0] = '\0';
    }
    ct_diag_emit(L->program->diag, CT_SEVERITY_ERROR, as_pos(pos), "%s", buf);
}

static void oom(Lower *L) {
    JogPos pos;
    memset(&pos, 0, sizeof(pos));
    fail_at(L, pos, "out of memory");
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

static bool is_unit(const JogType *type) {
    return !type || type->kind == JOG_TY_UNIT;
}

/* Same rule as jog_check_type_has_zero. A var of one of these types is
   born zero; a non-null pointer is left uninitialized until assigned. */
static bool type_has_zero(const JogType *type) {
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
        return type_has_zero(type->as.array.elem);
    case JOG_TY_TUPLE:
        for (i = 0; i < type->as.tuple.count; i++) {
            if (!type->as.tuple.elems || !type_has_zero(type->as.tuple.elems[i])) {
                return false;
            }
        }
        return true;
    case JOG_TY_DISTINCT:
        return type_has_zero(type->as.nominal.base);
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

static const char *pkg_source(const Lower *L, const JogPackage *pkg) {
    if (pkg && pkg->path && pkg->path[0] != '\0') {
        return pkg->path;
    }
    if (pkg && pkg->name && pkg->name[0] != '\0') {
        return pkg->name;
    }
    if (L->program->module_name && L->program->module_name[0] != '\0') {
        return L->program->module_name;
    }
    return "main";
}

static bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void append_c_piece(CtSb *sb, const char *src) {
    size_t i;
    if (!src || src[0] == '\0') {
        ct_sb_append_cstr(sb, "main");
        return;
    }
    for (i = 0; src[i] != '\0'; i++) {
        char c = src[i];
        if (c == '.') {
            c = '_';
        } else if (!is_ident_char(c)) {
            c = '_';
        }
        ct_sb_putc(sb, c);
    }
}

/* Operator methods spell the symbol out so the C name is an identifier. */
static const char *method_spell(const char *name) {
    static const struct {
        const char *from;
        const char *to;
    } table[] = {
        {"op+", "op_add"},       {"op-", "op_sub"},         {"op*", "op_mul"},
        {"op/", "op_div"},       {"op%", "op_rem"},         {"op&", "op_and"},
        {"op|", "op_or"},        {"op^", "op_xor"},         {"op<<", "op_shl"},
        {"op>>", "op_shr"},      {"op==", "op_eq"},         {"op!=", "op_ne"},
        {"op<", "op_lt"},        {"op<=", "op_le"},         {"op>", "op_gt"},
        {"op>=", "op_ge"},       {"op<=>", "op_cmp"},       {"op[]", "op_index"},
        {"op[]=", "op_index_set"}, {"op!", "op_not"},       {"op~", "op_bitnot"},
    };
    size_t i;
    if (!name) {
        return "fn";
    }
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].from) == 0) {
            return table[i].to;
        }
    }
    return name;
}

static const char *type_piece(Lower *L, JogType *type) {
    CtSb sb;
    const char *text;
    if (type && type->kind == JOG_TY_PTR) {
        type = type->as.elem.elem;
    }
    if (type && (type->kind == JOG_TY_STRUCT || type->kind == JOG_TY_ENUM || type->kind == JOG_TY_INTERFACE ||
                 type->kind == JOG_TY_DISTINCT) &&
        type->as.nominal.name && type->as.nominal.name[0] != '\0') {
        return type->as.nominal.name;
    }
    ct_sb_init(&sb);
    jog_type_print(type, &sb);
    text = NULL;
    if (!sb.failed) {
        text = ct_arena_strdup(L->program->arena, ct_sb_cstr(&sb));
    }
    ct_sb_free(&sb);
    if (!text) {
        oom(L);
        return "Type";
    }
    return text;
}

static const char *dup_sb(Lower *L, CtSb *sb) {
    const char *text = NULL;
    if (!sb->failed) {
        text = ct_arena_strdup(L->program->arena, ct_sb_cstr(sb));
    }
    ct_sb_free(sb);
    if (!text) {
        oom(L);
    }
    return text;
}

static const char *export_c_name(JogNode *fn) {
    size_t i;
    if (!fn) {
        return NULL;
    }
    for (i = 0; i < fn->attrs.count; i++) {
        JogNode *attr = list_at(fn->attrs, i);
        JogNode *arg;
        JogNode *value;
        if (!attr || attr->kind != JOG_NODE_ATTRIBUTE || !attr->as.attribute.name) {
            continue;
        }
        if (strcmp(attr->as.attribute.name, "export") != 0) {
            continue;
        }
        arg = list_at(attr->as.attribute.args, 0);
        value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : NULL;
        if (value && value->kind == JOG_NODE_STRING_LIT && value->as.literal.text && value->as.literal.text[0] != '\0') {
            return value->as.literal.text;
        }
        return fn->as.fn.name ? fn->as.fn.name : "export";
    }
    return NULL;
}

static const char *mangle_fn(Lower *L, const JogPackage *pkg, JogNode *fn) {
    CtSb sb;
    const char *exported;
    const char *piece;
    ct_sb_init(&sb);
    exported = export_c_name(fn);
    if (fn->as.fn.is_extern) {
        ct_sb_append_cstr(&sb, fn->as.fn.name ? fn->as.fn.name : "extern_fn");
        return dup_sb(L, &sb);
    }
    if (exported) {
        ct_sb_append_cstr(&sb, exported);
        return dup_sb(L, &sb);
    }
    ct_sb_append_cstr(&sb, "jog_");
    append_c_piece(&sb, pkg_source(L, pkg));
    ct_sb_append_cstr(&sb, "__");
    if (fn->as.fn.receiver) {
        piece = type_piece(L, fn->as.fn.receiver->type);
        append_c_piece(&sb, piece);
        ct_sb_append_cstr(&sb, "__");
    }
    piece = method_spell(fn->as.fn.name);
    append_c_piece(&sb, piece);
    return dup_sb(L, &sb);
}

static const char *mangle_global(Lower *L, const JogPackage *pkg, const char *name) {
    CtSb sb;
    ct_sb_init(&sb);
    ct_sb_append_cstr(&sb, "jog_");
    append_c_piece(&sb, pkg_source(L, pkg));
    ct_sb_append_cstr(&sb, "__");
    append_c_piece(&sb, name && name[0] != '\0' ? name : "global");
    return dup_sb(L, &sb);
}

static const char *jog_fn_name(Lower *L, const JogPackage *pkg, JogNode *fn) {
    CtSb sb;
    ct_sb_init(&sb);
    ct_sb_append_cstr(&sb, pkg_source(L, pkg));
    ct_sb_putc(&sb, '.');
    if (fn->as.fn.receiver) {
        append_c_piece(&sb, type_piece(L, fn->as.fn.receiver->type));
        ct_sb_putc(&sb, '.');
    }
    ct_sb_append_cstr(&sb, fn->as.fn.name ? fn->as.fn.name : "fn");
    return dup_sb(L, &sb);
}

static JirLinkage linkage_of(JogNode *fn) {
    if (fn->as.fn.is_extern) {
        return JIR_LINK_EXTERN;
    }
    if (export_c_name(fn)) {
        return JIR_LINK_EXPORT;
    }
    if (fn->visibility == JOG_VIS_PUBLIC) {
        return JIR_LINK_PUBLIC;
    }
    return JIR_LINK_INTERNAL;
}

/* Context parameters become ordinary trailing parameters. No env slot. */
static JogType *jir_fn_type(Lower *L, JogType *fn) {
    size_t n;
    size_t i;
    JogType **params;
    if (!fn || fn->kind != JOG_TY_FN || fn->as.fn.context_count == 0) {
        return fn;
    }
    n = fn->as.fn.param_count + fn->as.fn.context_count;
    params = ct_arena_alloc(L->program->arena, n * sizeof(*params), _Alignof(JogType *));
    if (!params) {
        oom(L);
        return NULL;
    }
    for (i = 0; i < fn->as.fn.param_count; i++) {
        params[i] = fn->as.fn.params ? fn->as.fn.params[i] : NULL;
    }
    for (i = 0; i < fn->as.fn.context_count; i++) {
        params[fn->as.fn.param_count + i] = fn->as.fn.contexts ? fn->as.fn.contexts[i] : NULL;
    }
    return jog_type_fn(L->program->types, params, n, fn->as.fn.ret, NULL, 0, fn->as.fn.variadic, fn->as.fn.escaping,
                       fn->as.fn.is_extern);
}

static bool bind_fn(Lower *L, JogNode *decl, int32_t index) {
    FnBind bind;
    memset(&bind, 0, sizeof(bind));
    bind.decl = decl;
    bind.index = index;
    if (!ct_da_push(&L->fns, &bind)) {
        oom(L);
        return false;
    }
    return true;
}

static int32_t find_fn(Lower *L, const JogNode *decl) {
    size_t i;
    if (!decl) {
        return -1;
    }
    for (i = 0; i < L->fns.count; i++) {
        FnBind *bind = ct_da_at(&L->fns, i);
        if (bind && bind->decl == decl) {
            return bind->index;
        }
    }
    return -1;
}

static bool bind_local(Lower *L, JogSymbol *symbol, uint32_t local) {
    LocalBind bind;
    if (!symbol) {
        return true;
    }
    bind.symbol = symbol;
    bind.local = local;
    if (!ct_da_push(&L->locals, &bind)) {
        oom(L);
        return false;
    }
    return true;
}

static bool find_local(Lower *L, const JogSymbol *symbol, uint32_t *out) {
    size_t i;
    if (!symbol) {
        return false;
    }
    for (i = L->locals.count; i-- > 0;) {
        LocalBind *bind = ct_da_at(&L->locals, i);
        if (bind && bind->symbol == symbol) {
            *out = bind->local;
            return true;
        }
    }
    return false;
}

static bool new_block(Lower *L, const char *label, uint32_t *out) {
    size_t before;
    if (L->failed) {
        return false;
    }
    before = L->func->blocks.count;
    *out = jir_block_add(L->func, label);
    if (L->func->blocks.count == before) {
        oom(L);
        return false;
    }
    return true;
}

static void use_block(Lower *L, uint32_t block) {
    JirBlock *b = jir_block_at(L->func, block);
    L->block = block;
    L->terminated = !b || b->term.kind != JIR_TERM_NONE;
}

static bool new_local(Lower *L, const char *name, JogType *type, uint32_t *out) {
    size_t before;
    if (L->failed) {
        return false;
    }
    type = jog_lower_type(L, type);
    if (jog_lower_type_is_open(type)) {
        fail_at(L, L->func ? L->func->pos : (JogPos){0}, "generic type was not instantiated");
        return false;
    }
    if (!type || type->kind == JOG_TY_ERROR) {
        fail_at(L, L->func ? L->func->pos : (JogPos){0}, "lowering is missing a type");
        return false;
    }
    before = L->func->locals.count;
    *out = jir_local_add(L->func, name, type);
    if (L->func->locals.count == before) {
        oom(L);
        return false;
    }
    return true;
}

static void seal(Lower *L, JirTerm term) {
    if (L->failed || L->terminated) {
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

static void emit_ret(Lower *L, JirOperand value, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_RET;
    term.value = value;
    term.pos = pos;
    seal(L, term);
}

static void emit_unreachable(Lower *L, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_UNREACHABLE;
    term.pos = pos;
    seal(L, term);
}

static JirInst *emit(Lower *L, JirOp op, int32_t dst, JogPos pos) {
    JirInst *inst;
    if (L->failed) {
        return NULL;
    }
    if (L->terminated) {
        fail_at(L, pos, "internal: instruction after a terminator");
        return NULL;
    }
    inst = jir_emit(L->func, L->block, op, dst);
    if (!inst) {
        oom(L);
        return NULL;
    }
    inst->pos = pos;
    return inst;
}

static GlobalBind *global_by_symbol(Lower *L, const JogSymbol *symbol) {
    size_t i;
    if (!symbol) {
        return NULL;
    }
    for (i = 0; i < L->globals.count; i++) {
        GlobalBind *bind = ct_da_at(&L->globals, i);
        if (bind && bind->symbol == symbol) {
            return bind;
        }
    }
    return NULL;
}

static void emit_copy(Lower *L, uint32_t dst, JirOperand src, JogPos pos) {
    JirInst *inst;
    if (src.kind == JIR_OPND_LOCAL && src.as.local == dst) {
        return;
    }
    inst = emit(L, JIR_COPY, (int32_t)dst, pos);
    if (inst) {
        inst->a = src;
    }
}

static void emit_zero(Lower *L, uint32_t dst, JogPos pos) {
    (void)emit(L, JIR_ZERO, (int32_t)dst, pos);
}

static JirOperand operand_local(Lower *L, uint32_t local) {
    if (L->failed) {
        return jir_none();
    }
    return jir_local(L->func, local);
}

static JirOperand load_global(Lower *L, uint32_t index, JogPos pos) {
    JirGlobal *global = jir_global(L->module, index);
    JirOperand addr;
    uint32_t dst;
    JirInst *inst;
    if (!global || !global->type) {
        fail_at(L, pos, "missing global");
        return jir_none();
    }
    if (!new_local(L, NULL, global->type, &dst)) {
        return jir_none();
    }
    addr = jir_global_ref(L->module, index);
    if (addr.kind != JIR_OPND_GLOBAL || !addr.type) {
        oom(L);
        return jir_none();
    }
    inst = emit(L, JIR_LOAD, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = addr;
    return operand_local(L, dst);
}

static void store_global(Lower *L, uint32_t index, JirOperand value, JogPos pos) {
    JirOperand addr = jir_global_ref(L->module, index);
    JirInst *inst;
    if (addr.kind != JIR_OPND_GLOBAL || !addr.type) {
        oom(L);
        return;
    }
    inst = emit(L, JIR_STORE, -1, pos);
    if (!inst) {
        return;
    }
    inst->a = addr;
    inst->b = value;
}

static JirOperand emit_binop(Lower *L, JirBinOp op, bool checked, JirOperand a, JirOperand b, JogType *type, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (!new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = emit(L, JIR_BINARY, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->aux = (uint32_t)op;
    inst->checked = checked;
    inst->a = a;
    inst->b = b;
    return operand_local(L, dst);
}

static JirOperand emit_unop(Lower *L, JirUnOp op, bool checked, JirOperand a, JogType *type, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (!new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = emit(L, JIR_UNARY, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->aux = (uint32_t)op;
    inst->checked = checked;
    inst->a = a;
    return operand_local(L, dst);
}

static bool map_binop(JogTokenKind op, JirBinOp *out, bool *checked) {
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
    case JOG_TOK_EQ:
        *out = JIR_EQ;
        return true;
    case JOG_TOK_NEQ:
        *out = JIR_NE;
        return true;
    case JOG_TOK_LT:
        *out = JIR_LT;
        return true;
    case JOG_TOK_LE:
        *out = JIR_LE;
        return true;
    case JOG_TOK_GT:
        *out = JIR_GT;
        return true;
    case JOG_TOK_GE:
        *out = JIR_GE;
        return true;
    default:
        return false;
    }
}

static JirOperand lower_expr_raw(Lower *L, JogNode *node);
static JirOperand lower_expr(Lower *L, JogNode *node);
static JirOperand lower_lambda(Lower *L, JogNode *node);
static JirOperand lower_callee(Lower *L, JogNode *node);
static void lower_stmt(Lower *L, JogNode *node);
static void lower_block(Lower *L, JogNode *block);

static const char *float_lit_msg(JogFloatLitStatus st) {
    if (st == JOG_FLOAT_LIT_DIV0) {
        return "division by zero";
    }
    if (st == JOG_FLOAT_LIT_BAD) {
        return "invalid float literal";
    }
    return "constant does not fit";
}

static bool try_fold(Lower *L, JogNode *node, JirOperand *out) {
    JogConst value;
    if (node && jog_float_lit_tree(node) && node->type &&
        (node->type->kind == JOG_TY_FLOAT || node->type->kind == JOG_TY_UNTYPED_FLOAT)) {
        double fvalue = 0.0;
        unsigned bits = node->type->kind == JOG_TY_FLOAT && node->type->as.flt.bits == 32 ? 32 : 64;
        JogFloatLitStatus st = jog_float_lit_eval(node, bits, &fvalue);
        JogType *ty = node->type->kind == JOG_TY_FLOAT ? node->type : NULL;
        if (st != JOG_FLOAT_LIT_OK) {
            fail_at(L, node->pos, "%s", float_lit_msg(st));
            *out = jir_none();
            return true;
        }
        if (!ty) {
            ty = jog_type_builtin(L->program->types, JOG_BUILTIN_F64);
        }
        if (!ty) {
            oom(L);
            *out = jir_none();
            return true;
        }
        *out = jir_float(ty, fvalue);
        return true;
    }
    if (!is_int_lit_tree(node) && !is_bool_lit_tree(node)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    if (!jog_const_eval(L->program->arena, L->program->diag, L->program->types, node, node->type, &value) || !value.type) {
        L->failed = true;
        *out = jir_none();
        return true;
    }
    if (value.kind != JOG_CONST_INT && value.kind != JOG_CONST_BOOL) {
        fail_at(L, node->pos, "constant expression is not an integer or bool");
        *out = jir_none();
        return true;
    }
    *out = jir_int(value.type, value.bits);
    return true;
}

static JirOperand value_none(void) {
    return jir_none();
}

static JirOperand lookup_overlay(const Lower *L, const JogSymbol *symbol) {
    size_t i;
    if (!symbol) {
        return value_none();
    }
    for (i = 0; i < L->overlay_count; i++) {
        if (L->overlay[i].symbol == symbol) {
            return L->overlay[i].value;
        }
    }
    return value_none();
}

static bool overlay_has(const Lower *L, const JogSymbol *symbol) {
    size_t i;
    if (!symbol || !L->overlay) {
        return false;
    }
    for (i = 0; i < L->overlay_count; i++) {
        if (L->overlay[i].symbol == symbol) {
            return true;
        }
    }
    return false;
}

static JirOperand lower_name(Lower *L, JogNode *node, JogSymbol *symbol, bool as_callee) {
    uint32_t local;
    JirOperand over;
    int32_t index;
    if (overlay_has(L, symbol)) {
        over = lookup_overlay(L, symbol);
        return over;
    }
    {
        JirOperand cap;
        if (jog_lower_coro_name(L, node, symbol, &cap)) {
            return cap;
        }
    }
    if (find_local(L, symbol, &local)) {
        return operand_local(L, local);
    }
    {
        JirOperand cap;
        if (jog_lower_lambda_name(L, node, symbol, &cap)) {
            return cap;
        }
    }
    {
        JirOperand def;
        if (jog_lower_context_default(L, node, symbol, &def)) {
            return def;
        }
    }
    if (symbol && symbol->kind == JOG_SYM_FUNCTION) {
        index = find_fn(L, symbol->decl);
        if (index < 0) {
            fail_at(L, node->pos, "missing lowered function '%s'", symbol->name ? symbol->name : "?");
            return value_none();
        }
        if (!as_callee) {
            if (symbol->type_param_count > 0) {
                fail_at(L, node->pos, "a generic function is not a value yet");
                return value_none();
            }
            if (symbol->type && symbol->type->kind == JOG_TY_FN && symbol->type->as.fn.context_count > 0) {
                fail_at(L, node->pos, "a function with a context parameter is not a value yet");
                return value_none();
            }
        }
        return jir_func_ref(L->module, (uint32_t)index);
    }
    if (symbol && symbol->decl == NULL && symbol->kind == JOG_SYM_CONSTANT) {
        if (symbol->name && strcmp(symbol->name, "true") == 0) {
            return jir_int(node->type ? node->type : symbol->type, 1);
        }
        if (symbol->name && strcmp(symbol->name, "false") == 0) {
            return jir_int(node->type ? node->type : symbol->type, 0);
        }
        if (symbol->name && strcmp(symbol->name, "null") == 0) {
            return jir_null(node->type ? node->type : symbol->type);
        }
        {
            JirOperand err;
            if (jog_lower_coro_error(L, node, symbol, &err)) {
                return err;
            }
        }
    }
    if (symbol && (symbol->kind == JOG_SYM_CONSTANT || symbol->kind == JOG_SYM_VARIABLE ||
                   symbol->kind == JOG_SYM_CONTEXT)) {
        GlobalBind *global = global_by_symbol(L, symbol);
        if (global && global->index >= 0) {
            return load_global(L, (uint32_t)global->index, node->pos);
        }
        if (L->in_comptime) {
            fail_at(L, node->pos, "compile-time expression reads a runtime value");
            return value_none();
        }
        if (L->in_lambda) {
            fail_at(L, node->pos, "capturing lambdas are not lowered yet");
            return value_none();
        }
        fail_at(L, node->pos, "global values are not lowered yet");
        return value_none();
    }
    if (L->in_comptime && symbol && (symbol->kind == JOG_SYM_PARAMETER || symbol->kind == JOG_SYM_VARIABLE ||
                                     symbol->kind == JOG_SYM_CONSTANT || symbol->kind == JOG_SYM_CONTEXT)) {
        fail_at(L, node->pos, "compile-time expression reads a runtime value");
        return value_none();
    }
    if (L->in_lambda && symbol && (symbol->kind == JOG_SYM_PARAMETER || symbol->kind == JOG_SYM_VARIABLE ||
                                   symbol->kind == JOG_SYM_CONSTANT || symbol->kind == JOG_SYM_CONTEXT)) {
        fail_at(L, node->pos, "capturing lambdas are not lowered yet");
        return value_none();
    }
    fail_at(L, node->pos, "name '%s' is not lowered yet", symbol && symbol->name ? symbol->name : "?");
    return value_none();
}

static JirOperand as_local_op(Lower *L, JirOperand src, JogPos pos) {
    uint32_t dst;
    if (L->failed || src.kind == JIR_OPND_LOCAL) {
        return src;
    }
    if (!new_local(L, NULL, src.type, &dst)) {
        return value_none();
    }
    emit_copy(L, dst, src, pos);
    return operand_local(L, dst);
}

/* str and bytes compare by length, then by bytes. Unequal lengths do not
   read memory. Two empty views are equal. */
static JirOperand lower_bytes_cmp(Lower *L, JirOperand lhs, JirOperand rhs, bool negate, JogPos pos) {
    JogType *boolean;
    JogType *usize;
    JogType *u8;
    JogType *ptr_ty;
    JogType *bytes;
    uint32_t result;
    uint32_t index;
    uint32_t len_l;
    uint32_t ptr_l;
    uint32_t len_r;
    uint32_t ptr_r;
    uint32_t loop;
    uint32_t cmp;
    uint32_t step;
    uint32_t yes;
    uint32_t no;
    uint32_t join;
    JirInst *inst;
    JirOperand len_eq;
    JirOperand done;
    JirOperand left_b = jir_none();
    JirOperand right_b = jir_none();
    JirOperand same;
    JirOperand one;
    bool want_true;
    if (L->failed) {
        return value_none();
    }
    boolean = jog_type_builtin(L->program->types, JOG_BUILTIN_BOOL);
    usize = jog_type_builtin(L->program->types, JOG_BUILTIN_USIZE);
    u8 = jog_type_builtin(L->program->types, JOG_BUILTIN_U8);
    bytes = lhs.type ? lhs.type : rhs.type;
    if (!boolean || !usize || !u8 || !bytes) {
        oom(L);
        return value_none();
    }
    ptr_ty = jog_type_ptr(L->program->types, u8, bytes->is_const);
    if (!ptr_ty) {
        oom(L);
        return value_none();
    }
    lhs = as_local_op(L, lhs, pos);
    rhs = as_local_op(L, rhs, pos);
    if (!new_local(L, NULL, boolean, &result) || !new_local(L, NULL, usize, &index) || !new_local(L, NULL, usize, &len_l) ||
        !new_local(L, NULL, ptr_ty, &ptr_l) || !new_local(L, NULL, usize, &len_r) || !new_local(L, NULL, ptr_ty, &ptr_r)) {
        return value_none();
    }
    inst = emit(L, JIR_EXTRACT, (int32_t)ptr_l, pos);
    if (inst) {
        inst->a = lhs;
        inst->aux = 0;
    }
    inst = emit(L, JIR_EXTRACT, (int32_t)len_l, pos);
    if (inst) {
        inst->a = lhs;
        inst->aux = 1;
    }
    inst = emit(L, JIR_EXTRACT, (int32_t)ptr_r, pos);
    if (inst) {
        inst->a = rhs;
        inst->aux = 0;
    }
    inst = emit(L, JIR_EXTRACT, (int32_t)len_r, pos);
    if (inst) {
        inst->a = rhs;
        inst->aux = 1;
    }
    emit_copy(L, index, jir_int(usize, 0), pos);
    len_eq = emit_binop(L, JIR_EQ, false, operand_local(L, len_l), operand_local(L, len_r), boolean, pos);
    if (!new_block(L, "eq.loop", &loop) || !new_block(L, "eq.cmp", &cmp) || !new_block(L, "eq.step", &step) ||
        !new_block(L, "eq.yes", &yes) || !new_block(L, "eq.no", &no) || !new_block(L, "eq.join", &join)) {
        return value_none();
    }
    emit_cond(L, len_eq, loop, no, pos);

    use_block(L, loop);
    done = emit_binop(L, JIR_EQ, false, operand_local(L, index), operand_local(L, len_l), boolean, pos);
    emit_cond(L, done, yes, cmp, pos);

    use_block(L, cmp);
    {
        uint32_t addr_l;
        uint32_t addr_r;
        uint32_t byte_l;
        uint32_t byte_r;
        if (!new_local(L, NULL, ptr_ty, &addr_l) || !new_local(L, NULL, ptr_ty, &addr_r) || !new_local(L, NULL, u8, &byte_l) ||
            !new_local(L, NULL, u8, &byte_r)) {
            return value_none();
        }
        inst = emit(L, JIR_ELEM_ADDR, (int32_t)addr_l, pos);
        if (inst) {
            inst->a = operand_local(L, ptr_l);
            inst->b = operand_local(L, index);
        }
        inst = emit(L, JIR_ELEM_ADDR, (int32_t)addr_r, pos);
        if (inst) {
            inst->a = operand_local(L, ptr_r);
            inst->b = operand_local(L, index);
        }
        inst = emit(L, JIR_LOAD, (int32_t)byte_l, pos);
        if (inst) {
            inst->a = operand_local(L, addr_l);
        }
        inst = emit(L, JIR_LOAD, (int32_t)byte_r, pos);
        if (inst) {
            inst->a = operand_local(L, addr_r);
        }
        left_b = operand_local(L, byte_l);
        right_b = operand_local(L, byte_r);
    }
    same = emit_binop(L, JIR_EQ, false, left_b, right_b, boolean, pos);
    emit_cond(L, same, step, no, pos);

    use_block(L, step);
    one = jir_int(usize, 1);
    {
        JirOperand next = emit_binop(L, JIR_ADD, true, operand_local(L, index), one, usize, pos);
        emit_copy(L, index, next, pos);
    }
    emit_br(L, loop, pos);

    want_true = !negate;
    use_block(L, yes);
    emit_copy(L, result, jir_int(boolean, want_true ? 1 : 0), pos);
    emit_br(L, join, pos);
    use_block(L, no);
    emit_copy(L, result, jir_int(boolean, want_true ? 0 : 1), pos);
    emit_br(L, join, pos);
    use_block(L, join);
    return operand_local(L, result);
}

static JirOperand lower_logic(Lower *L, JogNode *node, bool is_or) {
    JogType *boolean;
    uint32_t result;
    uint32_t rhs_b;
    uint32_t early;
    uint32_t join;
    JirOperand lhs;
    JirOperand rhs;
    const char *early_label;
    boolean = node->type;
    if (!boolean) {
        boolean = jog_type_builtin(L->program->types, JOG_BUILTIN_BOOL);
    }
    if (!new_local(L, NULL, boolean, &result)) {
        return value_none();
    }
    lhs = lower_expr(L, node->as.binary.lhs);
    early_label = is_or ? "or.true" : "and.false";
    if (!new_block(L, is_or ? "or.rhs" : "and.rhs", &rhs_b) || !new_block(L, early_label, &early) ||
        !new_block(L, is_or ? "or.join" : "and.join", &join)) {
        return value_none();
    }
    if (is_or) {
        emit_cond(L, lhs, early, rhs_b, node->pos);
    } else {
        emit_cond(L, lhs, rhs_b, early, node->pos);
    }
    use_block(L, rhs_b);
    rhs = lower_expr(L, node->as.binary.rhs);
    if (!L->terminated) {
        emit_copy(L, result, rhs, node->pos);
        emit_br(L, join, node->pos);
    }
    use_block(L, early);
    emit_copy(L, result, jir_int(boolean, is_or ? 1 : 0), node->pos);
    emit_br(L, join, node->pos);
    use_block(L, join);
    return operand_local(L, result);
}

static JirOperand lower_block_value(Lower *L, JogNode *block) {
    size_t count;
    size_t i;
    size_t base;
    JogNode *tail;
    JirOperand value;
    if (!block || block->kind != JOG_NODE_BLOCK) {
        fail_at(L, block ? block->pos : (JogPos){0}, "expected a block value");
        return value_none();
    }
    base = L->cleanups.count;
    count = block->as.block.stmts.count;
    if (count == 0) {
        fail_at(L, block->pos, "if branch has no value");
        return value_none();
    }
    for (i = 0; i + 1 < count; i++) {
        if (L->terminated || L->failed) {
            break;
        }
        lower_stmt(L, list_at(block->as.block.stmts, i));
    }
    if (L->terminated || L->failed) {
        jog_lower_error_block_end(L, base, block->pos);
        return value_none();
    }
    tail = list_at(block->as.block.stmts, count - 1);
    if (!tail || tail->kind != JOG_NODE_EXPR_STMT) {
        fail_at(L, block->pos, "if branch has no value");
        jog_lower_error_block_end(L, base, block->pos);
        return value_none();
    }
    value = lower_expr(L, tail->as.expr_stmt.expr);
    if (!L->failed && !L->terminated && L->cleanups.count > base && value.kind != JIR_OPND_NONE && value.type) {
        uint32_t dst;
        if (!jog_lower_new_local(L, NULL, value.type, &dst)) {
            return value_none();
        }
        jog_lower_copy(L, dst, value, block->pos);
        value = jog_lower_local_op(L, dst);
    }
    jog_lower_error_block_end(L, base, block->pos);
    return value;
}

static void lower_if_stmt(Lower *L, JogNode *node) {
    uint32_t then_b;
    uint32_t else_b;
    uint32_t join;
    JirOperand cond;
    JogNode *else_branch;
    bool has_else;
    if (L->failed || L->terminated) {
        return;
    }
    else_branch = node->as.if_.else_branch;
    has_else = else_branch != NULL;
    cond = lower_expr(L, node->as.if_.cond);
    if (!new_block(L, "if.then", &then_b) || !new_block(L, "if.join", &join)) {
        return;
    }
    if (has_else) {
        if (!new_block(L, "if.else", &else_b)) {
            return;
        }
        emit_cond(L, cond, then_b, else_b, node->pos);
    } else {
        else_b = join;
        emit_cond(L, cond, then_b, join, node->pos);
    }
    use_block(L, then_b);
    if (node->as.if_.then_block) {
        lower_block(L, node->as.if_.then_block);
    }
    if (!L->terminated) {
        emit_br(L, join, node->pos);
    }
    if (has_else) {
        use_block(L, else_b);
        if (else_branch->kind == JOG_NODE_IF) {
            lower_if_stmt(L, else_branch);
        } else if (else_branch->kind == JOG_NODE_BLOCK) {
            lower_block(L, else_branch);
        } else {
            fail_at(L, else_branch->pos, "if else is not lowered yet");
        }
        if (!L->terminated) {
            emit_br(L, join, node->pos);
        }
    }
    use_block(L, join);
}

static JirOperand lower_if_expr(Lower *L, JogNode *node) {
    uint32_t result;
    uint32_t then_b;
    uint32_t else_b;
    uint32_t join;
    JirOperand cond;
    JirOperand value;
    JogNode *else_branch;
    if (!node->type || !new_local(L, NULL, node->type, &result)) {
        return value_none();
    }
    cond = lower_expr(L, node->as.if_.cond);
    else_branch = node->as.if_.else_branch;
    if (!new_block(L, "if.then", &then_b) || !new_block(L, "if.else", &else_b) || !new_block(L, "if.join", &join)) {
        return value_none();
    }
    emit_cond(L, cond, then_b, else_b, node->pos);
    use_block(L, then_b);
    value = lower_block_value(L, node->as.if_.then_block);
    if (!L->terminated) {
        emit_copy(L, result, value, node->pos);
        emit_br(L, join, node->pos);
    }
    use_block(L, else_b);
    if (else_branch && else_branch->kind == JOG_NODE_IF) {
        value = lower_if_expr(L, else_branch);
    } else {
        value = lower_block_value(L, else_branch);
    }
    if (!L->terminated) {
        emit_copy(L, result, value, node->pos);
        emit_br(L, join, node->pos);
    }
    use_block(L, join);
    return operand_local(L, result);
}

static int param_index_named(JogNode *decl, const char *name, size_t count) {
    size_t i;
    if (!decl || decl->kind != JOG_NODE_FN || !name) {
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

static JogNode *fn_decl_of(JogNode *callee) {
    JogSymbol *symbol;
    if (!callee) {
        return NULL;
    }
    symbol = callee->symbol;
    if (symbol && symbol->decl && symbol->decl->kind == JOG_NODE_FN) {
        return symbol->decl;
    }
    return NULL;
}

/* Index of a pack in the function type, or -1. The parameter is a slice.
   A receiver, when present, is param 0 and is not the pack. */
static int pack_slot_of(JogNode *decl, size_t nparam) {
    JogNode *param;
    size_t ast_n;
    if (!decl || decl->kind != JOG_NODE_FN || nparam == 0) {
        return -1;
    }
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

/* void is incomplete. Do not pass it to jog_type_layout. */
static bool type_is_void(const JogType *type) {
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

/* True when call is the builtin named by builtin and *out is a usize constant.
   The checker stored the type on the argument. Substitution replaces a
   type parameter; the number is that concrete layout.
   use_align selects JogType.align; otherwise the constant is JogType.size. */
static bool lower_builtin_layout(Lower *L, JogNode *node, JirOperand *out, const char *builtin, int use_align) {
    const JogNode *callee;
    JogNode *arg;
    JogNode *value;
    JogType *ty;
    JogType *usz;
    if (!node || node->kind != JOG_NODE_CALL || !out || !builtin) {
        return false;
    }
    callee = node->as.call.callee;
    if (!callee || callee->kind != JOG_NODE_IDENT || callee->symbol || callee->as.ident.type_args.count != 0 ||
        !callee->as.ident.name || strcmp(callee->as.ident.name, builtin) != 0) {
        return false;
    }
    if (node->as.call.args.count != 1 || node->as.call.trailing || node->as.call.contexts.count != 0) {
        return false;
    }
    *out = jir_none();
    if (!L || L->failed) {
        return true;
    }
    arg = list_at(node->as.call.args, 0);
    value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : NULL;
    ty = value && value->type ? jog_lower_type(L, value->type) : NULL;
    usz = node->type ? jog_lower_type(L, node->type) : NULL;
    if (!usz || usz->kind != JOG_TY_INT) {
        usz = jog_type_builtin(L->program->types, JOG_BUILTIN_USIZE);
    }
    /* Every finished V1 layout is sized, including 0. An open parameter
       means instantiation did not substitute. void is not laid out. */
    if (!ty || !usz || jog_lower_type_is_open(ty) || type_is_void(ty) ||
        (!ty->has_layout && !jog_type_layout(L->program->types, ty)) || !ty->has_layout) {
        jog_lower_fail(L, node->pos, use_align ? "type has no alignment" : "type has no size");
        return true;
    }
    *out = jir_int(usz, use_align ? (uint64_t)ty->align : ty->size);
    return true;
}

static bool lower_builtin_size(Lower *L, JogNode *node, JirOperand *out) {
    return lower_builtin_layout(L, node, out, "size", 0);
}

static bool lower_builtin_align(Lower *L, JogNode *node, JirOperand *out) {
    return lower_builtin_layout(L, node, out, "align", 1);
}

static JirOperand lower_call(Lower *L, JogNode *node) {
    JogNode *callee_node;
    JogNode *decl;
    JogType *fn;
    JirOperand callee;
    JirOperand result;
    JirOperand special_value;
    Slot *slots;
    CtDa extras;
    size_t nparam;
    size_t i;
    size_t positional;
    size_t extra_count;
    size_t total;
    JirOperand *args;
    JirInst *inst;
    JogType *ret;
    int32_t dst;
    uint32_t dst_local;
    bool produces;
    int pack;
    CtDa pack_elems;
    if (jog_lower_error_panic(L, node)) {
        return value_none();
    }
    {
        JirOperand sized = jir_none();
        if (lower_builtin_size(L, node, &sized)) {
            return sized;
        }
        if (lower_builtin_align(L, node, &sized)) {
            return sized;
        }
    }
    result = value_none();
    slots = NULL;
    special_value = value_none();
    if (jog_lower_coro_call(L, node, &special_value) || L->failed) {
        return L->failed ? value_none() : special_value;
    }
    if (jog_lower_string_call(L, node, &special_value) || L->failed) {
        return L->failed ? value_none() : special_value;
    }
    if (jog_lower_iface_call(L, node, &special_value) || L->failed) {
        return L->failed ? value_none() : special_value;
    }
    if (jog_lower_constraint_call(L, node, &special_value) || L->failed) {
        return L->failed ? value_none() : special_value;
    }
    if (jog_lower_struct_call(L, node, &special_value) || L->failed) {
        return L->failed ? value_none() : special_value;
    }
    if (jog_lower_memory_call(L, node, &special_value) || L->failed) {
        return L->failed ? value_none() : special_value;
    }
    callee_node = node->as.call.callee;
    pack = -1;
    {
        int extras_ok = ct_da_init(&extras, sizeof(JirOperand));
        int pack_ok = ct_da_init(&pack_elems, sizeof(JirOperand));
        if (!extras_ok || !pack_ok) {
            oom(L);
            ct_da_free(&extras);
            ct_da_free(&pack_elems);
            return result;
        }
    }
    if (node->solved_type_arg_count > 0) {
        int32_t gindex = jog_lower_generic_callee(L, node);
        if (gindex < 0) {
            goto done;
        }
        callee = jir_func_ref(L->module, (uint32_t)gindex);
        fn = callee_node ? jog_lower_type(L, callee_node->type) : NULL;
    } else {
        callee = lower_callee(L, callee_node);
        fn = callee_node ? callee_node->type : NULL;
    }
    if (L->failed) {
        goto done;
    }
    if (!fn || fn->kind != JOG_TY_FN) {
        fail_at(L, node->pos, "call target is not a function");
        goto done;
    }
    nparam = fn->as.fn.param_count;
    decl = fn_decl_of(callee_node);
    pack = pack_slot_of(decl, nparam);
    if (nparam > 0) {
        slots = ct_arena_alloc(L->program->arena, nparam * sizeof(*slots), _Alignof(Slot));
        if (!slots) {
            oom(L);
            goto done;
        }
        memset(slots, 0, nparam * sizeof(*slots));
    }
    positional = 0;
    for (i = 0; i < node->as.call.args.count && !L->failed; i++) {
        JogNode *arg = list_at(node->as.call.args, i);
        JogNode *value;
        JirOperand lowered;
        int index;
        if (!arg || arg->kind != JOG_NODE_ARG) {
            fail_at(L, node->pos, "expected an argument");
            break;
        }
        value = arg->as.arg.value;
        lowered = lower_expr(L, value);
        if (L->failed) {
            break;
        }
        if (arg->as.arg.name) {
            index = param_index_named(decl, arg->as.arg.name, nparam);
            if (index < 0) {
                fail_at(L, arg->pos, "unknown parameter '%s'", arg->as.arg.name);
                break;
            }
            if (pack >= 0 && index == pack && pack_elems.count > 0) {
                fail_at(L, arg->pos, "duplicate argument '%s'", arg->as.arg.name);
                break;
            }
            slots[index].present = true;
            slots[index].value = lowered;
        } else if (pack >= 0 && positional >= (size_t)pack) {
            if (slots && slots[pack].present) {
                fail_at(L, arg->pos, "duplicate argument");
                break;
            }
            if (!ct_da_push(&pack_elems, &lowered)) {
                oom(L);
                break;
            }
            positional++;
        } else if (positional < nparam) {
            slots[positional].present = true;
            slots[positional].value = lowered;
            positional++;
        } else if (!ct_da_push(&extras, &lowered)) {
            oom(L);
            break;
        }
    }
    if (L->failed) {
        goto done;
    }
    if (pack >= 0 && slots && !slots[pack].present) {
        const JirOperand *els = pack_elems.count > 0 ? (const JirOperand *)pack_elems.data : NULL;
        JogType *slice = fn->as.fn.params ? fn->as.fn.params[pack] : NULL;
        slots[pack].value = jog_lower_pack_slice(L, slice, els, pack_elems.count, node->pos);
        slots[pack].present = !L->failed;
        if (L->failed) {
            goto done;
        }
    }
    if (node->as.call.trailing) {
        JirOperand lam;
        if (nparam == 0 || !slots || slots[nparam - 1].present) {
            fail_at(L, node->pos, "trailing lambda does not fit");
            goto done;
        }
        lam = lower_expr(L, node->as.call.trailing);
        if (L->failed) {
            goto done;
        }
        slots[nparam - 1].present = true;
        slots[nparam - 1].value = lam;
    }
    for (i = 0; i < nparam && !L->failed; i++) {
        JogNode *param;
        const Overlay *saved;
        size_t saved_count;
        Overlay *overlay;
        size_t noverlay;
        size_t k;
        size_t w;
        if (slots[i].present) {
            continue;
        }
        param = decl ? list_at(decl->as.fn.params, i) : NULL;
        if (!param || param->kind != JOG_NODE_PARAM || !param->as.param.default_value) {
            fail_at(L, node->pos, "missing argument");
            break;
        }
        noverlay = 0;
        for (k = 0; k < nparam; k++) {
            JogNode *earlier = list_at(decl->as.fn.params, k);
            if (slots[k].present && earlier && earlier->symbol) {
                noverlay++;
            }
        }
        overlay = NULL;
        if (noverlay > 0) {
            overlay = ct_arena_alloc(L->program->arena, noverlay * sizeof(*overlay), _Alignof(Overlay));
            if (!overlay) {
                oom(L);
                break;
            }
            w = 0;
            for (k = 0; k < nparam; k++) {
                JogNode *earlier = list_at(decl->as.fn.params, k);
                if (slots[k].present && earlier && earlier->symbol) {
                    overlay[w].symbol = earlier->symbol;
                    overlay[w].value = slots[k].value;
                    w++;
                }
            }
        }
        saved = L->overlay;
        saved_count = L->overlay_count;
        L->overlay = overlay;
        L->overlay_count = noverlay;
        slots[i].value = lower_expr(L, param->as.param.default_value);
        slots[i].present = !L->failed;
        L->overlay = saved;
        L->overlay_count = saved_count;
    }
    if (L->failed) {
        goto done;
    }
    extra_count = extras.count;
    {
        size_t ctx_n = node->context_arg_count;
        total = nparam + ctx_n + extra_count;
        args = NULL;
        if (total > 0) {
            args = ct_arena_alloc(L->program->arena, total * sizeof(*args), _Alignof(JirOperand));
            if (!args) {
                oom(L);
                goto done;
            }
            for (i = 0; i < nparam; i++) {
                args[i] = slots[i].value;
            }
            if (!jog_lower_context_args(L, node, args, nparam)) {
                goto done;
            }
            for (i = 0; i < extra_count; i++) {
                JirOperand *extra = ct_da_at(&extras, i);
                args[nparam + ctx_n + i] = extra ? *extra : value_none();
            }
        } else if (fn->as.fn.context_count != 0) {
            fail_at(L, node->pos, "missing context argument");
            goto done;
        }
    }
    ret = fn->as.fn.ret;
    produces = ret && ret->kind != JOG_TY_UNIT && ret->kind != JOG_TY_ERROR;
    dst = -1;
    dst_local = 0;
    if (produces) {
        if (!new_local(L, NULL, ret, &dst_local)) {
            goto done;
        }
        dst = (int32_t)dst_local;
    }
    inst = emit(L, JIR_CALL, dst, node->pos);
    if (!inst) {
        goto done;
    }
    inst->a = callee;
    if (!jir_set_args(L->module, inst, args, total)) {
        oom(L);
        goto done;
    }
    if (produces) {
        result = operand_local(L, dst_local);
    }
done:
    ct_da_free(&extras);
    ct_da_free(&pack_elems);
    return result;
}

/* True when node is x?.len or x?.ptr and x is an option of str, bytes, or a
   slice. The base is lowered here. A struct field returns false so
   jog_lower_enum_safe lowers that base once. */
static bool lower_option_view(Lower *L, JogNode *node, JirOperand *out) {
    JogNode *base_node;
    JogType *opt;
    JogType *inner;
    JogType *field_ty;
    JogType *boolean;
    JirOperand base;
    JirOperand some;
    JirOperand inner_val;
    JirOperand field_val;
    JirOperand args[2];
    JirInst *inst;
    uint32_t result;
    uint32_t yes;
    uint32_t no;
    uint32_t join;
    uint32_t aux;
    const char *name;
    if (!L || !node || !out || !node->as.member.safe) {
        return false;
    }
    name = node->as.member.name;
    if (!name || (strcmp(name, "ptr") != 0 && strcmp(name, "len") != 0)) {
        return false;
    }
    aux = strcmp(name, "ptr") == 0 ? 0u : 1u;
    base_node = node->as.member.base;
    opt = base_node && base_node->type ? jog_type_underlying(base_node->type) : NULL;
    inner = opt && opt->kind == JOG_TY_OPTION ? jog_type_underlying(opt->as.elem.elem) : NULL;
    if (!inner || (inner->kind != JOG_TY_BYTES && inner->kind != JOG_TY_SLICE) || !node->type) {
        return false;
    }
    field_ty = aux == 1 ? jog_type_builtin(L->program->types, JOG_BUILTIN_USIZE) : NULL;
    if (aux == 0) {
        JogType *elem = inner->kind == JOG_TY_BYTES ? jog_type_builtin(L->program->types, JOG_BUILTIN_U8) : inner->as.elem.elem;
        field_ty = elem ? jog_type_ptr(L->program->types, elem, inner->is_const) : NULL;
    }
    if (!field_ty) {
        oom(L);
        *out = jir_none();
        return true;
    }
    if (!new_local(L, NULL, node->type, &result)) {
        *out = jir_none();
        return true;
    }
    base = jog_lower_as_local(L, lower_expr(L, base_node), node->pos);
    if (L->failed || L->terminated) {
        *out = jir_none();
        return true;
    }
    boolean = jog_type_builtin(L->program->types, JOG_BUILTIN_BOOL);
    if (!boolean) {
        oom(L);
        *out = jir_none();
        return true;
    }
    if (jog_type_option_is_niche(opt)) {
        some = emit_binop(L, JIR_NE, false, base, jir_null(base.type), boolean, node->pos);
    } else {
        uint32_t has;
        if (!new_local(L, NULL, boolean, &has)) {
            *out = jir_none();
            return true;
        }
        inst = emit(L, JIR_EXTRACT, (int32_t)has, node->pos);
        if (!inst) {
            *out = jir_none();
            return true;
        }
        inst->a = base;
        inst->aux = 0;
        some = operand_local(L, has);
    }
    if (L->failed || !new_block(L, "view.some", &yes) || !new_block(L, "view.none", &no) || !new_block(L, "view.join", &join)) {
        *out = jir_none();
        return true;
    }
    emit_cond(L, some, yes, no, node->pos);
    use_block(L, no);
    if (jog_type_option_is_niche(jog_type_underlying(node->type))) {
        jog_lower_copy(L, result, jir_null(node->type), node->pos);
    } else {
        jog_lower_zero(L, result, node->pos);
    }
    if (!L->terminated) {
        emit_br(L, join, node->pos);
    }
    use_block(L, yes);
    if (jog_type_option_is_niche(opt)) {
        uint32_t dst;
        if (!new_local(L, NULL, inner, &dst)) {
            *out = jir_none();
            return true;
        }
        jog_lower_copy(L, dst, base, node->pos);
        inner_val = operand_local(L, dst);
    } else {
        uint32_t dst;
        if (!new_local(L, NULL, inner, &dst)) {
            *out = jir_none();
            return true;
        }
        inst = emit(L, JIR_EXTRACT, (int32_t)dst, node->pos);
        if (!inst) {
            *out = jir_none();
            return true;
        }
        inst->a = base;
        inst->aux = 1;
        inner_val = operand_local(L, dst);
    }
    {
        uint32_t dst;
        if (L->failed || !new_local(L, NULL, field_ty, &dst)) {
            *out = jir_none();
            return true;
        }
        inst = emit(L, JIR_EXTRACT, (int32_t)dst, node->pos);
        if (!inst) {
            *out = jir_none();
            return true;
        }
        inst->a = inner_val;
        inst->aux = aux;
        field_val = operand_local(L, dst);
    }
    if (!L->failed && !L->terminated) {
        args[0] = jir_int(boolean, 1);
        args[1] = field_val;
        inst = emit(L, JIR_AGGREGATE, (int32_t)result, node->pos);
        if (!inst) {
            *out = jir_none();
            return true;
        }
        if (!jir_set_args(L->module, inst, args, 2)) {
            oom(L);
            *out = jir_none();
            return true;
        }
        emit_br(L, join, node->pos);
    }
    use_block(L, join);
    *out = operand_local(L, result);
    return true;
}

/* bytes and str are one header. Const only changes the pointer's view,
   so this reinterprets those bits and does not copy the text. */
static bool bytes_to_const_view(const JogType *got, const JogType *want) {
    return got && want && got->kind == JOG_TY_BYTES && want->kind == JOG_TY_BYTES && !got->is_const && want->is_const;
}

static JirOperand bitcast_value(Lower *L, JirOperand src, JogType *dest, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (!dest || !new_local(L, NULL, dest, &dst)) {
        return value_none();
    }
    inst = emit(L, JIR_BITCAST, (int32_t)dst, pos);
    if (!inst) {
        return value_none();
    }
    inst->a = src;
    return operand_local(L, dst);
}

static JirOperand lower_expr_raw(Lower *L, JogNode *node) {
    JirOperand folded;
    JirOperand lhs;
    JirOperand rhs;
    JirBinOp bin;
    bool checked;
    JogType *boolean;
    if (L->failed) {
        return value_none();
    }
    if (!node) {
        fail_at(L, (JogPos){0}, "missing expression");
        return value_none();
    }
    if (L->terminated) {
        fail_at(L, node->pos, "internal: expression after a terminator");
        return value_none();
    }
    if (try_fold(L, node, &folded)) {
        return folded;
    }
    switch (node->kind) {
    case JOG_NODE_IDENT: {
        JirOperand raw;
        JirOperand narrowed;
        if (!node->symbol) {
            fail_at(L, node->pos, "missing symbol");
            return value_none();
        }
        raw = lower_name(L, node, node->symbol, false);
        if (jog_lower_iface_narrowed(L, node, raw, &narrowed)) {
            return L->failed ? value_none() : narrowed;
        }
        return raw;
    }
    case JOG_NODE_MEMBER:
        if (node->as.member.safe) {
            JirOperand view;
            if (lower_option_view(L, node, &view)) {
                return L->failed ? value_none() : view;
            }
            return jog_lower_enum_safe(L, node);
        }
        {
            JirOperand en;
            if (jog_lower_enum_member(L, node, &en)) {
                return L->failed ? value_none() : en;
            }
        }
        if (!node->as.member.name) {
            return jog_lower_error_field(L, node);
        }
        if (node->symbol && node->symbol->kind == JOG_SYM_FIELD) {
            JirOperand prop;
            if (jog_lower_iface_load(L, node, &prop)) {
                return L->failed ? value_none() : prop;
            }
            return jog_lower_field(L, node);
        }
        if (!node->symbol) {
            JirOperand view;
            if (jog_lower_package_view(L, node, &view)) {
                return L->failed ? value_none() : view;
            }
            fail_at(L, node->pos, "member is not lowered yet");
            return value_none();
        }
        return lower_name(L, node, node->symbol, false);
    case JOG_NODE_BOOL_LIT:
        return jir_int(node->type, node->as.literal.value ? 1 : 0);
    case JOG_NODE_INT_LIT:
        if (try_fold(L, node, &folded)) {
            return folded;
        }
        fail_at(L, node->pos, "integer literal did not fold");
        return value_none();
    case JOG_NODE_FLOAT_LIT:
        if (try_fold(L, node, &folded)) {
            return folded;
        }
        fail_at(L, node->pos, "float literal did not fold");
        return value_none();
    case JOG_NODE_CHAR_LIT: {
        uint32_t cp = 0;
        if (!node->type || !jog_char_codepoint(node->as.literal.text, node->as.literal.len, &cp)) {
            fail_at(L, node->pos, "invalid char literal");
            return value_none();
        }
        return jir_int(node->type, cp);
    }
    case JOG_NODE_STRING_LIT:
        return jir_string(node->type, node->as.literal.text ? node->as.literal.text : "", node->as.literal.len);
    case JOG_NODE_CSTRING_LIT:
        return jir_cstring(node->type, node->as.literal.text ? node->as.literal.text : "", node->as.literal.len);
    case JOG_NODE_NULL_LIT:
        return jog_lower_error_null(L, node);
    case JOG_NODE_UNARY:
        if (node->as.unary.op == JOG_TOK_AMP || node->as.unary.op == JOG_TOK_STAR) {
            return jog_lower_memory_unary(L, node);
        }
        lhs = lower_expr(L, node->as.unary.operand);
        if (L->failed) {
            return value_none();
        }
        if (node->as.unary.op == JOG_TOK_MINUS) {
            return emit_unop(L, JIR_NEG, true, lhs, node->type, node->pos);
        }
        if (node->as.unary.op == JOG_TOK_TILDE) {
            return emit_unop(L, JIR_BITNOT, false, lhs, node->type, node->pos);
        }
        if (node->as.unary.op == JOG_TOK_BANG) {
            return emit_unop(L, JIR_NOT, false, lhs, node->type, node->pos);
        }
        fail_at(L, node->pos, "operator '%s' is not lowered yet", jog_token_spelling(node->as.unary.op));
        return value_none();
    case JOG_NODE_FORCE:
        return jog_lower_enum_force(L, node);
    case JOG_NODE_BINARY:
        if (node->as.binary.op == JOG_TOK_ELVIS) {
            return jog_lower_enum_elvis(L, node);
        }
        if (node->as.binary.op == JOG_TOK_AMPAMP || node->as.binary.op == JOG_TOK_PIPEPIPE) {
            return lower_logic(L, node, node->as.binary.op == JOG_TOK_PIPEPIPE);
        }
        lhs = lower_expr(L, node->as.binary.lhs);
        rhs = lower_expr(L, node->as.binary.rhs);
        if (L->failed) {
            return value_none();
        }
        {
            JirOperand mem;
            if (jog_lower_memory_binary(L, node, lhs, rhs, &mem)) {
                return mem;
            }
        }
        {
            JirOperand cmp;
            if (jog_lower_error_compare(L, node, lhs, rhs, &cmp)) {
                return cmp;
            }
        }
        if ((node->as.binary.op == JOG_TOK_EQ || node->as.binary.op == JOG_TOK_NEQ) && lhs.type &&
            lhs.type->kind == JOG_TY_BYTES) {
            return lower_bytes_cmp(L, lhs, rhs, node->as.binary.op == JOG_TOK_NEQ, node->pos);
        }
        {
            JirOperand constrained;
            if (jog_lower_constraint_binop(L, node, lhs, rhs, &constrained)) {
                return L->failed ? value_none() : constrained;
            }
        }
        if (!map_binop(node->as.binary.op, &bin, &checked)) {
            fail_at(L, node->pos, "operator '%s' is not lowered yet", jog_token_spelling(node->as.binary.op));
            return value_none();
        }
        boolean = node->type;
        return emit_binop(L, bin, checked, lhs, rhs, boolean, node->pos);
    case JOG_NODE_CAST: {
        JirOperand en;
        if (jog_lower_context_cast(L, node, &en)) {
            return L->failed ? value_none() : en;
        }
        if (jog_lower_iface_cast(L, node, &en)) {
            return L->failed ? value_none() : en;
        }
        if (jog_lower_enum_cast(L, node, &en)) {
            return L->failed ? value_none() : en;
        }
        if (node->as.cast.op != JOG_TOK_KW_AS) {
            fail_at(L, node->pos, "as? is not lowered yet");
            return value_none();
        }
        lhs = lower_expr(L, node->as.cast.expr);
        if (L->failed) {
            return value_none();
        }
        if (lhs.type == node->type) {
            return lhs;
        }
        {
            JirOperand bit;
            if (jog_lower_package_cast(L, node, lhs, &bit)) {
                return L->failed ? value_none() : bit;
            }
        }
        {
            JogType *dest_ty = node->type ? jog_lower_type(L, node->type) : NULL;
            if (L->failed) {
                return value_none();
            }
            if (bytes_to_const_view(lhs.type, dest_ty)) {
                return bitcast_value(L, lhs, dest_ty, node->pos);
            }
        }
        {
            uint32_t dst;
            JirInst *inst;
            if (!new_local(L, NULL, node->type, &dst)) {
                return value_none();
            }
            inst = emit(L, JIR_CONVERT, (int32_t)dst, node->pos);
            if (!inst) {
                return value_none();
            }
            inst->checked = true;
            inst->a = lhs;
            return operand_local(L, dst);
        }
    }
    case JOG_NODE_CALL: {
        JirOperand en;
        if (jog_lower_enum_call(L, node, &en)) {
            return L->failed ? value_none() : en;
        }
        return lower_call(L, node);
    }
    case JOG_NODE_IS: {
        JirOperand en;
        if (jog_lower_iface_is(L, node, &en)) {
            return L->failed ? value_none() : en;
        }
        return jog_lower_enum_is(L, node);
    }
    case JOG_NODE_DOT_VARIANT:
        return jog_lower_enum_dot(L, node);
    case JOG_NODE_WHEN: {
        JirOperand en;
        if (jog_lower_iface_when(L, node, true, &en)) {
            return L->failed ? value_none() : en;
        }
        return jog_lower_enum_when(L, node, true);
    }
    case JOG_NODE_IF:
        return lower_if_expr(L, node);
    case JOG_NODE_INDEX:
    case JOG_NODE_SLICE:
    case JOG_NODE_ARRAY_LIT:
        return jog_lower_memory_expr(L, node);
    case JOG_NODE_TUPLE:
        return jog_lower_error_tuple(L, node);
    case JOG_NODE_INTERP:
        return jog_lower_string_interp(L, node);
    case JOG_NODE_GO:
        return jog_lower_go(L, node);
    case JOG_NODE_LAMBDA:
        return lower_lambda(L, node);
    case JOG_NODE_BUILTIN: {
        JirOperand got;
        if (jog_lower_comptime_builtin(L, node, &got)) {
            return got;
        }
        fail_at(L, node->pos, "%s is not lowered yet", jog_node_kind_name(node->kind));
        return value_none();
    }
    default:
        fail_at(L, node->pos, "%s is not lowered yet", jog_node_kind_name(node->kind));
        return value_none();
    }
}

static JirOperand lower_expr(Lower *L, JogNode *node) {
    JirOperand op;
    JogType *want;
    op = lower_expr_raw(L, node);
    if (!node || L->failed) {
        return op;
    }
    want = node->type ? jog_lower_type(L, node->type) : NULL;
    if (L->failed || !want) {
        return op;
    }
    if (bytes_to_const_view(op.type, want)) {
        return bitcast_value(L, op, want, node->pos);
    }
    if (want->kind != JOG_TY_INTERFACE) {
        return op;
    }
    if (!op.type || op.type->kind == JOG_TY_INTERFACE || op.type->kind == JOG_TY_ERROR || op.kind == JIR_OPND_NONE) {
        return op;
    }
    return jog_lower_iface_convert(L, op, want, node->pos);
}

static JirOperand lower_callee(Lower *L, JogNode *node) {
    if (!node) {
        fail_at(L, (JogPos){0}, "missing call target");
        return value_none();
    }
    if (node->kind == JOG_NODE_IDENT || node->kind == JOG_NODE_MEMBER) {
        if (!node->symbol) {
            fail_at(L, node->pos, "missing call target");
            return value_none();
        }
        return lower_name(L, node, node->symbol, true);
    }
    return lower_expr(L, node);
}

static void lower_binding(Lower *L, JogNode *node) {
    const char *name;
    uint32_t local;
    JirOperand init;
    if (node->as.binding.destructure && node->as.binding.names.count > 1) {
        jog_lower_error_binding(L, node);
        return;
    }
    if (node->as.binding.names.count != 1) {
        fail_at(L, node->pos, "destructuring is not lowered yet");
        return;
    }
    name = node->as.binding.names.items ? node->as.binding.names.items[0] : NULL;
    if (!new_local(L, is_discard(name) ? NULL : name, node->type ? node->type : node->symbol ? node->symbol->type : NULL,
                   &local)) {
        return;
    }
    if (!bind_local(L, node->symbol, local)) {
        return;
    }
    if (node->as.binding.init) {
        init = lower_expr(L, node->as.binding.init);
        if (!L->terminated) {
            emit_copy(L, local, init, node->pos);
        }
        return;
    }
    if (type_has_zero(node->type)) {
        emit_zero(L, local, node->pos);
    }
}

static void assign_global(Lower *L, JogNode *node, GlobalBind *bind) {
    JogNode *target = node->as.assign.target;
    JogTokenKind base;
    JirOperand value;
    JirBinOp bin;
    bool checked;
    JirGlobal *global;
    if (!bind || bind->index < 0 || !target || !target->symbol) {
        fail_at(L, node->pos, "missing global");
        return;
    }
    global = jir_global(L->module, (uint32_t)bind->index);
    if (target->symbol->kind == JOG_SYM_CONSTANT || (global && global->is_const)) {
        fail_at(L, target->pos, "cannot assign to val");
        return;
    }
    base = JOG_TOK_EOF;
    switch (node->as.assign.op) {
    case JOG_TOK_PLUS_ASSIGN:
        base = JOG_TOK_PLUS;
        break;
    case JOG_TOK_MINUS_ASSIGN:
        base = JOG_TOK_MINUS;
        break;
    case JOG_TOK_STAR_ASSIGN:
        base = JOG_TOK_STAR;
        break;
    case JOG_TOK_SLASH_ASSIGN:
        base = JOG_TOK_SLASH;
        break;
    case JOG_TOK_PERCENT_ASSIGN:
        base = JOG_TOK_PERCENT;
        break;
    case JOG_TOK_AMP_ASSIGN:
        base = JOG_TOK_AMP;
        break;
    case JOG_TOK_PIPE_ASSIGN:
        base = JOG_TOK_PIPE;
        break;
    case JOG_TOK_CARET_ASSIGN:
        base = JOG_TOK_CARET;
        break;
    case JOG_TOK_SHL_ASSIGN:
        base = JOG_TOK_SHL;
        break;
    case JOG_TOK_SHR_ASSIGN:
        base = JOG_TOK_SHR;
        break;
    case JOG_TOK_ASSIGN:
        break;
    default:
        fail_at(L, node->pos, "operator '%s' is not lowered yet", jog_token_spelling(node->as.assign.op));
        return;
    }
    if (base != JOG_TOK_EOF) {
        /* Read the global before the right-hand side, matching a = a + e. */
        JirOperand lhs = load_global(L, (uint32_t)bind->index, node->pos);
        JirOperand rhs = lower_expr(L, node->as.assign.value);
        if (L->failed || L->terminated || !map_binop(base, &bin, &checked)) {
            return;
        }
        value = emit_binop(L, bin, checked, lhs, rhs, target->type ? target->type : lhs.type, node->pos);
        if (!L->terminated) {
            store_global(L, (uint32_t)bind->index, value, node->pos);
        }
        return;
    }
    value = lower_expr(L, node->as.assign.value);
    if (!L->terminated) {
        store_global(L, (uint32_t)bind->index, value, node->pos);
    }
}

static JirOperand load_at(Lower *L, JirOperand ptr, JogPos pos) {
    JogType *elem;
    uint32_t dst;
    JirInst *inst;
    if (L->failed) {
        return jir_none();
    }
    elem = ptr.type && ptr.type->kind == JOG_TY_PTR ? ptr.type->as.elem.elem : NULL;
    if (!elem) {
        fail_at(L, pos, "cannot load this pointer");
        return jir_none();
    }
    if (!new_local(L, NULL, elem, &dst)) {
        return jir_none();
    }
    inst = emit(L, JIR_LOAD, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = ptr;
    return operand_local(L, dst);
}

static void store_at(Lower *L, JirOperand ptr, JirOperand value, JogPos pos) {
    JirInst *inst;
    if (L->failed || L->terminated) {
        return;
    }
    inst = emit(L, JIR_STORE, -1, pos);
    if (!inst) {
        return;
    }
    inst->a = ptr;
    inst->b = value;
}

/* A nameless member is a tuple field. The address is the one destructuring
   already stores through, not the temporary a named-field load would use. */
static void lower_tuple_field_assign(Lower *L, JogNode *node) {
    JogNode *target = node->as.assign.target;
    JogTokenKind base;
    JirOperand ptr;
    JirOperand value;
    JirBinOp bin;
    bool checked;
    base = JOG_TOK_EOF;
    switch (node->as.assign.op) {
    case JOG_TOK_PLUS_ASSIGN:
        base = JOG_TOK_PLUS;
        break;
    case JOG_TOK_MINUS_ASSIGN:
        base = JOG_TOK_MINUS;
        break;
    case JOG_TOK_STAR_ASSIGN:
        base = JOG_TOK_STAR;
        break;
    case JOG_TOK_SLASH_ASSIGN:
        base = JOG_TOK_SLASH;
        break;
    case JOG_TOK_PERCENT_ASSIGN:
        base = JOG_TOK_PERCENT;
        break;
    case JOG_TOK_AMP_ASSIGN:
        base = JOG_TOK_AMP;
        break;
    case JOG_TOK_PIPE_ASSIGN:
        base = JOG_TOK_PIPE;
        break;
    case JOG_TOK_CARET_ASSIGN:
        base = JOG_TOK_CARET;
        break;
    case JOG_TOK_SHL_ASSIGN:
        base = JOG_TOK_SHL;
        break;
    case JOG_TOK_SHR_ASSIGN:
        base = JOG_TOK_SHR;
        break;
    case JOG_TOK_ASSIGN:
        break;
    default:
        fail_at(L, node->pos, "operator '%s' is not lowered yet", jog_token_spelling(node->as.assign.op));
        return;
    }
    ptr = jog_lower_tuple_field_ptr(L, target);
    if (L->failed || ptr.kind == JIR_OPND_NONE) {
        return;
    }
    if (base != JOG_TOK_EOF) {
        if ((base == JOG_TOK_PLUS || base == JOG_TOK_MINUS) && target->type && target->type->kind == JOG_TY_PTR) {
            jog_lower_memory_ptr_update(L, ptr, base, node->as.assign.value, target->type, node->pos);
            return;
        }
        {
            JirOperand lhs = load_at(L, ptr, node->pos);
            JirOperand rhs;
            if (L->failed) {
                return;
            }
            rhs = lower_expr(L, node->as.assign.value);
            if (L->failed || L->terminated || !map_binop(base, &bin, &checked)) {
                return;
            }
            value = emit_binop(L, bin, checked, lhs, rhs, target->type ? target->type : lhs.type, node->pos);
        }
        if (!L->terminated) {
            store_at(L, ptr, value, node->pos);
        }
        return;
    }
    value = lower_expr(L, node->as.assign.value);
    if (!L->terminated) {
        store_at(L, ptr, value, node->pos);
    }
}

static void lower_assign(Lower *L, JogNode *node) {
    JogNode *target = node->as.assign.target;
    JogTokenKind base;
    uint32_t local;
    JirOperand value;
    JirBinOp bin;
    bool checked;
    if (target && target->kind == JOG_NODE_IDENT && is_discard(target->as.ident.name)) {
        (void)lower_expr(L, node->as.assign.value);
        return;
    }
    if (jog_lower_package_view_assign(L, node)) {
        return;
    }
    if (jog_lower_memory_assign(L, node)) {
        return;
    }
    if (jog_lower_lambda_store(L, node)) {
        return;
    }
    if (target && target->kind == JOG_NODE_MEMBER) {
        if (!target->as.member.name) {
            lower_tuple_field_assign(L, node);
            return;
        }
        if (!jog_lower_iface_store(L, node)) {
            jog_lower_field_assign(L, node);
        }
        return;
    }
    if (!target || target->kind != JOG_NODE_IDENT || !target->symbol) {
        fail_at(L, node->pos, "assignment target is not lowered yet");
        return;
    }
    if (jog_lower_coro_store(L, node)) {
        return;
    }
    if (!find_local(L, target->symbol, &local)) {
        GlobalBind *bind = global_by_symbol(L, target->symbol);
        if (bind) {
            assign_global(L, node, bind);
            return;
        }
        if (L->in_lambda) {
            fail_at(L, target->pos, "capturing lambdas are not lowered yet");
            return;
        }
        fail_at(L, target->pos, "global values are not lowered yet");
        return;
    }
    base = JOG_TOK_EOF;
    switch (node->as.assign.op) {
    case JOG_TOK_PLUS_ASSIGN:
        base = JOG_TOK_PLUS;
        break;
    case JOG_TOK_MINUS_ASSIGN:
        base = JOG_TOK_MINUS;
        break;
    case JOG_TOK_STAR_ASSIGN:
        base = JOG_TOK_STAR;
        break;
    case JOG_TOK_SLASH_ASSIGN:
        base = JOG_TOK_SLASH;
        break;
    case JOG_TOK_PERCENT_ASSIGN:
        base = JOG_TOK_PERCENT;
        break;
    case JOG_TOK_AMP_ASSIGN:
        base = JOG_TOK_AMP;
        break;
    case JOG_TOK_PIPE_ASSIGN:
        base = JOG_TOK_PIPE;
        break;
    case JOG_TOK_CARET_ASSIGN:
        base = JOG_TOK_CARET;
        break;
    case JOG_TOK_SHL_ASSIGN:
        base = JOG_TOK_SHL;
        break;
    case JOG_TOK_SHR_ASSIGN:
        base = JOG_TOK_SHR;
        break;
    case JOG_TOK_ASSIGN:
        break;
    default:
        fail_at(L, node->pos, "operator '%s' is not lowered yet", jog_token_spelling(node->as.assign.op));
        return;
    }
    if (base != JOG_TOK_EOF) {
        /* Read the local before the right-hand side, matching a = a + e. */
        uint32_t saved;
        JirOperand lhs = operand_local(L, local);
        if (!new_local(L, NULL, lhs.type, &saved)) {
            return;
        }
        emit_copy(L, saved, lhs, node->pos);
        {
            JirOperand rhs = lower_expr(L, node->as.assign.value);
            if (L->failed || !map_binop(base, &bin, &checked)) {
                return;
            }
            value = emit_binop(L, bin, checked, operand_local(L, saved), rhs, target->type ? target->type : lhs.type,
                               node->pos);
        }
        emit_copy(L, local, value, node->pos);
        return;
    }
    value = lower_expr(L, node->as.assign.value);
    if (!L->terminated) {
        emit_copy(L, local, value, node->pos);
    }
}

static void lower_return(Lower *L, JogNode *node) {
    if (jog_lower_coro_return(L, node)) {
        return;
    }
    jog_lower_coro_on_exit(L, node->pos);
    jog_lower_error_return(L, node);
}

static bool emit_into(Lower *L, JirOp op, uint32_t dst, JirOperand a, JirOperand b, bool has_b, uint32_t aux, JogPos pos) {
    JirInst *inst = emit(L, op, (int32_t)dst, pos);
    if (!inst) {
        return false;
    }
    inst->aux = aux;
    inst->a = a;
    if (has_b) {
        inst->b = b;
    }
    return true;
}

static JogSymbol *for_bound(const JogNode *node, size_t index) {
    if (!node->bound_symbols || index >= node->bound_symbol_count) {
        return NULL;
    }
    return node->bound_symbols[index];
}

/* elem and ptr_ty are the indexed element and a pointer to it.
   view is a slice or bytes header. Otherwise n is the inline length. */
static bool collection_layout(Lower *L, JogType *base, JogType **elem, JogType **ptr_ty, bool *view, uint64_t *n) {
    JogType *byte;
    *elem = NULL;
    *ptr_ty = NULL;
    *view = false;
    *n = 0;
    if (!base) {
        return false;
    }
    if (base->kind == JOG_TY_SLICE) {
        *view = true;
        *elem = base->as.elem.elem;
        *ptr_ty = *elem ? jog_type_ptr(L->program->types, *elem, base->is_const) : NULL;
        return *elem != NULL && *ptr_ty != NULL;
    }
    if (base->kind == JOG_TY_BYTES) {
        byte = jog_type_bytes_n(L->program->types, 1);
        *view = true;
        *elem = byte;
        *ptr_ty = byte ? jog_type_ptr(L->program->types, byte, base->is_const) : NULL;
        return byte != NULL && *ptr_ty != NULL;
    }
    if (base->kind == JOG_TY_ARRAY) {
        *elem = base->as.array.elem;
        *n = base->as.array.len;
        *ptr_ty = *elem ? jog_type_ptr(L->program->types, *elem, false) : NULL;
        return *elem != NULL && *ptr_ty != NULL;
    }
    if (base->kind == JOG_TY_BYTES_N) {
        byte = jog_type_bytes_n(L->program->types, 1);
        *elem = byte;
        *n = base->as.bytes_n.n;
        *ptr_ty = byte ? jog_type_ptr(L->program->types, byte, false) : NULL;
        return byte != NULL && *ptr_ty != NULL;
    }
    return false;
}

static bool is_collection_type(const JogType *type) {
    return type && (type->kind == JOG_TY_ARRAY || type->kind == JOG_TY_SLICE || type->kind == JOG_TY_BYTES ||
                    type->kind == JOG_TY_BYTES_N);
}

/* for x in xs copies xs once. for i, x in xs binds i as usize.
   The element is a copy: T, or bytes[1] for str, bytes, and bytes[N].
   continue increments the index. A channel is lowered in lower_chan_for.c. */
static void lower_collection_for(Lower *L, JogNode *node) {
    JogNode *expr = node->as.for_.expr;
    JogPos pos = node->pos;
    JogType *base = expr && expr->type ? jog_lower_type(L, expr->type) : NULL;
    JogType *elem = NULL;
    JogType *ptr_ty = NULL;
    JogType *usize;
    JogType *boolean;
    bool view = false;
    bool two;
    uint64_t n = 0;
    JogSymbol *index_sym;
    JogSymbol *elem_sym;
    JirOperand value;
    JirOperand cmp;
    JirOperand next;
    uint32_t saved;
    uint32_t index_l;
    uint32_t len_l;
    uint32_t ptr_l = 0;
    uint32_t elem_l = 0;
    uint32_t at_l = 0;
    uint32_t head;
    uint32_t body;
    uint32_t step;
    uint32_t exit_b;
    LoopFrame frame;
    if (!collection_layout(L, base, &elem, &ptr_ty, &view, &n)) {
        if (L->failed) {
            return;
        }
        if (is_collection_type(base)) {
            oom(L);
        } else {
            fail_at(L, pos, "for-in over a collection is not lowered yet");
        }
        return;
    }
    usize = jog_type_builtin(L->program->types, JOG_BUILTIN_USIZE);
    boolean = jog_type_builtin(L->program->types, JOG_BUILTIN_BOOL);
    if (!usize || !boolean) {
        oom(L);
        return;
    }
    two = node->as.for_.second != NULL;
    if (two) {
        index_sym = for_bound(node, 0);
        elem_sym = for_bound(node, 1);
    } else {
        index_sym = NULL;
        elem_sym = node->bound_symbol_count > 0 ? for_bound(node, 0) : node->symbol;
    }
    value = lower_expr(L, expr);
    if (L->failed || value.kind == JIR_OPND_NONE) {
        return;
    }
    if (!new_local(L, NULL, base, &saved) || !new_local(L, index_sym ? index_sym->name : NULL, usize, &index_l) ||
        !new_local(L, NULL, usize, &len_l)) {
        return;
    }
    emit_copy(L, saved, value, pos);
    if (!bind_local(L, index_sym, index_l)) {
        return;
    }
    if (elem_sym) {
        if (!new_local(L, elem_sym->name, elem, &elem_l) || !new_local(L, NULL, ptr_ty, &ptr_l) ||
            !new_local(L, NULL, ptr_ty, &at_l) || !bind_local(L, elem_sym, elem_l)) {
            return;
        }
    }
    if (view && base->kind == JOG_TY_BYTES) {
        if (elem_sym) {
            JogType *u8 = jog_type_builtin(L->program->types, JOG_BUILTIN_U8);
            JogType *raw_ty = u8 ? jog_type_ptr(L->program->types, u8, base->is_const) : NULL;
            uint32_t raw_l;
            if (!raw_ty || !new_local(L, NULL, raw_ty, &raw_l) ||
                !emit_into(L, JIR_EXTRACT, raw_l, operand_local(L, saved), jir_none(), false, 0, pos) ||
                !emit_into(L, JIR_BITCAST, ptr_l, operand_local(L, raw_l), jir_none(), false, 0, pos)) {
                if (!L->failed) {
                    oom(L);
                }
                return;
            }
        }
    } else if (view) {
        if (elem_sym && !emit_into(L, JIR_EXTRACT, ptr_l, operand_local(L, saved), jir_none(), false, 0, pos)) {
            return;
        }
    } else if (elem_sym) {
        JogType *addr_ty = jog_type_ptr(L->program->types, base, false);
        uint32_t addr_l;
        if (!addr_ty || !new_local(L, NULL, addr_ty, &addr_l) ||
            !emit_into(L, JIR_ADDR, addr_l, operand_local(L, saved), jir_none(), false, 0, pos) ||
            !emit_into(L, JIR_BITCAST, ptr_l, operand_local(L, addr_l), jir_none(), false, 0, pos)) {
            if (!L->failed) {
                oom(L);
            }
            return;
        }
    }
    if (view) {
        if (!emit_into(L, JIR_EXTRACT, len_l, operand_local(L, saved), jir_none(), false, 1, pos)) {
            return;
        }
    } else {
        emit_copy(L, len_l, jir_int(usize, n), pos);
    }
    emit_copy(L, index_l, jir_int(usize, 0), pos);
    if (L->failed || !new_block(L, "for.head", &head) || !new_block(L, "for.body", &body) || !new_block(L, "for.step", &step) ||
        !new_block(L, "for.exit", &exit_b)) {
        return;
    }
    emit_br(L, head, pos);
    use_block(L, head);
    cmp = emit_binop(L, JIR_LT, false, operand_local(L, index_l), operand_local(L, len_l), boolean, pos);
    emit_cond(L, cmp, body, exit_b, pos);
    use_block(L, body);
    if (elem_sym) {
        if (!emit_into(L, JIR_ELEM_ADDR, at_l, operand_local(L, ptr_l), operand_local(L, index_l), true, 0, pos) ||
            !emit_into(L, JIR_LOAD, elem_l, operand_local(L, at_l), jir_none(), false, 0, pos)) {
            return;
        }
    }
    memset(&frame, 0, sizeof(frame));
    frame.cont = step;
    frame.brk = exit_b;
    frame.cleanup_base = L->cleanups.count;
    if (!ct_da_push(&L->loops, &frame)) {
        oom(L);
        return;
    }
    if (node->as.for_.body) {
        lower_block(L, node->as.for_.body);
    }
    L->loops.count--;
    if (!L->terminated) {
        emit_br(L, step, pos);
    }
    use_block(L, step);
    next = emit_binop(L, JIR_ADD, true, operand_local(L, index_l), jir_int(usize, 1), usize, pos);
    emit_copy(L, index_l, next, pos);
    emit_br(L, head, pos);
    use_block(L, exit_b);
}

static bool for_in_is_range(const JogNode *node) {
    const JogNode *expr = node ? node->as.for_.expr : NULL;
    return node && !node->as.for_.second && expr && expr->kind == JOG_NODE_BINARY && expr->as.binary.op == JOG_TOK_DOTDOT;
}

static void lower_for(Lower *L, JogNode *node) {
    LoopFrame frame;
    uint32_t head;
    uint32_t body;
    uint32_t step;
    uint32_t exit_b;
    if (node->as.for_.form == JOG_FOR_IN && !for_in_is_range(node)) {
        if (jog_lower_chan_for(L, node)) {
            return;
        }
        lower_collection_for(L, node);
        return;
    }
    if (node->as.for_.form == JOG_FOR_IN) {
        JogNode *range = node->as.for_.expr;
        JogType *elem;
        uint32_t index;
        uint32_t end_local;
        JirOperand start;
        JirOperand end;
        JirOperand cmp;
        JogType *boolean;
        elem = node->symbol ? node->symbol->type : range->type;
        boolean = jog_type_builtin(L->program->types, JOG_BUILTIN_BOOL);
        start = lower_expr(L, range->as.binary.lhs);
        end = lower_expr(L, range->as.binary.rhs);
        if (!new_local(L, is_discard(node->as.for_.first) ? NULL : node->as.for_.first, elem, &index) ||
            !new_local(L, NULL, elem, &end_local)) {
            return;
        }
        if (!bind_local(L, node->symbol, index)) {
            return;
        }
        emit_copy(L, index, start, node->pos);
        emit_copy(L, end_local, end, node->pos);
        if (!new_block(L, "for.head", &head) || !new_block(L, "for.body", &body) || !new_block(L, "for.step", &step) ||
            !new_block(L, "for.exit", &exit_b)) {
            return;
        }
        emit_br(L, head, node->pos);
        use_block(L, head);
        cmp = emit_binop(L, JIR_LT, false, operand_local(L, index), operand_local(L, end_local), boolean, node->pos);
        emit_cond(L, cmp, body, exit_b, node->pos);
        use_block(L, body);
        frame.cont = step;
        frame.brk = exit_b;
        frame.cleanup_base = L->cleanups.count;
        if (!ct_da_push(&L->loops, &frame)) {
            oom(L);
            return;
        }
        if (node->as.for_.body) {
            lower_block(L, node->as.for_.body);
        }
        L->loops.count--;
        if (!L->terminated) {
            emit_br(L, step, node->pos);
        }
        use_block(L, step);
        {
            JirOperand next = emit_binop(L, JIR_ADD, true, operand_local(L, index), jir_int(elem, 1), elem, node->pos);
            emit_copy(L, index, next, node->pos);
        }
        emit_br(L, head, node->pos);
        use_block(L, exit_b);
        return;
    }
    if (node->as.for_.form == JOG_FOR_FOREVER) {
        if (!new_block(L, "for.head", &head) || !new_block(L, "for.exit", &exit_b)) {
            return;
        }
        emit_br(L, head, node->pos);
        use_block(L, head);
        frame.cont = head;
        frame.brk = exit_b;
        frame.cleanup_base = L->cleanups.count;
    } else {
        if (!new_block(L, "for.head", &head) || !new_block(L, "for.body", &body) || !new_block(L, "for.exit", &exit_b)) {
            return;
        }
        emit_br(L, head, node->pos);
        use_block(L, head);
        {
            JirOperand cond = lower_expr(L, node->as.for_.expr);
            emit_cond(L, cond, body, exit_b, node->pos);
        }
        use_block(L, body);
        frame.cont = head;
        frame.brk = exit_b;
        frame.cleanup_base = L->cleanups.count;
    }
    if (!ct_da_push(&L->loops, &frame)) {
        oom(L);
        return;
    }
    if (node->as.for_.body) {
        lower_block(L, node->as.for_.body);
    }
    L->loops.count--;
    if (!L->terminated) {
        emit_br(L, head, node->pos);
    }
    use_block(L, exit_b);
}

static void lower_stmt(Lower *L, JogNode *node) {
    LoopFrame *frame;
    if (!node || L->failed || L->terminated) {
        return;
    }
    switch (node->kind) {
    case JOG_NODE_VAL:
    case JOG_NODE_VAR:
    case JOG_NODE_CONTEXT:
        lower_binding(L, node);
        return;
    case JOG_NODE_ASSIGN:
        lower_assign(L, node);
        return;
    case JOG_NODE_RETURN:
        lower_return(L, node);
        return;
    case JOG_NODE_FOR:
        lower_for(L, node);
        return;
    case JOG_NODE_BREAK:
    case JOG_NODE_CONTINUE:
        if (L->loops.count == 0) {
            fail_at(L, node->pos, node->kind == JOG_NODE_BREAK ? "break outside of a loop" : "continue outside of a loop");
            return;
        }
        frame = ct_da_at(&L->loops, L->loops.count - 1);
        jog_lower_error_exit(L, frame->cleanup_base, node->kind == JOG_NODE_BREAK ? frame->brk : frame->cont, node->pos);
        return;
    case JOG_NODE_EXPR_STMT:
        if (node->as.expr_stmt.expr && node->as.expr_stmt.expr->kind == JOG_NODE_WHEN) {
            if (!jog_lower_iface_when(L, node->as.expr_stmt.expr, false, NULL)) {
                (void)jog_lower_enum_when(L, node->as.expr_stmt.expr, false);
            }
        } else if (node->as.expr_stmt.expr && node->as.expr_stmt.expr->kind == JOG_NODE_IF) {
            lower_if_stmt(L, node->as.expr_stmt.expr);
        } else {
            (void)lower_expr(L, node->as.expr_stmt.expr);
        }
        return;
    case JOG_NODE_BLOCK:
        lower_block(L, node);
        return;
    case JOG_NODE_DEFER:
    case JOG_NODE_ERRDEFER:
        jog_lower_error_note(L, node);
        return;
    case JOG_NODE_DESTRUCTURE:
        jog_lower_error_destructure(L, node);
        return;
    case JOG_NODE_WITH:
        jog_lower_context_with(L, node);
        return;
    case JOG_NODE_ATTR_BLOCK: {
        bool saved = L->unchecked;
        if (jog_lower_memory_unchecked(node)) {
            L->unchecked = true;
        }
        if (node->as.attr_block.body) {
            if (node->as.attr_block.body->kind == JOG_NODE_BLOCK) {
                lower_block(L, node->as.attr_block.body);
            } else {
                lower_stmt(L, node->as.attr_block.body);
            }
        }
        L->unchecked = saved;
        return;
    }
    case JOG_NODE_WHEN:
        if (!jog_lower_iface_when(L, node, false, NULL)) {
            (void)jog_lower_enum_when(L, node, false);
        }
        return;
    default:
        fail_at(L, node->pos, "%s is not lowered yet", jog_node_kind_name(node->kind));
        return;
    }
}

static void lower_block(Lower *L, JogNode *block) {
    size_t i;
    size_t base;
    if (!block || block->kind != JOG_NODE_BLOCK) {
        fail_at(L, block ? block->pos : (JogPos){0}, "expected a block");
        return;
    }
    base = L->cleanups.count;
    for (i = 0; i < block->as.block.stmts.count; i++) {
        if (L->failed || L->terminated) {
            break;
        }
        lower_stmt(L, list_at(block->as.block.stmts, i));
    }
    jog_lower_error_block_end(L, base, block->pos);
}

static void finish_open(Lower *L, JogType *ret, JogPos pos) {
    if (L->failed || L->terminated) {
        return;
    }
    jog_lower_coro_on_exit(L, pos);
    if (L->failed || L->terminated) {
        return;
    }
    if (is_unit(ret)) {
        emit_ret(L, value_none(), pos);
        return;
    }
    emit_unreachable(L, pos);
}

static void lower_body(Lower *L, JogNode *fn) {
    JogType *ret;
    uint32_t entry;
    size_t i;
    JirOperand value;
    L->locals.count = 0;
    L->loops.count = 0;
    L->overlay = NULL;
    L->overlay_count = 0;
    L->terminated = false;
    L->unchecked = false;
    jog_lower_error_reset(L);
    if (fn->as.fn.is_extern) {
        return;
    }
    if (!fn->as.fn.body) {
        fail_at(L, fn->pos, "function '%s' has no body", fn->as.fn.name ? fn->as.fn.name : "?");
        return;
    }
    if (!new_block(L, "entry", &entry)) {
        return;
    }
    use_block(L, entry);
    L->ast_fn = fn;
    {
        size_t slot = 0;
        if (fn->as.fn.receiver) {
            JogSymbol *self = jog_lower_self_symbol(fn);
            if (self && !bind_local(L, self, 0)) {
                L->ast_fn = NULL;
                return;
            }
            slot = 1;
        }
        for (i = 0; i < fn->as.fn.params.count; i++) {
            JogNode *param = list_at(fn->as.fn.params, i);
            if (!param || param->kind != JOG_NODE_PARAM) {
                continue;
            }
            if (!bind_local(L, param->symbol, (uint32_t)(slot + i))) {
                L->ast_fn = NULL;
                return;
            }
        }
        {
            size_t base = slot + fn->as.fn.params.count;
            for (i = 0; i < fn->as.fn.contexts.count; i++) {
                JogNode *param = list_at(fn->as.fn.contexts, i);
                if (!param || param->kind != JOG_NODE_PARAM || !param->symbol) {
                    continue;
                }
                if (!bind_local(L, param->symbol, (uint32_t)(base + i))) {
                    L->ast_fn = NULL;
                    return;
                }
            }
        }
    }
    ret = fn->type && fn->type->kind == JOG_TY_FN ? fn->type->as.fn.ret : NULL;
    if (fn->as.fn.expr_body) {
        value = lower_expr(L, fn->as.fn.body);
        if (!L->terminated && !L->failed) {
            if (is_unit(ret)) {
                emit_ret(L, value_none(), fn->as.fn.body->pos);
            } else {
                emit_ret(L, value, fn->as.fn.body->pos);
            }
        }
        return;
    }
    lower_block(L, fn->as.fn.body);
    finish_open(L, ret, fn->pos);
}

static bool declare_fn(Lower *L, const JogPackage *pkg, JogNode *fn) {
    JogType *type;
    const char *name;
    const char *jog_name;
    const char **param_names;
    size_t nparam;
    size_t i;
    int32_t index;
    JirFunc *func;
    if (!fn || fn->kind != JOG_NODE_FN) {
        return false;
    }
    if (fn->symbol && fn->symbol->type_param_count > 0) {
        return true;
    }
    if (!fn->type || fn->type->kind != JOG_TY_FN) {
        fail_at(L, fn->pos, "function '%s' has no type", fn->as.fn.name ? fn->as.fn.name : "?");
        return false;
    }
    type = jir_fn_type(L, fn->type);
    if (!type) {
        return false;
    }
    name = mangle_fn(L, pkg, fn);
    jog_name = jog_fn_name(L, pkg, fn);
    if (!name || !jog_name) {
        return false;
    }
    nparam = type->as.fn.param_count;
    param_names = NULL;
    if (nparam > 0) {
        param_names = ct_arena_alloc(L->program->arena, nparam * sizeof(*param_names), _Alignof(const char *));
        if (!param_names) {
            oom(L);
            return false;
        }
        memset(param_names, 0, nparam * sizeof(*param_names));
        for (i = 0; i < nparam; i++) {
            param_names[i] = jog_lower_param_name(fn, i);
        }
    }
    index = jir_func_add(L->module, name, jog_name, linkage_of(fn), type, param_names);
    if (index < 0) {
        oom(L);
        return false;
    }
    func = jir_func(L->module, (uint32_t)index);
    if (func) {
        func->pos = fn->pos;
    }
    if (!bind_fn(L, fn, index)) {
        return false;
    }
    if (!fn->as.fn.is_extern && fn->as.fn.name && strcmp(fn->as.fn.name, "main") == 0 && pkg == L->program->root) {
        L->module->main_func = index;
    }
    return true;
}

static void declare_decl(Lower *L, const JogPackage *pkg, JogNode *decl) {
    size_t i;
    if (!decl || L->failed) {
        return;
    }
    /* type Pair<A, B> = (A, B) is a name. The template is not emitted. */
    if (jog_lower_is_alias_template(decl)) {
        return;
    }
    switch (decl->kind) {
    case JOG_NODE_FN:
        (void)declare_fn(L, pkg, decl);
        return;
    case JOG_NODE_EXTERN_BLOCK:
        for (i = 0; i < decl->as.extern_block.fns.count && !L->failed; i++) {
            (void)declare_fn(L, pkg, list_at(decl->as.extern_block.fns, i));
        }
        return;
    case JOG_NODE_VAL:
    case JOG_NODE_VAR:
    case JOG_NODE_CONTEXT:
    case JOG_NODE_TYPE_DECL:
    case JOG_NODE_IMPORT:
    case JOG_NODE_STRUCT:
    case JOG_NODE_INTERFACE:
    case JOG_NODE_ENUM:
        return;
    default:
        fail_at(L, decl->pos, "%s is not lowered yet", jog_node_kind_name(decl->kind));
        return;
    }
}

typedef struct Snap {
    JirFunc *func;
    JogNode *ast_fn;
    uint32_t block;
    bool terminated;
    bool unchecked;
    bool in_cleanup;
    int in_coro;
    bool in_comptime;
    int in_lambda;
    int32_t ensuring;
    const Overlay *overlay;
    size_t overlay_count;
    void *coro_caps;
    size_t coro_cap_count;
    void *lambda_caps;
    size_t lambda_cap_count;
    uint32_t lambda_env;
    uint32_t *coro_scopes;
    size_t coro_scope_n;
    size_t coro_scope_cap;
    uint32_t *scope_copy;
    JogType **subst_from;
    JogType **subst_to;
    size_t subst_n;
    size_t locals_n;
    size_t loops_n;
    size_t clean_n;
    void *locals;
    void *loops;
    void *clean;
} Snap;

static void *da_copy(const CtDa *da, size_t n) {
    void *copy;
    if (n == 0) {
        return NULL;
    }
    if (!da || da->item_size == 0 || !da->data) {
        return NULL;
    }
    copy = malloc(n * da->item_size);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, da->data, n * da->item_size);
    return copy;
}

static int da_fit(CtDa *da, size_t n) {
    void *grown;
    if (!da || n <= da->capacity) {
        return 1;
    }
    if (da->item_size == 0) {
        return 0;
    }
    grown = realloc(da->data, n * da->item_size);
    if (!grown) {
        return 0;
    }
    da->data = grown;
    da->capacity = n;
    return 1;
}

static void snap_free(Snap *s) {
    if (!s) {
        return;
    }
    free(s->locals);
    free(s->loops);
    free(s->clean);
    free(s->scope_copy);
    s->locals = NULL;
    s->loops = NULL;
    s->clean = NULL;
    s->scope_copy = NULL;
}

static int snap_save(Lower *L, Snap *s) {
    memset(s, 0, sizeof(*s));
    s->func = L->func;
    s->ast_fn = L->ast_fn;
    s->block = L->block;
    s->terminated = L->terminated;
    s->unchecked = L->unchecked;
    s->in_cleanup = L->in_cleanup;
    s->in_coro = L->in_coro;
    s->in_comptime = L->in_comptime;
    s->in_lambda = L->in_lambda;
    s->ensuring = L->ensuring;
    s->overlay = L->overlay;
    s->overlay_count = L->overlay_count;
    s->coro_caps = L->coro_caps;
    s->coro_cap_count = L->coro_cap_count;
    s->lambda_caps = L->lambda_caps;
    s->lambda_cap_count = L->lambda_cap_count;
    s->lambda_env = L->lambda_env;
    s->coro_scopes = L->coro_scopes;
    s->coro_scope_n = L->coro_scope_n;
    s->coro_scope_cap = L->coro_scope_cap;
    s->subst_from = L->subst_from;
    s->subst_to = L->subst_to;
    s->subst_n = L->subst_n;
    s->locals_n = L->locals.count;
    s->loops_n = L->loops.count;
    s->clean_n = L->cleanups.count;
    s->locals = da_copy(&L->locals, s->locals_n);
    s->loops = da_copy(&L->loops, s->loops_n);
    s->clean = da_copy(&L->cleanups, s->clean_n);
    if ((s->locals_n && !s->locals) || (s->loops_n && !s->loops) || (s->clean_n && !s->clean)) {
        snap_free(s);
        oom(L);
        return 0;
    }
    if (s->coro_scope_n && L->coro_scopes) {
        s->scope_copy = malloc(s->coro_scope_n * sizeof(uint32_t));
        if (!s->scope_copy) {
            snap_free(s);
            oom(L);
            return 0;
        }
        memcpy(s->scope_copy, L->coro_scopes, s->coro_scope_n * sizeof(uint32_t));
    }
    return 1;
}

static int restore_da(CtDa *da, void *saved, size_t n) {
    if (n > 0) {
        if (!saved || !da_fit(da, n)) {
            return 0;
        }
        memcpy(da->data, saved, n * da->item_size);
    }
    da->count = n;
    return 1;
}

static void snap_restore(Lower *L, Snap *s) {
    int ok = 1;
    L->func = s->func;
    L->ast_fn = s->ast_fn;
    L->block = s->block;
    L->terminated = s->terminated;
    L->unchecked = s->unchecked;
    L->in_cleanup = s->in_cleanup;
    L->in_coro = s->in_coro;
    L->in_comptime = s->in_comptime;
    L->in_lambda = s->in_lambda;
    L->ensuring = s->ensuring;
    L->overlay = s->overlay;
    L->overlay_count = s->overlay_count;
    L->coro_caps = s->coro_caps;
    L->coro_cap_count = s->coro_cap_count;
    L->lambda_caps = s->lambda_caps;
    L->lambda_cap_count = s->lambda_cap_count;
    L->lambda_env = s->lambda_env;
    L->subst_from = s->subst_from;
    L->subst_to = s->subst_to;
    L->subst_n = s->subst_n;
    if (!restore_da(&L->locals, s->locals, s->locals_n) || !restore_da(&L->loops, s->loops, s->loops_n) ||
        !restore_da(&L->cleanups, s->clean, s->clean_n)) {
        ok = 0;
    }
    /* Scope ids live in the program arena. A body that grows the list
       allocates a new buffer and leaves the previous one intact. */
    if (L->coro_scopes == s->coro_scopes && s->scope_copy && L->coro_scopes && s->coro_scope_n) {
        memcpy(L->coro_scopes, s->scope_copy, s->coro_scope_n * sizeof(uint32_t));
    }
    L->coro_scopes = s->coro_scopes;
    L->coro_scope_n = s->coro_scope_n;
    L->coro_scope_cap = s->coro_scope_cap;
    snap_free(s);
    if (!ok) {
        oom(L);
    }
}

/* Last expression of a non-unit lambda is the return value. Defers in this
   body run before that return and do not see the caller's cleanup stack. */
static void lower_lambda_block(Lower *L, JogNode *block, JogType *ret) {
    size_t i;
    size_t n;
    size_t base;
    JogNode *last = NULL;
    bool tail = false;
    JirOperand value;
    if (!block || block->kind != JOG_NODE_BLOCK) {
        fail_at(L, block ? block->pos : (JogPos){0}, "expected a block");
        return;
    }
    base = L->cleanups.count;
    n = block->as.block.stmts.count;
    if (n > 0) {
        last = list_at(block->as.block.stmts, n - 1);
        tail = last && last->kind == JOG_NODE_EXPR_STMT && !is_unit(ret);
        if (tail) {
            n--;
        }
    }
    for (i = 0; i < n && !L->failed && !L->terminated; i++) {
        lower_stmt(L, list_at(block->as.block.stmts, i));
    }
    if (L->failed || L->terminated) {
        return;
    }
    if (tail && last) {
        value = lower_expr(L, last->as.expr_stmt.expr);
        if (L->failed || L->terminated) {
            return;
        }
        jog_lower_error_block_end(L, base, block->pos);
        if (L->failed || L->terminated) {
            return;
        }
        if (value.kind == JIR_OPND_NONE) {
            fail_at(L, last->pos, "lambda does not produce a value");
            return;
        }
        emit_ret(L, value, last->pos);
        return;
    }
    jog_lower_error_block_end(L, base, block->pos);
    if (L->failed || L->terminated) {
        return;
    }
    if (is_unit(ret)) {
        emit_ret(L, value_none(), block->pos);
        return;
    }
    emit_unreachable(L, block->pos);
}

static JirOperand lower_lambda(Lower *L, JogNode *node) {
    JogType *type;
    JogType *ret;
    char *name;
    int32_t index;
    JirFunc *func;
    Snap snap;
    JogLambdaPrep prep;
    uint32_t entry = 0;
    uint32_t env_local = 0;
    JirOperand value;
    size_t i;
    int failed = 0;
    memset(&prep, 0, sizeof(prep));
    prep.env = jir_none();
    if (!node || L->failed) {
        return value_none();
    }
    type = node->type;
    if (!type || type->kind != JOG_TY_FN || type->as.fn.is_extern) {
        fail_at(L, node->pos, "lambda is not a function");
        return value_none();
    }
    name = ct_arena_alloc(L->program->arena, 32, 1);
    if (!name) {
        oom(L);
        return value_none();
    }
    snprintf(name, 32, "jog_lambda_%u", (unsigned)L->module->funcs.count);
    /* The tuple is built in the caller, before this function is entered. */
    if (!jog_lower_lambda_prep(L, node, &prep)) {
        free(prep.caps);
        return value_none();
    }
    if (prep.count > 0 && type->as.fn.escaping) {
        fail_at(L, node->pos, "escaping captures are not lowered yet");
        free(prep.caps);
        return value_none();
    }
    if (prep.count > 0 && L->in_coro) {
        fail_at(L, node->pos, "capturing lambdas are not lowered yet");
        free(prep.caps);
        return value_none();
    }
    index = jir_func_add(L->module, name, name, JIR_LINK_INTERNAL, type, NULL);
    func = index >= 0 ? jir_func(L->module, (uint32_t)index) : NULL;
    if (!func) {
        oom(L);
        free(prep.caps);
        return value_none();
    }
    func->pos = node->pos;
    if (prep.count > 0) {
        size_t before = func->locals.count;
        JirLocal *slot;
        if (!prep.void_ptr) {
            fail_at(L, node->pos, "missing void");
            free(prep.caps);
            return value_none();
        }
        env_local = jir_local_add(func, "env", prep.void_ptr);
        if (func->locals.count == before) {
            oom(L);
            free(prep.caps);
            return value_none();
        }
        func->param_count += 1;
        slot = jir_local_at(func, env_local);
        if (slot) {
            slot->is_param = true;
        }
    }
    if (!snap_save(L, &snap)) {
        free(prep.caps);
        return value_none();
    }
    L->func = func;
    /* fn_ret reads ast_fn->type. The lambda node's type is the function type,
       so return uses this result instead of the enclosing function's. */
    L->ast_fn = node;
    L->locals.count = 0;
    L->loops.count = 0;
    L->cleanups.count = 0;
    L->overlay = NULL;
    L->overlay_count = 0;
    L->terminated = false;
    L->in_cleanup = false;
    L->in_coro = 0;
    L->in_comptime = false;
    L->in_lambda = 1;
    L->lambda_caps = NULL;
    L->lambda_cap_count = 0;
    L->lambda_env = 0;
    if (prep.count > 0) {
        L->lambda_caps = prep.caps;
        L->lambda_cap_count = prep.count;
        L->lambda_env = env_local;
    }
    if (node->as.lambda.implicit_it) {
        if (!bind_local(L, node->symbol, 0)) {
            failed = 1;
            goto restore;
        }
    } else {
        for (i = 0; i < node->as.lambda.params.count; i++) {
            JogNode *param = list_at(node->as.lambda.params, i);
            if (!param || param->kind != JOG_NODE_PARAM) {
                continue;
            }
            if (!bind_local(L, param->symbol, (uint32_t)i)) {
                failed = 1;
                goto restore;
            }
        }
    }
    if (!new_block(L, "entry", &entry)) {
        failed = 1;
        goto restore;
    }
    use_block(L, entry);
    ret = type->as.fn.ret;
    if (node->as.lambda.expr_body) {
        value = lower_expr(L, node->as.lambda.body);
        if (!L->failed && !L->terminated) {
            if (is_unit(ret)) {
                emit_ret(L, value_none(), node->as.lambda.body ? node->as.lambda.body->pos : node->pos);
            } else if (value.kind == JIR_OPND_NONE) {
                fail_at(L, node->pos, "lambda does not produce a value");
            } else {
                emit_ret(L, value, node->as.lambda.body ? node->as.lambda.body->pos : node->pos);
            }
        }
    } else {
        lower_lambda_block(L, node->as.lambda.body, ret);
    }
    failed = L->failed;
restore:
    snap_restore(L, &snap);
    free(prep.caps);
    prep.caps = NULL;
    if (failed) {
        L->failed = true;
        return value_none();
    }
    if (prep.count > 0) {
        return jog_lower_lambda_closure(L, type, (uint32_t)index, prep.env, node->pos);
    }
    return jir_func_ref(L->module, (uint32_t)index);
}

static FnBind *bind_by_index(Lower *L, int32_t index) {
    size_t i;
    for (i = 0; i < L->fns.count; i++) {
        FnBind *bind = ct_da_at(&L->fns, i);
        if (bind && bind->index == index) {
            return bind;
        }
    }
    return NULL;
}

static JogGenericInst *generic_by_index(Lower *L, int32_t index) {
    size_t i;
    for (i = 0; i < L->generic_pending.count; i++) {
        JogGenericInst **slot = ct_da_at(&L->generic_pending, i);
        JogGenericInst *inst = slot ? *slot : NULL;
        if (inst && inst->jir_index == index) {
            return inst;
        }
    }
    return NULL;
}

static int type_is_unit(const JogType *type) {
    return !type || type->kind == JOG_TY_UNIT;
}

int32_t jog_lower_isolate_expr(Lower *L, JogNode *expr, JogType *type, const char *name, const char *jog_name) {
    JogType *fn_type;
    int32_t index;
    JirFunc *func;
    Snap snap;
    uint32_t entry = 0;
    JirOperand value;
    int failed;
    if (!L || !expr || !type || !name || !jog_name || L->failed) {
        return -1;
    }
    fn_type = jog_type_fn(L->program->types, NULL, 0, type, NULL, 0, false, false, false);
    if (!fn_type) {
        oom(L);
        return -1;
    }
    index = jir_func_add(L->module, name, jog_name, JIR_LINK_INTERNAL, fn_type, NULL);
    func = index >= 0 ? jir_func(L->module, (uint32_t)index) : NULL;
    if (!func) {
        oom(L);
        return -1;
    }
    func->pos = expr->pos;
    if (!snap_save(L, &snap)) {
        return -1;
    }
    L->func = func;
    L->ast_fn = NULL;
    L->locals.count = 0;
    L->loops.count = 0;
    L->cleanups.count = 0;
    L->overlay = NULL;
    L->overlay_count = 0;
    L->terminated = false;
    L->unchecked = false;
    L->in_cleanup = false;
    L->in_coro = 0;
    L->in_comptime = true;
    if (!new_block(L, "entry", &entry)) {
        failed = 1;
    } else {
        use_block(L, entry);
        value = lower_expr(L, expr);
        if (!L->failed && !L->terminated) {
            if (type_is_unit(type)) {
                emit_ret(L, jir_none(), expr->pos);
            } else if (value.kind == JIR_OPND_NONE) {
                fail_at(L, expr->pos, "compile-time expression does not produce a value");
            } else {
                emit_ret(L, value, expr->pos);
            }
        }
        failed = L->failed;
    }
    snap_restore(L, &snap);
    if (failed) {
        L->failed = true;
    }
    return failed ? -1 : index;
}

bool jog_lower_ensure_index(Lower *L, int32_t index) {
    JirFunc *func;
    FnBind *bind;
    JogGenericInst *inst;
    Snap snap;
    int failed;
    if (!L || L->failed || index < 0) {
        return 0;
    }
    func = jir_func(L->module, (uint32_t)index);
    if (!func) {
        fail_at(L, (JogPos){0}, "missing lowered function");
        return 0;
    }
    if (func->linkage == JIR_LINK_EXTERN) {
        return 1;
    }
    if (L->ensuring == index) {
        fail_at(L, func->pos, "compile-time cycle");
        return 0;
    }
    bind = bind_by_index(L, index);
    if (bind && bind->body_done) {
        return 1;
    }
    if (bind && bind->decl && bind->decl->symbol && bind->decl->symbol->type_param_count > 0) {
        bind->body_done = true;
        return 1;
    }
    inst = bind ? NULL : generic_by_index(L, index);
    if (inst && inst->body_done) {
        return 1;
    }
    if (!bind && !inst) {
        if (func->blocks.count > 0) {
            return 1;
        }
        fail_at(L, func->pos, "compile-time function has no body");
        return 0;
    }
    if (!snap_save(L, &snap)) {
        return 0;
    }
    L->ensuring = index;
    L->func = func;
    L->in_comptime = false;
    L->in_coro = 0;
    L->unchecked = false;
    L->in_cleanup = false;
    L->overlay = NULL;
    L->overlay_count = 0;
    if (bind) {
        bind->body_done = true;
        lower_body(L, bind->decl);
    } else if (!inst->fn) {
        fail_at(L, func->pos, "missing lowered function");
    } else {
        inst->body_done = true;
        if (inst->fn->symbol) {
            L->subst_from = inst->fn->symbol->type_params;
            L->subst_to = inst->args;
            L->subst_n = inst->arg_count;
        }
        lower_body(L, inst->fn);
    }
    failed = L->failed;
    snap_restore(L, &snap);
    if (failed) {
        L->failed = true;
    }
    return !L->failed;
}

static void lower_decl_body(Lower *L, JogNode *decl) {
    size_t i;
    int32_t index;
    if (!decl || L->failed) {
        return;
    }
    if (decl->kind == JOG_NODE_EXTERN_BLOCK) {
        for (i = 0; i < decl->as.extern_block.fns.count && !L->failed; i++) {
            lower_decl_body(L, list_at(decl->as.extern_block.fns, i));
        }
        return;
    }
    if (decl->kind != JOG_NODE_FN || decl->as.fn.is_extern) {
        return;
    }
    if (decl->symbol && decl->symbol->type_param_count > 0) {
        return;
    }
    index = find_fn(L, decl);
    if (index < 0) {
        fail_at(L, decl->pos, "missing lowered function");
        return;
    }
    (void)jog_lower_ensure_index(L, index);
}

/* NULL when this milestone can lower the global's type. */
static const char *global_gap(const JogType *type) {
    if (!type) {
        return "globals of this type";
    }
    if (type->kind == JOG_TY_DISTINCT) {
        return "distinct types";
    }
    switch (type->kind) {
    case JOG_TY_BOOL:
    case JOG_TY_INT:
    case JOG_TY_FLOAT:
    case JOG_TY_BYTES:
    case JOG_TY_BYTES_N:
    case JOG_TY_PTR:
    case JOG_TY_SLICE:
    case JOG_TY_ARRAY:
        return NULL;
    case JOG_TY_STRUCT:
    case JOG_TY_ENUM:
    case JOG_TY_TUPLE:
    case JOG_TY_FN:
        return NULL;
    case JOG_TY_ERROR:
    case JOG_TY_OPTION:
        return "errors";
    case JOG_TY_TYPE_PARAM:
    case JOG_TY_INTERFACE:
        return "generics";
    default:
        return "globals of this type";
    }
}

/* 1 when the folded integer or bool is now the data image. 0 when the
   initializer is not that kind of constant. -1 after a diagnostic. */
static int place_scalar(Lower *L, JirGlobal *global, JogNode *init) {
    JogConst value;
    uint64_t bits;
    size_t n;
    size_t i;
    uint8_t *buf;
    JogType *type = global ? global->type : NULL;
    if (!init || !type) {
        return 0;
    }
    if (type->kind != JOG_TY_INT && type->kind != JOG_TY_BOOL) {
        return 0;
    }
    if (!type->has_layout || type->size == 0 || type->size > 8) {
        return 0;
    }
    if (!is_int_lit_tree(init) && !is_bool_lit_tree(init)) {
        return 0;
    }
    memset(&value, 0, sizeof(value));
    if (!jog_const_eval(L->program->arena, L->program->diag, L->program->types, init, type, &value) ||
        (value.kind != JOG_CONST_INT && value.kind != JOG_CONST_BOOL)) {
        L->failed = true;
        return -1;
    }
    if (type->kind == JOG_TY_BOOL && value.bits > 1) {
        fail_at(L, init->pos, "constant expression is not an integer or bool");
        return -1;
    }
    n = (size_t)type->size;
    buf = ct_arena_alloc(L->program->arena, n, 1);
    if (!buf) {
        oom(L);
        return -1;
    }
    bits = value.bits;
    for (i = 0; i < n; i++) {
        buf[i] = (uint8_t)(bits & 0xffu);
        bits >>= 8;
    }
    global->data = buf;
    global->data_len = n;
    return 1;
}

/* 1 when a float literal tree is now the data image. 0 when init is not one.
   -1 after a diagnostic. */
static int place_float(Lower *L, JirGlobal *global, JogNode *init) {
    double value = 0.0;
    JogFloatLitStatus st;
    unsigned bits;
    size_t n;
    size_t i;
    uint8_t *buf;
    uint64_t raw = 0;
    JogType *type = global ? global->type : NULL;
    if (!init || !type || type->kind != JOG_TY_FLOAT || !jog_float_lit_tree(init)) {
        return 0;
    }
    bits = type->as.flt.bits == 32 ? 32u : 64u;
    n = bits == 32 ? 4u : 8u;
    if (!type->has_layout || type->size != n) {
        fail_at(L, init->pos, "float global has no layout");
        return -1;
    }
    st = jog_float_lit_eval(init, bits, &value);
    if (st != JOG_FLOAT_LIT_OK) {
        fail_at(L, init->pos, "%s", float_lit_msg(st));
        return -1;
    }
    if (bits == 32) {
        float narrow = (float)value;
        uint32_t word = 0;
        memcpy(&word, &narrow, sizeof(word));
        raw = word;
    } else {
        memcpy(&raw, &value, sizeof(value));
    }
    buf = ct_arena_alloc(L->program->arena, n, 1);
    if (!buf) {
        oom(L);
        return -1;
    }
    for (i = 0; i < n; i++) {
        buf[i] = (uint8_t)(raw & 0xffu);
        raw >>= 8;
    }
    global->data = buf;
    global->data_len = n;
    return 1;
}

static bool push_global(Lower *L, const JogPackage *pkg, JogNode *node, JogSymbol *symbol, const char *name,
                        JogType *type, int32_t part) {
    const char *gap;
    const char *mangled;
    int32_t index;
    GlobalBind bind;
    JirGlobal *global;
    if (is_discard(name)) {
        return true;
    }
    if (!symbol) {
        fail_at(L, node->pos, "missing symbol");
        return false;
    }
    if (!type || type->kind == JOG_TY_ERROR) {
        fail_at(L, node->pos, "lowering is missing a type");
        return false;
    }
    gap = global_gap(type);
    if (gap) {
        fail_at(L, node->pos, "%s are not lowered yet", gap);
        return false;
    }
    mangled = mangle_global(L, pkg, name);
    if (!mangled) {
        return false;
    }
    index = jir_global_add(L->module, mangled, type, node->kind == JOG_NODE_VAL || node->kind == JOG_NODE_CONTEXT);
    if (index < 0) {
        oom(L);
        return false;
    }
    global = jir_global(L->module, (uint32_t)index);
    if (!global) {
        oom(L);
        return false;
    }
    global->pos = node->pos;
    memset(&bind, 0, sizeof(bind));
    bind.symbol = symbol;
    bind.decl = node;
    bind.index = index;
    bind.runtime = node->as.binding.init != NULL;
    bind.rt = -1;
    bind.part = part;
    if (!ct_da_push(&L->globals, &bind)) {
        oom(L);
        return false;
    }
    return true;
}

static void clear_image(JirGlobal *global) {
    if (!global) {
        return;
    }
    global->data = NULL;
    global->data_len = 0;
    global->relocs = NULL;
    global->reloc_count = 0;
}

static int place_init(Lower *L, JirGlobal *global, JogNode *init) {
    int placed = place_scalar(L, global, init);
    if (placed == 0) {
        placed = place_float(L, global, init);
    }
    if (placed == 0) {
        placed = jog_lower_global_place(L, global, init);
    }
    return placed;
}

static bool first_of_decl(Lower *L, size_t index) {
    GlobalBind *bind = ct_da_at(&L->globals, index);
    size_t i;
    if (!bind) {
        return false;
    }
    for (i = 0; i < index; i++) {
        GlobalBind *earlier = ct_da_at(&L->globals, i);
        if (earlier && earlier->decl == bind->decl) {
            return false;
        }
    }
    return true;
}

static int copy_tuple_piece(Lower *L, JirGlobal *dst, JirGlobal *src, size_t off, size_t n) {
    uint8_t *buf;
    JirReloc *rels = NULL;
    size_t count = 0;
    size_t w = 0;
    size_t r;
    if (!dst || !src || n == 0) {
        return 1;
    }
    if (src->data_len == 0) {
        return 1;
    }
    if (!src->data || off > src->data_len || n > src->data_len - off) {
        return 0;
    }
    buf = ct_arena_alloc(L->program->arena, n, 1);
    if (!buf) {
        oom(L);
        return -1;
    }
    memcpy(buf, src->data + off, n);
    for (r = 0; r < src->reloc_count; r++) {
        uint32_t at = src->relocs[r].offset;
        if ((size_t)at >= off && (size_t)at - off < n) {
            count++;
        }
    }
    if (count > 0) {
        rels = ct_arena_alloc(L->program->arena, count * sizeof(JirReloc), _Alignof(JirReloc));
        if (!rels) {
            oom(L);
            return -1;
        }
        for (r = 0; r < src->reloc_count; r++) {
            JirReloc rel = src->relocs[r];
            if ((size_t)rel.offset < off || (size_t)rel.offset - off >= n) {
                continue;
            }
            rel.offset = (uint32_t)((size_t)rel.offset - off);
            rels[w++] = rel;
        }
    }
    dst->data = buf;
    dst->data_len = n;
    dst->relocs = rels;
    dst->reloc_count = w;
    return 1;
}

/* 1 when every name was copied out of an already placed tuple global.
   0 when init is not that. -1 after a diagnostic. */
static int place_tuple_name(Lower *L, JogNode *decl, JogNode *init) {
    GlobalBind *src_bind;
    JirGlobal *src;
    JogType *tuple;
    size_t i;
    int all = 1;
    if (!init || init->kind != JOG_NODE_IDENT || !init->symbol) {
        return 0;
    }
    src_bind = global_by_symbol(L, init->symbol);
    if (!src_bind || src_bind->runtime || src_bind->index < 0) {
        return 0;
    }
    src = jir_global(L->module, (uint32_t)src_bind->index);
    tuple = src && src->type ? jog_type_underlying(src->type) : NULL;
    if (!src || !tuple || tuple->kind != JOG_TY_TUPLE || !tuple->as.tuple.elems || !tuple->as.tuple.offsets ||
        tuple->as.tuple.count != decl->as.binding.names.count) {
        return 0;
    }
    for (i = 0; i < L->globals.count; i++) {
        GlobalBind *bind = ct_da_at(&L->globals, i);
        JirGlobal *dst;
        JogType *elem;
        int copied;
        size_t off;
        size_t n;
        if (!bind || bind->decl != decl || bind->part < 0) {
            continue;
        }
        elem = tuple->as.tuple.elems[bind->part];
        if (!elem || !elem->has_layout || elem->size > SIZE_MAX) {
            return 0;
        }
        dst = jir_global(L->module, (uint32_t)bind->index);
        off = (size_t)tuple->as.tuple.offsets[bind->part];
        n = (size_t)elem->size;
        copied = copy_tuple_piece(L, dst, src, off, n);
        if (copied < 0) {
            return -1;
        }
        if (copied == 0) {
            all = 0;
            break;
        }
    }
    if (!all) {
        for (i = 0; i < L->globals.count; i++) {
            GlobalBind *bind = ct_da_at(&L->globals, i);
            if (bind && bind->decl == decl) {
                clear_image(jir_global(L->module, (uint32_t)bind->index));
            }
        }
        return 0;
    }
    for (i = 0; i < L->globals.count; i++) {
        GlobalBind *bind = ct_da_at(&L->globals, i);
        if (bind && bind->decl == decl) {
            bind->runtime = false;
        }
    }
    return 1;
}

static bool place_group(Lower *L, JogNode *decl) {
    JogNode *init;
    size_t n;
    size_t i;
    int all = 1;
    int named;
    if (!decl) {
        return false;
    }
    init = decl->as.binding.init;
    n = decl->as.binding.names.count;
    named = place_tuple_name(L, decl, init);
    if (named < 0) {
        return false;
    }
    if (named == 1) {
        return true;
    }
    if (!init || init->kind != JOG_NODE_TUPLE || init->as.tuple.elems.count != n) {
        return true;
    }
    for (i = 0; i < n; i++) {
        const char *name = decl->as.binding.names.items ? decl->as.binding.names.items[i] : NULL;
        if (is_discard(name)) {
            return true;
        }
    }
    for (i = 0; i < L->globals.count; i++) {
        GlobalBind *bind = ct_da_at(&L->globals, i);
        JirGlobal *global;
        JogNode *elem;
        int placed;
        if (!bind || bind->decl != decl || bind->part < 0) {
            continue;
        }
        global = jir_global(L->module, (uint32_t)bind->index);
        elem = list_at(init->as.tuple.elems, (size_t)bind->part);
        if (!global || !elem) {
            fail_at(L, decl->pos, "destructuring is missing a tuple type");
            return false;
        }
        placed = place_init(L, global, elem);
        if (placed < 0) {
            return false;
        }
        if (placed == 0) {
            all = 0;
            break;
        }
    }
    if (!all) {
        for (i = 0; i < L->globals.count; i++) {
            GlobalBind *bind = ct_da_at(&L->globals, i);
            if (bind && bind->decl == decl) {
                clear_image(jir_global(L->module, (uint32_t)bind->index));
            }
        }
        return true;
    }
    for (i = 0; i < L->globals.count; i++) {
        GlobalBind *bind = ct_da_at(&L->globals, i);
        if (bind && bind->decl == decl) {
            bind->runtime = false;
        }
    }
    return true;
}

static bool place_declared(Lower *L) {
    size_t i;
    for (i = 0; i < L->globals.count && !L->failed; i++) {
        GlobalBind *bind = ct_da_at(&L->globals, i);
        JirGlobal *global;
        int placed;
        if (!bind || !bind->runtime) {
            continue;
        }
        if (bind->part >= 0) {
            if (!first_of_decl(L, i)) {
                continue;
            }
            if (!place_group(L, bind->decl)) {
                return false;
            }
            continue;
        }
        if (!bind->decl || !bind->decl->as.binding.init) {
            continue;
        }
        global = jir_global(L->module, (uint32_t)bind->index);
        if (!global) {
            oom(L);
            return false;
        }
        placed = place_init(L, global, bind->decl->as.binding.init);
        if (placed < 0) {
            return false;
        }
        if (placed == 1) {
            bind->runtime = false;
        }
    }
    return !L->failed;
}

static bool declare_global(Lower *L, const JogPackage *pkg, JogNode *node) {
    const char *name;
    JogType *type;
    size_t i;
    if (!node || L->failed) {
        return !L->failed;
    }
    if (node->kind != JOG_NODE_VAL && node->kind != JOG_NODE_VAR && node->kind != JOG_NODE_CONTEXT) {
        return true;
    }
    if (node->as.binding.destructure || node->as.binding.names.count != 1) {
        JogType *tuple = node->type;
        size_t n = node->as.binding.names.count;
        if (!tuple || tuple->kind != JOG_TY_TUPLE || !tuple->as.tuple.elems || tuple->as.tuple.count != n ||
            node->bound_symbol_count != n) {
            fail_at(L, node->pos, "destructuring is missing a tuple type");
            return false;
        }
        for (i = 0; i < n && !L->failed; i++) {
            name = node->as.binding.names.items ? node->as.binding.names.items[i] : NULL;
            type = tuple->as.tuple.elems[i];
            if (!push_global(L, pkg, node, node->bound_symbols ? node->bound_symbols[i] : NULL, name, type,
                             (int32_t)i)) {
                return false;
            }
        }
        return !L->failed;
    }
    name = node->as.binding.names.items ? node->as.binding.names.items[0] : NULL;
    if (is_discard(name)) {
        return true;
    }
    type = node->type ? node->type : node->symbol ? node->symbol->type : NULL;
    return push_global(L, pkg, node, node->symbol, name, type, -1);
}

static void declare_globals(Lower *L) {
    size_t p;
    size_t f;
    size_t d;
    for (p = 0; p < L->program->package_count && !L->failed; p++) {
        JogPackage *pkg = L->program->packages ? L->program->packages[p] : NULL;
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count && !L->failed; f++) {
            JogSourceFile *file = pkg->files ? pkg->files[f] : NULL;
            JogNode *root = file ? file->ast : NULL;
            if (!root || root->kind != JOG_NODE_FILE) {
                continue;
            }
            for (d = 0; d < root->as.file.decls.count && !L->failed; d++) {
                JogNode *decl = list_at(root->as.file.decls, d);
                if (!decl) {
                    continue;
                }
                if (decl->kind == JOG_NODE_VAL || decl->kind == JOG_NODE_VAR || decl->kind == JOG_NODE_CONTEXT) {
                    if (!declare_global(L, pkg, decl)) {
                        return;
                    }
                }
            }
        }
    }
    if (!L->failed) {
        (void)place_declared(L);
    }
}

/* Direct global uses, the same names the checker waits on. A nested
   function is its own declaration. Names reached only through a callee
   body are not edges: the checker does not wait on them either. */
static void walk_init_deps(Lower *L, JogNode *node, int32_t self, uint8_t *row);

static void note_runtime_dep(Lower *L, JogSymbol *symbol, int32_t self, uint8_t *row) {
    GlobalBind *other = global_by_symbol(L, symbol);
    if (!row || !other || !other->runtime || other->rt < 0 || other->rt == self) {
        return;
    }
    row[other->rt] = 1;
}

static void walk_list_deps(Lower *L, JogList list, int32_t self, uint8_t *row) {
    size_t i;
    for (i = 0; i < list.count; i++) {
        walk_init_deps(L, list_at(list, i), self, row);
    }
}

static void walk_init_deps(Lower *L, JogNode *node, int32_t self, uint8_t *row) {
    if (!node || L->failed) {
        return;
    }
    if (node->kind == JOG_NODE_FN) {
        return;
    }
    if ((node->kind == JOG_NODE_IDENT || node->kind == JOG_NODE_MEMBER) && node->symbol) {
        note_runtime_dep(L, node->symbol, self, row);
    }
    switch (node->kind) {
    case JOG_NODE_IDENT:
        walk_list_deps(L, node->as.ident.type_args, self, row);
        return;
    case JOG_NODE_UNARY:
    case JOG_NODE_FORCE:
        walk_init_deps(L, node->as.unary.operand, self, row);
        return;
    case JOG_NODE_BINARY:
        walk_init_deps(L, node->as.binary.lhs, self, row);
        walk_init_deps(L, node->as.binary.rhs, self, row);
        return;
    case JOG_NODE_CALL:
        walk_init_deps(L, node->as.call.callee, self, row);
        walk_list_deps(L, node->as.call.args, self, row);
        walk_init_deps(L, node->as.call.trailing, self, row);
        walk_list_deps(L, node->as.call.contexts, self, row);
        return;
    case JOG_NODE_ARG:
        walk_init_deps(L, node->as.arg.value, self, row);
        return;
    case JOG_NODE_INDEX:
        walk_init_deps(L, node->as.index.base, self, row);
        walk_init_deps(L, node->as.index.index, self, row);
        return;
    case JOG_NODE_SLICE:
        walk_init_deps(L, node->as.slice.base, self, row);
        walk_init_deps(L, node->as.slice.lo, self, row);
        walk_init_deps(L, node->as.slice.hi, self, row);
        return;
    case JOG_NODE_MEMBER:
        walk_init_deps(L, node->as.member.base, self, row);
        return;
    case JOG_NODE_TUPLE:
        walk_list_deps(L, node->as.tuple.elems, self, row);
        return;
    case JOG_NODE_CAST:
        walk_init_deps(L, node->as.cast.expr, self, row);
        walk_init_deps(L, node->as.cast.type, self, row);
        return;
    case JOG_NODE_IS:
        walk_init_deps(L, node->as.is.expr, self, row);
        walk_init_deps(L, node->as.is.type, self, row);
        return;
    case JOG_NODE_DOT_VARIANT:
        return;
    case JOG_NODE_WHEN:
        walk_init_deps(L, node->as.when.subject, self, row);
        walk_list_deps(L, node->as.when.arms, self, row);
        return;
    case JOG_NODE_WHEN_ARM:
        walk_list_deps(L, node->as.when_arm.conds, self, row);
        walk_init_deps(L, node->as.when_arm.body, self, row);
        return;
    case JOG_NODE_PAT_VARIANT:
        walk_list_deps(L, node->as.pat_variant.fields, self, row);
        return;
    case JOG_NODE_PAT_FIELD:
        walk_init_deps(L, node->as.pat_field.pattern, self, row);
        return;
    case JOG_NODE_PAT_EXPR:
    case JOG_NODE_PAT_IN:
        walk_init_deps(L, node->as.pat_expr.expr, self, row);
        return;
    case JOG_NODE_PAT_IS:
        walk_init_deps(L, node->as.pat_is.type, self, row);
        return;
    case JOG_NODE_PAT_BIND:
        if (node->symbol) {
            note_runtime_dep(L, node->symbol, self, row);
        }
        return;
    case JOG_NODE_IF:
        walk_init_deps(L, node->as.if_.cond, self, row);
        walk_init_deps(L, node->as.if_.then_block, self, row);
        walk_init_deps(L, node->as.if_.else_branch, self, row);
        return;
    case JOG_NODE_BLOCK:
        walk_list_deps(L, node->as.block.stmts, self, row);
        return;
    case JOG_NODE_EXPR_STMT:
        walk_init_deps(L, node->as.expr_stmt.expr, self, row);
        return;
    case JOG_NODE_ASSIGN:
        walk_init_deps(L, node->as.assign.target, self, row);
        walk_init_deps(L, node->as.assign.value, self, row);
        return;
    case JOG_NODE_DESTRUCTURE:
        walk_list_deps(L, node->as.destructure.targets, self, row);
        walk_init_deps(L, node->as.destructure.value, self, row);
        return;
    case JOG_NODE_RETURN:
        walk_list_deps(L, node->as.ret.values, self, row);
        return;
    case JOG_NODE_ARRAY_LIT:
        walk_init_deps(L, node->as.array_lit.type, self, row);
        walk_list_deps(L, node->as.array_lit.elems, self, row);
        return;
    case JOG_NODE_INTERP:
        walk_list_deps(L, node->as.interp.parts, self, row);
        return;
    case JOG_NODE_LAMBDA: {
        size_t i;
        for (i = 0; i < node->as.lambda.params.count && !L->failed; i++) {
            JogNode *param = list_at(node->as.lambda.params, i);
            if (param && param->kind == JOG_NODE_PARAM) {
                walk_init_deps(L, param->as.param.type, self, row);
            }
        }
        walk_init_deps(L, node->as.lambda.ret, self, row);
        walk_init_deps(L, node->as.lambda.body, self, row);
        return;
    }
    case JOG_NODE_GO:
        walk_list_deps(L, node->as.go.args, self, row);
        walk_init_deps(L, node->as.go.body, self, row);
        return;
    case JOG_NODE_BUILTIN:
        walk_list_deps(L, node->as.builtin.args, self, row);
        return;
    default:
        return;
    }
}

void jog_lower_walk_init_deps(Lower *L, JogNode *node, int32_t self, uint8_t *row) {
    walk_init_deps(L, node, self, row);
}

/* True when every runtime global this initializer names is already done.
   The checker waits on those names before the binding can finish. */
static bool runtime_deps_done(const uint8_t *deps, const uint8_t *done, size_t n, size_t i) {
    size_t j;
    for (j = 0; j < n; j++) {
        if (deps[i * n + j] && !done[j]) {
            return false;
        }
    }
    return true;
}

static const char *init_c_name(Lower *L) {
    CtSb sb;
    ct_sb_init(&sb);
    ct_sb_append_cstr(&sb, "jog_");
    append_c_piece(&sb, pkg_source(L, L->program->root));
    /* 0init cannot be a Jog identifier, so this does not collide with fn init. */
    ct_sb_append_cstr(&sb, "__0init");
    return dup_sb(L, &sb);
}

static const char *init_jog_name(Lower *L) {
    CtSb sb;
    ct_sb_init(&sb);
    ct_sb_append_cstr(&sb, pkg_source(L, L->program->root));
    ct_sb_append_cstr(&sb, ".init");
    return dup_sb(L, &sb);
}

static bool lower_init(Lower *L) {
    size_t count = 0;
    size_t i;
    size_t step;
    size_t cursor;
    size_t since_finish;
    int32_t *rts;
    uint8_t *deps;
    uint8_t *done;
    int32_t *order;
    JogType *unit;
    JogType *fn_type;
    const char *name;
    const char *jog_name;
    int32_t index;
    uint32_t entry;
    if (L->failed) {
        return false;
    }
    /* Imported packages get their own init functions. A root-only program
       keeps the inlined function below, including its rt assignment. */
    if (jog_lower_package_inits(L)) {
        return !L->failed;
    }
    for (i = 0; i < L->globals.count; i++) {
        GlobalBind *bind = ct_da_at(&L->globals, i);
        if (!bind) {
            continue;
        }
        if (bind->runtime) {
            bind->rt = (int32_t)count;
            count++;
        } else {
            bind->rt = -1;
        }
    }
    if (count == 0) {
        return true;
    }
    if (count > SIZE_MAX / count || count > SIZE_MAX / sizeof(int32_t)) {
        oom(L);
        return false;
    }
    rts = ct_arena_alloc(L->program->arena, count * sizeof(int32_t), _Alignof(int32_t));
    deps = ct_arena_alloc(L->program->arena, count * count, 1);
    done = ct_arena_alloc(L->program->arena, count, 1);
    order = ct_arena_alloc(L->program->arena, count * sizeof(int32_t), _Alignof(int32_t));
    if (!rts || !deps || !done || !order) {
        oom(L);
        return false;
    }
    memset(deps, 0, count * count);
    memset(done, 0, count);
    count = 0;
    for (i = 0; i < L->globals.count; i++) {
        GlobalBind *bind = ct_da_at(&L->globals, i);
        if (!bind || !bind->runtime) {
            continue;
        }
        rts[count] = (int32_t)i;
        count++;
    }
    for (i = 0; i < count; i++) {
        GlobalBind *bind = ct_da_at(&L->globals, (size_t)rts[i]);
        if (bind && bind->decl && bind->decl->as.binding.init) {
            walk_init_deps(L, bind->decl->as.binding.init, bind->rt, deps + (size_t)bind->rt * count);
        }
    }
    /* Same visit order as the checker's coroutines: source order, round-robin.
       A binding is skipped until every global it names has finished. It does
       not jump the cursor when its last dependency finishes, so a later
       independent initializer can run first. */
    cursor = count - 1;
    since_finish = 0;
    step = 0;
    while (step < count) {
        size_t at = (cursor + 1) % count;
        GlobalBind *stuck;
        cursor = at;
        if (done[at]) {
            since_finish++;
            if (since_finish > count) {
                stuck = NULL;
                for (i = 0; i < count; i++) {
                    if (!done[i]) {
                        stuck = ct_da_at(&L->globals, (size_t)rts[i]);
                        break;
                    }
                }
                fail_at(L, stuck && stuck->decl ? stuck->decl->pos : (JogPos){0}, "global initializer cycle");
                return false;
            }
            continue;
        }
        if (!runtime_deps_done(deps, done, count, at)) {
            since_finish++;
            if (since_finish > count) {
                stuck = ct_da_at(&L->globals, (size_t)rts[at]);
                fail_at(L, stuck && stuck->decl ? stuck->decl->pos : (JogPos){0}, "global initializer cycle");
                return false;
            }
            continue;
        }
        done[at] = 1;
        order[step] = (int32_t)at;
        step++;
        since_finish = 0;
    }
    unit = jog_type_builtin(L->program->types, JOG_BUILTIN_UNIT);
    if (!unit) {
        oom(L);
        return false;
    }
    fn_type = jog_type_fn(L->program->types, NULL, 0, unit, NULL, 0, false, false, false);
    if (!fn_type) {
        oom(L);
        return false;
    }
    name = init_c_name(L);
    jog_name = init_jog_name(L);
    if (!name || !jog_name) {
        return false;
    }
    index = jir_func_add(L->module, name, jog_name, JIR_LINK_INTERNAL, fn_type, NULL);
    if (index < 0) {
        oom(L);
        return false;
    }
    L->module->init_func = index;
    L->func = jir_func(L->module, (uint32_t)index);
    if (!L->func) {
        oom(L);
        return false;
    }
    L->locals.count = 0;
    L->loops.count = 0;
    L->overlay = NULL;
    L->overlay_count = 0;
    L->terminated = false;
    if (!new_block(L, "entry", &entry)) {
        return false;
    }
    use_block(L, entry);
    for (step = 0; step < count && !L->failed; step++) {
        GlobalBind *bind = ct_da_at(&L->globals, (size_t)rts[order[step]]);
        JogNode *init;
        JirOperand value;
        if (!bind || !bind->decl) {
            oom(L);
            return false;
        }
        if (bind->part >= 0) {
            JogType *tuple;
            JirOperand local;
            size_t g;
            if (bind->group_done) {
                continue;
            }
            init = bind->decl->as.binding.init;
            tuple = jog_lower_type(L, bind->decl->type);
            if (!init || !tuple || tuple->kind != JOG_TY_TUPLE) {
                fail_at(L, bind->decl->pos, "destructuring is missing a tuple type");
                return false;
            }
            value = lower_expr(L, init);
            if (L->failed) {
                return false;
            }
            if (L->terminated) {
                fail_at(L, init->pos, "initializer does not produce a value");
                return false;
            }
            local = as_local_op(L, value, init->pos);
            if (L->failed) {
                return false;
            }
            for (g = 0; g < L->globals.count && !L->failed; g++) {
                GlobalBind *part = ct_da_at(&L->globals, g);
                JogType *elem;
                uint32_t dst;
                JirInst *inst;
                JirOperand field;
                if (!part || part->decl != bind->decl || part->part < 0) {
                    continue;
                }
                part->group_done = true;
                elem = tuple->as.tuple.elems ? tuple->as.tuple.elems[part->part] : NULL;
                if (!elem || elem->size == 0) {
                    continue;
                }
                if (!new_local(L, NULL, elem, &dst)) {
                    return false;
                }
                inst = emit(L, JIR_EXTRACT, (int32_t)dst, init->pos);
                if (!inst) {
                    return false;
                }
                inst->a = local;
                inst->aux = (uint32_t)part->part;
                field = operand_local(L, dst);
                store_global(L, (uint32_t)part->index, field, init->pos);
            }
            continue;
        }
        init = bind->decl->as.binding.init;
        value = lower_expr(L, init);
        if (L->failed) {
            return false;
        }
        if (L->terminated) {
            fail_at(L, init ? init->pos : bind->decl->pos, "initializer does not produce a value");
            return false;
        }
        store_global(L, (uint32_t)bind->index, value, init ? init->pos : bind->decl->pos);
    }
    if (L->failed) {
        return false;
    }
    emit_ret(L, jir_none(), (JogPos){0});
    return !L->failed;
}

void jog_lower_fail(Lower *L, JogPos pos, const char *fmt, ...) {
    char buf[512];
    va_list args;
    int wrote;
    va_start(args, fmt);
    wrote = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (wrote < 0) {
        buf[0] = '\0';
    }
    fail_at(L, pos, "%s", buf);
}

bool jog_lower_new_local(Lower *L, const char *name, JogType *type, uint32_t *out) {
    return new_local(L, name, type, out);
}

JirInst *jog_lower_emit(Lower *L, JirOp op, int32_t dst, JogPos pos) {
    return emit(L, op, dst, pos);
}

void jog_lower_copy(Lower *L, uint32_t dst, JirOperand src, JogPos pos) {
    emit_copy(L, dst, src, pos);
}

void jog_lower_zero(Lower *L, uint32_t dst, JogPos pos) {
    emit_zero(L, dst, pos);
}

JirOperand jog_lower_local_op(Lower *L, uint32_t local) {
    return operand_local(L, local);
}

JirOperand jog_lower_expr(Lower *L, JogNode *node) {
    return lower_expr(L, node);
}

void jog_lower_stmt(Lower *L, JogNode *node) {
    lower_stmt(L, node);
}

JirOperand jog_lower_as_local(Lower *L, JirOperand src, JogPos pos) {
    return as_local_op(L, src, pos);
}

bool jog_lower_find_local(Lower *L, const JogSymbol *symbol, uint32_t *out) {
    return find_local(L, symbol, out);
}

bool jog_lower_bind_local(Lower *L, JogSymbol *symbol, uint32_t local) {
    return bind_local(L, symbol, local);
}

int32_t jog_lower_find_fn(Lower *L, const JogNode *decl) {
    return find_fn(L, decl);
}

JirOperand jog_lower_global_addr(Lower *L, const JogSymbol *symbol) {
    GlobalBind *bind;
    if (!L || !symbol) {
        return jir_none();
    }
    bind = global_by_symbol(L, symbol);
    if (!bind || bind->index < 0) {
        return jir_none();
    }
    return jir_global_ref(L->module, (uint32_t)bind->index);
}

GlobalBind *jog_lower_global_bind(Lower *L, const JogSymbol *symbol) {
    if (!L || !symbol) {
        return NULL;
    }
    return global_by_symbol(L, symbol);
}

JirModule *jog_lower(JogProgram *program) {
    Lower L;
    JirModule *module;
    size_t p;
    size_t f;
    size_t d;
    if (!program || !program->arena || !program->diag || !program->types) {
        return NULL;
    }
    if (ct_diag_error_count(program->diag) != 0) {
        return NULL;
    }
    module = jir_module_create(program->arena, program->types);
    if (!module) {
        CtPos pos = {0};
        ct_diag_emit(program->diag, CT_SEVERITY_ERROR, pos, "out of memory");
        return NULL;
    }
    memset(&L, 0, sizeof(L));
    L.ensuring = -1;
    L.program = program;
    L.module = module;
    if (!ct_da_init(&L.locals, sizeof(LocalBind)) || !ct_da_init(&L.fns, sizeof(FnBind)) ||
        !ct_da_init(&L.loops, sizeof(LoopFrame)) || !ct_da_init(&L.globals, sizeof(GlobalBind)) ||
        !ct_da_init(&L.generic_pending, sizeof(JogGenericInst *)) || !jog_lower_error_open(&L)) {
        ct_da_free(&L.locals);
        ct_da_free(&L.fns);
        ct_da_free(&L.loops);
        ct_da_free(&L.globals);
        ct_da_free(&L.generic_pending);
        jog_lower_error_close(&L);
        jir_module_destroy(module);
        CtPos pos = {0};
        ct_diag_emit(program->diag, CT_SEVERITY_ERROR, pos, "out of memory");
        return NULL;
    }
    for (p = 0; p < program->package_count && !L.failed; p++) {
        JogPackage *pkg = program->packages ? program->packages[p] : NULL;
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count && !L.failed; f++) {
            JogSourceFile *file = pkg->files ? pkg->files[f] : NULL;
            JogNode *root = file ? file->ast : NULL;
            if (!root || root->kind != JOG_NODE_FILE) {
                continue;
            }
            for (d = 0; d < root->as.file.decls.count && !L.failed; d++) {
                declare_decl(&L, pkg, list_at(root->as.file.decls, d));
            }
        }
    }
    if (!L.failed) {
        declare_globals(&L);
    }
    if (!L.failed) {
        (void)jog_lower_comptime_vals(&L);
    }
    for (p = 0; p < program->package_count && !L.failed; p++) {
        JogPackage *pkg = program->packages ? program->packages[p] : NULL;
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count && !L.failed; f++) {
            JogSourceFile *file = pkg->files ? pkg->files[f] : NULL;
            JogNode *root = file ? file->ast : NULL;
            if (!root || root->kind != JOG_NODE_FILE) {
                continue;
            }
            for (d = 0; d < root->as.file.decls.count && !L.failed; d++) {
                lower_decl_body(&L, list_at(root->as.file.decls, d));
            }
        }
    }
    if (!L.failed) {
        (void)lower_init(&L);
    }
    if (!L.failed && program->options.include_tests) {
        (void)jog_lower_package_tests(&L);
    }
    while (!L.failed) {
        JogNode *gfn = NULL;
        int32_t gindex = -1;
        if (!jog_lower_generic_take(&L, &gfn, &gindex)) {
            break;
        }
        L.func = jir_func(L.module, (uint32_t)gindex);
        if (!L.func) {
            jog_lower_fail(&L, gfn ? gfn->pos : (JogPos){0}, "missing lowered function");
            break;
        }
        L.ensuring = gindex;
        lower_body(&L, gfn);
        L.ensuring = -1;
        L.ast_fn = NULL;
        L.subst_from = NULL;
        L.subst_to = NULL;
        L.subst_n = 0;
    }
    ct_da_free(&L.locals);
    ct_da_free(&L.fns);
    ct_da_free(&L.loops);
    ct_da_free(&L.globals);
    ct_da_free(&L.generic_pending);
    jog_lower_error_close(&L);
    if (L.failed || !jir_verify(module, program->diag)) {
        jir_module_destroy(module);
        return NULL;
    }
    return module;
}
