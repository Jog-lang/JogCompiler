#include "types.h"

#include "map.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

struct JogTypes {
    CtArena *arena;
    CtMap *interned; /* canonical shape key -> JogType * */
    uint32_t ptr_size;
    uint32_t next_id;
    JogType *builtins[JOG_BUILTIN_COUNT];
    JogType *core_str; /* distinct str over const bytes; NULL until the prelude binds it */
    CtSb key;
    struct JogScalarOp *scalar_ops;
};

/* An operator method from core/scalar.jog. The name is not a scope binding. */
struct JogScalarOp {
    JogType *on;
    const char *name;
    int value_params;
    JogType *ret;
    struct JogScalarOp *next;
};

static uint64_t round_up(uint64_t value, uint64_t align) {
    return (value + align - 1) / align * align;
}

static JogType *new_type(JogTypes *types, JogTypeKind kind) {
    JogType *type = ct_arena_alloc(types->arena, sizeof(*type), _Alignof(JogType));
    if (!type) {
        return NULL;
    }
    memset(type, 0, sizeof(*type));
    type->kind = kind;
    type->id = types->next_id++;
    return type;
}

static void set_layout(JogType *type, uint64_t size, uint32_t align) {
    type->size = size;
    type->align = align;
    type->has_layout = true;
}

static JogType *make_int(JogTypes *types, uint8_t bits, bool is_signed, bool is_size) {
    JogType *type = new_type(types, JOG_TY_INT);
    if (type) {
        type->as.integer.bits = bits;
        type->as.integer.is_signed = is_signed;
        type->as.integer.is_size = is_size;
        set_layout(type, bits / 8u, bits / 8u);
    }
    return type;
}

JogTypes *jog_types_create(CtArena *arena, uint32_t ptr_size) {
    if (!arena || (ptr_size != 4 && ptr_size != 8)) {
        return NULL;
    }
    JogTypes *types = ct_arena_alloc(arena, sizeof(*types), _Alignof(JogTypes));
    if (!types) {
        return NULL;
    }
    memset(types, 0, sizeof(*types));
    types->arena = arena;
    types->ptr_size = ptr_size;
    types->interned = ct_map_create(arena);
    ct_sb_init(&types->key);
    if (!types->interned) {
        return NULL;
    }

    JogType **b = types->builtins;
    uint8_t ptr_bits = (uint8_t)(ptr_size * 8);
    b[JOG_BUILTIN_I8] = make_int(types, 8, true, false);
    b[JOG_BUILTIN_I16] = make_int(types, 16, true, false);
    b[JOG_BUILTIN_I32] = make_int(types, 32, true, false);
    b[JOG_BUILTIN_I64] = make_int(types, 64, true, false);
    b[JOG_BUILTIN_U8] = make_int(types, 8, false, false);
    b[JOG_BUILTIN_U16] = make_int(types, 16, false, false);
    b[JOG_BUILTIN_U32] = make_int(types, 32, false, false);
    b[JOG_BUILTIN_U64] = make_int(types, 64, false, false);
    b[JOG_BUILTIN_ISIZE] = make_int(types, ptr_bits, true, true);
    b[JOG_BUILTIN_USIZE] = make_int(types, ptr_bits, false, true);

    b[JOG_BUILTIN_F32] = new_type(types, JOG_TY_FLOAT);
    b[JOG_BUILTIN_F64] = new_type(types, JOG_TY_FLOAT);
    b[JOG_BUILTIN_BOOL] = new_type(types, JOG_TY_BOOL);
    b[JOG_BUILTIN_UNIT] = new_type(types, JOG_TY_UNIT);
    b[JOG_BUILTIN_BYTES] = new_type(types, JOG_TY_BYTES);
    b[JOG_BUILTIN_CONST_BYTES] = new_type(types, JOG_TY_BYTES);
    b[JOG_BUILTIN_UNTYPED_INT] = new_type(types, JOG_TY_UNTYPED_INT);
    b[JOG_BUILTIN_UNTYPED_FLOAT] = new_type(types, JOG_TY_UNTYPED_FLOAT);
    b[JOG_BUILTIN_NULL] = new_type(types, JOG_TY_NULL);
    b[JOG_BUILTIN_ERROR] = new_type(types, JOG_TY_ERROR);
    for (int i = 0; i < JOG_BUILTIN_COUNT; i++) {
        if (!b[i]) {
            return NULL;
        }
    }
    b[JOG_BUILTIN_F32]->as.flt.bits = 32;
    set_layout(b[JOG_BUILTIN_F32], 4, 4);
    b[JOG_BUILTIN_F64]->as.flt.bits = 64;
    set_layout(b[JOG_BUILTIN_F64], 8, 8);
    set_layout(b[JOG_BUILTIN_BOOL], 1, 1);
    set_layout(b[JOG_BUILTIN_UNIT], 0, 1);
    b[JOG_BUILTIN_CONST_BYTES]->is_const = true;
    set_layout(b[JOG_BUILTIN_BYTES], 2u * ptr_size, ptr_size);
    set_layout(b[JOG_BUILTIN_CONST_BYTES], 2u * ptr_size, ptr_size);
    set_layout(b[JOG_BUILTIN_UNTYPED_INT], 0, 1);
    set_layout(b[JOG_BUILTIN_UNTYPED_FLOAT], 0, 1);
    set_layout(b[JOG_BUILTIN_NULL], 0, 1);
    set_layout(b[JOG_BUILTIN_ERROR], 0, 1);
    return types;
}

