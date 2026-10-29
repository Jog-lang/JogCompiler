#include "lower_enum.h"

#include "const_eval.h"
#include "lower_error.h"

#include <string.h>

/* Enum values are JIR_AGGREGATE with aux = variant index and payload fields
   in declaration order. A when on an enum reads JIR_ENUM_TAG and switches.
   Refutable payloads are compare chains in that variant's block. Integer
   literal arms switch on the value. Anything else is a compare chain.
   A value pattern uses the same compare as ==. .name is a tag-indexed
   table of the variant source names.
   !! is JIR_NULL_CHECK plus the option value. ?: and ?. branch on the
   same has-value test as T? == null. */

enum { ENUM_FIELDS = 64 };

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static bool is_discard(const char *name) {
    return !name || name[0] == '\0' || (name[0] == '_' && name[1] == '\0');
}

static JogType *peel(JogType *type) {
    return jog_type_underlying(type);
}

static bool is_unit(const JogType *type) {
    return !type || type->kind == JOG_TY_UNIT;
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

static bool is_int_lit_tree(const JogNode *node) {
    if (!node) {
        return false;
    }
    if (node->kind == JOG_NODE_INT_LIT) {
        return true;
    }
    if (node->kind == JOG_NODE_UNARY) {
        JogTokenKind op = node->as.unary.op;
        return (op == JOG_TOK_MINUS || op == JOG_TOK_TILDE) && is_int_lit_tree(node->as.unary.operand);
    }
    if (node->kind != JOG_NODE_BINARY || !is_arith_op(node->as.binary.op)) {
        return false;
    }
    return is_int_lit_tree(node->as.binary.lhs) && is_int_lit_tree(node->as.binary.rhs);
}

static void use_block(Lower *L, uint32_t block) {
    JirBlock *b = jir_block_at(L->func, block);
    L->block = block;
    L->terminated = !b || b->term.kind != JIR_TERM_NONE;
}

static bool new_block(Lower *L, const char *label, uint32_t *out) {
    size_t before;
    if (L->failed || !L->func) {
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

static void emit_unreachable(Lower *L, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_UNREACHABLE;
    term.pos = pos;
    seal(L, term);
}

static void emit_switch(Lower *L, JirOperand value, uint32_t def, JirCase *cases, size_t count, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_SWITCH;
    term.value = value;
    term.target = def;
    term.cases = cases;
    term.case_count = count;
    term.pos = pos;
    seal(L, term);
}

static JogType *bool_type(Lower *L, JogPos pos) {
    JogType *type = jog_type_builtin(L->program->types, JOG_BUILTIN_BOOL);
    if (!type) {
        jog_lower_fail(L, pos, "out of memory");
    }
    return type;
}

static bool emit_cmp(Lower *L, JirBinOp op, JirOperand a, JirOperand b, JogPos pos, JirOperand *out) {
    JogType *boolean = bool_type(L, pos);
    uint32_t dst;
    JirInst *inst;
    if (!boolean || !jog_lower_new_local(L, NULL, boolean, &dst)) {
        return false;
    }
    inst = jog_lower_emit(L, JIR_BINARY, (int32_t)dst, pos);
    if (!inst) {
        return false;
    }
    inst->aux = (uint32_t)op;
    inst->a = a;
    inst->b = b;
    *out = jog_lower_local_op(L, dst);
    return true;
}

static bool extract_at(Lower *L, JirOperand src, uint32_t index, uint32_t aux2, JogType *field, JogPos pos, JirOperand *out) {
    uint32_t dst;
    JirInst *inst;
    if (!field || is_unit(field)) {
        jog_lower_fail(L, pos, "lowering is missing a type");
        return false;
    }
    src = jog_lower_as_local(L, src, pos);
    if (L->failed || !jog_lower_new_local(L, NULL, field, &dst)) {
        return false;
    }
    inst = jog_lower_emit(L, JIR_EXTRACT, (int32_t)dst, pos);
    if (!inst) {
        return false;
    }
    inst->a = src;
    inst->aux = index;
    inst->aux2 = aux2;
    *out = jog_lower_local_op(L, dst);
    return true;
}

static bool fill_aggregate(Lower *L, uint32_t dst, uint32_t aux, const JirOperand *args, size_t count, JogPos pos) {
    JirInst *inst = jog_lower_emit(L, JIR_AGGREGATE, (int32_t)dst, pos);
    if (!inst) {
        return false;
    }
    inst->aux = aux;
    if (count > 0 && !jir_set_args(L->module, inst, args, count)) {
        jog_lower_fail(L, pos, "out of memory");
        return false;
    }
    return true;
}

static int variant_index(const JogType *type, const JogSymbol *symbol) {
    size_t i;
    if (!type || type->kind != JOG_TY_ENUM || !symbol || !type->as.nominal.variants) {
        return -1;
    }
    for (i = 0; i < type->as.nominal.variant_count; i++) {
        const JogVariant *variant = &type->as.nominal.variants[i];
        if (variant->decl && (variant->decl->symbol == symbol || variant->decl == symbol->decl)) {
            return (int)i;
        }
    }
    return -1;
}

static int variant_by_name(const JogType *type, const char *name) {
    size_t i;
    if (!type || type->kind != JOG_TY_ENUM || !name || !type->as.nominal.variants) {
        return -1;
    }
    for (i = 0; i < type->as.nominal.variant_count; i++) {
        if (type->as.nominal.variants[i].name && strcmp(type->as.nominal.variants[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool payload_free(const JogVariant *variant) {
    size_t i;
    if (!variant) {
        return false;
    }
    for (i = 0; i < variant->field_count; i++) {
        if (variant->fields && !is_unit(variant->fields[i].type)) {
            return false;
        }
    }
    return true;
}

static JirOperand make_enum(Lower *L, JogType *type, uint32_t index, const JirOperand *args, size_t count, JogPos pos) {
    uint32_t dst;
    if (!type || !jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    if (!fill_aggregate(L, dst, index, args, count, pos)) {
        return jir_none();
    }
    return jog_lower_local_op(L, dst);
}

static bool option_is_some(Lower *L, JirOperand opt, JogPos pos, JirOperand *out) {
    JogType *type = peel(opt.type);
    if (type && type->kind == JOG_TY_OPTION && jog_type_option_is_niche(type)) {
        return emit_cmp(L, JIR_NE, opt, jir_null(opt.type), pos, out);
    }
    return extract_at(L, opt, 0, 0, bool_type(L, pos), pos, out);
}

static bool option_value(Lower *L, JirOperand opt, JogType *inner, JogPos pos, JirOperand *out) {
    JogType *type = peel(opt.type);
    uint32_t dst;
    if (!inner) {
        jog_lower_fail(L, pos, "lowering is missing a type");
        return false;
    }
    if (type && type->kind == JOG_TY_OPTION && jog_type_option_is_niche(type)) {
        if (!jog_lower_new_local(L, NULL, inner, &dst)) {
            return false;
        }
        jog_lower_copy(L, dst, opt, pos);
        *out = jog_lower_local_op(L, dst);
        return !L->failed;
    }
    return extract_at(L, opt, 1, 0, inner, pos, out);
}

static bool enum_tag_of(Lower *L, JirOperand value, JogType *en, JogPos pos, JirOperand *out) {
    JogType *tag;
    uint32_t dst;
    JirInst *inst;
    if (!en || en->kind != JOG_TY_ENUM || !en->as.nominal.base) {
        jog_lower_fail(L, pos, "enum is missing a tag type");
        return false;
    }
    tag = en->as.nominal.base;
    value = jog_lower_as_local(L, value, pos);
    if (L->failed || !jog_lower_new_local(L, NULL, tag, &dst)) {
        return false;
    }
    inst = jog_lower_emit(L, JIR_ENUM_TAG, (int32_t)dst, pos);
    if (!inst) {
        return false;
    }
    inst->a = value;
    *out = jog_lower_local_op(L, dst);
    return true;
}

static bool bind_name(Lower *L, JogSymbol *symbol, const char *name, JirOperand value, JogPos pos) {
    uint32_t dst;
    LocalBind bind;
    if (!symbol || symbol->kind == JOG_SYM_VARIANT || is_discard(name)) {
        return true;
    }
    if (!value.type || !jog_lower_new_local(L, name, value.type, &dst)) {
        return false;
    }
    jog_lower_copy(L, dst, value, pos);
    bind.symbol = symbol;
    bind.local = dst;
    if (!ct_da_push(&L->locals, &bind)) {
        jog_lower_fail(L, pos, "out of memory");
        return false;
    }
    return !L->failed;
}

static bool pat_irrefutable(const JogNode *pat, const JogType *type);
static bool fields_irrefutable(const JogNode *pat, const JogVariant *variant);

/* Slots are the PAT_FIELD nodes, so a later bind can see field->symbol.
   Callers pass mapped_inner to irrefutable tests and to test_pat. */
static bool map_fields(const JogNode *pat, const JogField *fields, size_t field_count, JogNode **out) {
    unsigned char filled[ENUM_FIELDS];
    size_t positional = 0;
    size_t i;
    if (field_count > ENUM_FIELDS) {
        return false;
    }
    memset(out, 0, field_count * sizeof(*out));
    if (field_count == 0) {
        return !pat || (pat->kind == JOG_NODE_PAT_VARIANT && pat->as.pat_variant.fields.count == 0);
    }
    if (!fields || !pat || pat->kind != JOG_NODE_PAT_VARIANT) {
        return false;
    }
    memset(filled, 0, field_count);
    for (i = 0; i < pat->as.pat_variant.fields.count; i++) {
        JogNode *field = list_at(pat->as.pat_variant.fields, i);
        int slot = -1;
        if (!field || field->kind != JOG_NODE_PAT_FIELD) {
            return false;
        }
        if (field->as.pat_field.name) {
            size_t f;
            for (f = 0; f < field_count; f++) {
                if (fields[f].name && strcmp(fields[f].name, field->as.pat_field.name) == 0) {
                    slot = (int)f;
                    break;
                }
            }
        } else {
            while (positional < field_count && filled[positional]) {
                positional++;
            }
            if (positional < field_count) {
                slot = (int)positional++;
            }
        }
        if (slot < 0 || filled[slot]) {
            return false;
        }
        filled[slot] = 1;
        out[slot] = field;
    }
    return true;
}

static JogNode *mapped_inner(const JogNode *field) {
    if (field && field->kind == JOG_NODE_PAT_FIELD) {
        return field->as.pat_field.pattern;
    }
    return NULL;
}

static bool fields_irrefutable(const JogNode *pat, const JogVariant *variant) {
    JogNode *slots[ENUM_FIELDS];
    size_t i;
    if (!variant) {
        return false;
    }
    if (variant->field_count == 0) {
        return pat && pat->kind == JOG_NODE_PAT_VARIANT && pat->as.pat_variant.fields.count == 0;
    }
    if (!map_fields(pat, variant->fields, variant->field_count, slots)) {
        return false;
    }
    for (i = 0; i < variant->field_count; i++) {
        JogNode *inner = mapped_inner(slots[i]);
        if (!inner || !pat_irrefutable(inner, variant->fields[i].type)) {
            return false;
        }
    }
    return true;
}

static bool struct_pat_irrefutable(const JogNode *pat, const JogType *type) {
    JogNode *slots[ENUM_FIELDS];
    size_t i;
    size_t n;
    if (!type || type->kind != JOG_TY_STRUCT) {
        return false;
    }
    n = type->as.nominal.field_count;
    if (n > ENUM_FIELDS) {
        return false;
    }
    if (n == 0) {
        return pat && pat->kind == JOG_NODE_PAT_VARIANT && pat->as.pat_variant.fields.count == 0;
    }
    if (!map_fields(pat, type->as.nominal.fields, n, slots)) {
        return false;
    }
    for (i = 0; i < n; i++) {
        JogNode *inner = mapped_inner(slots[i]);
        if (!inner || !pat_irrefutable(inner, type->as.nominal.fields[i].type)) {
            return false;
        }
    }
    return true;
}

static bool pat_irrefutable(const JogNode *pat, const JogType *type) {
    type = peel((JogType *)type);
    if (!pat) {
        return false;
    }
    if (pat->kind == JOG_NODE_PAT_WILDCARD) {
        return true;
    }
    if (pat->kind == JOG_NODE_PAT_BIND) {
        if (pat->symbol && pat->symbol->kind == JOG_SYM_VARIANT) {
            return false;
        }
        if (type && type->kind == JOG_TY_OPTION && pat->as.ident.name && strcmp(pat->as.ident.name, "None") == 0) {
            return false;
        }
        return true;
    }
    if (type && type->kind == JOG_TY_ENUM && type->as.nominal.variant_count == 1 && pat->kind == JOG_NODE_PAT_VARIANT) {
        int index = variant_by_name(type, pat->as.pat_variant.name);
        if (index != 0) {
            return false;
        }
        return fields_irrefutable(pat, &type->as.nominal.variants[0]);
    }
    if (type && type->kind == JOG_TY_STRUCT && pat->kind == JOG_NODE_PAT_VARIANT && type->as.nominal.name &&
        pat->as.pat_variant.name && strcmp(pat->as.pat_variant.name, type->as.nominal.name) == 0) {
        return struct_pat_irrefutable(pat, type);
    }
    return false;
}

static bool pat_selects_variant(const JogNode *pat, const JogType *en, size_t *index, bool *irref) {
    int idx = -1;
    *irref = false;
    if (!pat || !en || en->kind != JOG_TY_ENUM) {
        return false;
    }
    if (pat->kind == JOG_NODE_PAT_VARIANT) {
        idx = variant_by_name(en, pat->as.pat_variant.name);
        if (idx < 0) {
            idx = variant_index(en, pat->symbol);
        }
        if (idx < 0) {
            return false;
        }
        *index = (size_t)idx;
        *irref = fields_irrefutable(pat, &en->as.nominal.variants[idx]);
        return true;
    }
    if (pat->kind == JOG_NODE_PAT_BIND && pat->symbol && pat->symbol->kind == JOG_SYM_VARIANT) {
        idx = variant_index(en, pat->symbol);
        if (idx < 0) {
            return false;
        }
        *index = (size_t)idx;
        *irref = payload_free(&en->as.nominal.variants[idx]);
        return true;
    }
    if (pat->kind == JOG_NODE_PAT_IS && pat->symbol) {
        idx = variant_index(en, pat->symbol);
        if (idx < 0) {
            return false;
        }
        *index = (size_t)idx;
        *irref = true;
        return true;
    }
    if (pat->kind == JOG_NODE_PAT_EXPR && pat->as.pat_expr.expr && pat->as.pat_expr.expr->symbol &&
        pat->as.pat_expr.expr->symbol->kind == JOG_SYM_VARIANT) {
        idx = variant_index(en, pat->as.pat_expr.expr->symbol);
        if (idx < 0 || !payload_free(&en->as.nominal.variants[idx])) {
            return false;
        }
        *index = (size_t)idx;
        *irref = true;
        return true;
    }
    return false;
}

static bool pat_is_catch_all(const JogNode *pat, const JogType *subject, bool has_subject) {
    JogType *type;
    if (!pat || !has_subject) {
        return false;
    }
    if (pat->kind == JOG_NODE_PAT_WILDCARD) {
        return true;
    }
    if (pat->kind != JOG_NODE_PAT_BIND) {
        return false;
    }
    if (pat->symbol && pat->symbol->kind == JOG_SYM_VARIANT) {
        return false;
    }
    type = peel((JogType *)subject);
    if (type && type->kind == JOG_TY_OPTION && pat->as.ident.name && strcmp(pat->as.ident.name, "None") == 0) {
        return false;
    }
    return true;
}

static bool arm_is_total(const JogNode *arm, const JogType *subject, bool has_subject) {
    size_t i;
    if (!arm) {
        return false;
    }
    if (arm->as.when_arm.conds.count == 0) {
        return true;
    }
    for (i = 0; i < arm->as.when_arm.conds.count; i++) {
        if (pat_is_catch_all(list_at(arm->as.when_arm.conds, i), subject, has_subject)) {
            return true;
        }
    }
    return false;
}

static int first_total(const JogNode *node, const JogType *subject, bool has_subject) {
    size_t i;
    for (i = 0; i < node->as.when.arms.count; i++) {
        if (arm_is_total(list_at(node->as.when.arms, i), subject, has_subject)) {
            return (int)i;
        }
    }
    return -1;
}

static bool arm_mentions(const JogNode *arm, const JogType *en, size_t index, bool *irref) {
    size_t i;
    bool any = false;
    *irref = false;
    if (!arm) {
        return false;
    }
    for (i = 0; i < arm->as.when_arm.conds.count; i++) {
        size_t got = 0;
        bool one = false;
        if (!pat_selects_variant(list_at(arm->as.when_arm.conds, i), en, &got, &one)) {
            continue;
        }
        if (got != index) {
            continue;
        }
        any = true;
        if (one) {
            *irref = true;
        }
    }
    return any;
}

static void bind_pat(Lower *L, JirOperand subject, JogNode *pat);

static void bind_variant_fields(Lower *L, JirOperand subject, JogType *en, int index, JogNode *pat) {
    const JogVariant *variant;
    JogNode *slots[ENUM_FIELDS];
    size_t i;
    if (index < 0 || !en || (size_t)index >= en->as.nominal.variant_count) {
        return;
    }
    variant = &en->as.nominal.variants[index];
    if (variant->field_count == 0 || variant->field_count > ENUM_FIELDS) {
        return;
    }
    if (!map_fields(pat, variant->fields, variant->field_count, slots)) {
        jog_lower_fail(L, pat ? pat->pos : (JogPos){0}, "variant pattern is missing a field");
        return;
    }
    for (i = 0; i < variant->field_count && !L->failed; i++) {
        JirOperand field;
        JogNode *node = slots[i];
        JogNode *inner = mapped_inner(node);
        if (is_unit(variant->fields[i].type)) {
            continue;
        }
        if (!extract_at(L, subject, (uint32_t)i, (uint32_t)index + 1, variant->fields[i].type, pat->pos, &field)) {
            return;
        }
        bind_pat(L, field, inner);
        if (node && node->symbol && !bind_name(L, node->symbol, node->as.pat_field.name, field, node->pos)) {
            return;
        }
    }
}

static void bind_struct_fields(Lower *L, JirOperand subject, const JogType *type, JogNode *pat) {
    JogNode *slots[ENUM_FIELDS];
    size_t n;
    size_t i;
    if (!type || type->kind != JOG_TY_STRUCT || !pat) {
        return;
    }
    n = type->as.nominal.field_count;
    if (n == 0 || n > ENUM_FIELDS) {
        return;
    }
    if (!map_fields(pat, type->as.nominal.fields, n, slots)) {
        jog_lower_fail(L, pat->pos, "struct pattern is missing a field");
        return;
    }
    for (i = 0; i < n && !L->failed; i++) {
        JirOperand field;
        JogNode *node = slots[i];
        JogNode *inner = mapped_inner(node);
        if (is_unit(type->as.nominal.fields[i].type)) {
            continue;
        }
        if (!extract_at(L, subject, (uint32_t)i, 0, type->as.nominal.fields[i].type, pat->pos, &field)) {
            return;
        }
        bind_pat(L, field, inner);
        if (node && node->symbol && !bind_name(L, node->symbol, node->as.pat_field.name, field, node->pos)) {
            return;
        }
    }
}

static void bind_pat(Lower *L, JirOperand subject, JogNode *pat) {
    JogType *type;
    if (!pat || L->failed || L->terminated) {
        return;
    }
    type = peel(subject.type);
    if (pat->kind == JOG_NODE_PAT_BIND) {
        if (type && type->kind == JOG_TY_OPTION && pat->as.ident.name && strcmp(pat->as.ident.name, "None") == 0) {
            return;
        }
        if (pat->symbol && pat->symbol->kind == JOG_SYM_VARIANT) {
            return;
        }
        (void)bind_name(L, pat->symbol, pat->as.ident.name, subject, pat->pos);
        return;
    }
    if (pat->kind == JOG_NODE_PAT_VARIANT && type && type->kind == JOG_TY_OPTION && pat->as.pat_variant.name &&
        strcmp(pat->as.pat_variant.name, "Some") == 0) {
        JogNode *field = list_at(pat->as.pat_variant.fields, 0);
        JogNode *inner = field && field->kind == JOG_NODE_PAT_FIELD ? field->as.pat_field.pattern : NULL;
        JirOperand value;
        JogType *elem = type->as.elem.elem;
        if (!elem || is_unit(elem)) {
            return;
        }
        if (!option_value(L, subject, elem, pat->pos, &value)) {
            return;
        }
        bind_pat(L, value, inner);
        if (field && field->symbol && !bind_name(L, field->symbol, field->as.pat_field.name, value, field->pos)) {
            return;
        }
        return;
    }
    if (pat->kind == JOG_NODE_PAT_VARIANT && type && type->kind == JOG_TY_STRUCT && type->as.nominal.name &&
        pat->as.pat_variant.name && strcmp(pat->as.pat_variant.name, type->as.nominal.name) == 0) {
        bind_struct_fields(L, subject, type, pat);
        return;
    }
    if (pat->kind == JOG_NODE_PAT_VARIANT && type && type->kind == JOG_TY_ENUM) {
        int index = variant_by_name(type, pat->as.pat_variant.name);
        if (index < 0) {
            index = variant_index(type, pat->symbol);
        }
        bind_variant_fields(L, subject, type, index, pat);
    }
}

static void test_pat(Lower *L, JirOperand subject, bool has_subject, JogNode *pat, uint32_t yes, uint32_t no);

static void test_fields(Lower *L, JirOperand subject, JogType *en, int index, JogNode *pat, uint32_t yes, uint32_t no) {
    const JogVariant *variant;
    JogNode *slots[ENUM_FIELDS];
    size_t i;
    if (L->failed || L->terminated) {
        return;
    }
    if (index < 0 || !en || (size_t)index >= en->as.nominal.variant_count) {
        emit_br(L, no, pat ? pat->pos : (JogPos){0});
        return;
    }
    variant = &en->as.nominal.variants[index];
    if (variant->field_count == 0) {
        emit_br(L, yes, pat->pos);
        return;
    }
    if (variant->field_count > ENUM_FIELDS || !map_fields(pat, variant->fields, variant->field_count, slots)) {
        jog_lower_fail(L, pat->pos, "variant pattern is missing a field");
        return;
    }
    for (i = 0; i < variant->field_count; i++) {
        JirOperand field;
        uint32_t mid;
        bool later = false;
        size_t j;
        JogNode *inner = mapped_inner(slots[i]);
        if (is_unit(variant->fields[i].type) || pat_irrefutable(inner, variant->fields[i].type)) {
            continue;
        }
        if (!extract_at(L, subject, (uint32_t)i, (uint32_t)index + 1, variant->fields[i].type, pat->pos, &field)) {
            return;
        }
        for (j = i + 1; j < variant->field_count; j++) {
            if (!is_unit(variant->fields[j].type) && !pat_irrefutable(mapped_inner(slots[j]), variant->fields[j].type)) {
                later = true;
                break;
            }
        }
        if (!later) {
            test_pat(L, field, true, inner, yes, no);
            return;
        }
        if (!new_block(L, "when.field", &mid)) {
            return;
        }
        test_pat(L, field, true, inner, mid, no);
        use_block(L, mid);
    }
    if (!L->terminated && !L->failed) {
        emit_br(L, yes, pat->pos);
    }
}

static void test_struct_fields(Lower *L, JirOperand subject, const JogType *type, JogNode *pat, uint32_t yes, uint32_t no) {
    JogNode *slots[ENUM_FIELDS];
    size_t n;
    size_t i;
    if (L->failed || L->terminated) {
        return;
    }
    if (!type || type->kind != JOG_TY_STRUCT || !pat) {
        emit_br(L, no, pat ? pat->pos : (JogPos){0});
        return;
    }
    n = type->as.nominal.field_count;
    if (n == 0) {
        emit_br(L, yes, pat->pos);
        return;
    }
    if (n > ENUM_FIELDS || !map_fields(pat, type->as.nominal.fields, n, slots)) {
        jog_lower_fail(L, pat->pos, "struct pattern is missing a field");
        return;
    }
    for (i = 0; i < n; i++) {
        JirOperand field;
        uint32_t mid;
        bool later = false;
        size_t j;
        JogNode *inner = mapped_inner(slots[i]);
        if (is_unit(type->as.nominal.fields[i].type) || pat_irrefutable(inner, type->as.nominal.fields[i].type)) {
            continue;
        }
        if (!extract_at(L, subject, (uint32_t)i, 0, type->as.nominal.fields[i].type, pat->pos, &field)) {
            return;
        }
        for (j = i + 1; j < n; j++) {
            if (!is_unit(type->as.nominal.fields[j].type) &&
                !pat_irrefutable(mapped_inner(slots[j]), type->as.nominal.fields[j].type)) {
                later = true;
                break;
            }
        }
        if (!later) {
            test_pat(L, field, true, inner, yes, no);
            return;
        }
        if (!new_block(L, "when.field", &mid)) {
            return;
        }
        test_pat(L, field, true, inner, mid, no);
        use_block(L, mid);
    }
    if (!L->terminated && !L->failed) {
        emit_br(L, yes, pat->pos);
    }
}

static bool fold_int(Lower *L, JogNode *expr, uint64_t *bits) {
    JogConst value;
    memset(&value, 0, sizeof(value));
    if (!expr || !is_int_lit_tree(expr) ||
        !jog_const_eval(L->program->arena, L->program->diag, L->program->types, expr, expr->type, &value) ||
        value.kind != JOG_CONST_INT) {
        jog_lower_fail(L, expr ? expr->pos : (JogPos){0}, "integer pattern did not fold");
        return false;
    }
    *bits = value.bits;
    return true;
}

/* A value arm uses the same compare as ==: str and bytes by length then
   bytes, enums by tag and payload, integers and bools by JIR_EQ. */
static bool value_eq(Lower *L, JirOperand subject, JirOperand rhs, JogPos pos, JirOperand *out) {
    if (jog_type_bytes_view(subject.type)) {
        *out = jog_lower_bytes_cmp(L, subject, rhs, false, pos);
        return !L->failed && out->kind != JIR_OPND_NONE;
    }
    if (subject.type) {
        JogType *under = jog_type_underlying(subject.type);
        if (under && under->kind == JOG_TY_ENUM) {
            *out = jog_lower_enum_eq(L, subject, rhs, false, pos);
            return !L->failed && out->kind != JIR_OPND_NONE;
        }
    }
    return emit_cmp(L, JIR_EQ, subject, rhs, pos, out);
}

static void test_pat(Lower *L, JirOperand subject, bool has_subject, JogNode *pat, uint32_t yes, uint32_t no) {
    JogType *type;
    JogPos pos;
    if (L->failed || L->terminated) {
        return;
    }
    if (!pat) {
        emit_br(L, no, (JogPos){0});
        return;
    }
    pos = pat->pos;
    type = has_subject ? peel(subject.type) : NULL;
    if (pat->kind == JOG_NODE_PAT_WILDCARD || (has_subject && pat_irrefutable(pat, type))) {
        emit_br(L, yes, pos);
        return;
    }
    if (!has_subject) {
        JirOperand cond;
        JogNodeKind saved = pat->kind;
        if (pat->kind == JOG_NODE_PAT_BIND) {
            pat->kind = JOG_NODE_IDENT;
        }
        cond = pat->kind == JOG_NODE_PAT_EXPR ? jog_lower_expr(L, pat->as.pat_expr.expr) : jog_lower_expr(L, pat);
        pat->kind = saved;
        if (L->failed || L->terminated) {
            return;
        }
        emit_cond(L, cond, yes, no, pos);
        return;
    }
    if (pat->kind == JOG_NODE_PAT_EXPR) {
        JogNode *expr = pat->as.pat_expr.expr;
        JirOperand rhs;
        JirOperand cmp;
        if (expr && expr->kind == JOG_NODE_NULL_LIT && type && type->kind == JOG_TY_OPTION) {
            JirOperand some;
            if (!option_is_some(L, subject, pos, &some)) {
                return;
            }
            emit_cond(L, some, no, yes, pos);
            return;
        }
        if (type && type->kind == JOG_TY_ENUM && expr && expr->symbol && expr->symbol->kind == JOG_SYM_VARIANT) {
            int index = variant_index(type, expr->symbol);
            JirOperand tag;
            if (index < 0 || !enum_tag_of(L, subject, type, pos, &tag)) {
                return;
            }
            if (!emit_cmp(L, JIR_EQ, tag, jir_int(type->as.nominal.base, (uint64_t)type->as.nominal.variants[index].tag), pos,
                          &cmp)) {
                return;
            }
            emit_cond(L, cmp, yes, no, pos);
            return;
        }
        rhs = jog_lower_expr(L, expr);
        if (L->failed || L->terminated) {
            return;
        }
        if (!value_eq(L, subject, rhs, pos, &cmp)) {
            return;
        }
        emit_cond(L, cmp, yes, no, pos);
        return;
    }
    if (pat->kind == JOG_NODE_PAT_IN) {
        JogNode *range = pat->as.pat_expr.expr;
        JirOperand lo;
        JirOperand hi;
        JirOperand ge;
        JirOperand lt;
        uint32_t mid;
        if (!range || range->kind != JOG_NODE_BINARY) {
            jog_lower_fail(L, pos, "in pattern needs a range");
            return;
        }
        lo = jog_lower_expr(L, range->as.binary.lhs);
        hi = jog_lower_expr(L, range->as.binary.rhs);
        if (L->failed || L->terminated) {
            return;
        }
        if (!emit_cmp(L, JIR_GE, subject, lo, pos, &ge) || !new_block(L, "when.range", &mid)) {
            return;
        }
        emit_cond(L, ge, mid, no, pos);
        use_block(L, mid);
        if (!emit_cmp(L, JIR_LT, subject, hi, pos, &lt)) {
            return;
        }
        emit_cond(L, lt, yes, no, pos);
        return;
    }
    if (pat->kind == JOG_NODE_PAT_IS && type && type->kind == JOG_TY_ENUM) {
        int index = variant_index(type, pat->symbol);
        JirOperand tag;
        JirOperand cmp;
        if (index < 0 || !enum_tag_of(L, subject, type, pos, &tag)) {
            return;
        }
        if (!emit_cmp(L, JIR_EQ, tag, jir_int(type->as.nominal.base, (uint64_t)type->as.nominal.variants[index].tag), pos,
                      &cmp)) {
            return;
        }
        emit_cond(L, cmp, yes, no, pos);
        return;
    }
    if (pat->kind == JOG_NODE_PAT_BIND && type && type->kind == JOG_TY_ENUM && pat->symbol &&
        pat->symbol->kind == JOG_SYM_VARIANT) {
        int index = variant_index(type, pat->symbol);
        JirOperand tag;
        JirOperand cmp;
        if (index < 0 || !enum_tag_of(L, subject, type, pos, &tag)) {
            return;
        }
        if (!emit_cmp(L, JIR_EQ, tag, jir_int(type->as.nominal.base, (uint64_t)type->as.nominal.variants[index].tag), pos,
                      &cmp)) {
            return;
        }
        emit_cond(L, cmp, yes, no, pos);
        return;
    }
    if (pat->kind == JOG_NODE_PAT_BIND && type && type->kind == JOG_TY_OPTION) {
        JirOperand some;
        if (!option_is_some(L, subject, pos, &some)) {
            return;
        }
        emit_cond(L, some, no, yes, pos);
        return;
    }
    if (pat->kind == JOG_NODE_PAT_VARIANT && type && type->kind == JOG_TY_OPTION && pat->as.pat_variant.name &&
        strcmp(pat->as.pat_variant.name, "None") == 0) {
        JirOperand some;
        if (!option_is_some(L, subject, pos, &some)) {
            return;
        }
        emit_cond(L, some, no, yes, pos);
        return;
    }
    if (pat->kind == JOG_NODE_PAT_VARIANT && type && type->kind == JOG_TY_OPTION && pat->as.pat_variant.name &&
        strcmp(pat->as.pat_variant.name, "Some") == 0) {
        JogNode *field = list_at(pat->as.pat_variant.fields, 0);
        JogNode *inner = field && field->kind == JOG_NODE_PAT_FIELD ? field->as.pat_field.pattern : NULL;
        JirOperand some;
        JirOperand value;
        uint32_t mid;
        JogType *elem = type->as.elem.elem;
        if (pat_irrefutable(inner, elem)) {
            if (!option_is_some(L, subject, pos, &some)) {
                return;
            }
            emit_cond(L, some, yes, no, pos);
            return;
        }
        if (!option_is_some(L, subject, pos, &some) || !new_block(L, "when.some", &mid)) {
            return;
        }
        emit_cond(L, some, mid, no, pos);
        use_block(L, mid);
        if (!elem || is_unit(elem) || !option_value(L, subject, elem, pos, &value)) {
            if (!L->terminated && !L->failed) {
                emit_br(L, yes, pos);
            }
            return;
        }
        test_pat(L, value, true, inner, yes, no);
        return;
    }
    if (pat->kind == JOG_NODE_PAT_VARIANT && type && type->kind == JOG_TY_ENUM) {
        int index = variant_by_name(type, pat->as.pat_variant.name);
        JirOperand tag;
        JirOperand cmp;
        uint32_t mid;
        if (index < 0) {
            index = variant_index(type, pat->symbol);
        }
        if (index < 0 || !enum_tag_of(L, subject, type, pos, &tag)) {
            return;
        }
        if (fields_irrefutable(pat, &type->as.nominal.variants[index])) {
            if (!emit_cmp(L, JIR_EQ, tag, jir_int(type->as.nominal.base, (uint64_t)type->as.nominal.variants[index].tag), pos,
                          &cmp)) {
                return;
            }
            emit_cond(L, cmp, yes, no, pos);
            return;
        }
        if (!emit_cmp(L, JIR_EQ, tag, jir_int(type->as.nominal.base, (uint64_t)type->as.nominal.variants[index].tag), pos,
                      &cmp) ||
            !new_block(L, "when.payload", &mid)) {
            return;
        }
        emit_cond(L, cmp, mid, no, pos);
        use_block(L, mid);
        test_fields(L, subject, type, index, pat, yes, no);
        return;
    }
    if (pat->kind == JOG_NODE_PAT_VARIANT && type && type->kind == JOG_TY_STRUCT && type->as.nominal.name &&
        pat->as.pat_variant.name && strcmp(pat->as.pat_variant.name, type->as.nominal.name) == 0) {
        test_struct_fields(L, subject, type, pat, yes, no);
        return;
    }
    jog_lower_fail(L, pos, "pattern is not lowered yet");
}

static void test_selected(Lower *L, JirOperand subject, JogNode *arm, const JogType *en, size_t index, uint32_t yes, uint32_t no) {
    size_t i;
    size_t n = 0;
    size_t seen = 0;
    if (L->failed) {
        return;
    }
    for (i = 0; i < arm->as.when_arm.conds.count; i++) {
        size_t got = 0;
        bool irref = false;
        if (pat_selects_variant(list_at(arm->as.when_arm.conds, i), en, &got, &irref) && got == index) {
            n++;
        }
    }
    if (n == 0) {
        emit_br(L, no, arm->pos);
        return;
    }
    for (i = 0; i < arm->as.when_arm.conds.count && !L->failed; i++) {
        JogNode *pat = list_at(arm->as.when_arm.conds, i);
        size_t got = 0;
        bool irref = false;
        uint32_t next;
        if (!pat_selects_variant(pat, en, &got, &irref) || got != index) {
            continue;
        }
        seen++;
        if (seen == n) {
            test_pat(L, subject, true, pat, yes, no);
            return;
        }
        if (!new_block(L, "when.or", &next)) {
            return;
        }
        test_pat(L, subject, true, pat, yes, next);
        use_block(L, next);
    }
}

static void test_arm(Lower *L, JirOperand subject, bool has_subject, JogNode *arm, uint32_t yes, uint32_t no) {
    size_t i;
    size_t n;
    if (L->failed || !arm) {
        return;
    }
    n = arm->as.when_arm.conds.count;
    if (n == 0) {
        emit_br(L, yes, arm->pos);
        return;
    }
    for (i = 0; i < n && !L->failed; i++) {
        uint32_t next = no;
        if (i + 1 < n && !new_block(L, "when.or", &next)) {
            return;
        }
        test_pat(L, subject, has_subject, list_at(arm->as.when_arm.conds, i), yes, next);
        if (i + 1 < n) {
            use_block(L, next);
        }
    }
}

static JirOperand lower_block_value(Lower *L, JogNode *block) {
    size_t count;
    size_t i;
    size_t base;
    JogNode *tail;
    JirOperand value;
    if (!block || block->kind != JOG_NODE_BLOCK) {
        jog_lower_fail(L, block ? block->pos : (JogPos){0}, "expected a block value");
        return jir_none();
    }
    base = L->cleanups.count;
    count = block->as.block.stmts.count;
    if (count == 0) {
        jog_lower_fail(L, block->pos, "when arm has no value");
        return jir_none();
    }
    for (i = 0; i + 1 < count; i++) {
        if (L->terminated || L->failed) {
            break;
        }
        jog_lower_stmt(L, list_at(block->as.block.stmts, i));
    }
    if (L->terminated || L->failed) {
        jog_lower_error_block_end(L, base, block->pos);
        return jir_none();
    }
    tail = list_at(block->as.block.stmts, count - 1);
    if (!tail || tail->kind != JOG_NODE_EXPR_STMT) {
        jog_lower_fail(L, block->pos, "when arm has no value");
        jog_lower_error_block_end(L, base, block->pos);
        return jir_none();
    }
    value = jog_lower_expr(L, tail->as.expr_stmt.expr);
    if (!L->failed && !L->terminated && L->cleanups.count > base && value.kind != JIR_OPND_NONE && value.type) {
        uint32_t dst;
        if (!jog_lower_new_local(L, NULL, value.type, &dst)) {
            return jir_none();
        }
        jog_lower_copy(L, dst, value, block->pos);
        value = jog_lower_local_op(L, dst);
    }
    jog_lower_error_block_end(L, base, block->pos);
    return value;
}

static void lower_arm_body(Lower *L, JogNode *arm, JirOperand subject, bool has_subject, bool as_value, uint32_t result,
                           bool have_result, uint32_t join) {
    size_t saved = L->locals.count;
    JogNode *body;
    if (L->failed || L->terminated || !arm) {
        return;
    }
    if (has_subject && arm->as.when_arm.conds.count == 1) {
        bind_pat(L, subject, list_at(arm->as.when_arm.conds, 0));
    }
    body = arm->as.when_arm.body;
    if (!as_value) {
        if (body && body->kind == JOG_NODE_BLOCK) {
            jog_lower_stmt(L, body);
        } else if (body) {
            (void)jog_lower_expr(L, body);
        }
        if (!L->terminated && !L->failed) {
            emit_br(L, join, arm->pos);
        }
    } else {
        JirOperand value = jir_none();
        if (body && body->kind == JOG_NODE_BLOCK) {
            value = lower_block_value(L, body);
        } else if (body) {
            value = jog_lower_expr(L, body);
        } else {
            jog_lower_fail(L, arm->pos, "when arm has no value");
        }
        if (!L->failed && !L->terminated) {
            if (have_result && value.kind != JIR_OPND_NONE) {
                jog_lower_copy(L, result, value, arm->pos);
            }
            emit_br(L, join, arm->pos);
        }
    }
    L->locals.count = saved;
}

static bool enum_switch_ok(const JogNode *node, const JogType *en, const JogType *subject, bool has_subject) {
    size_t i;
    for (i = 0; i < node->as.when.arms.count; i++) {
        JogNode *arm = list_at(node->as.when.arms, i);
        size_t c;
        if (arm_is_total(arm, subject, has_subject)) {
            continue;
        }
        if (!arm || arm->as.when_arm.conds.count == 0) {
            return false;
        }
        for (c = 0; c < arm->as.when_arm.conds.count; c++) {
            size_t index = 0;
            bool irref = false;
            if (!pat_selects_variant(list_at(arm->as.when_arm.conds, c), en, &index, &irref)) {
                return false;
            }
        }
    }
    return true;
}

static bool int_switch_ok(const JogNode *node, const JogType *subject, bool has_subject) {
    size_t i;
    if (!subject || subject->kind != JOG_TY_INT) {
        return false;
    }
    for (i = 0; i < node->as.when.arms.count; i++) {
        JogNode *arm = list_at(node->as.when.arms, i);
        size_t c;
        if (arm_is_total(arm, subject, has_subject)) {
            continue;
        }
        if (!arm || arm->as.when_arm.conds.count == 0) {
            return false;
        }
        for (c = 0; c < arm->as.when_arm.conds.count; c++) {
            JogNode *pat = list_at(arm->as.when_arm.conds, c);
            if (!pat || pat->kind != JOG_NODE_PAT_EXPR || !is_int_lit_tree(pat->as.pat_expr.expr)) {
                return false;
            }
        }
    }
    return true;
}

static void finish_sink(Lower *L, bool have_sink, uint32_t sink, JogPos pos) {
    if (!have_sink || L->failed) {
        return;
    }
    use_block(L, sink);
    emit_unreachable(L, pos);
}

static void emit_enum_switch(Lower *L, JirOperand subject, JogType *en, JogNode *node, uint32_t *arm_blocks, size_t limit,
                             uint32_t def, bool have_sink, uint32_t sink) {
    JirOperand tag;
    uint32_t *entries;
    unsigned char *have;
    JirCase *cases;
    size_t vc;
    size_t v;
    size_t nc = 0;
    if (!enum_tag_of(L, subject, en, node->pos, &tag)) {
        return;
    }
    vc = en->as.nominal.variant_count;
    entries = ct_arena_alloc(L->program->arena, (vc ? vc : 1) * sizeof(uint32_t), _Alignof(uint32_t));
    have = ct_arena_alloc(L->program->arena, vc ? vc : 1, 1);
    cases = ct_arena_alloc(L->program->arena, (vc ? vc : 1) * sizeof(JirCase), _Alignof(JirCase));
    if (!entries || !have || !cases) {
        jog_lower_fail(L, node->pos, "out of memory");
        return;
    }
    memset(have, 0, vc);
    for (v = 0; v < vc && !L->failed; v++) {
        size_t a;
        bool any = false;
        for (a = 0; a < limit; a++) {
            bool irref = false;
            if (arm_mentions(list_at(node->as.when.arms, a), en, v, &irref)) {
                any = true;
                break;
            }
        }
        if (!any) {
            continue;
        }
        if (!new_block(L, "when.tag", &entries[v])) {
            return;
        }
        have[v] = 1;
        cases[nc].value = (uint64_t)en->as.nominal.variants[v].tag;
        cases[nc].target = entries[v];
        nc++;
    }
    emit_switch(L, tag, def, cases, nc, node->pos);
    for (v = 0; v < vc && !L->failed; v++) {
        size_t a;
        bool stopped = false;
        if (!have[v]) {
            continue;
        }
        use_block(L, entries[v]);
        for (a = 0; a < limit && !stopped && !L->failed; a++) {
            JogNode *arm = list_at(node->as.when.arms, a);
            bool irref = false;
            uint32_t failb;
            if (!arm_mentions(arm, en, v, &irref)) {
                continue;
            }
            if (irref) {
                emit_br(L, arm_blocks[a], arm->pos);
                stopped = true;
                break;
            }
            if (!new_block(L, "when.fail", &failb)) {
                return;
            }
            test_selected(L, subject, arm, en, v, arm_blocks[a], failb);
            use_block(L, failb);
        }
        if (!stopped && !L->terminated && !L->failed) {
            emit_br(L, def, node->pos);
        }
    }
    finish_sink(L, have_sink, sink, node->pos);
}

static void emit_int_switch(Lower *L, JirOperand subject, JogNode *node, uint32_t *arm_blocks, size_t limit, uint32_t def,
                            bool have_sink, uint32_t sink) {
    size_t cap = 0;
    size_t a;
    size_t nc = 0;
    JirCase *cases;
    uint64_t *seen;
    for (a = 0; a < limit; a++) {
        JogNode *arm = list_at(node->as.when.arms, a);
        cap += arm ? arm->as.when_arm.conds.count : 0;
    }
    if (cap == 0) {
        cap = 1;
    }
    cases = ct_arena_alloc(L->program->arena, cap * sizeof(JirCase), _Alignof(JirCase));
    seen = ct_arena_alloc(L->program->arena, cap * sizeof(uint64_t), _Alignof(uint64_t));
    if (!cases || !seen) {
        jog_lower_fail(L, node->pos, "out of memory");
        return;
    }
    for (a = 0; a < limit && !L->failed; a++) {
        JogNode *arm = list_at(node->as.when.arms, a);
        size_t c;
        if (!arm) {
            continue;
        }
        for (c = 0; c < arm->as.when_arm.conds.count; c++) {
            JogNode *pat = list_at(arm->as.when_arm.conds, c);
            uint64_t bits = 0;
            size_t s;
            bool dup = false;
            if (!pat || !fold_int(L, pat->as.pat_expr.expr, &bits)) {
                return;
            }
            for (s = 0; s < nc; s++) {
                if (seen[s] == bits) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            seen[nc] = bits;
            cases[nc].value = bits;
            cases[nc].target = arm_blocks[a];
            nc++;
        }
    }
    emit_switch(L, subject, def, cases, nc, node->pos);
    finish_sink(L, have_sink, sink, node->pos);
}

static void emit_chain(Lower *L, JirOperand subject, bool has_subject, JogNode *node, uint32_t *arm_blocks, size_t limit,
                       uint32_t def, bool have_sink, uint32_t sink) {
    size_t i;
    if (limit == 0) {
        emit_br(L, def, node->pos);
    }
    for (i = 0; i < limit && !L->failed; i++) {
        JogNode *arm = list_at(node->as.when.arms, i);
        uint32_t no = def;
        if (i + 1 < limit && !new_block(L, "when.next", &no)) {
            return;
        }
        test_arm(L, subject, has_subject, arm, arm_blocks[i], no);
        if (i + 1 < limit) {
            use_block(L, no);
        }
    }
    finish_sink(L, have_sink, sink, node->pos);
}

static int place_arg(const JogVariant *variant, const char *name, size_t *positional, unsigned char *filled) {
    size_t i;
    if (name) {
        for (i = 0; i < variant->field_count; i++) {
            if (variant->fields[i].name && strcmp(variant->fields[i].name, name) == 0) {
                if (filled[i]) {
                    return -2;
                }
                filled[i] = 1;
                return (int)i;
            }
        }
        return -3;
    }
    while (*positional < variant->field_count && filled[*positional]) {
        (*positional)++;
    }
    if (*positional >= variant->field_count) {
        return -4;
    }
    filled[*positional] = 1;
    return (int)(*positional)++;
}

static JirOperand lower_variant_call(Lower *L, JogNode *call, JogType *type, int index) {
    const JogVariant *variant;
    unsigned char filled[ENUM_FIELDS];
    JirOperand values[ENUM_FIELDS];
    JirOperand args[ENUM_FIELDS];
    size_t positional = 0;
    size_t i;
    size_t n = 0;
    if (index < 0 || !type || (size_t)index >= type->as.nominal.variant_count) {
        jog_lower_fail(L, call->pos, "unknown variant");
        return jir_none();
    }
    variant = &type->as.nominal.variants[index];
    if (variant->field_count > ENUM_FIELDS) {
        jog_lower_fail(L, call->pos, "variant has too many fields");
        return jir_none();
    }
    memset(filled, 0, sizeof(filled));
    memset(values, 0, sizeof(values));
    for (i = 0; i < call->as.call.args.count && !L->failed; i++) {
        JogNode *arg = list_at(call->as.call.args, i);
        int slot;
        if (!arg || arg->kind != JOG_NODE_ARG) {
            jog_lower_fail(L, call->pos, "expected an argument");
            return jir_none();
        }
        slot = place_arg(variant, arg->as.arg.name, &positional, filled);
        if (slot < 0) {
            jog_lower_fail(L, arg->pos, "variant argument does not match a field");
            return jir_none();
        }
        values[slot] = jog_lower_expr(L, arg->as.arg.value);
        if (L->failed || L->terminated) {
            return jir_none();
        }
    }
    for (i = 0; i < variant->field_count; i++) {
        if (is_unit(variant->fields[i].type)) {
            continue;
        }
        if (!filled[i]) {
            JogNode *decl = variant->fields[i].decl;
            if (decl && decl->kind == JOG_NODE_PARAM && decl->as.param.default_value) {
                values[i] = jog_lower_expr(L, decl->as.param.default_value);
                if (L->failed || L->terminated) {
                    return jir_none();
                }
            } else {
                jog_lower_fail(L, call->pos, "missing variant field");
                return jir_none();
            }
        }
        args[n++] = values[i];
    }
    return make_enum(L, call->type ? call->type : type, (uint32_t)index, args, n, call->pos);
}

bool jog_lower_enum_call(Lower *L, JogNode *node, JirOperand *out) {
    JogNode *callee;
    JogSymbol *symbol;
    JogType *type;
    int index;
    if (!L || !node || !out || node->kind != JOG_NODE_CALL || L->failed) {
        return false;
    }
    callee = node->as.call.callee;
    symbol = node->symbol;
    if ((!symbol || symbol->kind != JOG_SYM_VARIANT) && callee) {
        symbol = callee->symbol;
    }
    if (!symbol || symbol->kind != JOG_SYM_VARIANT) {
        return false;
    }
    type = peel(node->type);
    if (!type || type->kind != JOG_TY_ENUM) {
        type = callee ? peel(callee->type) : NULL;
    }
    index = variant_index(type, symbol);
    *out = lower_variant_call(L, node, type, index);
    return true;
}

bool jog_lower_enum_member(Lower *L, JogNode *node, JirOperand *out) {
    JogType *type;
    int index;
    if (!L || !node || !out || node->kind != JOG_NODE_MEMBER || L->failed) {
        return false;
    }
    if (!node->symbol || node->symbol->kind != JOG_SYM_VARIANT) {
        return false;
    }
    type = peel(node->type);
    index = variant_index(type, node->symbol);
    if (index < 0 || !payload_free(&type->as.nominal.variants[index])) {
        jog_lower_fail(L, node->pos, "variant '%s' needs arguments", node->symbol->name ? node->symbol->name : "?");
        *out = jir_none();
        return true;
    }
    *out = make_enum(L, node->type, (uint32_t)index, NULL, 0, node->pos);
    return true;
}

JirOperand jog_lower_enum_dot(Lower *L, JogNode *node) {
    JogType *type;
    int index;
    if (!L || !node || L->failed) {
        return jir_none();
    }
    if (!node->symbol || node->symbol->kind != JOG_SYM_VARIANT) {
        jog_lower_fail(L, node->pos, "enum variant needs a known type");
        return jir_none();
    }
    type = peel(node->type);
    index = variant_index(type, node->symbol);
    if (index < 0 || !payload_free(&type->as.nominal.variants[index])) {
        jog_lower_fail(L, node->pos, "variant '%s' needs arguments", node->symbol->name ? node->symbol->name : "?");
        return jir_none();
    }
    return make_enum(L, node->type, (uint32_t)index, NULL, 0, node->pos);
}

static bool enum_payload_free(const JogType *en) {
    size_t i;
    if (!en || en->kind != JOG_TY_ENUM || !en->as.nominal.base || !en->as.nominal.variants || en->as.nominal.variant_count == 0) {
        return false;
    }
    for (i = 0; i < en->as.nominal.variant_count; i++) {
        if (!payload_free(&en->as.nominal.variants[i])) {
            return false;
        }
    }
    return true;
}

static bool enum_has_field(const JogType *en, const char *name) {
    size_t i;
    size_t f;
    if (!en || en->kind != JOG_TY_ENUM || !name || !en->as.nominal.variants) {
        return false;
    }
    for (i = 0; i < en->as.nominal.variant_count; i++) {
        const JogVariant *variant = &en->as.nominal.variants[i];
        for (f = 0; f < variant->field_count; f++) {
            const char *field = variant->fields ? variant->fields[f].name : NULL;
            if (field && strcmp(field, name) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* One block per variant, in declaration order. The current block switches
   on the tag. *miss is the default. */
static bool open_tag_switch(Lower *L, JirOperand tag, const JogType *en, JogPos pos, uint32_t **arms, uint32_t *miss) {
    size_t n;
    size_t i;
    JirCase *cases;
    uint32_t *blocks;
    if (!en || !en->as.nominal.variants || en->as.nominal.variant_count == 0) {
        jog_lower_fail(L, pos, "enum has no variants");
        return false;
    }
    n = en->as.nominal.variant_count;
    cases = ct_arena_alloc(L->program->arena, n * sizeof(JirCase), _Alignof(JirCase));
    blocks = ct_arena_alloc(L->program->arena, n * sizeof(uint32_t), _Alignof(uint32_t));
    if (!cases || !blocks) {
        jog_lower_fail(L, pos, "out of memory");
        return false;
    }
    if (!new_block(L, "tag.miss", miss)) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (!new_block(L, "tag.arm", &blocks[i])) {
            return false;
        }
        cases[i].value = (uint64_t)en->as.nominal.variants[i].tag;
        cases[i].target = blocks[i];
    }
    emit_switch(L, tag, *miss, cases, n, pos);
    *arms = blocks;
    return !L->failed;
}

static void panic_range(Lower *L, JogPos pos) {
    JogType *str;
    JirInst *inst;
    const char *msg = "value out of range";
    str = jog_type_builtin(L->program->types, JOG_BUILTIN_CONST_BYTES);
    inst = jog_lower_emit(L, JIR_PANIC, -1, pos);
    if (inst && str) {
        inst->a = jir_string(str, msg, strlen(msg));
    }
    emit_unreachable(L, pos);
}

static bool convert_checked(Lower *L, JirOperand src, JogType *dest, JogPos pos, JirOperand *out) {
    uint32_t dst;
    JirInst *inst;
    if (!dest) {
        jog_lower_fail(L, pos, "enum is missing a tag type");
        return false;
    }
    if (src.type == dest) {
        *out = src;
        return true;
    }
    if (!jog_lower_new_local(L, NULL, dest, &dst)) {
        return false;
    }
    inst = jog_lower_emit(L, JIR_CONVERT, (int32_t)dst, pos);
    if (!inst) {
        return false;
    }
    inst->checked = true;
    inst->a = src;
    *out = jog_lower_local_op(L, dst);
    return true;
}

static JogType *enum_of_value(JogType *type) {
    type = peel(type);
    if (type && type->kind == JOG_TY_PTR) {
        type = peel(type->as.elem.elem);
    }
    if (!type || type->kind != JOG_TY_ENUM) {
        return NULL;
    }
    return type;
}

/* .name is a tag-indexed table: each variant tag selects that variant's
   source name, stored as a string literal. */
bool jog_lower_enum_name(Lower *L, JogNode *node, JirOperand *out) {
    JogNode *base_node;
    JogType *en;
    JogType *str;
    JirOperand base;
    JirOperand value;
    JirOperand tag;
    uint32_t *arms = NULL;
    uint32_t miss = 0;
    uint32_t join = 0;
    uint32_t result = 0;
    size_t i;
    if (!L || !node || !out || node->kind != JOG_NODE_MEMBER || L->failed) {
        return false;
    }
    if (node->as.member.safe || !node->as.member.name || strcmp(node->as.member.name, "name") != 0) {
        return false;
    }
    if (node->symbol && node->symbol->kind == JOG_SYM_FIELD) {
        return false;
    }
    base_node = node->as.member.base;
    en = enum_of_value(base_node ? base_node->type : NULL);
    if (!en || enum_has_field(en, "name")) {
        return false;
    }
    str = node->type ? node->type : jog_type_builtin(L->program->types, JOG_BUILTIN_CONST_BYTES);
    if (!str || !jog_lower_new_local(L, NULL, str, &result)) {
        *out = jir_none();
        return true;
    }
    base = jog_lower_as_local(L, jog_lower_expr(L, base_node), node->pos);
    if (L->failed || L->terminated) {
        *out = jir_none();
        return true;
    }
    if (peel(base.type) && peel(base.type)->kind == JOG_TY_PTR) {
        JogType *elem = peel(peel(base.type)->as.elem.elem);
        uint32_t loaded;
        JirInst *inst;
        if (!elem || !jog_lower_new_local(L, NULL, elem, &loaded)) {
            *out = jir_none();
            return true;
        }
        inst = jog_lower_emit(L, JIR_LOAD, (int32_t)loaded, node->pos);
        if (!inst) {
            *out = jir_none();
            return true;
        }
        inst->a = base;
        value = jog_lower_local_op(L, loaded);
    } else {
        value = base;
    }
    if (!enum_tag_of(L, value, en, node->pos, &tag) || !new_block(L, "name.join", &join) ||
        !open_tag_switch(L, tag, en, node->pos, &arms, &miss)) {
        *out = jir_none();
        return true;
    }
    for (i = 0; i < en->as.nominal.variant_count && !L->failed; i++) {
        const char *name = en->as.nominal.variants[i].name ? en->as.nominal.variants[i].name : "";
        use_block(L, arms[i]);
        jog_lower_copy(L, result, jir_string(str, name, strlen(name)), node->pos);
        emit_br(L, join, node->pos);
    }
    use_block(L, miss);
    emit_unreachable(L, node->pos);
    use_block(L, join);
    *out = jog_lower_local_op(L, result);
    return true;
}

static bool lower_tag_to_enum(Lower *L, JogNode *node, JogType *en, JirOperand *out) {
    JirOperand raw;
    JirOperand tag;
    JirOperand built = jir_none();
    uint32_t *arms = NULL;
    uint32_t miss = 0;
    uint32_t join = 0;
    uint32_t result = 0;
    size_t i;
    if (!node->type || !jog_lower_new_local(L, NULL, node->type, &result)) {
        *out = jir_none();
        return true;
    }
    raw = jog_lower_expr(L, node->as.cast.expr);
    if (L->failed || L->terminated || !convert_checked(L, raw, en->as.nominal.base, node->pos, &tag)) {
        *out = jir_none();
        return true;
    }
    if (!new_block(L, "cast.join", &join) || !open_tag_switch(L, tag, en, node->pos, &arms, &miss)) {
        *out = jir_none();
        return true;
    }
    for (i = 0; i < en->as.nominal.variant_count && !L->failed; i++) {
        use_block(L, arms[i]);
        built = make_enum(L, node->type, (uint32_t)i, NULL, 0, node->pos);
        if (L->failed || built.kind == JIR_OPND_NONE) {
            *out = jir_none();
            return true;
        }
        jog_lower_copy(L, result, built, node->pos);
        emit_br(L, join, node->pos);
    }
    use_block(L, miss);
    panic_range(L, node->pos);
    use_block(L, join);
    *out = jog_lower_local_op(L, result);
    return true;
}

bool jog_lower_enum_cast(Lower *L, JogNode *node, JirOperand *out) {
    JogType *en;
    JogType *src;
    JogType *dest;
    JirOperand value;
    JirOperand tag;
    if (!L || !node || !out || node->kind != JOG_NODE_CAST || node->as.cast.op != JOG_TOK_KW_AS || L->failed) {
        return false;
    }
    src = node->as.cast.expr ? peel(node->as.cast.expr->type) : NULL;
    dest = peel(node->type);
    if (dest && dest->kind == JOG_TY_ENUM && enum_payload_free(dest) && src && src->kind == JOG_TY_INT) {
        return lower_tag_to_enum(L, node, dest, out);
    }
    en = src;
    if (!en || en->kind != JOG_TY_ENUM) {
        return false;
    }
    if (!en->as.nominal.base || node->type != en->as.nominal.base) {
        jog_lower_fail(L, node->pos, "cannot cast an enum to that type");
        *out = jir_none();
        return true;
    }
    value = jog_lower_expr(L, node->as.cast.expr);
    if (L->failed || L->terminated || !enum_tag_of(L, value, en, node->pos, &tag)) {
        *out = jir_none();
        return true;
    }
    *out = tag;
    return true;
}

JirOperand jog_lower_enum_is(Lower *L, JogNode *node) {
    JirOperand value;
    JirOperand tag;
    JirOperand cmp;
    JogType *en;
    int index;
    if (!L || !node || L->failed) {
        return jir_none();
    }
    value = jog_lower_as_local(L, jog_lower_expr(L, node->as.is.expr), node->pos);
    if (L->failed || L->terminated) {
        return jir_none();
    }
    en = peel(node->as.is.expr ? node->as.is.expr->type : NULL);
    index = variant_index(en, node->symbol);
    if (index < 0 || !enum_tag_of(L, value, en, node->pos, &tag)) {
        return jir_none();
    }
    if (!emit_cmp(L, JIR_EQ, tag, jir_int(en->as.nominal.base, (uint64_t)en->as.nominal.variants[index].tag), node->pos, &cmp)) {
        return jir_none();
    }
    return cmp;
}

JirOperand jog_lower_enum_force(Lower *L, JogNode *node) {
    JirOperand value;
    JirInst *inst;
    JogType *type;
    JogType *inner;
    JirOperand out;
    if (!L || !node || L->failed) {
        return jir_none();
    }
    value = jog_lower_as_local(L, jog_lower_expr(L, node->as.unary.operand), node->pos);
    if (L->failed || L->terminated) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_NULL_CHECK, -1, node->pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = value;
    type = peel(value.type);
    inner = node->type ? node->type : (type && type->kind == JOG_TY_OPTION ? type->as.elem.elem : NULL);
    if (type && type->kind == JOG_TY_OPTION && jog_type_option_is_niche(type)) {
        uint32_t dst;
        if (!inner || !jog_lower_new_local(L, NULL, inner, &dst)) {
            return jir_none();
        }
        jog_lower_copy(L, dst, value, node->pos);
        return jog_lower_local_op(L, dst);
    }
    if (!option_value(L, value, inner, node->pos, &out)) {
        return jir_none();
    }
    return out;
}

JirOperand jog_lower_enum_elvis(Lower *L, JogNode *node) {
    JirOperand left;
    JirOperand some;
    JirOperand value;
    JogType *inner;
    uint32_t result;
    uint32_t yes;
    uint32_t no;
    uint32_t join;
    if (!L || !node || L->failed || L->terminated) {
        return jir_none();
    }
    inner = node->type;
    if (!inner || !jog_lower_new_local(L, NULL, inner, &result)) {
        return jir_none();
    }
    left = jog_lower_as_local(L, jog_lower_expr(L, node->as.binary.lhs), node->pos);
    if (L->failed || L->terminated || !option_is_some(L, left, node->pos, &some)) {
        return jir_none();
    }
    if (!new_block(L, "elvis.some", &yes) || !new_block(L, "elvis.none", &no) || !new_block(L, "elvis.join", &join)) {
        return jir_none();
    }
    emit_cond(L, some, yes, no, node->pos);
    use_block(L, yes);
    if (option_value(L, left, inner, node->pos, &value) && !L->terminated) {
        jog_lower_copy(L, result, value, node->pos);
        emit_br(L, join, node->pos);
    }
    use_block(L, no);
    value = jog_lower_expr(L, node->as.binary.rhs);
    if (!L->failed && !L->terminated) {
        jog_lower_copy(L, result, value, node->pos);
        emit_br(L, join, node->pos);
    }
    use_block(L, join);
    return jog_lower_local_op(L, result);
}

static int struct_field_index(JogType *owner, const JogSymbol *symbol, const char *name) {
    size_t i;
    owner = peel(owner);
    if (!owner || owner->kind != JOG_TY_STRUCT || !owner->as.nominal.fields) {
        return -1;
    }
    for (i = 0; i < owner->as.nominal.field_count; i++) {
        const JogField *field = &owner->as.nominal.fields[i];
        if (symbol && field->decl && symbol->decl == field->decl) {
            return (int)i;
        }
    }
    for (i = 0; i < owner->as.nominal.field_count; i++) {
        const JogField *field = &owner->as.nominal.fields[i];
        if (name && field->name && strcmp(field->name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

JirOperand jog_lower_enum_safe(Lower *L, JogNode *node) {
    JirOperand base;
    JirOperand some;
    JirOperand inner_val;
    JirOperand field_val;
    JogType *opt;
    JogType *elem;
    JogType *owner;
    JogType *result_ty;
    JogType *field_ty;
    bool as_ptr;
    int index;
    uint32_t result;
    uint32_t yes;
    uint32_t no;
    uint32_t join;
    if (!L || !node || L->failed || L->terminated) {
        return jir_none();
    }
    result_ty = node->type;
    if (!result_ty || !jog_lower_new_local(L, NULL, result_ty, &result)) {
        return jir_none();
    }
    base = jog_lower_as_local(L, jog_lower_expr(L, node->as.member.base), node->pos);
    if (L->failed || L->terminated) {
        return jir_none();
    }
    opt = peel(base.type);
    elem = opt && opt->kind == JOG_TY_OPTION ? peel(opt->as.elem.elem) : NULL;
    as_ptr = elem && elem->kind == JOG_TY_PTR;
    owner = as_ptr ? peel(elem->as.elem.elem) : elem;
    index = struct_field_index(owner, node->symbol, node->as.member.name);
    if (index < 0 || !owner->as.nominal.fields) {
        jog_lower_fail(L, node->pos, "operator '?.' needs a struct field");
        return jir_none();
    }
    field_ty = owner->as.nominal.fields[index].type;
    if (!option_is_some(L, base, node->pos, &some)) {
        return jir_none();
    }
    if (!new_block(L, "safe.some", &yes) || !new_block(L, "safe.none", &no) || !new_block(L, "safe.join", &join)) {
        return jir_none();
    }
    emit_cond(L, some, yes, no, node->pos);
    use_block(L, no);
    if (peel(result_ty) && peel(result_ty)->kind == JOG_TY_OPTION && jog_type_option_is_niche(peel(result_ty))) {
        jog_lower_copy(L, result, jir_null(result_ty), node->pos);
    } else {
        jog_lower_zero(L, result, node->pos);
    }
    if (!L->terminated) {
        emit_br(L, join, node->pos);
    }
    use_block(L, yes);
    if (!option_value(L, base, as_ptr ? elem : owner, node->pos, &inner_val)) {
        return jir_none();
    }
    if (as_ptr) {
        JogType *ptr_ty = jog_type_ptr(L->program->types, field_ty, false);
        uint32_t addr;
        uint32_t loaded;
        JirInst *inst;
        if (!ptr_ty || is_unit(field_ty) || !jog_lower_new_local(L, NULL, ptr_ty, &addr)) {
            return jir_none();
        }
        inst = jog_lower_emit(L, JIR_FIELD_ADDR, (int32_t)addr, node->pos);
        if (!inst) {
            return jir_none();
        }
        inst->a = inner_val;
        inst->aux = (uint32_t)index;
        if (!jog_lower_new_local(L, NULL, field_ty, &loaded)) {
            return jir_none();
        }
        inst = jog_lower_emit(L, JIR_LOAD, (int32_t)loaded, node->pos);
        if (!inst) {
            return jir_none();
        }
        inst->a = jog_lower_local_op(L, addr);
        field_val = jog_lower_local_op(L, loaded);
    } else if (!is_unit(field_ty)) {
        if (!extract_at(L, inner_val, (uint32_t)index, 0, field_ty, node->pos, &field_val)) {
            return jir_none();
        }
    } else {
        field_val = jir_none();
    }
    if (!L->failed && !L->terminated) {
        JirOperand args[2];
        JogType *boolean = bool_type(L, node->pos);
        if (!boolean) {
            return jir_none();
        }
        args[0] = jir_int(boolean, 1);
        args[1] = field_val;
        if (!is_unit(field_ty)) {
            if (!fill_aggregate(L, result, 0, args, 2, node->pos)) {
                return jir_none();
            }
        } else if (!fill_aggregate(L, result, 0, args, 1, node->pos)) {
            return jir_none();
        }
        emit_br(L, join, node->pos);
    }
    use_block(L, join);
    return jog_lower_local_op(L, result);
}

JirOperand jog_lower_enum_when(Lower *L, JogNode *node, bool as_value) {
    JirOperand subject;
    JogType *sty = NULL;
    bool has_subject;
    bool must_cover;
    bool have_result = false;
    bool have_sink = false;
    uint32_t result = 0;
    uint32_t join;
    uint32_t sink = 0;
    uint32_t def;
    uint32_t *arm_blocks;
    size_t n;
    size_t i;
    size_t limit;
    int total;
    if (!L || !node || L->failed || L->terminated) {
        return jir_none();
    }
    n = node->as.when.arms.count;
    if (n == 0) {
        jog_lower_fail(L, node->pos, "when needs an arm");
        return jir_none();
    }
    has_subject = node->as.when.subject != NULL;
    subject = jir_none();
    if (has_subject) {
        subject = jog_lower_as_local(L, jog_lower_expr(L, node->as.when.subject), node->pos);
        if (L->failed || L->terminated) {
            return jir_none();
        }
        sty = peel(node->as.when.subject->type);
    }
    if (as_value && node->type && !is_unit(node->type)) {
        if (!jog_lower_new_local(L, NULL, node->type, &result)) {
            return jir_none();
        }
        have_result = true;
    }
    if (!new_block(L, "when.join", &join)) {
        return jir_none();
    }
    arm_blocks = ct_arena_alloc(L->program->arena, n * sizeof(uint32_t), _Alignof(uint32_t));
    if (!arm_blocks) {
        jog_lower_fail(L, node->pos, "out of memory");
        return jir_none();
    }
    for (i = 0; i < n; i++) {
        if (!new_block(L, "when.arm", &arm_blocks[i])) {
            return jir_none();
        }
    }
    total = first_total(node, sty, has_subject);
    limit = total >= 0 ? (size_t)total : n;
    must_cover = has_subject && sty && (sty->kind == JOG_TY_ENUM || sty->kind == JOG_TY_OPTION);
    if (total >= 0) {
        def = arm_blocks[total];
    } else if (!must_cover) {
        def = join;
    } else {
        if (!new_block(L, "when.miss", &sink)) {
            return jir_none();
        }
        have_sink = true;
        def = sink;
    }
    if (has_subject && sty && sty->kind == JOG_TY_ENUM && enum_switch_ok(node, sty, sty, true)) {
        emit_enum_switch(L, subject, sty, node, arm_blocks, limit, def, have_sink, sink);
    } else if (has_subject && int_switch_ok(node, sty, true)) {
        emit_int_switch(L, subject, node, arm_blocks, limit, def, have_sink, sink);
    } else {
        emit_chain(L, subject, has_subject, node, arm_blocks, limit, def, have_sink, sink);
    }
    for (i = 0; i < n && !L->failed; i++) {
        use_block(L, arm_blocks[i]);
        lower_arm_body(L, list_at(node->as.when.arms, i), subject, has_subject, as_value, result, have_result, join);
    }
    if (L->failed) {
        return jir_none();
    }
    use_block(L, join);
    if (have_result) {
        return jog_lower_local_op(L, result);
    }
    return jir_none();
}
