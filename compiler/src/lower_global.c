#include "lower_global.h"

#include "check_comptime.h"
#include "const_eval.h"
#include "file.h"
#include "float_lit.h"

#include <stdlib.h>
#include <string.h>

/* Places a constant global image. Pointer slots stay zero and a JirReloc
   names the string, global, or function. Anything that is not a constant
   image is left for init_func. */

typedef struct Img {
    Lower *L;
    uint8_t *bytes;
    size_t size;
    JirReloc *rel;
    size_t nrel;
    size_t cap;
} Img;

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static JogType *peeled(JogType *type) {
    JogType *base = type ? jog_type_underlying(type) : NULL;
    return base ? base : type;
}

static int add_reloc(Img *img, uint32_t offset, uint8_t kind, uint32_t index, uint32_t addend, const char *ptr,
                     size_t len) {
    JirReloc *grown;
    size_t ncap;
    size_t i;
    if (!img || img->L->failed) {
        return -1;
    }
    for (i = 0; i < img->nrel; i++) {
        if (img->rel[i].offset == offset) {
            jog_lower_fail(img->L, (JogPos){0}, "global image has two pointers at one offset");
            return -1;
        }
    }
    if (img->nrel == img->cap) {
        ncap = img->cap ? img->cap * 2 : 4;
        grown = realloc(img->rel, ncap * sizeof(*grown));
        if (!grown) {
            jog_lower_fail(img->L, (JogPos){0}, "out of memory");
            return -1;
        }
        img->rel = grown;
        img->cap = ncap;
    }
    memset(&img->rel[img->nrel], 0, sizeof(img->rel[img->nrel]));
    img->rel[img->nrel].offset = offset;
    img->rel[img->nrel].index = index;
    img->rel[img->nrel].addend = addend;
    img->rel[img->nrel].kind = kind;
    img->rel[img->nrel].ptr = ptr;
    img->rel[img->nrel].len = len;
    img->nrel++;
    return 1;
}