void jog_types_release(JogTypes *types) {
    if (types) {
        ct_sb_free(&types->key);
    }
}

uint32_t jog_types_ptr_size(const JogTypes *types) {
    return types ? types->ptr_size : 0;
}

JogType *jog_type_builtin(JogTypes *types, JogBuiltin which) {
    if (!types || (unsigned)which >= JOG_BUILTIN_COUNT) {
        return NULL;
    }
    return types->builtins[which];
}

/* ---- interning ---- */

static JogType *lookup(JogTypes *types) {
    void *found = NULL;
    if (ct_map_get(types->interned, ct_sb_cstr(&types->key), &found)) {
        return found;
    }
    return NULL;
}

static JogType *remember(JogTypes *types, JogType *type) {
    if (type && !ct_map_put(types->interned, ct_sb_cstr(&types->key), type)) {
        return NULL;
    }
    return type;
}

static void key_begin(JogTypes *types, char tag) {
    ct_sb_clear(&types->key);
    ct_sb_putc(&types->key, tag);
}

static void key_id(JogTypes *types, const JogType *type) {
    ct_sb_appendf(&types->key, ",%u", (unsigned)type->id);
}

static JogType **copy_types(JogTypes *types, JogType *const *items, size_t count) {
    if (count == 0) {
        return NULL;
    }
    JogType **copy = ct_arena_alloc(types->arena, count * sizeof(*copy), _Alignof(JogType *));
    if (copy) {
        memcpy(copy, items, count * sizeof(*copy));
    }
    return copy;
}

JogType *jog_type_bytes_n(JogTypes *types, uint64_t n) {
    if (!types) {
        return NULL;
    }
    key_begin(types, 'b');
    ct_sb_appendf(&types->key, ",%" PRIu64, n);
    JogType *type = lookup(types);
    if (type) {
        return type;
    }
    type = new_type(types, JOG_TY_BYTES_N);
    if (!type) {
        return NULL;
    }
    type->as.bytes_n.n = n;
    set_layout(type, n, 1);
    return remember(types, type);
}

static JogType *elem_type(JogTypes *types, JogTypeKind kind, char tag, JogType *elem, bool is_const) {
    if (!types || !elem) {
        return NULL;
    }
    key_begin(types, tag);
    key_id(types, elem);
    ct_sb_putc(&types->key, is_const ? 'c' : 'm');
    JogType *type = lookup(types);
    if (type) {
        return type;
    }
    type = new_type(types, kind);
    if (!type) {
        return NULL;
    }
    type->as.elem.elem = elem;
    type->is_const = is_const;
    jog_type_layout(types, type);
    return remember(types, type);
}

JogType *jog_type_ptr(JogTypes *types, JogType *elem, bool is_const) {
    return elem_type(types, JOG_TY_PTR, 'p', elem, is_const);
}

/* True when type is the universe name byte. Comparing kind and length avoids
   allocating [1]byte just to ask the question. */
static bool type_is_byte(const JogType *type) {
    return type && type->kind == JOG_TY_BYTES_N && type->as.bytes_n.n == 1;
}

JogType *jog_type_slice(JogTypes *types, JogType *elem, bool is_const) {
    if (!types || !elem) {
        return NULL;
    }
    /* []byte is the bytes view. []const byte is const bytes. */
    if (type_is_byte(elem)) {
        return jog_type_builtin(types, is_const ? JOG_BUILTIN_CONST_BYTES : JOG_BUILTIN_BYTES);
    }
    return elem_type(types, JOG_TY_SLICE, 's', elem, is_const);
}

JogType *jog_type_option(JogTypes *types, JogType *elem) {
    return elem_type(types, JOG_TY_OPTION, 'o', elem, false);
}

JogType *jog_type_array(JogTypes *types, JogType *elem, uint64_t len) {
    if (!types || !elem) {
        return NULL;
    }
    /* [N]byte is the inline byte string, not an array of one-byte arrays. */
    if (type_is_byte(elem)) {
        return jog_type_bytes_n(types, len);
    }
    key_begin(types, 'a');
    key_id(types, elem);
    ct_sb_appendf(&types->key, ",%" PRIu64, len);
    JogType *type = lookup(types);
    if (type) {
        return type;
    }
    type = new_type(types, JOG_TY_ARRAY);
    if (!type) {
        return NULL;
    }
    type->as.array.elem = elem;
    type->as.array.len = len;
    jog_type_layout(types, type);
    return remember(types, type);
}

JogType *jog_type_tuple(JogTypes *types, JogType *const *elems, size_t count) {
    if (!types || (count > 0 && !elems)) {
        return NULL;
    }
    key_begin(types, 't');
    for (size_t i = 0; i < count; i++) {
        key_id(types, elems[i]);
    }
    JogType *type = lookup(types);
    if (type) {
        return type;
    }
    type = new_type(types, JOG_TY_TUPLE);
    if (!type) {
        return NULL;
    }
    type->as.tuple.elems = copy_types(types, elems, count);
    type->as.tuple.count = count;
    if (count > 0) {
        type->as.tuple.offsets = ct_arena_alloc(types->arena, count * sizeof(uint64_t), _Alignof(uint64_t));
        if (!type->as.tuple.elems || !type->as.tuple.offsets) {
            return NULL;
        }
    }
    jog_type_layout(types, type);
    return remember(types, type);
}

JogType *jog_type_fn(JogTypes *types, JogType *const *params, size_t param_count, JogType *ret,
                     JogType *const *contexts, size_t context_count, bool variadic, bool escaping, bool is_extern) {
    if (!types || !ret || (param_count > 0 && !params) || (context_count > 0 && !contexts)) {
        return NULL;
    }
    key_begin(types, 'f');
    for (size_t i = 0; i < param_count; i++) {
        key_id(types, params[i]);
    }
    ct_sb_append_cstr(&types->key, ";r");
    key_id(types, ret);
    ct_sb_append_cstr(&types->key, ";c");
    for (size_t i = 0; i < context_count; i++) {
        key_id(types, contexts[i]);
    }
    ct_sb_appendf(&types->key, ";%d%d%d", variadic, escaping, is_extern);
    JogType *type = lookup(types);
    if (type) {
        return type;
    }
    type = new_type(types, JOG_TY_FN);
    if (!type) {
        return NULL;
    }
    type->as.fn.params = copy_types(types, params, param_count);
    type->as.fn.param_count = param_count;
    type->as.fn.ret = ret;
    type->as.fn.contexts = copy_types(types, contexts, context_count);
    type->as.fn.context_count = context_count;
    type->as.fn.variadic = variadic;
    type->as.fn.escaping = escaping;
    type->as.fn.is_extern = is_extern;
    if ((param_count > 0 && !type->as.fn.params) || (context_count > 0 && !type->as.fn.contexts)) {
        return NULL;
    }
    jog_type_layout(types, type);
    return remember(types, type);
}