static void write_le(uint8_t *dst, size_t n, uint64_t bits) {
    size_t i;
    for (i = 0; i < n; i++) {
        dst[i] = (uint8_t)(bits & 0xffu);
        bits >>= 8;
    }
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
    if (node->kind == JOG_NODE_INT_LIT || node->kind == JOG_NODE_CHAR_LIT) {
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

static int place_at(Img *img, size_t offset, JogType *type, JogNode *node, int depth);

static int place_scalar_bits(Img *img, size_t offset, JogType *type, uint64_t bits, size_t n) {
    if (n == 0 || offset > img->size || n > img->size - offset) {
        jog_lower_fail(img->L, (JogPos){0}, "global field is outside its type");
        return -1;
    }
    (void)type;
    write_le(img->bytes + offset, n, bits);
    return 1;
}

static int place_int_tree(Img *img, size_t offset, JogType *type, JogNode *node) {
    JogConst value;
    JogType *base = peeled(type);
    size_t n;
    if (!base || (base->kind != JOG_TY_INT && base->kind != JOG_TY_BOOL) || !type->has_layout || type->size == 0 ||
        type->size > 8) {
        return 0;
    }
    if (node->kind == JOG_NODE_CHAR_LIT) {
        uint32_t cp = 0;
        if (!jog_char_codepoint(node->as.literal.text, node->as.literal.len, &cp)) {
            jog_lower_fail(img->L, node->pos, "invalid char literal");
            return -1;
        }
        return place_scalar_bits(img, offset, type, cp, (size_t)type->size);
    }
    if (!is_int_lit_tree(node) && !is_bool_lit_tree(node)) {
        return 0;
    }
    memset(&value, 0, sizeof(value));
    if (!jog_const_eval(img->L->program->arena, img->L->program->diag, img->L->program->types, node, type, &value) ||
        (value.kind != JOG_CONST_INT && value.kind != JOG_CONST_BOOL)) {
        img->L->failed = true;
        return -1;
    }
    n = (size_t)type->size;
    if (base->kind == JOG_TY_BOOL && value.bits > 1) {
        jog_lower_fail(img->L, node->pos, "constant expression is not an integer or bool");
        return -1;
    }
    return place_scalar_bits(img, offset, type, value.bits, n);
}

static const char *float_msg(JogFloatLitStatus st) {
    if (st == JOG_FLOAT_LIT_FIT) {
        return "float constant does not fit";
    }
    if (st == JOG_FLOAT_LIT_DIV0) {
        return "division by zero";
    }
    return "invalid float literal";
}

static int place_float_tree(Img *img, size_t offset, JogType *type, JogNode *node) {
    JogType *base = peeled(type);
    double value = 0.0;
    unsigned bits;
    size_t n;
    uint64_t raw = 0;
    JogFloatLitStatus st;
    if (!base || base->kind != JOG_TY_FLOAT || !jog_float_lit_tree(node)) {
        return 0;
    }
    bits = base->as.flt.bits == 32 ? 32u : 64u;
    n = bits == 32 ? 4u : 8u;
    if (!type->has_layout || type->size != n) {
        return 0;
    }
    st = jog_float_lit_eval(node, bits, &value);
    if (st != JOG_FLOAT_LIT_OK) {
        jog_lower_fail(img->L, node->pos, "%s", float_msg(st));
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
    return place_scalar_bits(img, offset, type, raw, n);
}

static int place_bytes(Img *img, size_t offset, JogType *type, const char *ptr, size_t len, int cstring) {
    uint32_t psz;
    JogType *base = peeled(type);
    if (!base) {
        return 0;
    }
    psz = jog_types_ptr_size(img->L->program->types);
    if (psz == 0 || offset > img->size || (size_t)psz > img->size - offset) {
        jog_lower_fail(img->L, (JogPos){0}, "global field is outside its type");
        return -1;
    }
    if (cstring) {
        if (base->kind != JOG_TY_PTR || type->size != psz) {
            return 0;
        }
        return add_reloc(img, (uint32_t)offset, JIR_RELOC_CSTRING, 0, 0, ptr ? ptr : "", len);
    }
    if (base->kind != JOG_TY_BYTES || type->size != (uint64_t)psz * 2 || (size_t)psz > img->size - offset - psz) {
        return 0;
    }
    write_le(img->bytes + offset + psz, psz, (uint64_t)len);
    return add_reloc(img, (uint32_t)offset, JIR_RELOC_STRING, 0, 0, ptr ? ptr : "", len);
}

static void fail_embed(Lower *L, JogPos pos, JogEmbedStatus status, const char *literal) {
    const char *text = literal ? literal : "";
    switch (status) {
    case JOG_EMBED_OK:
        return;
    case JOG_EMBED_OOM:
        jog_lower_fail(L, pos, "out of memory");
        return;
    case JOG_EMBED_COUNT:
        jog_lower_fail(L, pos, "@embed expects one argument");
        return;
    case JOG_EMBED_LITERAL:
        jog_lower_fail(L, pos, "@embed expects a string literal");
        return;
    case JOG_EMBED_RELATIVE:
        jog_lower_fail(L, pos, "@embed path must be a relative file path");
        return;
    case JOG_EMBED_ESCAPE:
        jog_lower_fail(L, pos, "@embed path escapes the module");
        return;
    case JOG_EMBED_DIR:
        jog_lower_fail(L, pos, "cannot embed '%s': path is a directory", text);
        return;
    case JOG_EMBED_MISSING:
        jog_lower_fail(L, pos, "cannot embed '%s': file is missing", text);
        return;
    }
}

static int place_embed(Img *img, size_t offset, JogType *type, JogNode *node) {
    const char *literal = NULL;
    size_t literal_len = 0;
    const char *full = NULL;
    CtFile file;
    JogEmbedStatus status;
    int arg;
    arg = jog_embed_literal(node, &literal, &literal_len);
    if (arg != JOG_EMBED_OK) {
        fail_embed(img->L, node->pos, (JogEmbedStatus)arg, NULL);
        return -1;
    }
    status = jog_embed_resolve(img->L->program, node->pos.file, literal, literal_len, &full);
    if (status == JOG_EMBED_OK) {
        status = jog_embed_stat(full);
    }
    if (status != JOG_EMBED_OK) {
        fail_embed(img->L, node->pos, status, literal);
        return -1;
    }
    memset(&file, 0, sizeof(file));
    if (!img->L->program->arena || !ct_file_read(img->L->program->arena, NULL, full, &file)) {
        fail_embed(img->L, node->pos, JOG_EMBED_MISSING, literal);
        return -1;
    }
    return place_bytes(img, offset, type, file.data ? file.data : "", file.size, 0);
}

static int place_fn_value(Img *img, size_t offset, JogType *type, JogNode *node, JogSymbol *symbol) {
    JogType *base = peeled(type);
    int32_t index;
    uint32_t psz;
    (void)node;
    if (!symbol || symbol->kind != JOG_SYM_FUNCTION || !base || base->kind != JOG_TY_FN) {
        return 0;
    }
    if (symbol->type_param_count > 0 || base->as.fn.context_count > 0 || base->as.fn.variadic) {
        return 0;
    }
    index = jog_lower_find_fn(img->L, symbol->decl);
    if (index < 0) {
        return 0;
    }
    psz = jog_types_ptr_size(img->L->program->types);
    if (psz == 0 || type->size < psz || offset > img->size || (size_t)psz > img->size - offset) {
        return 0;
    }
    return add_reloc(img, (uint32_t)offset, JIR_RELOC_FUNC, (uint32_t)index, 0, NULL, 0);
}

static int copy_global_image(Img *img, size_t offset, JogType *type, GlobalBind *bind) {
    JirGlobal *src;
    size_t i;
    if (!bind || bind->runtime || bind->index < 0) {
        return 0;
    }
    src = jir_global(img->L->module, (uint32_t)bind->index);
    if (!src || !src->type || !type->has_layout || !src->type->has_layout || src->type->size != type->size) {
        return 0;
    }
    if (src->data_len == 0) {
        return 1;
    }
    if (src->data_len != (size_t)type->size || !src->data || offset > img->size || src->data_len > img->size - offset) {
        return 0;
    }
    memcpy(img->bytes + offset, src->data, src->data_len);
    for (i = 0; i < src->reloc_count; i++) {
        const JirReloc *rel = &src->relocs[i];
        uint64_t at = (uint64_t)offset + rel->offset;
        int st;
        if (at > UINT32_MAX) {
            jog_lower_fail(img->L, (JogPos){0}, "global field is outside its type");
            return -1;
        }
        st = add_reloc(img, (uint32_t)at, rel->kind, rel->index, rel->addend, rel->ptr, rel->len);
        if (st < 0) {
            return st;
        }
    }
    return 1;
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

static int place_fields(Img *img, size_t base, const JogField *fields, size_t count, JogNode *call, int depth) {
    size_t i;
    size_t positional = 0;
    unsigned char *filled;
    if (count == 0) {
        return 1;
    }
    filled = calloc(count, 1);
    if (!filled) {
        jog_lower_fail(img->L, call ? call->pos : (JogPos){0}, "out of memory");
        return -1;
    }
    if (call) {
        for (i = 0; i < call->as.call.args.count; i++) {
            JogNode *arg = list_at(call->as.call.args, i);
            int slot = -1;
            size_t s;
            int st;
            if (!arg || arg->kind != JOG_NODE_ARG) {
                free(filled);
                return 0;
            }
            if (arg->as.arg.name) {
                for (s = 0; s < count; s++) {
                    if (fields[s].name && strcmp(fields[s].name, arg->as.arg.name) == 0) {
                        slot = (int)s;
                        break;
                    }
                }
            } else {
                slot = (int)positional;
                positional++;
            }
            if (slot < 0 || (size_t)slot >= count || filled[slot]) {
                free(filled);
                return 0;
            }
            filled[slot] = 1;
            st = place_at(img, base + (size_t)fields[slot].offset, fields[slot].type, arg->as.arg.value, depth + 1);
            if (st <= 0) {
                free(filled);
                return st;
            }
        }
    }
    for (i = 0; i < count; i++) {
        JogNode *decl;
        JogNode *def = NULL;
        int st;
        if (filled[i]) {
            continue;
        }
        decl = fields[i].decl;
        if (decl && decl->kind == JOG_NODE_FIELD) {
            def = decl->as.field.default_value;
        } else if (decl && decl->kind == JOG_NODE_PARAM) {
            def = decl->as.param.default_value;
        }
        if (!def) {
            continue;
        }
        st = place_at(img, base + (size_t)fields[i].offset, fields[i].type, def, depth + 1);
        if (st <= 0) {
            free(filled);
            return st;
        }
    }
    free(filled);
    return 1;
}

static int place_enum(Img *img, size_t offset, JogType *type, int index, JogNode *call, int depth) {
    const JogVariant *variant;
    JogType *tag;
    size_t payload;
    int st;
    if (index < 0 || !type->as.nominal.variants || (size_t)index >= type->as.nominal.variant_count) {
        return 0;
    }
    tag = type->as.nominal.base;
    if (!tag || !tag->has_layout || tag->size == 0 || tag->size > 8) {
        return 0;
    }
    variant = &type->as.nominal.variants[index];
    st = place_scalar_bits(img, offset, tag, (uint64_t)variant->tag, (size_t)tag->size);
    if (st <= 0) {
        return st;
    }
    payload = (size_t)type->as.nominal.payload_offset;
    if (variant->field_count == 0) {
        return 1;
    }
    if (payload > img->size - offset) {
        jog_lower_fail(img->L, call ? call->pos : (JogPos){0}, "global field is outside its type");
        return -1;
    }
    return place_fields(img, offset + payload, variant->fields, variant->field_count, call, depth);
}

static int place_at(Img *img, size_t offset, JogType *type, JogNode *node, int depth) {
    JogType *base;
    int st;
    if (!img || img->L->failed) {
        return -1;
    }
    if (depth > 64) {
        jog_lower_fail(img->L, node ? node->pos : (JogPos){0}, "global initializer is too deep");
        return -1;
    }
    if (!type || !jog_type_layout(img->L->program->types, type) || !type->has_layout) {
        return 0;
    }
    if (type->size == 0) {
        return 1;
    }
    if (offset > img->size || type->size > img->size - offset) {
        jog_lower_fail(img->L, node ? node->pos : (JogPos){0}, "global field is outside its type");
        return -1;
    }
    base = peeled(type);
    if (!base) {
        return 0;
    }
    if (!node) {
        return 1;
    }
    st = place_int_tree(img, offset, type, node);
    if (st != 0) {
        return st;
    }
    st = place_float_tree(img, offset, type, node);
    if (st != 0) {
        return st;
    }
    switch (node->kind) {
    case JOG_NODE_STRING_LIT:
        return place_bytes(img, offset, type, node->as.literal.text, node->as.literal.len, 0);
    case JOG_NODE_CSTRING_LIT:
        return place_bytes(img, offset, type, node->as.literal.text, node->as.literal.len, 1);
    case JOG_NODE_NULL_LIT:
        return 1;
    case JOG_NODE_BUILTIN:
        if (node->as.builtin.name && strcmp(node->as.builtin.name, "embed") == 0) {
            return place_embed(img, offset, type, node);
        }
        return 0;
    case JOG_NODE_IDENT:
        if (node->symbol && node->symbol->kind == JOG_SYM_FUNCTION) {
            return place_fn_value(img, offset, type, node, node->symbol);
        }
        if (node->symbol) {
            return copy_global_image(img, offset, type, jog_lower_global_bind(img->L, node->symbol));
        }
        return 0;
    case JOG_NODE_UNARY:
        if (node->as.unary.op == JOG_TOK_AMP && node->as.unary.operand &&
            node->as.unary.operand->kind == JOG_NODE_IDENT && node->as.unary.operand->symbol &&
            base->kind == JOG_TY_PTR) {
            GlobalBind *bind = jog_lower_global_bind(img->L, node->as.unary.operand->symbol);
            uint32_t psz = jog_types_ptr_size(img->L->program->types);
            if (!bind || bind->index < 0 || psz == 0 || type->size != psz) {
                return 0;
            }
            return add_reloc(img, (uint32_t)offset, JIR_RELOC_GLOBAL, (uint32_t)bind->index, 0, NULL, 0);
        }
        return 0;
    case JOG_NODE_TUPLE:
        if (base->kind != JOG_TY_TUPLE || !base->as.tuple.offsets ||
            node->as.tuple.elems.count != base->as.tuple.count) {
            return 0;
        }
        for (size_t i = 0; i < base->as.tuple.count; i++) {
            JogType *elem = base->as.tuple.elems ? base->as.tuple.elems[i] : NULL;
            st = place_at(img, offset + (size_t)base->as.tuple.offsets[i], elem, list_at(node->as.tuple.elems, i),
                          depth + 1);
            if (st <= 0) {
                return st;
            }
        }
        return 1;
    case JOG_NODE_ARRAY_LIT:
        if (base->kind != JOG_TY_ARRAY || !base->as.array.elem || base->as.array.elem->size == 0) {
            return 0;
        }
        if (node->as.array_lit.elems.count != base->as.array.len) {
            return 0;
        }
        for (size_t i = 0; i < node->as.array_lit.elems.count; i++) {
            JogNode *arg = list_at(node->as.array_lit.elems, i);
            JogNode *value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : NULL;
            size_t stride = (size_t)base->as.array.elem->size;
            if (!value) {
                return 0;
            }
            st = place_at(img, offset + i * stride, base->as.array.elem, value, depth + 1);
            if (st <= 0) {
                return st;
            }
        }
        return 1;
    case JOG_NODE_CALL: {
        JogNode *callee = node->as.call.callee;
        JogSymbol *symbol = node->symbol ? node->symbol : callee ? callee->symbol : NULL;
        if (symbol && symbol->kind == JOG_SYM_VARIANT && base->kind == JOG_TY_ENUM) {
            return place_enum(img, offset, base, variant_index(base, symbol), node, depth);
        }
        if (symbol && symbol->kind == JOG_SYM_TYPE && base->kind == JOG_TY_STRUCT) {
            return place_fields(img, offset, base->as.nominal.fields, base->as.nominal.field_count, node, depth);
        }
        return 0;
    }
    case JOG_NODE_MEMBER:
    case JOG_NODE_DOT_VARIANT:
        if (node->symbol && node->symbol->kind == JOG_SYM_VARIANT && base->kind == JOG_TY_ENUM) {
            int index = variant_index(base, node->symbol);
            if (index < 0) {
                return 0;
            }
            if (base->as.nominal.variants[index].field_count > 0) {
                return 0;
            }
            return place_enum(img, offset, base, index, NULL, depth);
        }
        return 0;
    default:
        return 0;
    }
}

int jog_lower_global_place(Lower *L, JirGlobal *global, JogNode *init) {
    Img img;
    JogType *type;
    int st;
    uint8_t *perm = NULL;
    JirReloc *relocs = NULL;
    if (!L || !global || !init || L->failed) {
        return L && L->failed ? -1 : 0;
    }
    type = global->type;
    if (!type || !jog_type_layout(L->program->types, type) || !type->has_layout || type->size > SIZE_MAX) {
        return 0;
    }
    if (type->size == 0) {
        global->data = NULL;
        global->data_len = 0;
        global->relocs = NULL;
        global->reloc_count = 0;
        return 1;
    }
    if (type->size > 32u * 1024u * 1024u) {
        jog_lower_fail(L, init->pos, "global initializer is too large");
        return -1;
    }
    memset(&img, 0, sizeof(img));
    img.L = L;
    img.size = (size_t)type->size;
    img.bytes = calloc(img.size, 1);
    if (!img.bytes) {
        jog_lower_fail(L, init->pos, "out of memory");
        return -1;
    }
    st = place_at(&img, 0, type, init, 0);
    if (st == 1) {
        perm = ct_arena_alloc(L->program->arena, img.size, 1);
        if (img.nrel > 0) {
            relocs = ct_arena_alloc(L->program->arena, img.nrel * sizeof(JirReloc), _Alignof(JirReloc));
        }
        if (!perm || (img.nrel > 0 && !relocs)) {
            jog_lower_fail(L, init->pos, "out of memory");
            st = -1;
        } else {
            memcpy(perm, img.bytes, img.size);
            if (img.nrel > 0) {
                memcpy(relocs, img.rel, img.nrel * sizeof(JirReloc));
            }
            global->data = perm;
            global->data_len = img.size;
            global->relocs = relocs;
            global->reloc_count = img.nrel;
        }
    }
    free(img.bytes);
    free(img.rel);
    return st;
}