JogType *jog_type_new_nominal(JogTypes *types, JogTypeKind kind, const char *name, const char *package,
                              JogNode *decl) {
    if (!types || (kind != JOG_TY_STRUCT && kind != JOG_TY_ENUM && kind != JOG_TY_INTERFACE &&
                   kind != JOG_TY_DISTINCT)) {
        return NULL;
    }
    JogType *type = new_type(types, kind);
    if (type) {
        type->as.nominal.name = name;
        type->as.nominal.package = package;
        type->as.nominal.decl = decl;
    }
    return type;
}

JogType *jog_type_new_param(JogTypes *types, const char *name, JogNode *decl, JogType *constraint) {
    if (!types) {
        return NULL;
    }
    JogType *type = new_type(types, JOG_TY_TYPE_PARAM);
    if (type) {
        type->as.param.name = name;
        type->as.param.decl = decl;
        type->as.param.constraint = constraint;
    }
    return type;
}

/* ---- layout ---- */

/* Lays out fields one after another with C rules. Returns false if a field
   type has no layout. */
static bool layout_fields(JogTypes *types, JogField *fields, size_t count, uint64_t *size, uint32_t *align) {
    uint64_t offset = 0;
    uint32_t max_align = 1;
    for (size_t i = 0; i < count; i++) {
        JogType *t = fields[i].type;
        if (!t || !jog_type_layout(types, t)) {
            return false;
        }
        offset = round_up(offset, t->align);
        fields[i].offset = offset;
        offset += t->size;
        if (t->align > max_align) {
            max_align = t->align;
        }
    }
    *size = round_up(offset, max_align);
    *align = max_align;
    return true;
}

static JogType *smallest_tag(JogTypes *types, const JogType *type) {
    int64_t max_tag = 0;
    for (size_t i = 0; i < type->as.nominal.variant_count; i++) {
        if (type->as.nominal.variants[i].tag > max_tag) {
            max_tag = type->as.nominal.variants[i].tag;
        }
    }
    if (max_tag <= UINT8_MAX) {
        return types->builtins[JOG_BUILTIN_U8];
    }
    if (max_tag <= UINT16_MAX) {
        return types->builtins[JOG_BUILTIN_U16];
    }
    if (max_tag <= (int64_t)UINT32_MAX) {
        return types->builtins[JOG_BUILTIN_U32];
    }
    return types->builtins[JOG_BUILTIN_U64];
}

bool jog_type_layout(JogTypes *types, JogType *type) {
    if (!types || !type) {
        return false;
    }
    if (type->has_layout) {
        return true;
    }
    uint32_t ptr = types->ptr_size;
    switch (type->kind) {
    case JOG_TY_PTR:
        set_layout(type, ptr, ptr);
        return true;
    case JOG_TY_SLICE:
    case JOG_TY_INTERFACE:
        set_layout(type, 2u * ptr, ptr);
        return true;
    case JOG_TY_FN:
        if (type->as.fn.is_extern) {
            set_layout(type, ptr, ptr);
        } else {
            set_layout(type, 2u * ptr, ptr); /* code pointer + environment */
        }
        return true;
    case JOG_TY_ARRAY: {
        JogType *elem = type->as.array.elem;
        if (!jog_type_layout(types, elem)) {
            return false;
        }
        set_layout(type, elem->size * type->as.array.len, elem->align);
        return true;
    }
    case JOG_TY_OPTION: {
        JogType *elem = type->as.elem.elem;
        if (!jog_type_layout(types, elem)) {
            return false;
        }
        if (jog_type_option_is_niche(type)) {
            set_layout(type, elem->size, elem->align);
        } else {
            uint32_t align = elem->align > 1 ? elem->align : 1;
            uint64_t value_offset = round_up(1, align);
            set_layout(type, round_up(value_offset + elem->size, align), align);
        }
        return true;
    }
    case JOG_TY_TUPLE: {
        uint64_t offset = 0;
        uint32_t max_align = 1;
        for (size_t i = 0; i < type->as.tuple.count; i++) {
            JogType *t = type->as.tuple.elems[i];
            if (!jog_type_layout(types, t)) {
                return false;
            }
            offset = round_up(offset, t->align);
            type->as.tuple.offsets[i] = offset;
            offset += t->size;
            if (t->align > max_align) {
                max_align = t->align;
            }
        }
        set_layout(type, round_up(offset, max_align), max_align);
        return true;
    }
    case JOG_TY_STRUCT: {
        uint64_t size;
        uint32_t align;
        if (!layout_fields(types, type->as.nominal.fields, type->as.nominal.field_count, &size, &align)) {
            return false;
        }
        set_layout(type, size, align);
        return true;
    }
    case JOG_TY_ENUM: {
        if (!type->as.nominal.base) {
            type->as.nominal.base = smallest_tag(types, type);
        }
        JogType *tag = type->as.nominal.base;
        if (!jog_type_layout(types, tag)) {
            return false;
        }
        uint64_t payload_size = 0;
        uint32_t payload_align = 1;
        for (size_t i = 0; i < type->as.nominal.variant_count; i++) {
            JogVariant *v = &type->as.nominal.variants[i];
            uint64_t size;
            uint32_t align;
            if (!layout_fields(types, v->fields, v->field_count, &size, &align)) {
                return false;
            }
            if (size > payload_size) {
                payload_size = size;
            }
            if (align > payload_align) {
                payload_align = align;
            }
        }
        uint32_t align = tag->align > payload_align ? tag->align : payload_align;
        uint64_t payload_offset = payload_size ? round_up(tag->size, payload_align) : tag->size;
        type->as.nominal.payload_offset = payload_offset;
        set_layout(type, round_up(payload_offset + payload_size, align), align);
        return true;
    }
    case JOG_TY_DISTINCT: {
        JogType *base = type->as.nominal.base;
        if (!base || !jog_type_layout(types, base)) {
            return false;
        }
        set_layout(type, base->size, base->align);
        return true;
    }
    default:
        return false;
    }
}

JogType *jog_type_underlying(JogType *type) {
    while (type && type->kind == JOG_TY_DISTINCT) {
        type = type->as.nominal.base;
    }
    return type;
}

JogType *jog_type_core_str(const JogTypes *types) {
    return types ? types->core_str : NULL;
}

void jog_types_set_core_str(JogTypes *types, JogType *type) {
    if (types) {
        types->core_str = type;
    }
}

int jog_type_is_core_str(const JogTypes *types, const JogType *type) {
    return types && type && types->core_str && type == types->core_str;
}

JogType *jog_type_bytes_view(JogType *type) {
    type = jog_type_underlying(type);
    return type && type->kind == JOG_TY_BYTES ? type : NULL;
}

static int type_is_u8(const JogType *type) {
    return type && type->kind == JOG_TY_INT && !type->as.integer.is_signed && type->as.integer.bits == 8 &&
           !type->as.integer.is_size;
}

int jog_type_converts_to_str(const JogTypes *types, const JogType *got, const JogType *want) {
    const JogType *elem;
    if (!jog_type_is_core_str(types, want) || !got || got == want) {
        return 0;
    }
    if (got->kind == JOG_TY_BYTES) {
        return 1;
    }
    if (got->kind != JOG_TY_SLICE) {
        return 0;
    }
    elem = got->as.elem.elem;
    if (elem && elem->kind == JOG_TY_DISTINCT) {
        elem = jog_type_underlying((JogType *)elem);
    }
    return type_is_u8(elem);
}

bool jog_type_option_is_niche(const JogType *type) {
    if (!type || type->kind != JOG_TY_OPTION) {
        return false;
    }
    const JogType *elem = jog_type_underlying(type->as.elem.elem);
    return elem && (elem->kind == JOG_TY_PTR || (elem->kind == JOG_TY_FN && elem->as.fn.is_extern));
}

/* ---- printing ---- */

static void print_list(JogType *const *items, size_t count, CtSb *out) {
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            ct_sb_append_cstr(out, ", ");
        }
        jog_type_print(items[i], out);
    }
}

void jog_type_print(const JogType *type, CtSb *out) {
    if (!out) {
        return;
    }
    if (!type) {
        ct_sb_append_cstr(out, "<null type>");
        return;
    }
    switch (type->kind) {
    case JOG_TY_ERROR:
        ct_sb_append_cstr(out, "<error>");
        return;
    case JOG_TY_UNIT:
        ct_sb_append_cstr(out, "()");
        return;
    case JOG_TY_BOOL:
        ct_sb_append_cstr(out, "bool");
        return;
    case JOG_TY_INT:
        if (type->as.integer.is_size) {
            ct_sb_append_cstr(out, type->as.integer.is_signed ? "isize" : "usize");
        } else {
            ct_sb_appendf(out, "%c%u", type->as.integer.is_signed ? 'i' : 'u', (unsigned)type->as.integer.bits);
        }
        return;
    case JOG_TY_FLOAT:
        ct_sb_appendf(out, "f%u", (unsigned)type->as.flt.bits);
        return;
    case JOG_TY_UNTYPED_INT:
        ct_sb_append_cstr(out, "untyped int");
        return;
    case JOG_TY_UNTYPED_FLOAT:
        ct_sb_append_cstr(out, "untyped float");
        return;
    case JOG_TY_NULL:
        ct_sb_append_cstr(out, "null");
        return;
    case JOG_TY_BYTES_N:
        ct_sb_appendf(out, "[%" PRIu64 "]byte", type->as.bytes_n.n);
        return;
    case JOG_TY_BYTES:
        ct_sb_append_cstr(out, type->is_const ? "const bytes" : "bytes");
        return;
    case JOG_TY_PTR:
        ct_sb_append_cstr(out, type->is_const ? "*const " : "*");
        jog_type_print(type->as.elem.elem, out);
        return;
    case JOG_TY_SLICE:
        ct_sb_append_cstr(out, type->is_const ? "[]const " : "[]");
        jog_type_print(type->as.elem.elem, out);
        return;
    case JOG_TY_ARRAY:
        ct_sb_appendf(out, "[%" PRIu64 "]", type->as.array.len);
        jog_type_print(type->as.array.elem, out);
        return;
    case JOG_TY_OPTION: {
        const JogType *elem = type->as.elem.elem;
        bool paren = elem && elem->kind == JOG_TY_FN;
        if (paren) {
            ct_sb_putc(out, '(');
        }
        jog_type_print(elem, out);
        ct_sb_append_cstr(out, paren ? ")?" : "?");
        return;
    }
    case JOG_TY_TUPLE:
        ct_sb_putc(out, '(');
        print_list(type->as.tuple.elems, type->as.tuple.count, out);
        ct_sb_putc(out, ')');
        return;
    case JOG_TY_FN:
        if (type->as.fn.is_extern) {
            ct_sb_append_cstr(out, "extern ");
        } else if (type->as.fn.escaping) {
            ct_sb_append_cstr(out, "escaping ");
        }
        ct_sb_putc(out, '(');
        print_list(type->as.fn.params, type->as.fn.param_count, out);
        if (type->as.fn.variadic) {
            ct_sb_append_cstr(out, type->as.fn.param_count ? ", ..." : "...");
        }
        ct_sb_append_cstr(out, ") -> ");
        jog_type_print(type->as.fn.ret, out);
        if (type->as.fn.context_count > 0) {
            ct_sb_append_cstr(out, " with ");
            print_list(type->as.fn.contexts, type->as.fn.context_count, out);
        }
        return;
    case JOG_TY_STRUCT:
    case JOG_TY_ENUM:
    case JOG_TY_INTERFACE:
    case JOG_TY_DISTINCT:
        ct_sb_append_cstr(out, type->as.nominal.name ? type->as.nominal.name : "<anonymous>");
        if (type->as.nominal.type_arg_count > 0) {
            ct_sb_putc(out, '<');
            print_list(type->as.nominal.type_args, type->as.nominal.type_arg_count, out);
            ct_sb_putc(out, '>');
        }
        return;
    case JOG_TY_TYPE_PARAM:
        ct_sb_append_cstr(out, type->as.param.name ? type->as.param.name : "T");
        return;
    }
}

static void fill_machine(const JogType *type, JogMachine *out) {
    memset(out, 0, sizeof(*out));
    if (!type) {
        return;
    }
    if (type->kind == JOG_TY_INT) {
        out->kind = JOG_INTRINSIC_INT;
        out->bits = type->as.integer.bits;
        out->is_signed = type->as.integer.is_signed ? 1 : 0;
        out->is_ptr = type->as.integer.is_size ? 1 : 0;
        return;
    }
    if (type->kind == JOG_TY_FLOAT) {
        out->kind = JOG_INTRINSIC_FLOAT;
        out->bits = type->as.flt.bits;
        return;
    }
    if (type->kind == JOG_TY_BOOL) {
        out->kind = JOG_INTRINSIC_BOOL;
        out->bits = 1;
        return;
    }
    if (type->intrinsic == JOG_INTRINSIC_INT || type->intrinsic == JOG_INTRINSIC_FLOAT ||
        type->intrinsic == JOG_INTRINSIC_BOOL) {
        out->kind = type->intrinsic;
        out->bits = type->intrinsic_bits;
        out->is_signed = type->intrinsic_signed ? 1 : 0;
        out->is_ptr = type->intrinsic_ptr ? 1 : 0;
    }
}

int jog_type_machine(const JogType *type, JogMachine *out) {
    const JogType *cursor = type;
    if (!out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    while (cursor && cursor->kind == JOG_TY_DISTINCT) {
        if (cursor->intrinsic) {
            fill_machine(cursor, out);
            return out->kind != JOG_INTRINSIC_NONE;
        }
        cursor = cursor->as.nominal.base;
    }
    fill_machine(cursor, out);
    return out->kind != JOG_INTRINSIC_NONE;
}

void jog_type_set_intrinsic(JogType *type, int kind, unsigned bits, int is_signed, int is_ptr) {
    uint32_t align;
    if (!type) {
        return;
    }
    type->intrinsic = (uint8_t)kind;
    type->intrinsic_bits = (uint8_t)bits;
    type->intrinsic_signed = is_signed ? 1 : 0;
    type->intrinsic_ptr = is_ptr ? 1 : 0;
    if ((kind == JOG_INTRINSIC_INT || kind == JOG_INTRINSIC_FLOAT) && bits >= 8) {
        align = bits / 8u;
        if (align > type->align) {
            type->align = align;
        }
    }
}

int jog_types_add_scalar_op(JogTypes *types, JogType *on, const char *name, int value_params, JogType *ret) {
    struct JogScalarOp *op;
    if (!types || !on || !name || !ret) {
        return 0;
    }
    {
        JogType *existing = jog_types_scalar_op(types, on, name, value_params);
        /* jog_check binds the prelude again on the same type table. The same
           operator is not a second declaration. */
        if (existing) {
            return existing == ret;
        }
    }
    op = ct_arena_alloc(types->arena, sizeof(*op), _Alignof(struct JogScalarOp));
    if (!op) {
        return 0;
    }
    memset(op, 0, sizeof(*op));
    op->on = on;
    op->name = name;
    op->value_params = value_params;
    op->ret = ret;
    op->next = types->scalar_ops;
    types->scalar_ops = op;
    return 1;
}

JogType *jog_types_scalar_op(const JogTypes *types, const JogType *on, const char *name, int value_params) {
    struct JogScalarOp *op;
    if (!types || !on || !name) {
        return NULL;
    }
    for (op = types->scalar_ops; op; op = op->next) {
        if (op->on == on && op->value_params == value_params && op->name && strcmp(op->name, name) == 0) {
            return op->ret;
        }
    }
    return NULL;
}
