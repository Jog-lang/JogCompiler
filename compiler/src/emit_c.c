#include "phases.h"

#include "iface_table.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Types, then prototypes, constant data, function bodies, and
   jog_entry_init / jog_entry_main. String operands and JirGlobal bytes are
   that constant data.

   i8..i64 / u8..u64     intN_t / uintN_t
   isize / usize         intptr_t / uintptr_t
   f32 / f64             float / double
   bool                  _Bool
   ()                    void (function results); no value object
   untyped int / float   int64_t / double (the defaults from types.h)
   null, <error>, T      incomplete typedef struct jt<id>
   bytes[N], N > 0       struct { uint8_t b[N]; }
   bytes[0], [0]T, (),
     empty struct        incomplete typedef struct (C17 has no zero-size type)
   bytes, str, []T       struct { T *ptr; size_t len; } with const T * when const
   *T / *const T         typedef of T * / const T *
   *void / *const void   typedef of void * / const void *. Universe void is
                         not an incomplete struct and is not given a layout.
   [N]T                  struct { T v[N]; }
   tuple                 struct { f_0, f_1, ... }
   T? niche              typedef of the element (nullable pointer or extern fn)
   T? otherwise          struct { _Bool has; T value; }
   struct / enum /
     interface           jog_<package>__<Name>, '.' in the package as '_'.
                         An empty package is jog___Name.
   enum                  tag of the tag type, then union payload
   interface             struct { void *ptr; void *vtable; }
   distinct              typedef of its base
   extern fn             function pointer; variadic ends in ...
   other fn              struct { Ret (*code)(params, void *env); void *env; }

   That closure struct is the type of a function value. A Jog function
   definition uses the JIR function's parameters and does not take void *env.
   extern linkage is a prototype only. A variadic prototype with no named
   parameter is (int jog_va, ...); a call passes 0 for jog_va, because (...)
   is not legal C.

   JIR_OPND_STRING and JIR_OPND_CSTRING are static const uint8_t arrays,
   jog_sN and jog_cN. N is the index in one table, not a per-kind counter.
   A cstring array includes the trailing NUL. The same bytes are emitted
   once. A string value is a {pointer, length} compound; a cstring value
   is the array converted to its pointer type.

   Each JirGlobal with a C object is static. It is const when is_const and
   no instruction stores it. Init stores a non-constant top-level val once,
   so that object is writable and the assignment is well-formed. Folded
   integer and bool data with no store stays const. The name is emitted as
   written and must be a C identifier. No data, or a zero length, is a zero
   initializer. Otherwise data is little-endian target
   layout (x64 and wasm32) of exactly the type's size. A pointer slot in that
   image is zero bytes plus a JirReloc (string, cstring, global, or function).
   A non-zero pointer with no reloc is an error, and so is non-zero padding.
   jog_entry_init takes the address of every emitted global so an unused
   static does not fail -Wall. Incomplete types are not objects.
   An interface method table is not stored in a JirGlobal image. Lowering
   records it on JirModule.iface_tables and names a dummy u8 global; this
   file emits that name as a static const struct of a type id, size, align,
   function pointers, and size_t property offsets after those pointers.
   The type id is the address of jog_tid_<id>.

   Locals are l<index> and blocks are labels b<index>. Terminators are goto,
   if, switch, and return. Checked integer add, sub, mul, and neg use
   __builtin_*_overflow. Checked division and remainder reject zero and
   signed overflow. BOUNDS_CHECK and NULL_CHECK call jogrt_panic_*.
   release-fast omits bounds checks and keeps the other checks. Instruction
   and terminator positions become #line directives and the file and line
   passed to jogrt_panic_*.

   Structural names are jt<id>. Nominal generic instances append __ and the
   type-argument ids. Struct and tuple fields are f_<name> or f_<index>.
   Definitions follow value dependencies: a pointer or slice does not require
   the pointee body, so a struct may contain a pointer to itself. Context
   parameters are not added here; lowering puts them on the function type
   (see ir.h). */

typedef struct Emit {
    const JirModule *module;
    CtSb *out;
    CtDiag *diag;
    CtArena *arena;
    CtSb scratch;
    CtDa types;    /* JogType *, reach order */
    CtDa id_index; /* int32_t, -1 when that id was not reached */
    const char **names;
    uint8_t *color;
    int32_t *order;
    size_t order_n;
    CtDa strings; /* EmitStr, string and cstring operands */
    JogBuildMode mode;
    bool lined;
    bool failed;
} Emit;

typedef struct EmitStr {
    const char *ptr;
    size_t len;
    bool cstring;
} EmitStr;

enum { COLOR_WHITE = 0, COLOR_GRAY = 1, COLOR_BLACK = 2 };

static void emit_fail(Emit *e, const char *message) {
    CtPos pos = {0};
    ct_diag_emit(e->diag, CT_SEVERITY_ERROR, pos, "%s", message);
    e->failed = true;
}

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static bool c_ident(const char *name) {
    if (!name || !is_ident_start(name[0])) {
        return false;
    }
    for (const char *p = name + 1; *p != '\0'; p++) {
        if (!is_ident_char(*p)) {
            return false;
        }
    }
    return true;
}

/* Universe void is the nominal struct bound with no package, no fields,
   and no layout. It is C's void, not a pointer target that is an incomplete
   struct. A package struct named void is a different type. */
static bool type_is_universe_void(const JogType *type) {
    const char *name;
    if (!type || type->kind != JOG_TY_STRUCT || type->has_layout) {
        return false;
    }
    if (type->as.nominal.field_count != 0 || type->as.nominal.type_arg_count != 0) {
        return false;
    }
    if (type->as.nominal.package != NULL) {
        return false;
    }
    name = type->as.nominal.name;
    return name != NULL && strcmp(name, "void") == 0;
}

static bool type_is_incomplete(const JogType *type) {
    if (!type) {
        return true;
    }
    /* Spelled as C void, so it is not forward-declared as a struct. */
    if (type_is_universe_void(type)) {
        return false;
    }
    switch (type->kind) {
    case JOG_TY_NULL:
    case JOG_TY_ERROR:
    case JOG_TY_TYPE_PARAM:
        return true;
    case JOG_TY_BYTES_N:
        return type->as.bytes_n.n == 0;
    case JOG_TY_ARRAY:
        return type->as.array.len == 0 || !type->as.array.elem || type->as.array.elem->size == 0;
    case JOG_TY_TUPLE:
    case JOG_TY_STRUCT:
        return type->size == 0;
    default:
        return false;
    }
}

static bool type_is_struct_form(const JogType *type) {
    if (!type || type_is_incomplete(type)) {
        return false;
    }
    switch (type->kind) {
    case JOG_TY_BYTES_N:
        return type->as.bytes_n.n > 0;
    case JOG_TY_BYTES:
    case JOG_TY_SLICE:
    case JOG_TY_ENUM:
    case JOG_TY_INTERFACE:
        return true;
    case JOG_TY_ARRAY:
    case JOG_TY_TUPLE:
    case JOG_TY_STRUCT:
        return type->size > 0;
    case JOG_TY_OPTION:
        return !jog_type_option_is_niche(type);
    case JOG_TY_FN:
        return !type->as.fn.is_extern;
    default:
        return false;
    }
}

/* A definition in the body section: a struct body, or a typedef that is not
   one of the builtin spellings. Incomplete types are only forward-declared. */
static bool needs_def(const JogType *type) {
    if (!type || type_is_incomplete(type)) {
        return false;
    }
    switch (type->kind) {
    case JOG_TY_BYTES_N:
    case JOG_TY_BYTES:
    case JOG_TY_SLICE:
    case JOG_TY_ARRAY:
    case JOG_TY_TUPLE:
    case JOG_TY_STRUCT:
    case JOG_TY_ENUM:
    case JOG_TY_INTERFACE:
        return type_is_struct_form(type);
    case JOG_TY_PTR:
    case JOG_TY_DISTINCT:
    case JOG_TY_OPTION:
    case JOG_TY_FN:
        return true;
    default:
        return false;
    }
}

static bool has_c_name(const JogType *type) {
    return needs_def(type) || type_is_incomplete(type);
}

static bool is_nominal_kind(JogTypeKind kind) {
    return kind == JOG_TY_STRUCT || kind == JOG_TY_ENUM || kind == JOG_TY_INTERFACE || kind == JOG_TY_DISTINCT;
}

static JogType *type_at(const Emit *e, int32_t index) {
    if (index < 0 || (size_t)index >= e->types.count) {
        return NULL;
    }
    return *(JogType **)ct_da_at((CtDa *)&e->types, (size_t)index);
}

static int32_t index_of(const Emit *e, const JogType *type) {
    if (!type || type->id >= e->id_index.count) {
        return -1;
    }
    return *(const int32_t *)ct_da_at((CtDa *)&e->id_index, type->id);
}

static bool ensure_layout(Emit *e, JogType *type) {
    /* Laying out universe void would give the empty struct size 0. It has
       no size, and *void still has pointer layout. */
    if (type_is_universe_void(type)) {
        return true;
    }
    if (type->has_layout) {
        return true;
    }
    switch (type->kind) {
    case JOG_TY_PTR:
    case JOG_TY_SLICE:
    case JOG_TY_INTERFACE:
    case JOG_TY_FN:
    case JOG_TY_ARRAY:
    case JOG_TY_OPTION:
    case JOG_TY_TUPLE:
    case JOG_TY_STRUCT:
    case JOG_TY_ENUM:
    case JOG_TY_DISTINCT:
    case JOG_TY_BYTES:
    case JOG_TY_BYTES_N:
        if (e->module->types && jog_type_layout(e->module->types, type)) {
            return true;
        }
        break;
    default:
        return true;
    }
    CtSb spelling;
    ct_sb_init(&spelling);
    jog_type_print(type, &spelling);
    CtPos pos = {0};
    ct_diag_emit(e->diag, CT_SEVERITY_ERROR, pos, "C backend: %s has no layout", ct_sb_cstr(&spelling));
    ct_sb_free(&spelling);
    e->failed = true;
    return false;
}

static bool reach_list(Emit *e, JogType *const *items, size_t count);
static bool reach_fields(Emit *e, const JogField *fields, size_t count);
static bool reach(Emit *e, JogType *type);

static bool reach_list(Emit *e, JogType *const *items, size_t count) {
    if (count > 0 && !items) {
        emit_fail(e, "C backend: missing type list");
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (!reach(e, items[i])) {
            return false;
        }
    }
    return true;
}

static bool reach_fields(Emit *e, const JogField *fields, size_t count) {
    if (count > 0 && !fields) {
        emit_fail(e, "C backend: missing fields");
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (!reach(e, fields[i].type)) {
            return false;
        }
    }
    return true;
}

static bool reach(Emit *e, JogType *type) {
    if (e->failed) {
        return false;
    }
    if (!type) {
        return true;
    }
    while (e->id_index.count <= type->id) {
        int32_t missing = -1;
        if (!ct_da_push(&e->id_index, &missing)) {
            emit_fail(e, "C backend: out of memory");
            return false;
        }
    }
    int32_t *slot = ct_da_at(&e->id_index, type->id);
    if (*slot >= 0) {
        return true;
    }
    if (!ensure_layout(e, type)) {
        return false;
    }
    *slot = (int32_t)e->types.count;
    if (!ct_da_push(&e->types, &type)) {
        emit_fail(e, "C backend: out of memory");
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
    case JOG_TY_NULL:
    case JOG_TY_BYTES_N:
    case JOG_TY_BYTES:
        return true;
    case JOG_TY_PTR:
    case JOG_TY_SLICE:
    case JOG_TY_OPTION:
        return reach(e, type->as.elem.elem);
    case JOG_TY_ARRAY:
        return reach(e, type->as.array.elem);
    case JOG_TY_TUPLE:
        return reach_list(e, type->as.tuple.elems, type->as.tuple.count);
    case JOG_TY_FN:
        if (!reach_list(e, type->as.fn.params, type->as.fn.param_count) || !reach(e, type->as.fn.ret) ||
            !reach_list(e, type->as.fn.contexts, type->as.fn.context_count)) {
            return false;
        }
        return true;
    case JOG_TY_STRUCT:
        if (!reach_fields(e, type->as.nominal.fields, type->as.nominal.field_count) ||
            !reach_list(e, type->as.nominal.type_args, type->as.nominal.type_arg_count)) {
            return false;
        }
        return true;
    case JOG_TY_ENUM:
        if (type->as.nominal.variant_count > 0 && !type->as.nominal.variants) {
            emit_fail(e, "C backend: missing variants");
            return false;
        }
        if (!reach(e, type->as.nominal.base)) {
            return false;
        }
        for (size_t i = 0; i < type->as.nominal.variant_count; i++) {
            JogVariant *variant = &type->as.nominal.variants[i];
            if (!reach_fields(e, variant->fields, variant->field_count)) {
                return false;
            }
        }
        return reach_list(e, type->as.nominal.type_args, type->as.nominal.type_arg_count);
    case JOG_TY_INTERFACE:
        return reach_list(e, type->as.nominal.type_args, type->as.nominal.type_arg_count);
    case JOG_TY_DISTINCT:
        if (!reach(e, type->as.nominal.base) ||
            !reach_list(e, type->as.nominal.type_args, type->as.nominal.type_arg_count)) {
            return false;
        }
        return true;
    case JOG_TY_TYPE_PARAM:
        return reach(e, type->as.param.constraint);
    }
    return true;
}

static bool reach_module(Emit *e) {
    const JirModule *module = e->module;
    for (size_t i = 0; i < module->globals.count; i++) {
        JirGlobal *global = *(JirGlobal **)ct_da_at((CtDa *)&module->globals, i);
        if (!reach(e, global->type)) {
            return false;
        }
    }
    for (size_t i = 0; i < module->funcs.count; i++) {
        JirFunc *func = *(JirFunc **)ct_da_at((CtDa *)&module->funcs, i);
        if (!reach(e, func->type)) {
            return false;
        }
        for (size_t local_i = 0; local_i < func->locals.count; local_i++) {
            JirLocal *local = ct_da_at((CtDa *)&func->locals, local_i);
            if (!reach(e, local->type)) {
                return false;
            }
        }
        for (size_t block_i = 0; block_i < func->blocks.count; block_i++) {
            JirBlock *block = ct_da_at((CtDa *)&func->blocks, block_i);
            for (size_t inst_i = 0; inst_i < block->insts.count; inst_i++) {
                JirInst *inst = ct_da_at(&block->insts, inst_i);
                if (!reach(e, inst->a.type) || !reach(e, inst->b.type)) {
                    return false;
                }
                if (inst->args) {
                    for (size_t arg_i = 0; arg_i < inst->arg_count; arg_i++) {
                        if (!reach(e, inst->args[arg_i].type)) {
                            return false;
                        }
                    }
                }
            }
            if (!reach(e, block->term.value.type)) {
                return false;
            }
        }
    }
    return !e->failed;
}

static void append_sanitized(CtSb *sb, const char *src) {
    bool first = true;
    for (const char *p = src; *p != '\0'; p++) {
        char c = *p == '.' ? '_' : *p;
        if (!(first ? is_ident_start(c) : is_ident_char(c))) {
            c = '_';
        }
        ct_sb_putc(sb, c);
        first = false;
    }
}

static const char *dup_scratch(Emit *e) {
    if (e->scratch.failed) {
        emit_fail(e, "C backend: out of memory");
        return NULL;
    }
    const char *copy = ct_arena_strdup(e->arena, ct_sb_cstr(&e->scratch));
    if (!copy) {
        emit_fail(e, "C backend: out of memory");
    }
    return copy;
}

static const char *make_nominal(Emit *e, const JogType *type) {
    CtSb *sb = &e->scratch;
    ct_sb_clear(sb);
    ct_sb_append_cstr(sb, "jog_");
    if (type->as.nominal.package && type->as.nominal.package[0] != '\0') {
        append_sanitized(sb, type->as.nominal.package);
    }
    ct_sb_append_cstr(sb, "__");
    size_t before = sb->len;
    if (type->as.nominal.name) {
        append_sanitized(sb, type->as.nominal.name);
    }
    if (sb->len == before) {
        ct_sb_appendf(sb, "t%u", (unsigned)type->id);
    }
    if (type->as.nominal.type_arg_count > 0 && type->as.nominal.type_args) {
        ct_sb_append_cstr(sb, "__");
        for (size_t i = 0; i < type->as.nominal.type_arg_count; i++) {
            if (i > 0) {
                ct_sb_putc(sb, '_');
            }
            const JogType *arg = type->as.nominal.type_args[i];
            ct_sb_appendf(sb, "%u", arg ? (unsigned)arg->id : 0);
        }
    }
    return dup_scratch(e);
}

static const char *make_name(Emit *e, const JogType *type) {
    if (is_nominal_kind(type->kind)) {
        return make_nominal(e, type);
    }
    ct_sb_clear(&e->scratch);
    ct_sb_appendf(&e->scratch, "jt%u", (unsigned)type->id);
    return dup_scratch(e);
}

static const char *with_id_suffix(Emit *e, const char *name, uint32_t id) {
    ct_sb_clear(&e->scratch);
    ct_sb_appendf(&e->scratch, "%s__%u", name, (unsigned)id);
    return dup_scratch(e);
}

static bool assign_names(Emit *e) {
    if (e->types.count == 0) {
        return true;
    }
    e->names = ct_arena_alloc(e->arena, e->types.count * sizeof(*e->names), _Alignof(const char *));
    if (!e->names) {
        emit_fail(e, "C backend: out of memory");
        return false;
    }
    memset(e->names, 0, e->types.count * sizeof(*e->names));
    for (size_t i = 0; i < e->types.count; i++) {
        JogType *type = type_at(e, (int32_t)i);
        if (!has_c_name(type)) {
            continue;
        }
        e->names[i] = make_name(e, type);
        if (!e->names[i]) {
            return false;
        }
    }
    for (size_t round = 0; round < e->types.count; round++) {
        bool changed = false;
        for (size_t i = 0; i < e->types.count; i++) {
            JogType *left = type_at(e, (int32_t)i);
            if (!left || !is_nominal_kind(left->kind) || !e->names[i]) {
                continue;
            }
            for (size_t j = 0; j < i; j++) {
                JogType *right = type_at(e, (int32_t)j);
                if (!right || !is_nominal_kind(right->kind) || !e->names[j]) {
                    continue;
                }
                if (strcmp(e->names[i], e->names[j]) != 0) {
                    continue;
                }
                size_t which = left->id > right->id ? i : j;
                JogType *loser = type_at(e, (int32_t)which);
                e->names[which] = with_id_suffix(e, e->names[which], loser->id);
                if (!e->names[which]) {
                    return false;
                }
                changed = true;
                break;
            }
        }
        if (!changed) {
            break;
        }
    }
    return !e->failed;
}

static bool child_first(const JogType *child, bool by_value) {
    if (!child || !needs_def(child)) {
        return false;
    }
    if (!by_value && type_is_struct_form(child)) {
        return false;
    }
    return true;
}

static bool visit_type(Emit *e, JogType *type);

static bool visit_if(Emit *e, JogType *child, bool by_value) {
    if (!child_first(child, by_value)) {
        return true;
    }
    return visit_type(e, child);
}

static bool visit_fields(Emit *e, const JogField *fields, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!visit_if(e, fields[i].type, true)) {
            return false;
        }
    }
    return true;
}

static bool visit_list(Emit *e, JogType *const *items, size_t count, bool by_value) {
    for (size_t i = 0; i < count; i++) {
        if (!visit_if(e, items ? items[i] : NULL, by_value)) {
            return false;
        }
    }
    return true;
}

static bool visit_type(Emit *e, JogType *type) {
    int32_t index = index_of(e, type);
    if (index < 0 || !needs_def(type)) {
        return true;
    }
    if (e->color[index] == COLOR_BLACK) {
        return true;
    }
    if (e->color[index] == COLOR_GRAY) {
        emit_fail(e, "C backend: recursive value type");
        return false;
    }
    e->color[index] = COLOR_GRAY;
    switch (type->kind) {
    case JOG_TY_PTR:
    case JOG_TY_SLICE:
        if (!visit_if(e, type->as.elem.elem, false)) {
            return false;
        }
        break;
    case JOG_TY_ARRAY:
        if (!visit_if(e, type->as.array.elem, true)) {
            return false;
        }
        break;
    case JOG_TY_OPTION:
        if (!visit_if(e, type->as.elem.elem, true)) {
            return false;
        }
        break;
    case JOG_TY_TUPLE:
        if (!visit_list(e, type->as.tuple.elems, type->as.tuple.count, true)) {
            return false;
        }
        break;
    case JOG_TY_FN:
        if (!visit_list(e, type->as.fn.params, type->as.fn.param_count, true) || !visit_if(e, type->as.fn.ret, true)) {
            return false;
        }
        break;
    case JOG_TY_STRUCT:
        if (!visit_fields(e, type->as.nominal.fields, type->as.nominal.field_count)) {
            return false;
        }
        break;
    case JOG_TY_ENUM:
        if (!visit_if(e, type->as.nominal.base, true)) {
            return false;
        }
        for (size_t i = 0; i < type->as.nominal.variant_count; i++) {
            JogVariant *variant = &type->as.nominal.variants[i];
            if (!visit_fields(e, variant->fields, variant->field_count)) {
                return false;
            }
        }
        break;
    case JOG_TY_DISTINCT:
        if (!visit_if(e, type->as.nominal.base, true)) {
            return false;
        }
        break;
    default:
        break;
    }
    if (e->failed) {
        return false;
    }
    e->color[index] = COLOR_BLACK;
    e->order[e->order_n++] = index;
    return true;
}

static bool order_defs(Emit *e) {
    if (e->types.count == 0) {
        return true;
    }
    e->color = ct_arena_alloc(e->arena, e->types.count, 1);
    e->order = ct_arena_alloc(e->arena, e->types.count * sizeof(int32_t), _Alignof(int32_t));
    if (!e->color || !e->order) {
        emit_fail(e, "C backend: out of memory");
        return false;
    }
    memset(e->color, 0, e->types.count);
    for (size_t id = 0; id < e->id_index.count; id++) {
        int32_t index = *(int32_t *)ct_da_at(&e->id_index, id);
        if (index < 0) {
            continue;
        }
        if (!visit_type(e, type_at(e, index))) {
            return false;
        }
    }
    return !e->failed;
}

static void append_type_name(Emit *e, const JogType *type);

static void append_int_name(Emit *e, const JogType *type) {
    if (type->as.integer.is_size) {
        ct_sb_append_cstr(e->out, type->as.integer.is_signed ? "intptr_t" : "uintptr_t");
        return;
    }
    unsigned bits = type->as.integer.bits;
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) {
        emit_fail(e, "C backend: integer width has no stdint name");
        ct_sb_append_cstr(e->out, "int64_t");
        return;
    }
    ct_sb_appendf(e->out, "%s%u_t", type->as.integer.is_signed ? "int" : "uint", bits);
}

static void append_type_name(Emit *e, const JogType *type) {
    if (!type) {
        ct_sb_append_cstr(e->out, "void");
        return;
    }
    switch (type->kind) {
    case JOG_TY_INT:
        append_int_name(e, type);
        return;
    case JOG_TY_FLOAT:
        if (type->as.flt.bits == 32) {
            ct_sb_append_cstr(e->out, "float");
        } else if (type->as.flt.bits == 64) {
            ct_sb_append_cstr(e->out, "double");
        } else {
            emit_fail(e, "C backend: float width has no C name");
            ct_sb_append_cstr(e->out, "double");
        }
        return;
    case JOG_TY_BOOL:
        ct_sb_append_cstr(e->out, "_Bool");
        return;
    case JOG_TY_UNIT:
        ct_sb_append_cstr(e->out, "void");
        return;
    case JOG_TY_UNTYPED_INT:
        ct_sb_append_cstr(e->out, "int64_t");
        return;
    case JOG_TY_UNTYPED_FLOAT:
        ct_sb_append_cstr(e->out, "double");
        return;
    default:
        break;
    }
    if (type_is_universe_void(type)) {
        ct_sb_append_cstr(e->out, "void");
        return;
    }
    int32_t index = index_of(e, type);
    const char *name = index >= 0 && e->names ? e->names[index] : NULL;
    if (!name) {
        emit_fail(e, "C backend: type has no C name");
        ct_sb_append_cstr(e->out, "void");
        return;
    }
    ct_sb_append_cstr(e->out, name);
}

static void append_type_comment(Emit *e, const JogType *type) {
    ct_sb_append_cstr(e->out, " /* ");
    jog_type_print(type, e->out);
    ct_sb_append_cstr(e->out, " */");
}

static void sanitize_into(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    bool first = true;
    for (const char *p = src; *p != '\0' && n + 1 < cap; p++) {
        char c = *p == '.' ? '_' : *p;
        if (!(first ? is_ident_start(c) : is_ident_char(c))) {
            c = '_';
        }
        dst[n++] = c;
        first = false;
    }
    dst[n] = '\0';
}

static void append_field_ident(CtSb *out, const JogField *fields, size_t index) {
    char buf[128];
    sanitize_into(buf, sizeof buf, fields && fields[index].name ? fields[index].name : NULL);
    if (buf[0] == '\0') {
        ct_sb_appendf(out, "f_%u", (unsigned)index);
        return;
    }
    bool dup = false;
    for (size_t i = 0; fields && i < index; i++) {
        char earlier[128];
        sanitize_into(earlier, sizeof earlier, fields[i].name);
        if (strcmp(buf, earlier) == 0) {
            dup = true;
            break;
        }
    }
    if (dup) {
        ct_sb_appendf(out, "f_%s_%u", buf, (unsigned)index);
    } else {
        ct_sb_appendf(out, "f_%s", buf);
    }
}

/* Unit, universe void, and incomplete types have no C object. Untyped
   constants have size 0 in the type table but still spell as their default
   C type. */
static bool skip_value(const JogType *type) {
    if (!type || type->kind == JOG_TY_UNIT || type_is_universe_void(type) || type_is_incomplete(type)) {
        return true;
    }
    if (type->kind == JOG_TY_UNTYPED_INT || type->kind == JOG_TY_UNTYPED_FLOAT) {
        return false;
    }
    return type->size == 0;
}

static bool global_is_object(const JirGlobal *global) {
    return global && global->type && !skip_value(global->type);
}

/* True for a C aggregate. Distinct and niche options use the spelled base. */
static bool spelled_aggregate(const JogType *type) {
    while (type && type->kind == JOG_TY_DISTINCT) {
        type = type->as.nominal.base;
    }
    if (!type) {
        return false;
    }
    if (type->kind == JOG_TY_OPTION && jog_type_option_is_niche(type)) {
        return false;
    }
    if (type->kind == JOG_TY_FN && type->as.fn.is_extern) {
        return false;
    }
    return type_is_struct_form(type);
}

static void append_value_fields(Emit *e, const JogField *fields, size_t count, const char *indent) {
    for (size_t i = 0; i < count; i++) {
        const JogType *field = fields ? fields[i].type : NULL;
        ct_sb_append_cstr(e->out, indent);
        if (skip_value(field)) {
            ct_sb_append_cstr(e->out, "/* ");
            append_field_ident(e->out, fields, i);
            ct_sb_append_cstr(e->out, ": ");
            jog_type_print(field, e->out);
            ct_sb_append_cstr(e->out, " */\n");
            continue;
        }
        append_type_name(e, field);
        ct_sb_putc(e->out, ' ');
        append_field_ident(e->out, fields, i);
        ct_sb_append_cstr(e->out, ";\n");
    }
}

static void append_tuple_fields(Emit *e, const JogType *type) {
    for (size_t i = 0; i < type->as.tuple.count; i++) {
        JogType *elem = type->as.tuple.elems ? type->as.tuple.elems[i] : NULL;
        if (skip_value(elem)) {
            ct_sb_appendf(e->out, "    /* f_%u: ", (unsigned)i);
            jog_type_print(elem, e->out);
            ct_sb_append_cstr(e->out, " */\n");
            continue;
        }
        ct_sb_append_cstr(e->out, "    ");
        append_type_name(e, elem);
        ct_sb_appendf(e->out, " f_%u;\n", (unsigned)i);
    }
}

static bool variant_has_payload(const JogVariant *variant) {
    for (size_t i = 0; i < variant->field_count; i++) {
        if (variant->fields && !skip_value(variant->fields[i].type)) {
            return true;
        }
    }
    return false;
}

static void append_variant_ident(CtSb *out, const JogVariant *variants, size_t index) {
    char buf[128];
    sanitize_into(buf, sizeof buf, variants[index].name);
    if (buf[0] == '\0') {
        ct_sb_appendf(out, "f_v%u", (unsigned)index);
        return;
    }
    bool dup = false;
    for (size_t i = 0; i < index; i++) {
        char earlier[128];
        sanitize_into(earlier, sizeof earlier, variants[i].name);
        if (strcmp(buf, earlier) == 0) {
            dup = true;
            break;
        }
    }
    if (dup) {
        ct_sb_appendf(out, "f_%s_%u", buf, (unsigned)index);
    } else {
        ct_sb_appendf(out, "f_%s", buf);
    }
}

static void append_variant_comment(Emit *e, const JogVariant *variants, size_t index, const char *indent) {
    ct_sb_append_cstr(e->out, indent);
    ct_sb_append_cstr(e->out, "/* ");
    append_variant_ident(e->out, variants, index);
    ct_sb_appendf(e->out, " tag %" PRId64 " */\n", variants[index].tag);
}

static bool append_kept_param(Emit *e, JogType *param, bool any) {
    if (skip_value(param)) {
        return any;
    }
    if (any) {
        ct_sb_append_cstr(e->out, ", ");
    }
    append_type_name(e, param);
    return true;
}

static void append_fn_params(Emit *e, const JogType *type) {
    bool any = false;
    for (size_t i = 0; i < type->as.fn.param_count; i++) {
        any = append_kept_param(e, type->as.fn.params ? type->as.fn.params[i] : NULL, any);
    }
    if (!type->as.fn.is_extern) {
        if (any) {
            ct_sb_append_cstr(e->out, ", ");
        }
        ct_sb_append_cstr(e->out, "void *env");
        any = true;
    }
    /* (...) is not a legal C parameter list. env already names one parameter
       for a closure; an extern variadic with no parameters gets a dummy. */
    if (type->as.fn.variadic && !any) {
        ct_sb_append_cstr(e->out, "int jog_va");
        any = true;
    }
    if (type->as.fn.variadic) {
        if (any) {
            ct_sb_append_cstr(e->out, ", ");
        }
        ct_sb_append_cstr(e->out, "...");
        any = true;
    }
    if (!any) {
        ct_sb_append_cstr(e->out, "void");
    }
}

static void begin_struct(Emit *e, const JogType *type) {
    int32_t index = index_of(e, type);
    ct_sb_appendf(e->out, "struct %s {", index >= 0 && e->names ? e->names[index] : "jt");
    append_type_comment(e, type);
    ct_sb_putc(e->out, '\n');
}

static void emit_enum(Emit *e, const JogType *type) {
    begin_struct(e, type);
    ct_sb_append_cstr(e->out, "    ");
    append_type_name(e, type->as.nominal.base);
    ct_sb_append_cstr(e->out, " tag;\n");
    bool any = false;
    for (size_t i = 0; i < type->as.nominal.variant_count; i++) {
        if (type->as.nominal.variants && variant_has_payload(&type->as.nominal.variants[i])) {
            any = true;
            break;
        }
    }
    if (!any) {
        for (size_t i = 0; i < type->as.nominal.variant_count; i++) {
            append_variant_comment(e, type->as.nominal.variants, i, "    ");
        }
        ct_sb_append_cstr(e->out, "};\n");
        return;
    }
    ct_sb_append_cstr(e->out, "    union {\n");
    for (size_t i = 0; i < type->as.nominal.variant_count; i++) {
        const JogVariant *variant = &type->as.nominal.variants[i];
        if (!variant_has_payload(variant)) {
            append_variant_comment(e, type->as.nominal.variants, i, "        ");
            continue;
        }
        ct_sb_append_cstr(e->out, "        struct {\n");
        append_value_fields(e, variant->fields, variant->field_count, "            ");
        ct_sb_append_cstr(e->out, "        } ");
        append_variant_ident(e->out, type->as.nominal.variants, i);
        ct_sb_appendf(e->out, "; /* tag %" PRId64 " */\n", variant->tag);
    }
    ct_sb_append_cstr(e->out, "    } payload;\n};\n");
}

static void emit_struct_body(Emit *e, const JogType *type) {
    switch (type->kind) {
    case JOG_TY_BYTES_N:
        begin_struct(e, type);
        ct_sb_appendf(e->out, "    uint8_t b[%" PRIu64 "];\n};\n", type->as.bytes_n.n);
        return;
    case JOG_TY_BYTES:
        begin_struct(e, type);
        ct_sb_append_cstr(e->out, type->is_const ? "    const uint8_t *ptr;\n" : "    uint8_t *ptr;\n");
        ct_sb_append_cstr(e->out, "    size_t len;\n};\n");
        return;
    case JOG_TY_SLICE:
        begin_struct(e, type);
        ct_sb_append_cstr(e->out, "    ");
        if (type->is_const) {
            ct_sb_append_cstr(e->out, "const ");
        }
        append_type_name(e, type->as.elem.elem);
        ct_sb_append_cstr(e->out, " *ptr;\n    size_t len;\n};\n");
        return;
    case JOG_TY_ARRAY:
        begin_struct(e, type);
        ct_sb_append_cstr(e->out, "    ");
        append_type_name(e, type->as.array.elem);
        ct_sb_appendf(e->out, " v[%" PRIu64 "];\n};\n", type->as.array.len);
        return;
    case JOG_TY_OPTION:
        begin_struct(e, type);
        ct_sb_append_cstr(e->out, "    _Bool has;\n");
        if (skip_value(type->as.elem.elem)) {
            ct_sb_append_cstr(e->out, "    /* value: ");
            jog_type_print(type->as.elem.elem, e->out);
            ct_sb_append_cstr(e->out, " */\n};\n");
        } else {
            ct_sb_append_cstr(e->out, "    ");
            append_type_name(e, type->as.elem.elem);
            ct_sb_append_cstr(e->out, " value;\n};\n");
        }
        return;
    case JOG_TY_TUPLE:
        begin_struct(e, type);
        append_tuple_fields(e, type);
        ct_sb_append_cstr(e->out, "};\n");
        return;
    case JOG_TY_FN:
        begin_struct(e, type);
        ct_sb_append_cstr(e->out, "    ");
        append_type_name(e, type->as.fn.ret);
        ct_sb_append_cstr(e->out, " (*code)(");
        append_fn_params(e, type);
        ct_sb_append_cstr(e->out, ");\n    void *env;\n};\n");
        return;
    case JOG_TY_STRUCT:
        begin_struct(e, type);
        append_value_fields(e, type->as.nominal.fields, type->as.nominal.field_count, "    ");
        ct_sb_append_cstr(e->out, "};\n");
        return;
    case JOG_TY_ENUM:
        emit_enum(e, type);
        return;
    case JOG_TY_INTERFACE:
        begin_struct(e, type);
        ct_sb_append_cstr(e->out, "    void *ptr;\n    void *vtable;\n};\n");
        return;
    default:
        emit_fail(e, "C backend: not a struct type");
        return;
    }
}

static void emit_alias(Emit *e, const JogType *type) {
    int32_t index = index_of(e, type);
    const char *name = index >= 0 && e->names ? e->names[index] : NULL;
    if (!name) {
        emit_fail(e, "C backend: type has no C name");
        return;
    }
    switch (type->kind) {
    case JOG_TY_PTR:
        ct_sb_append_cstr(e->out, "typedef ");
        if (type->is_const) {
            ct_sb_append_cstr(e->out, "const ");
        }
        append_type_name(e, type->as.elem.elem);
        ct_sb_appendf(e->out, " *%s;", name);
        break;
    case JOG_TY_DISTINCT:
        ct_sb_append_cstr(e->out, "typedef ");
        append_type_name(e, type->as.nominal.base);
        ct_sb_appendf(e->out, " %s;", name);
        break;
    case JOG_TY_OPTION:
        ct_sb_append_cstr(e->out, "typedef ");
        append_type_name(e, type->as.elem.elem);
        ct_sb_appendf(e->out, " %s;", name);
        break;
    case JOG_TY_FN:
        ct_sb_append_cstr(e->out, "typedef ");
        append_type_name(e, type->as.fn.ret);
        ct_sb_appendf(e->out, " (*%s)(", name);
        append_fn_params(e, type);
        ct_sb_append_cstr(e->out, ");");
        break;
    default:
        emit_fail(e, "C backend: not a typedef type");
        return;
    }
    append_type_comment(e, type);
    ct_sb_putc(e->out, '\n');
}

static void emit_def(Emit *e, const JogType *type) {
    if (type_is_struct_form(type)) {
        emit_struct_body(e, type);
    } else {
        emit_alias(e, type);
    }
}

static bool emit_forwards(Emit *e) {
    bool any = false;
    for (size_t id = 0; id < e->id_index.count; id++) {
        int32_t index = *(int32_t *)ct_da_at(&e->id_index, id);
        if (index < 0) {
            continue;
        }
        JogType *type = type_at(e, index);
        if (!type_is_struct_form(type) && !type_is_incomplete(type)) {
            continue;
        }
        const char *name = e->names[index];
        if (!name) {
            emit_fail(e, "C backend: type has no C name");
            return any;
        }
        ct_sb_appendf(e->out, "typedef struct %s %s;", name, name);
        if (type_is_incomplete(type)) {
            append_type_comment(e, type);
        }
        ct_sb_putc(e->out, '\n');
        any = true;
    }
    return any;
}

static JogType *underlying(const JogType *type) {
    return jog_type_underlying((JogType *)type);
}

static bool type_kind_is(const JogType *type, JogTypeKind kind) {
    JogType *base = underlying(type);
    return base && base->kind == kind;
}

static bool type_is_bool(const JogType *type) {
    return type_kind_is(type, JOG_TY_BOOL);
}

static bool type_is_float(const JogType *type) {
    JogType *base = underlying(type);
    return base && (base->kind == JOG_TY_FLOAT || base->kind == JOG_TY_UNTYPED_FLOAT);
}

static bool type_is_int(const JogType *type) {
    JogType *base = underlying(type);
    return base && (base->kind == JOG_TY_INT || base->kind == JOG_TY_UNTYPED_INT || base->kind == JOG_TY_BOOL);
}

static unsigned int_width(const JogType *type) {
    JogType *base = underlying(type);
    if (!base) {
        return 64;
    }
    if (base->kind == JOG_TY_BOOL) {
        return 1;
    }
    if (base->kind == JOG_TY_UNTYPED_INT) {
        return 64;
    }
    if (base->kind == JOG_TY_INT && base->as.integer.bits > 0 && base->as.integer.bits <= 64) {
        return base->as.integer.bits;
    }
    return 64;
}

static bool int_signed(const JogType *type) {
    JogType *base = underlying(type);
    if (!base || base->kind == JOG_TY_BOOL) {
        return false;
    }
    if (base->kind == JOG_TY_UNTYPED_INT) {
        return true;
    }
    return base->kind == JOG_TY_INT && base->as.integer.is_signed;
}

static bool int_is_size(const JogType *type) {
    JogType *base = underlying(type);
    return base && base->kind == JOG_TY_INT && base->as.integer.is_size;
}

static unsigned float_width(const JogType *type) {
    JogType *base = underlying(type);
    if (base && base->kind == JOG_TY_FLOAT) {
        return base->as.flt.bits;
    }
    return 64;
}

static const char *signed_min_macro(const JogType *type) {
    if (int_is_size(type)) {
        return "INTPTR_MIN";
    }
    switch (int_width(type)) {
    case 8:
        return "INT8_MIN";
    case 16:
        return "INT16_MIN";
    case 32:
        return "INT32_MIN";
    case 64:
        return "INT64_MIN";
    default:
        return NULL;
    }
}

static const char *signed_max_macro(const JogType *type) {
    if (int_is_size(type)) {
        return "INTPTR_MAX";
    }
    switch (int_width(type)) {
    case 8:
        return "INT8_MAX";
    case 16:
        return "INT16_MAX";
    case 32:
        return "INT32_MAX";
    case 64:
        return "INT64_MAX";
    default:
        return NULL;
    }
}

static const char *unsigned_max_macro(const JogType *type) {
    if (int_is_size(type)) {
        return "UINTPTR_MAX";
    }
    switch (int_width(type)) {
    case 8:
        return "UINT8_MAX";
    case 16:
        return "UINT16_MAX";
    case 32:
        return "UINT32_MAX";
    case 64:
        return "UINT64_MAX";
    default:
        return NULL;
    }
}

static void append_c_literal(CtSb *out, const char *text) {
    ct_sb_putc(out, '"');
    if (!text) {
        ct_sb_putc(out, '"');
        return;
    }
    for (const char *p = text; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || c == '"') {
            ct_sb_putc(out, '\\');
            ct_sb_putc(out, (char)c);
        } else if (c == '\n') {
            ct_sb_append_cstr(out, "\\n");
        } else if (c == '\r') {
            ct_sb_append_cstr(out, "\\r");
        } else if (c < 32 || c >= 127) {
            ct_sb_appendf(out, "\\%03o", c);
        } else {
            ct_sb_putc(out, (char)c);
        }
    }
    ct_sb_putc(out, '"');
}

static void append_loc(CtSb *out, JogPos pos) {
    append_c_literal(out, pos.file ? pos.file : "");
    ct_sb_appendf(out, ", %u", pos.line);
}

static void begin_stmt(Emit *e, JogPos pos) {
    if (pos.file && pos.file[0] != '\0' && pos.line != 0) {
        ct_sb_append_cstr(e->out, "#line ");
        ct_sb_appendf(e->out, "%u ", pos.line);
        append_c_literal(e->out, pos.file);
        ct_sb_putc(e->out, '\n');
        e->lined = true;
    }
    ct_sb_append_cstr(e->out, "    ");
}

static int32_t find_bytes(const Emit *e, const char *ptr, size_t len, bool cstring) {
    for (size_t i = 0; i < e->strings.count; i++) {
        const EmitStr *s = ct_da_at((CtDa *)&e->strings, i);
        if (s->cstring != cstring || s->len != len) {
            continue;
        }
        if (len == 0 || (ptr && s->ptr && memcmp(s->ptr, ptr, len) == 0)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void note_bytes(Emit *e, const char *ptr, size_t len, bool cstring) {
    if (find_bytes(e, ptr, len, cstring) >= 0) {
        return;
    }
    if (len > 0 && !ptr) {
        emit_fail(e, "C backend: string operand is missing bytes");
        return;
    }
    EmitStr item;
    item.ptr = ptr;
    item.len = len;
    item.cstring = cstring;
    if (!ct_da_push(&e->strings, &item)) {
        emit_fail(e, "C backend: out of memory");
    }
}

static void note_operand(Emit *e, const JirOperand *op) {
    if (!op) {
        return;
    }
    if (op->kind == JIR_OPND_STRING) {
        note_bytes(e, op->as.str.ptr, op->as.str.len, false);
    } else if (op->kind == JIR_OPND_CSTRING) {
        note_bytes(e, op->as.str.ptr, op->as.str.len, true);
    }
}

static void note_global_relocs(Emit *e) {
    const JirModule *module = e->module;
    size_t i;
    if (!module) {
        return;
    }
    for (i = 0; i < module->globals.count; i++) {
        JirGlobal *global = jir_global(module, (uint32_t)i);
        size_t r;
        if (!global) {
            continue;
        }
        for (r = 0; r < global->reloc_count; r++) {
            const JirReloc *rel = &global->relocs[r];
            if (rel->kind == JIR_RELOC_STRING) {
                note_bytes(e, rel->ptr, rel->len, false);
            } else if (rel->kind == JIR_RELOC_CSTRING) {
                note_bytes(e, rel->ptr, rel->len, true);
            }
        }
    }
}

static void note_strings(Emit *e) {
    const JirModule *module = e->module;
    note_global_relocs(e);
    for (size_t fi = 0; fi < module->funcs.count; fi++) {
        JirFunc *func = jir_func(module, (uint32_t)fi);
        if (!func || func->linkage == JIR_LINK_EXTERN) {
            continue;
        }
        for (size_t bi = 0; bi < func->blocks.count; bi++) {
            JirBlock *block = jir_block_at(func, (uint32_t)bi);
            for (size_t ii = 0; ii < block->insts.count; ii++) {
                JirInst *inst = ct_da_at(&block->insts, ii);
                note_operand(e, &inst->a);
                note_operand(e, &inst->b);
                for (size_t ai = 0; ai < inst->arg_count; ai++) {
                    note_operand(e, &inst->args[ai]);
                }
            }
            note_operand(e, &block->term.value);
        }
    }
}

static void emit_string_data(Emit *e) {
    for (size_t i = 0; i < e->strings.count; i++) {
        EmitStr *s = ct_da_at(&e->strings, i);
        ct_sb_appendf(e->out, "static const uint8_t jog_%s%u[] = {", s->cstring ? "c" : "s", (unsigned)i);
        for (size_t b = 0; b < s->len; b++) {
            if (b > 0) {
                ct_sb_append_cstr(e->out, ", ");
            }
            ct_sb_appendf(e->out, "%u", (unsigned)(unsigned char)s->ptr[b]);
        }
        if (s->cstring) {
            if (s->len > 0) {
                ct_sb_append_cstr(e->out, ", ");
            }
            ct_sb_append_cstr(e->out, "0");
        } else if (s->len == 0) {
            ct_sb_append_cstr(e->out, "0");
        }
        ct_sb_append_cstr(e->out, "};\n");
    }
}

static void append_bits(Emit *e, const JogType *type, uint64_t bits) {
    JogType *base = underlying(type);
    if (base && base->kind == JOG_TY_BOOL) {
        ct_sb_append_cstr(e->out, (bits & 1) ? "1" : "0");
        return;
    }
    unsigned width = 64;
    bool is_signed = true;
    if (base && base->kind == JOG_TY_INT) {
        width = int_width(base);
        is_signed = int_signed(base);
    } else if (!base || base->kind != JOG_TY_UNTYPED_INT) {
        ct_sb_appendf(e->out, "%" PRIu64 "u", bits);
        return;
    }
    if (width == 0 || width > 64) {
        emit_fail(e, "C backend: integer width has no C constant");
        ct_sb_append_cstr(e->out, "0");
        return;
    }
    uint64_t mask = width == 64 ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1);
    bits &= mask;
    if (!is_signed) {
        ct_sb_appendf(e->out, "%" PRIu64 "%s", bits, bits > 4294967295ull ? "ull" : "u");
        return;
    }
    uint64_t sign = (uint64_t)1 << (width - 1);
    if ((bits & sign) != 0 && width == 64 && bits == sign) {
        ct_sb_append_cstr(e->out, "(-9223372036854775807LL-1)");
        return;
    }
    int64_t value;
    if ((bits & sign) != 0) {
        if (width == 64) {
            value = (int64_t)bits;
        } else {
            value = (int64_t)bits - (int64_t)((uint64_t)1 << width);
        }
    } else {
        value = (int64_t)bits;
    }
    if (value > 2147483647LL || value < -2147483647LL) {
        ct_sb_appendf(e->out, "%" PRId64 "LL", value);
    } else {
        ct_sb_appendf(e->out, "%" PRId64, value);
    }
}

static void append_float_literal(Emit *e, const JogType *type, double value) {
    bool single = float_width(type) == 32 && type_kind_is(type, JOG_TY_FLOAT);
    if (value != value) {
        ct_sb_append_cstr(e->out, single ? "(0.0f/0.0f)" : "(0.0/0.0)");
        return;
    }
    if (value != 0.0 && value + value == value) {
        ct_sb_append_cstr(e->out, value < 0.0 ? "(-1.0/0.0)" : "(1.0/0.0)");
        return;
    }
    char buf[128];
    int wrote = snprintf(buf, sizeof buf, "%a", value);
    if (wrote <= 0 || (size_t)wrote >= sizeof buf) {
        ct_sb_append_cstr(e->out, "0.0");
        return;
    }
    if (single) {
        ct_sb_appendf(e->out, "(float)%s", buf);
    } else {
        ct_sb_append_cstr(e->out, buf);
    }
}

static void append_expr(Emit *e, const JirOperand *op);

static void append_bytes_expr(Emit *e, const JirOperand *op, bool cstring) {
    int32_t id = find_bytes(e, op->as.str.ptr, op->as.str.len, cstring);
    if (id < 0) {
        emit_fail(e, "C backend: string operand was not emitted");
        ct_sb_append_cstr(e->out, "0");
        return;
    }
    const char *prefix = cstring ? "c" : "s";
    if (cstring || type_kind_is(op->type, JOG_TY_PTR)) {
        ct_sb_putc(e->out, '(');
        append_type_name(e, op->type);
        ct_sb_appendf(e->out, ")jog_%s%u", prefix, (unsigned)id);
        return;
    }
    ct_sb_putc(e->out, '(');
    append_type_name(e, op->type);
    ct_sb_appendf(e->out, "){jog_%s%u, %" PRIu64 "}", prefix, (unsigned)id, (uint64_t)op->as.str.len);
}

static void append_expr(Emit *e, const JirOperand *op) {
    if (!op || op->kind == JIR_OPND_NONE) {
        ct_sb_append_cstr(e->out, "0");
        return;
    }
    switch (op->kind) {
    case JIR_OPND_LOCAL:
        ct_sb_appendf(e->out, "l%u", op->as.local);
        return;
    case JIR_OPND_INT:
        append_bits(e, op->type, op->as.bits);
        return;
    case JIR_OPND_FLOAT:
        append_float_literal(e, op->type, op->as.f);
        return;
    case JIR_OPND_STRING:
        append_bytes_expr(e, op, false);
        return;
    case JIR_OPND_CSTRING:
        append_bytes_expr(e, op, true);
        return;
    case JIR_OPND_NULL:
        ct_sb_append_cstr(e->out, "0");
        return;
    case JIR_OPND_GLOBAL: {
        JirGlobal *global = jir_global(e->module, op->as.global);
        if (!global_is_object(global) || !c_ident(global->name)) {
            emit_fail(e, "C backend: global operand was not emitted");
            ct_sb_append_cstr(e->out, "0");
            return;
        }
        {
            JirIfaceTable *table = NULL;
            size_t ti;
            for (ti = 0; e->module && ti < e->module->iface_table_count; ti++) {
                if (e->module->iface_tables[ti].global == (int32_t)op->as.global) {
                    table = &e->module->iface_tables[ti];
                    break;
                }
            }
            if (table) {
                ct_sb_putc(e->out, '(');
                append_type_name(e, op->type);
                ct_sb_appendf(e->out, ")&%s", global->name);
                return;
            }
        }
        ct_sb_appendf(e->out, "&%s", global->name);
        return;
    }
    case JIR_OPND_FUNC: {
        JirFunc *func = jir_func(e->module, op->as.func);
        if (!func || !func->name) {
            emit_fail(e, "C backend: missing function operand");
            ct_sb_append_cstr(e->out, "0");
            return;
        }
        if (!func->type || func->type->kind != JOG_TY_FN || func->type->as.fn.is_extern ||
            func->linkage == JIR_LINK_EXTERN) {
            ct_sb_append_cstr(e->out, func->name);
            return;
        }
        if (func->type->as.fn.variadic) {
            emit_fail(e, "C backend: a Jog function used as a closure value needs a thunk");
            ct_sb_append_cstr(e->out, "0");
            return;
        }
        /* A Jog definition has no env parameter. The closure is the thunk plus null. */
        ct_sb_putc(e->out, '(');
        append_type_name(e, func->type);
        ct_sb_appendf(e->out, "){jog_fnthunk_%u, 0}", op->as.func);
        return;
    }
    case JIR_OPND_NONE:
        break;
    }
    ct_sb_append_cstr(e->out, "0");
}

static void append_paren(Emit *e, const JirOperand *op) {
    ct_sb_putc(e->out, '(');
    append_expr(e, op);
    ct_sb_putc(e->out, ')');
}

static bool append_dst(Emit *e, const JirFunc *func, int32_t dst) {
    if (dst < 0 || (uint32_t)dst >= func->locals.count) {
        emit_fail(e, "C backend: instruction has no destination");
        return false;
    }
    ct_sb_appendf(e->out, "l%d = ", dst);
    return true;
}

static size_t kept_func_params(const JirFunc *func) {
    size_t count = 0;
    uint32_t limit = func->param_count;
    if (limit > func->locals.count) {
        limit = (uint32_t)func->locals.count;
    }
    for (uint32_t i = 0; i < limit; i++) {
        JirLocal *local = jir_local_at(func, i);
        if (local && !skip_value(local->type)) {
            count++;
        }
    }
    return count;
}

static bool func_va_dummy(const JirFunc *func) {
    bool variadic = func->type && func->type->kind == JOG_TY_FN && func->type->as.fn.variadic;
    return variadic && kept_func_params(func) == 0;
}

static size_t kept_type_params(const JogType *type) {
    size_t count = 0;
    if (!type || type->kind != JOG_TY_FN) {
        return 0;
    }
    for (size_t i = 0; i < type->as.fn.param_count; i++) {
        JogType *param = type->as.fn.params ? type->as.fn.params[i] : NULL;
        if (!skip_value(param)) {
            count++;
        }
    }
    return count;
}

static void append_result_type(Emit *e, const JirFunc *func) {
    JogType *ret = NULL;
    if (func->type && func->type->kind == JOG_TY_FN) {
        ret = func->type->as.fn.ret;
    }
    if (skip_value(ret)) {
        ct_sb_append_cstr(e->out, "void");
        return;
    }
    append_type_name(e, ret);
}

static void append_def_params(Emit *e, const JirFunc *func) {
    bool any = false;
    bool variadic = func->type && func->type->kind == JOG_TY_FN && func->type->as.fn.variadic;
    uint32_t limit = func->param_count;
    if (limit > func->locals.count) {
        limit = (uint32_t)func->locals.count;
    }
    for (uint32_t i = 0; i < limit; i++) {
        JirLocal *local = jir_local_at(func, i);
        if (!local || skip_value(local->type)) {
            continue;
        }
        if (any) {
            ct_sb_append_cstr(e->out, ", ");
        }
        append_type_name(e, local->type);
        ct_sb_appendf(e->out, " l%u", i);
        any = true;
    }
    if (variadic && !any) {
        ct_sb_append_cstr(e->out, "int jog_va");
        any = true;
    }
    if (variadic) {
        ct_sb_append_cstr(e->out, ", ...");
        any = true;
    }
    if (!any) {
        ct_sb_append_cstr(e->out, "void");
    }
}

static void emit_signature(Emit *e, const JirFunc *func, bool prototype) {
    if (func->linkage == JIR_LINK_INTERNAL) {
        ct_sb_append_cstr(e->out, "static ");
    } else if (prototype && func->linkage == JIR_LINK_EXTERN) {
        ct_sb_append_cstr(e->out, "extern ");
    }
    append_result_type(e, func);
    ct_sb_putc(e->out, ' ');
    if (!func->name || func->name[0] == '\0') {
        emit_fail(e, "C backend: function has no name");
        ct_sb_append_cstr(e->out, "jog_missing");
    } else {
        ct_sb_append_cstr(e->out, func->name);
    }
    ct_sb_putc(e->out, '(');
    append_def_params(e, func);
    ct_sb_putc(e->out, ')');
}

static void append_call_args(Emit *e, const JirOperand *args, size_t count, bool leading_zero, bool trailing_env,
                             const JirOperand *env_src) {
    bool any = false;
    if (leading_zero) {
        ct_sb_append_cstr(e->out, "0");
        any = true;
    }
    for (size_t i = 0; i < count; i++) {
        if (!args || skip_value(args[i].type)) {
            continue;
        }
        if (any) {
            ct_sb_append_cstr(e->out, ", ");
        }
        append_paren(e, &args[i]);
        any = true;
    }
    if (trailing_env && env_src) {
        if (any) {
            ct_sb_append_cstr(e->out, ", ");
        }
        append_paren(e, env_src);
        ct_sb_append_cstr(e->out, ".env");
    }
}

static void append_field_path(Emit *e, JogType *type, uint32_t aux, uint32_t aux2) {
    type = underlying(type);
    if (!type) {
        emit_fail(e, "C backend: field of an unknown type");
        ct_sb_append_cstr(e->out, "f_0");
        return;
    }
    if (type->kind == JOG_TY_SLICE || type->kind == JOG_TY_BYTES) {
        if (aux > 1) {
            emit_fail(e, "C backend: slice field is out of range");
        }
        ct_sb_append_cstr(e->out, aux == 0 ? "ptr" : "len");
        return;
    }
    if (type->kind == JOG_TY_INTERFACE) {
        if (aux > 1) {
            emit_fail(e, "C backend: interface field is out of range");
        }
        ct_sb_append_cstr(e->out, aux == 0 ? "ptr" : "vtable");
        return;
    }
    if (type->kind == JOG_TY_TUPLE) {
        if (aux >= type->as.tuple.count) {
            emit_fail(e, "C backend: tuple field is out of range");
            ct_sb_append_cstr(e->out, "f_0");
            return;
        }
        ct_sb_appendf(e->out, "f_%u", aux);
        return;
    }
    if (type->kind == JOG_TY_STRUCT) {
        if (!type->as.nominal.fields || aux >= type->as.nominal.field_count) {
            emit_fail(e, "C backend: struct field is out of range");
            ct_sb_append_cstr(e->out, "f_0");
            return;
        }
        append_field_ident(e->out, type->as.nominal.fields, aux);
        return;
    }
    if (type->kind == JOG_TY_ENUM && aux2 > 0) {
        size_t variant = (size_t)aux2 - 1;
        if (!type->as.nominal.variants || variant >= type->as.nominal.variant_count) {
            emit_fail(e, "C backend: enum variant is out of range");
            ct_sb_append_cstr(e->out, "tag");
            return;
        }
        ct_sb_append_cstr(e->out, "payload.");
        append_variant_ident(e->out, type->as.nominal.variants, variant);
        ct_sb_putc(e->out, '.');
        JogVariant *payload = &type->as.nominal.variants[variant];
        if (!payload->fields || aux >= payload->field_count) {
            emit_fail(e, "C backend: enum field is out of range");
            ct_sb_append_cstr(e->out, "f_0");
            return;
        }
        append_field_ident(e->out, payload->fields, aux);
        return;
    }
    if (type->kind == JOG_TY_OPTION && !jog_type_option_is_niche(type)) {
        ct_sb_append_cstr(e->out, aux == 0 ? "has" : "value");
        return;
    }
    emit_fail(e, "C backend: cannot address that field");
    ct_sb_append_cstr(e->out, "f_0");
}

static const char *binop_spell(JirBinOp op) {
    switch (op) {
    case JIR_ADD:
        return "+";
    case JIR_SUB:
        return "-";
    case JIR_MUL:
        return "*";
    case JIR_DIV:
        return "/";
    case JIR_REM:
        return "%";
    case JIR_AND:
        return "&";
    case JIR_OR:
        return "|";
    case JIR_XOR:
        return "^";
    case JIR_SHL:
        return "<<";
    case JIR_SHR:
        return ">>";
    case JIR_EQ:
        return "==";
    case JIR_NE:
        return "!=";
    case JIR_LT:
        return "<";
    case JIR_LE:
        return "<=";
    case JIR_GT:
        return ">";
    case JIR_GE:
        return ">=";
    }
    return NULL;
}

static const char *overflow_builtin(JirBinOp op) {
    switch (op) {
    case JIR_ADD:
        return "__builtin_add_overflow";
    case JIR_SUB:
        return "__builtin_sub_overflow";
    case JIR_MUL:
        return "__builtin_mul_overflow";
    default:
        return NULL;
    }
}

static bool dst_is_int_num(const JirFunc *func, int32_t dst) {
    if (dst < 0 || (uint32_t)dst >= func->locals.count) {
        return false;
    }
    JirLocal *local = jir_local_at(func, (uint32_t)dst);
    return local && type_is_int(local->type) && !type_is_bool(local->type);
}

static void emit_plain_binop(Emit *e, const JirFunc *func, const JirInst *inst, const char *spell) {
    if (!append_dst(e, func, inst->dst)) {
        return;
    }
    append_paren(e, &inst->a);
    ct_sb_appendf(e->out, " %s ", spell);
    append_paren(e, &inst->b);
    ct_sb_append_cstr(e->out, ";\n");
}

static void emit_checked_divmod(Emit *e, const JirFunc *func, const JirInst *inst, const char *spell) {
    if (!inst->b.type) {
        emit_fail(e, "C backend: division is missing a type");
        return;
    }
    ct_sb_append_cstr(e->out, "{\n        ");
    append_type_name(e, inst->b.type);
    ct_sb_append_cstr(e->out, " jog_rhs = ");
    append_paren(e, &inst->b);
    ct_sb_append_cstr(e->out, ";\n        if (jog_rhs == 0) {\n            jogrt_panic_divide_by_zero(");
    append_loc(e->out, inst->pos);
    ct_sb_append_cstr(e->out, ");\n        }\n");
    if (int_signed(inst->a.type)) {
        const char *min_name = signed_min_macro(inst->a.type);
        if (!min_name) {
            emit_fail(e, "C backend: signed division has no minimum constant");
        } else {
            ct_sb_append_cstr(e->out, "        if (");
            append_paren(e, &inst->a);
            ct_sb_appendf(e->out, " == %s && jog_rhs == -1) {\n", min_name);
            ct_sb_append_cstr(e->out, "            jogrt_panic_overflow(");
            append_loc(e->out, inst->pos);
            ct_sb_append_cstr(e->out, ");\n        }\n");
        }
    }
    ct_sb_append_cstr(e->out, "        ");
    if (!append_dst(e, func, inst->dst)) {
        return;
    }
    append_paren(e, &inst->a);
    ct_sb_appendf(e->out, " %s jog_rhs;\n    }\n", spell);
}

static void emit_checked_shift(Emit *e, const JirFunc *func, const JirInst *inst, const char *spell) {
    if (!inst->b.type) {
        emit_fail(e, "C backend: shift is missing a type");
        return;
    }
    unsigned width = int_width(inst->a.type);
    ct_sb_append_cstr(e->out, "{\n        ");
    append_type_name(e, inst->b.type);
    ct_sb_append_cstr(e->out, " jog_rhs = ");
    append_paren(e, &inst->b);
    ct_sb_append_cstr(e->out, ";\n        if (");
    if (int_signed(inst->b.type)) {
        ct_sb_append_cstr(e->out, "jog_rhs < 0 || ");
    }
    ct_sb_appendf(e->out, "jog_rhs >= %u) {\n            jogrt_panic_overflow(", width);
    append_loc(e->out, inst->pos);
    ct_sb_append_cstr(e->out, ");\n        }\n        ");
    if (!append_dst(e, func, inst->dst)) {
        return;
    }
    append_paren(e, &inst->a);
    ct_sb_appendf(e->out, " %s jog_rhs;\n    }\n", spell);
}

static void emit_binary(Emit *e, const JirFunc *func, const JirInst *inst) {
    JirBinOp op = (JirBinOp)inst->aux;
    const char *spell = binop_spell(op);
    if (!spell) {
        emit_fail(e, "C backend: unknown binary operator");
        return;
    }
    begin_stmt(e, inst->pos);
    const char *builtin = overflow_builtin(op);
    if (inst->checked && builtin && dst_is_int_num(func, inst->dst)) {
        ct_sb_appendf(e->out, "if (%s(", builtin);
        append_paren(e, &inst->a);
        ct_sb_append_cstr(e->out, ", ");
        append_paren(e, &inst->b);
        ct_sb_appendf(e->out, ", &l%d)) {\n        jogrt_panic_overflow(", inst->dst);
        append_loc(e->out, inst->pos);
        ct_sb_append_cstr(e->out, ");\n    }\n");
        return;
    }
    if (inst->checked && (op == JIR_DIV || op == JIR_REM) && type_is_int(inst->a.type) && !type_is_bool(inst->a.type)) {
        emit_checked_divmod(e, func, inst, spell);
        return;
    }
    if (inst->checked && op == JIR_DIV && type_is_float(inst->a.type)) {
        ct_sb_append_cstr(e->out, "if (");
        append_paren(e, &inst->b);
        ct_sb_append_cstr(e->out, " == 0) {\n        jogrt_panic_divide_by_zero(");
        append_loc(e->out, inst->pos);
        ct_sb_append_cstr(e->out, ");\n    }\n    ");
        emit_plain_binop(e, func, inst, spell);
        return;
    }
    if (op == JIR_REM && type_is_float(inst->a.type)) {
        const char *fn = float_width(inst->a.type) == 32 ? "jog_fmodf" : "jog_fmod";
        if (inst->checked) {
            ct_sb_append_cstr(e->out, "if (");
            append_paren(e, &inst->b);
            ct_sb_append_cstr(e->out, " == 0) {\n        jogrt_panic_divide_by_zero(");
            append_loc(e->out, inst->pos);
            ct_sb_append_cstr(e->out, ");\n    }\n    ");
        }
        if (!append_dst(e, func, inst->dst)) {
            return;
        }
        ct_sb_append_cstr(e->out, fn);
        ct_sb_append_cstr(e->out, "(");
        append_paren(e, &inst->a);
        ct_sb_append_cstr(e->out, ", ");
        append_paren(e, &inst->b);
        ct_sb_append_cstr(e->out, ");\n");
        return;
    }
    if (inst->checked && (op == JIR_SHL || op == JIR_SHR) && type_is_int(inst->a.type) && !type_is_bool(inst->a.type)) {
        emit_checked_shift(e, func, inst, spell);
        return;
    }
    emit_plain_binop(e, func, inst, spell);
}

static void emit_unary(Emit *e, const JirFunc *func, const JirInst *inst) {
    begin_stmt(e, inst->pos);
    if (inst->aux == JIR_NEG && inst->checked && dst_is_int_num(func, inst->dst)) {
        ct_sb_appendf(e->out, "if (__builtin_sub_overflow(0, ");
        append_paren(e, &inst->a);
        ct_sb_appendf(e->out, ", &l%d)) {\n        jogrt_panic_overflow(", inst->dst);
        append_loc(e->out, inst->pos);
        ct_sb_append_cstr(e->out, ");\n    }\n");
        return;
    }
    if (!append_dst(e, func, inst->dst)) {
        return;
    }
    if (inst->aux == JIR_NEG) {
        ct_sb_append_cstr(e->out, "-");
    } else if (inst->aux == JIR_NOT) {
        ct_sb_append_cstr(e->out, "!");
    } else if (inst->aux == JIR_BITNOT) {
        ct_sb_append_cstr(e->out, "~");
    } else {
        emit_fail(e, "C backend: unknown unary operator");
        ct_sb_append_cstr(e->out, "0;\n");
        return;
    }
    append_paren(e, &inst->a);
    ct_sb_append_cstr(e->out, ";\n");
}

static bool int_always_fits(const JogType *src, const JogType *dst) {
    if (!type_is_int(src) || !type_is_int(dst) || type_is_bool(dst)) {
        return false;
    }
    if (type_is_bool(src)) {
        return true;
    }
    unsigned src_bits = int_width(src);
    unsigned dst_bits = int_width(dst);
    bool src_signed = int_signed(src);
    bool dst_signed = int_signed(dst);
    if (src_signed == dst_signed && dst_bits >= src_bits) {
        return true;
    }
    return !src_signed && dst_signed && dst_bits > src_bits;
}

static bool append_limit(Emit *e, const char *macro) {
    if (!macro) {
        emit_fail(e, "C backend: integer limit has no C name");
        ct_sb_append_cstr(e->out, "0");
        return false;
    }
    ct_sb_append_cstr(e->out, macro);
    return true;
}

/* True when src fits in dst. src is parenthesized by append_paren. */
static void append_int_fits(Emit *e, const JirOperand *src, const JogType *dst) {
    if (type_is_bool(dst)) {
        append_paren(e, src);
        ct_sb_append_cstr(e->out, " == 0 || ");
        append_paren(e, src);
        ct_sb_append_cstr(e->out, " == 1");
        return;
    }
    bool src_signed = int_signed(src->type);
    bool dst_signed = int_signed(dst);
    unsigned src_bits = int_width(src->type);
    unsigned dst_bits = int_width(dst);
    if (src_signed && !dst_signed) {
        append_paren(e, src);
        ct_sb_append_cstr(e->out, " >= 0");
        if (dst_bits < src_bits) {
            ct_sb_append_cstr(e->out, " && ");
            append_paren(e, src);
            ct_sb_append_cstr(e->out, " <= ");
            append_limit(e, unsigned_max_macro(dst));
        }
        return;
    }
    if (!src_signed && dst_signed) {
        append_paren(e, src);
        ct_sb_append_cstr(e->out, " <= ");
        append_limit(e, signed_max_macro(dst));
        return;
    }
    if (!src_signed && !dst_signed) {
        append_paren(e, src);
        ct_sb_append_cstr(e->out, " <= ");
        append_limit(e, unsigned_max_macro(dst));
        return;
    }
    append_paren(e, src);
    ct_sb_append_cstr(e->out, " >= ");
    append_limit(e, signed_min_macro(dst));
    ct_sb_append_cstr(e->out, " && ");
    append_paren(e, src);
    ct_sb_append_cstr(e->out, " <= ");
    append_limit(e, signed_max_macro(dst));
}

static void append_float_fits_int(Emit *e, const JirOperand *src, const JogType *dst) {
    append_paren(e, src);
    ct_sb_append_cstr(e->out, " == ");
    append_paren(e, src);
    ct_sb_append_cstr(e->out, " && ");
    if (type_is_bool(dst)) {
        append_paren(e, src);
        ct_sb_append_cstr(e->out, " == 0 || ");
        append_paren(e, src);
        ct_sb_append_cstr(e->out, " == 1");
        return;
    }
    if (!int_signed(dst) && int_width(dst) >= 64) {
        append_paren(e, src);
        ct_sb_append_cstr(e->out, " >= 0 && ");
        append_paren(e, src);
        ct_sb_append_cstr(e->out, " < 18446744073709551616.0");
        return;
    }
    if (int_signed(dst) && int_width(dst) >= 64) {
        append_paren(e, src);
        ct_sb_append_cstr(e->out, " >= -9223372036854775808.0 && ");
        append_paren(e, src);
        ct_sb_append_cstr(e->out, " < 9223372036854775808.0");
        return;
    }
    append_paren(e, src);
    if (!int_signed(dst)) {
        ct_sb_append_cstr(e->out, " >= 0 && ");
        append_paren(e, src);
        ct_sb_append_cstr(e->out, " <= (double)");
        append_limit(e, unsigned_max_macro(dst));
        return;
    }
    ct_sb_append_cstr(e->out, " >= (double)");
    append_limit(e, signed_min_macro(dst));
    ct_sb_append_cstr(e->out, " && ");
    append_paren(e, src);
    ct_sb_append_cstr(e->out, " <= (double)");
    append_limit(e, signed_max_macro(dst));
}

static void emit_convert_fail(Emit *e, JogPos pos) {
    ct_sb_append_cstr(e->out, " {\n        jogrt_panic_convert(");
    append_loc(e->out, pos);
    ct_sb_append_cstr(e->out, ");\n    }\n    ");
}

static void emit_cast_assign(Emit *e, const JirFunc *func, const JirInst *inst, JogType *dst_type) {
    if (!append_dst(e, func, inst->dst)) {
        return;
    }
    ct_sb_putc(e->out, '(');
    append_type_name(e, dst_type);
    ct_sb_putc(e->out, ')');
    append_paren(e, &inst->a);
    ct_sb_append_cstr(e->out, ";\n");
}

static void emit_convert(Emit *e, const JirFunc *func, const JirInst *inst) {
    if (inst->dst < 0 || (uint32_t)inst->dst >= func->locals.count) {
        emit_fail(e, "C backend: convert has no destination");
        return;
    }
    JogType *dst_type = jir_local_at(func, (uint32_t)inst->dst)->type;
    JogType *src_type = inst->a.type;
    begin_stmt(e, inst->pos);
    bool check = inst->checked && dst_type && src_type && !int_always_fits(src_type, dst_type);
    if (check && type_is_int(src_type) && type_is_float(dst_type)) {
        check = false;
    }
    if (check && type_is_int(src_type) && type_is_int(dst_type)) {
        ct_sb_append_cstr(e->out, "if (!(");
        append_int_fits(e, &inst->a, dst_type);
        ct_sb_append_cstr(e->out, "))");
        emit_convert_fail(e, inst->pos);
    } else if (check && type_is_float(src_type) && type_is_int(dst_type)) {
        ct_sb_append_cstr(e->out, "if (!(");
        append_float_fits_int(e, &inst->a, dst_type);
        ct_sb_append_cstr(e->out, "))");
        emit_convert_fail(e, inst->pos);
    } else if (check && type_is_float(src_type) && type_is_float(dst_type) && float_width(src_type) > float_width(dst_type)) {
        ct_sb_append_cstr(e->out, "if (");
        append_paren(e, &inst->a);
        ct_sb_append_cstr(e->out, " > 3.4028234663852886e+38 || ");
        append_paren(e, &inst->a);
        ct_sb_append_cstr(e->out, " < -3.4028234663852886e+38)");
        emit_convert_fail(e, inst->pos);
    }
    emit_cast_assign(e, func, inst, dst_type);
}

static void emit_bitcast(Emit *e, const JirFunc *func, const JirInst *inst) {
    if (inst->dst < 0 || (uint32_t)inst->dst >= func->locals.count) {
        emit_fail(e, "C backend: bitcast has no destination");
        return;
    }
    JogType *dst_type = jir_local_at(func, (uint32_t)inst->dst)->type;
    if (skip_value(dst_type) || skip_value(inst->a.type)) {
        emit_fail(e, "C backend: bitcast type has no C object");
        return;
    }
    begin_stmt(e, inst->pos);
    ct_sb_append_cstr(e->out, "{\n        union { ");
    append_type_name(e, inst->a.type);
    ct_sb_append_cstr(e->out, " s; ");
    append_type_name(e, dst_type);
    ct_sb_append_cstr(e->out, " d; } jog_pun;\n        jog_pun.s = ");
    append_paren(e, &inst->a);
    ct_sb_appendf(e->out, ";\n        l%d = jog_pun.d;\n    }\n", inst->dst);
}

static void emit_aggregate(Emit *e, const JirFunc *func, const JirInst *inst) {
    if (inst->dst < 0 || (uint32_t)inst->dst >= func->locals.count) {
        emit_fail(e, "C backend: aggregate has no destination");
        return;
    }
    JogType *dst_type = jir_local_at(func, (uint32_t)inst->dst)->type;
    JogType *type = underlying(dst_type);
    if (!type || skip_value(dst_type)) {
        emit_fail(e, "C backend: aggregate has no C representation");
        return;
    }
    begin_stmt(e, inst->pos);
    /* A capturing lambda is { function, env }, not { thunk, null }.
       The function already takes void *env, so it is the code pointer. */
    if (type->kind == JOG_TY_FN && !type->as.fn.is_extern) {
        JirFunc *code;
        if (inst->arg_count < 2 || !inst->args || inst->args[0].kind != JIR_OPND_FUNC) {
            emit_fail(e, "C backend: function aggregate needs a code pointer and an env");
            return;
        }
        code = jir_func(e->module, inst->args[0].as.func);
        if (!code || !code->name) {
            emit_fail(e, "C backend: missing function");
            return;
        }
        if (!append_dst(e, func, inst->dst)) {
            return;
        }
        ct_sb_putc(e->out, '(');
        append_type_name(e, dst_type);
        ct_sb_appendf(e->out, "){%s, ", code->name);
        append_expr(e, &inst->args[1]);
        ct_sb_append_cstr(e->out, "};\n");
        return;
    }
    if (type->kind == JOG_TY_OPTION && jog_type_option_is_niche(type)) {
        if (inst->arg_count < 2 || !inst->args) {
            emit_fail(e, "C backend: niche option aggregate needs a tag and a value");
            return;
        }
        if (!append_dst(e, func, inst->dst)) {
            return;
        }
        append_paren(e, &inst->args[0]);
        ct_sb_append_cstr(e->out, " ? ");
        append_paren(e, &inst->args[1]);
        ct_sb_append_cstr(e->out, " : 0;\n");
        return;
    }
    if (!append_dst(e, func, inst->dst)) {
        return;
    }
    ct_sb_putc(e->out, '(');
    append_type_name(e, dst_type);
    ct_sb_append_cstr(e->out, "){0};\n");
    if (type->kind == JOG_TY_SLICE || type->kind == JOG_TY_BYTES) {
        if (inst->arg_count < 2 || !inst->args) {
            emit_fail(e, "C backend: slice aggregate needs a pointer and a length");
            return;
        }
        ct_sb_appendf(e->out, "    l%d.", inst->dst);
        append_field_path(e, type, 0, 0);
        ct_sb_append_cstr(e->out, " = ");
        append_paren(e, &inst->args[0]);
        ct_sb_appendf(e->out, ";\n    l%d.", inst->dst);
        append_field_path(e, type, 1, 0);
        ct_sb_append_cstr(e->out, " = ");
        append_paren(e, &inst->args[1]);
        ct_sb_append_cstr(e->out, ";\n");
        return;
    }
    if (type->kind == JOG_TY_INTERFACE) {
        if (inst->arg_count < 2 || !inst->args) {
            emit_fail(e, "C backend: interface aggregate needs a pointer and a table");
            return;
        }
        ct_sb_appendf(e->out, "    l%d.ptr = ", inst->dst);
        append_paren(e, &inst->args[0]);
        ct_sb_appendf(e->out, ";\n    l%d.vtable = ", inst->dst);
        append_paren(e, &inst->args[1]);
        ct_sb_append_cstr(e->out, ";\n");
        return;
    }
    if (type->kind == JOG_TY_ENUM) {
        const JogVariant *variant;
        size_t argi = 0;
        if (!type->as.nominal.variants || inst->aux >= type->as.nominal.variant_count) {
            emit_fail(e, "C backend: enum aggregate variant is out of range");
            return;
        }
        variant = &type->as.nominal.variants[inst->aux];
        ct_sb_appendf(e->out, "    l%d.tag = %" PRId64 ";\n", inst->dst, variant->tag);
        for (size_t field = 0; field < variant->field_count; field++) {
            JogType *field_type = variant->fields ? variant->fields[field].type : NULL;
            if (skip_value(field_type)) {
                continue;
            }
            while (argi < inst->arg_count && inst->args && skip_value(inst->args[argi].type)) {
                argi++;
            }
            if (argi >= inst->arg_count || !inst->args) {
                emit_fail(e, "C backend: aggregate is missing a field");
                return;
            }
            ct_sb_appendf(e->out, "    l%d.", inst->dst);
            append_field_path(e, type, (uint32_t)field, inst->aux + 1);
            ct_sb_append_cstr(e->out, " = ");
            append_paren(e, &inst->args[argi]);
            ct_sb_append_cstr(e->out, ";\n");
            argi++;
        }
        return;
    }
    if (type->kind == JOG_TY_OPTION) {
        if (inst->arg_count < 1 || !inst->args) {
            emit_fail(e, "C backend: option aggregate needs a tag");
            return;
        }
        ct_sb_appendf(e->out, "    l%d.has = ", inst->dst);
        append_paren(e, &inst->args[0]);
        ct_sb_append_cstr(e->out, ";\n");
        if (!skip_value(type->as.elem.elem)) {
            if (inst->arg_count < 2) {
                emit_fail(e, "C backend: option aggregate needs a value");
                return;
            }
            ct_sb_appendf(e->out, "    l%d.value = ", inst->dst);
            append_paren(e, &inst->args[1]);
            ct_sb_append_cstr(e->out, ";\n");
        }
        return;
    }
    size_t fields = 0;
    if (type->kind == JOG_TY_STRUCT) {
        fields = type->as.nominal.field_count;
    } else if (type->kind == JOG_TY_TUPLE) {
        fields = type->as.tuple.count;
    } else {
        emit_fail(e, "C backend: aggregate type is not a struct, tuple, slice, or option");
        return;
    }
    size_t argi = 0;
    for (size_t field = 0; field < fields; field++) {
        JogType *field_type = NULL;
        if (type->kind == JOG_TY_STRUCT) {
            field_type = type->as.nominal.fields ? type->as.nominal.fields[field].type : NULL;
        } else {
            field_type = type->as.tuple.elems ? type->as.tuple.elems[field] : NULL;
        }
        if (skip_value(field_type)) {
            continue;
        }
        while (argi < inst->arg_count && inst->args && skip_value(inst->args[argi].type)) {
            argi++;
        }
        if (argi >= inst->arg_count || !inst->args) {
            emit_fail(e, "C backend: aggregate is missing a field");
            return;
        }
        ct_sb_appendf(e->out, "    l%d.", inst->dst);
        append_field_path(e, type, (uint32_t)field, 0);
        ct_sb_append_cstr(e->out, " = ");
        append_paren(e, &inst->args[argi]);
        ct_sb_append_cstr(e->out, ";\n");
        argi++;
    }
}

static void emit_call(Emit *e, const JirFunc *func, const JirInst *inst) {
    begin_stmt(e, inst->pos);
    JogType *ret = NULL;
    bool direct = inst->a.kind == JIR_OPND_FUNC;
    JirFunc *callee = NULL;
    JogType *fn_type = underlying(inst->a.type);
    if (direct) {
        callee = jir_func(e->module, inst->a.as.func);
        if (!callee) {
            emit_fail(e, "C backend: call target is missing");
            return;
        }
        fn_type = callee->type;
    }
    if (fn_type && fn_type->kind == JOG_TY_FN) {
        ret = fn_type->as.fn.ret;
    }
    if (inst->dst >= 0 && !skip_value(ret)) {
        if (!append_dst(e, func, inst->dst)) {
            return;
        }
    }
    bool closure = !direct && fn_type && fn_type->kind == JOG_TY_FN && !fn_type->as.fn.is_extern;
    bool variadic = fn_type && fn_type->kind == JOG_TY_FN && fn_type->as.fn.variadic;
    bool dummy = false;
    if (direct) {
        ct_sb_append_cstr(e->out, callee->name ? callee->name : "jog_missing");
        dummy = func_va_dummy(callee);
    } else if (closure) {
        append_paren(e, &inst->a);
        ct_sb_append_cstr(e->out, ".code");
    } else {
        append_paren(e, &inst->a);
        dummy = variadic && kept_type_params(fn_type) == 0;
    }
    ct_sb_putc(e->out, '(');
    append_call_args(e, inst->args, inst->arg_count, dummy, closure, closure ? &inst->a : NULL);
    ct_sb_append_cstr(e->out, ");\n");
}

static void emit_panic(Emit *e, const JirInst *inst) {
    ct_sb_append_cstr(e->out, "jogrt_panic(");
    if (inst->a.kind == JIR_OPND_STRING || inst->a.kind == JIR_OPND_CSTRING) {
        bool cstring = inst->a.kind == JIR_OPND_CSTRING;
        int32_t id = find_bytes(e, inst->a.as.str.ptr, inst->a.as.str.len, cstring);
        if (id < 0) {
            emit_fail(e, "C backend: panic string was not emitted");
            ct_sb_append_cstr(e->out, "0, 0");
        } else {
            ct_sb_appendf(e->out, "(const char *)jog_%s%u, %" PRIu64, cstring ? "c" : "s", (unsigned)id,
                          (uint64_t)inst->a.as.str.len);
        }
    } else if (type_kind_is(inst->a.type, JOG_TY_BYTES) || type_kind_is(inst->a.type, JOG_TY_SLICE)) {
        ct_sb_append_cstr(e->out, "(const char *)");
        append_paren(e, &inst->a);
        ct_sb_append_cstr(e->out, ".ptr, ");
        append_paren(e, &inst->a);
        ct_sb_append_cstr(e->out, ".len");
    } else {
        emit_fail(e, "C backend: panic message is not a string");
        ct_sb_append_cstr(e->out, "0, 0");
    }
    ct_sb_append_cstr(e->out, ", ");
    append_loc(e->out, inst->pos);
    ct_sb_append_cstr(e->out, ");\n");
}

static void emit_inst(Emit *e, const JirFunc *func, const JirInst *inst) {
    if (e->failed) {
        return;
    }
    switch (inst->op) {
    case JIR_COPY:
        begin_stmt(e, inst->pos);
        if (append_dst(e, func, inst->dst)) {
            append_paren(e, &inst->a);
            ct_sb_append_cstr(e->out, ";\n");
        }
        return;
    case JIR_ZERO:
        begin_stmt(e, inst->pos);
        if (append_dst(e, func, inst->dst)) {
            JirLocal *local = jir_local_at(func, (uint32_t)inst->dst);
            ct_sb_putc(e->out, '(');
            append_type_name(e, local ? local->type : NULL);
            ct_sb_append_cstr(e->out, "){0};\n");
        }
        return;
    case JIR_BINARY:
        emit_binary(e, func, inst);
        return;
    case JIR_UNARY:
        emit_unary(e, func, inst);
        return;
    case JIR_CONVERT:
        emit_convert(e, func, inst);
        return;
    case JIR_BITCAST:
        emit_bitcast(e, func, inst);
        return;
    case JIR_LOAD:
        begin_stmt(e, inst->pos);
        if (append_dst(e, func, inst->dst)) {
            ct_sb_putc(e->out, '*');
            append_paren(e, &inst->a);
            ct_sb_append_cstr(e->out, ";\n");
        }
        return;
    case JIR_STORE:
        begin_stmt(e, inst->pos);
        ct_sb_putc(e->out, '*');
        append_paren(e, &inst->a);
        ct_sb_append_cstr(e->out, " = ");
        append_paren(e, &inst->b);
        ct_sb_append_cstr(e->out, ";\n");
        return;
    case JIR_ADDR:
        begin_stmt(e, inst->pos);
        if (inst->a.kind != JIR_OPND_LOCAL) {
            emit_fail(e, "C backend: addr needs a local");
            return;
        }
        if (append_dst(e, func, inst->dst)) {
            ct_sb_appendf(e->out, "&l%u;\n", inst->a.as.local);
        }
        return;
    case JIR_FIELD_ADDR: {
        begin_stmt(e, inst->pos);
        if (!append_dst(e, func, inst->dst)) {
            return;
        }
        ct_sb_putc(e->out, '&');
        append_paren(e, &inst->a);
        ct_sb_append_cstr(e->out, "->");
        JogType *ptr = underlying(inst->a.type);
        JogType *pointee = ptr && ptr->kind == JOG_TY_PTR ? ptr->as.elem.elem : NULL;
        append_field_path(e, pointee, inst->aux, inst->aux2);
        ct_sb_append_cstr(e->out, ";\n");
        return;
    }
    case JIR_ELEM_ADDR:
        begin_stmt(e, inst->pos);
        if (append_dst(e, func, inst->dst)) {
            append_paren(e, &inst->a);
            ct_sb_append_cstr(e->out, " + ");
            append_paren(e, &inst->b);
            ct_sb_append_cstr(e->out, ";\n");
        }
        return;
    case JIR_EXTRACT:
        begin_stmt(e, inst->pos);
        if (!append_dst(e, func, inst->dst)) {
            return;
        }
        append_paren(e, &inst->a);
        ct_sb_putc(e->out, '.');
        append_field_path(e, inst->a.type, inst->aux, inst->aux2);
        ct_sb_append_cstr(e->out, ";\n");
        return;
    case JIR_AGGREGATE:
        emit_aggregate(e, func, inst);
        return;
    case JIR_ENUM_TAG:
        begin_stmt(e, inst->pos);
        if (append_dst(e, func, inst->dst)) {
            append_paren(e, &inst->a);
            ct_sb_append_cstr(e->out, ".tag;\n");
        }
        return;
    case JIR_CALL:
        emit_call(e, func, inst);
        return;
    case JIR_BOUNDS_CHECK:
        if (e->mode == JOG_BUILD_RELEASE_FAST) {
            return;
        }
        begin_stmt(e, inst->pos);
        ct_sb_append_cstr(e->out, "if (!(");
        append_paren(e, &inst->a);
        ct_sb_append_cstr(e->out, " < ");
        append_paren(e, &inst->b);
        ct_sb_append_cstr(e->out, ")) {\n        jogrt_panic_bounds((size_t)");
        append_paren(e, &inst->a);
        ct_sb_append_cstr(e->out, ", (size_t)");
        append_paren(e, &inst->b);
        ct_sb_append_cstr(e->out, ", ");
        append_loc(e->out, inst->pos);
        ct_sb_append_cstr(e->out, ");\n    }\n");
        return;
    case JIR_NULL_CHECK: {
        begin_stmt(e, inst->pos);
        JogType *type = underlying(inst->a.type);
        bool niche = !type || type->kind != JOG_TY_OPTION || jog_type_option_is_niche(type);
        ct_sb_append_cstr(e->out, "if (!");
        if (niche) {
            append_paren(e, &inst->a);
        } else {
            append_paren(e, &inst->a);
            ct_sb_append_cstr(e->out, ".has");
        }
        ct_sb_append_cstr(e->out, ") {\n        jogrt_panic_null(");
        append_loc(e->out, inst->pos);
        ct_sb_append_cstr(e->out, ");\n    }\n");
        return;
    }
    case JIR_PANIC:
        begin_stmt(e, inst->pos);
        emit_panic(e, inst);
        return;
    }
    emit_fail(e, "C backend: unknown instruction");
}

static void emit_term(Emit *e, const JirFunc *func, const JirTerm *term) {
    if (e->failed) {
        return;
    }
    switch (term->kind) {
    case JIR_TERM_BR:
        begin_stmt(e, term->pos);
        ct_sb_appendf(e->out, "goto b%u;\n", term->target);
        return;
    case JIR_TERM_COND_BR:
        begin_stmt(e, term->pos);
        ct_sb_append_cstr(e->out, "if (");
        append_paren(e, &term->value);
        ct_sb_appendf(e->out, ") {\n        goto b%u;\n    }\n    goto b%u;\n", term->target, term->else_target);
        return;
    case JIR_TERM_SWITCH:
        begin_stmt(e, term->pos);
        ct_sb_append_cstr(e->out, "switch (");
        append_paren(e, &term->value);
        ct_sb_append_cstr(e->out, ") {\n");
        if (term->case_count > 0 && !term->cases) {
            emit_fail(e, "C backend: switch is missing cases");
            return;
        }
        for (size_t i = 0; i < term->case_count; i++) {
            ct_sb_append_cstr(e->out, "    case ");
            append_bits(e, term->value.type, term->cases[i].value);
            ct_sb_appendf(e->out, ": goto b%u;\n", term->cases[i].target);
        }
        ct_sb_appendf(e->out, "    default: goto b%u;\n    }\n", term->target);
        return;
    case JIR_TERM_RET: {
        JogType *ret = NULL;
        if (func->type && func->type->kind == JOG_TY_FN) {
            ret = func->type->as.fn.ret;
        }
        begin_stmt(e, term->pos);
        if (skip_value(ret)) {
            ct_sb_append_cstr(e->out, "return;\n");
            return;
        }
        ct_sb_append_cstr(e->out, "return ");
        if (term->value.kind == JIR_OPND_NONE) {
            ct_sb_putc(e->out, '(');
            append_type_name(e, ret);
            ct_sb_append_cstr(e->out, "){0};\n");
            return;
        }
        append_paren(e, &term->value);
        ct_sb_append_cstr(e->out, ";\n");
        return;
    }
    case JIR_TERM_UNREACHABLE:
        begin_stmt(e, term->pos);
        ct_sb_append_cstr(e->out, "jogrt_panic_unreachable(");
        append_loc(e->out, term->pos);
        ct_sb_append_cstr(e->out, ");\n");
        return;
    case JIR_TERM_NONE:
        emit_fail(e, "C backend: block is not terminated");
        begin_stmt(e, term->pos);
        ct_sb_append_cstr(e->out, "jogrt_panic_unreachable(");
        append_loc(e->out, term->pos);
        ct_sb_append_cstr(e->out, ");\n");
        return;
    }
    emit_fail(e, "C backend: unknown terminator");
}

static void emit_body(Emit *e, const JirFunc *func) {
    e->lined = false;
    emit_signature(e, func, false);
    ct_sb_append_cstr(e->out, " {\n");
    if (func->blocks.count == 0) {
        emit_fail(e, "C backend: function has no blocks");
        ct_sb_append_cstr(e->out, "    jogrt_panic_unreachable(\"\", 0);\n}\n");
        return;
    }
    for (size_t i = func->param_count; i < func->locals.count; i++) {
        JirLocal *local = jir_local_at(func, (uint32_t)i);
        if (!local || skip_value(local->type)) {
            continue;
        }
        ct_sb_append_cstr(e->out, "    ");
        append_type_name(e, local->type);
        ct_sb_appendf(e->out, " l%u;\n", (unsigned)i);
    }
    if (func_va_dummy(func)) {
        ct_sb_append_cstr(e->out, "    (void)jog_va;\n");
    }
    for (size_t i = 0; i < func->locals.count; i++) {
        JirLocal *local = jir_local_at(func, (uint32_t)i);
        if (!local || skip_value(local->type)) {
            continue;
        }
        ct_sb_appendf(e->out, "    (void)l%u;\n", (unsigned)i);
    }
    ct_sb_append_cstr(e->out, "    goto b0;\n");
    if (func->blocks.count > 1) {
        ct_sb_append_cstr(e->out, "    if (0) {\n");
        for (size_t i = 1; i < func->blocks.count; i++) {
            ct_sb_appendf(e->out, "        goto b%u;\n", (unsigned)i);
        }
        ct_sb_append_cstr(e->out, "    }\n");
    }
    for (size_t bi = 0; bi < func->blocks.count; bi++) {
        JirBlock *block = jir_block_at(func, (uint32_t)bi);
        ct_sb_appendf(e->out, "b%u:;\n", (unsigned)bi);
        for (size_t ii = 0; ii < block->insts.count; ii++) {
            emit_inst(e, func, ct_da_at(&block->insts, ii));
        }
        emit_term(e, func, &block->term);
    }
    ct_sb_append_cstr(e->out, "}\n");
    if (e->lined) {
        ct_sb_append_cstr(e->out, "#line 1 \"jog_emit.c\"\n");
        e->lined = false;
    }
}

static bool bytes_are_zero(const uint8_t *data, size_t len) {
    if (!data) {
        return len == 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (data[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool range_zero(Emit *e, const uint8_t *data, size_t begin, size_t end) {
    if (end < begin) {
        emit_fail(e, "C backend: global field is outside its type");
        return false;
    }
    for (size_t i = begin; i < end; i++) {
        if (data[i] != 0) {
            emit_fail(e, "C backend: global data has non-zero padding");
            return false;
        }
    }
    return true;
}

static bool read_le(const uint8_t *data, size_t n, uint64_t *out) {
    uint64_t bits = 0;
    if (n > 8) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        bits |= (uint64_t)data[i] << (unsigned)(i * 8);
    }
    *out = bits;
    return true;
}

static bool host_is_le(void) {
    uint16_t value = 1;
    unsigned char bytes[2];
    memcpy(bytes, &value, sizeof value);
    return bytes[0] == 1;
}

static uint64_t fields_span(const JogField *fields, size_t count) {
    uint64_t end = 0;
    uint32_t max_align = 1;
    for (size_t i = 0; i < count; i++) {
        JogType *field = fields ? fields[i].type : NULL;
        uint64_t field_end;
        uint32_t align;
        if (!field || !field->has_layout) {
            continue;
        }
        field_end = fields[i].offset + field->size;
        if (field_end > end) {
            end = field_end;
        }
        align = field->align ? field->align : 1;
        if (align > max_align) {
            max_align = align;
        }
    }
    return (end + max_align - 1) / max_align * max_align;
}

static void append_data_init(Emit *e, const JirGlobal *global, size_t abs_off, JogType *type, const uint8_t *data);

static const JirReloc *reloc_at(const JirGlobal *global, size_t offset) {
    size_t i;
    if (!global || offset > UINT32_MAX) {
        return NULL;
    }
    for (i = 0; i < global->reloc_count; i++) {
        if (global->relocs[i].offset == (uint32_t)offset) {
            return &global->relocs[i];
        }
    }
    return NULL;
}

static bool range_has_reloc(const JirGlobal *global, size_t base, size_t size) {
    size_t i;
    if (!global || size == 0) {
        return false;
    }
    for (i = 0; i < global->reloc_count; i++) {
        uint32_t at = global->relocs[i].offset;
        if ((size_t)at >= base && (size_t)at - base < size) {
            return true;
        }
    }
    return false;
}

static void append_reloc_expr(Emit *e, const JirReloc *rel, JogType *ptr_type) {
    if (!rel) {
        ct_sb_append_cstr(e->out, "0");
        return;
    }
    if (rel->kind == JIR_RELOC_STRING || rel->kind == JIR_RELOC_CSTRING) {
        bool cstring = rel->kind == JIR_RELOC_CSTRING;
        int32_t id = find_bytes(e, rel->ptr, rel->len, cstring);
        if (id < 0) {
            emit_fail(e, "C backend: string relocation is missing bytes");
            ct_sb_append_cstr(e->out, "0");
            return;
        }
        ct_sb_appendf(e->out, "jog_%s%u", cstring ? "c" : "s", (unsigned)id);
        return;
    }
    if (rel->kind == JIR_RELOC_GLOBAL) {
        JirGlobal *target = jir_global(e->module, rel->index);
        if (!target || !c_ident(target->name)) {
            emit_fail(e, "C backend: global relocation target was not emitted");
            ct_sb_append_cstr(e->out, "0");
            return;
        }
        if (rel->addend == 0) {
            ct_sb_appendf(e->out, "&%s", target->name);
            return;
        }
        ct_sb_putc(e->out, '(');
        if (ptr_type) {
            append_type_name(e, ptr_type);
        } else {
            ct_sb_append_cstr(e->out, "void *");
        }
        ct_sb_appendf(e->out, ")((char *)&%s + %u)", target->name, rel->addend);
        return;
    }
    if (rel->kind == JIR_RELOC_FUNC) {
        JirFunc *func = jir_func(e->module, rel->index);
        if (!func || !func->name) {
            emit_fail(e, "C backend: function relocation target is missing");
            ct_sb_append_cstr(e->out, "0");
            return;
        }
        if (!func->type || func->type->kind != JOG_TY_FN || func->type->as.fn.is_extern ||
            func->linkage == JIR_LINK_EXTERN) {
            ct_sb_append_cstr(e->out, func->name);
            return;
        }
        if (func->type->as.fn.variadic) {
            emit_fail(e, "C backend: a Jog function used as a closure value needs a thunk");
            ct_sb_append_cstr(e->out, "0");
            return;
        }
        ct_sb_appendf(e->out, "jog_fnthunk_%u", rel->index);
        return;
    }
    emit_fail(e, "C backend: global relocation has a bad kind");
    ct_sb_append_cstr(e->out, "0");
}

static void append_named_fields(Emit *e, const JirGlobal *global, size_t abs_base, const JogField *fields, size_t count,
                                const uint8_t *data, size_t size) {
    size_t cursor = 0;
    bool any = false;
    if (count > 0 && !fields) {
        emit_fail(e, "C backend: global is missing fields");
        return;
    }
    ct_sb_putc(e->out, '{');
    for (size_t i = 0; i < count; i++) {
        JogType *field = fields[i].type;
        size_t off;
        if (!field || field->size == 0 || skip_value(field)) {
            continue;
        }
        if (fields[i].offset > size || field->size > (uint64_t)size - fields[i].offset) {
            emit_fail(e, "C backend: global field is outside its type");
            return;
        }
        off = (size_t)fields[i].offset;
        if (!range_zero(e, data, cursor, off)) {
            return;
        }
        if (any) {
            ct_sb_append_cstr(e->out, ", ");
        }
        ct_sb_putc(e->out, '.');
        append_field_ident(e->out, fields, i);
        ct_sb_append_cstr(e->out, " = ");
        append_data_init(e, global, abs_base + off, field, data + off);
        if (e->failed) {
            return;
        }
        cursor = off + (size_t)field->size;
        any = true;
    }
    if (!range_zero(e, data, cursor, size)) {
        return;
    }
    if (!any) {
        ct_sb_append_cstr(e->out, "0");
    }
    ct_sb_putc(e->out, '}');
}

static void append_tuple_init(Emit *e, const JirGlobal *global, size_t abs_base, const JogType *type, const uint8_t *data,
                              size_t size) {
    size_t cursor = 0;
    bool any = false;
    if (type->as.tuple.count > 0 && (!type->as.tuple.elems || !type->as.tuple.offsets)) {
        emit_fail(e, "C backend: global tuple is missing fields");
        return;
    }
    ct_sb_putc(e->out, '{');
    for (size_t i = 0; i < type->as.tuple.count; i++) {
        JogType *elem = type->as.tuple.elems[i];
        uint64_t raw;
        size_t off;
        if (!elem || elem->size == 0 || skip_value(elem)) {
            continue;
        }
        raw = type->as.tuple.offsets[i];
        if (raw > size || elem->size > (uint64_t)size - raw) {
            emit_fail(e, "C backend: global field is outside its type");
            return;
        }
        off = (size_t)raw;
        if (!range_zero(e, data, cursor, off)) {
            return;
        }
        if (any) {
            ct_sb_append_cstr(e->out, ", ");
        }
        ct_sb_appendf(e->out, ".f_%u = ", (unsigned)i);
        append_data_init(e, global, abs_base + off, elem, data + off);
        if (e->failed) {
            return;
        }
        cursor = off + (size_t)elem->size;
        any = true;
    }
    if (!range_zero(e, data, cursor, size)) {
        return;
    }
    if (!any) {
        ct_sb_append_cstr(e->out, "0");
    }
    ct_sb_putc(e->out, '}');
}

static void append_data_init(Emit *e, const JirGlobal *global, size_t abs_off, JogType *type, const uint8_t *data) {
    size_t size;
    if (e->failed) {
        return;
    }
    if (!type || !data || !type->has_layout || type->size > SIZE_MAX) {
        emit_fail(e, "C backend: global data has no layout");
        return;
    }
    size = (size_t)type->size;
    if (size == 0 || (bytes_are_zero(data, size) && !range_has_reloc(global, abs_off, size))) {
        ct_sb_append_cstr(e->out, spelled_aggregate(type) ? "{0}" : "0");
        return;
    }
    type = underlying(type);
    if (!type) {
        emit_fail(e, "C backend: global data has no layout");
        return;
    }
    switch (type->kind) {
    case JOG_TY_BOOL:
        if (size != 1 || data[0] > 1) {
            emit_fail(e, "C backend: bool global data is not 0 or 1");
            return;
        }
        append_bits(e, type, data[0]);
        return;
    case JOG_TY_INT:
    case JOG_TY_UNTYPED_INT: {
        uint64_t bits = 0;
        if (size > 8 || !read_le(data, size, &bits)) {
            emit_fail(e, "C backend: integer global data has a bad width");
            return;
        }
        append_bits(e, type, bits);
        return;
    }
    case JOG_TY_FLOAT:
    case JOG_TY_UNTYPED_FLOAT: {
        uint64_t bits = 0;
        double value;
        if ((size != 4 && size != 8) || !read_le(data, size, &bits)) {
            emit_fail(e, "C backend: float global data has a bad width");
            return;
        }
        if (!host_is_le()) {
            emit_fail(e, "C backend: float global data needs a little-endian host");
            return;
        }
        if (size == 4) {
            uint32_t narrow = (uint32_t)bits;
            float single;
            memcpy(&single, &narrow, sizeof single);
            value = (double)single;
        } else {
            memcpy(&value, &bits, sizeof value);
        }
        append_float_literal(e, type, value);
        return;
    }
    case JOG_TY_PTR: {
        const JirReloc *rel = reloc_at(global, abs_off);
        if (rel) {
            append_reloc_expr(e, rel, type);
            return;
        }
        if (!bytes_are_zero(data, size)) {
            emit_fail(e, "C backend: global data has a pointer and no relocation");
            ct_sb_append_cstr(e->out, "0");
            return;
        }
        ct_sb_append_cstr(e->out, "0");
        return;
    }
    case JOG_TY_FN:
        if (type->as.fn.is_extern) {
            const JirReloc *rel = reloc_at(global, abs_off);
            if (rel) {
                append_reloc_expr(e, rel, NULL);
                return;
            }
            if (!bytes_are_zero(data, size)) {
                emit_fail(e, "C backend: global data has a pointer and no relocation");
            }
            ct_sb_append_cstr(e->out, "0");
            return;
        }
        {
            const JirReloc *rel = reloc_at(global, abs_off);
            uint32_t ptr = jog_types_ptr_size(e->module->types);
            if (!rel && bytes_are_zero(data, size)) {
                ct_sb_append_cstr(e->out, "{0}");
                return;
            }
            if (!rel || rel->kind != JIR_RELOC_FUNC || ptr == 0 || size < (size_t)ptr * 2 ||
                !bytes_are_zero(data + ptr, size - ptr)) {
                emit_fail(e, "C backend: global data has a pointer and no relocation");
                ct_sb_append_cstr(e->out, "{0}");
                return;
            }
            ct_sb_append_cstr(e->out, "{.code = ");
            append_reloc_expr(e, rel, NULL);
            ct_sb_append_cstr(e->out, ", .env = 0}");
            return;
        }
    case JOG_TY_INTERFACE:
        emit_fail(e, "C backend: global data has a pointer and no relocation");
        ct_sb_append_cstr(e->out, spelled_aggregate(type) ? "{0}" : "0");
        return;
    case JOG_TY_BYTES:
    case JOG_TY_SLICE: {
        uint32_t ptr = jog_types_ptr_size(e->module->types);
        uint64_t len = 0;
        JogType *usize;
        const JirReloc *rel = reloc_at(global, abs_off);
        if (ptr == 0 || size != (size_t)ptr * 2 || !read_le(data + ptr, ptr, &len)) {
            emit_fail(e, "C backend: global slice has a bad size");
            return;
        }
        if (!rel && !bytes_are_zero(data, ptr)) {
            emit_fail(e, "C backend: global data has a pointer and no relocation");
            return;
        }
        usize = jog_type_builtin(e->module->types, JOG_BUILTIN_USIZE);
        ct_sb_append_cstr(e->out, "{.ptr = ");
        if (rel) {
            append_reloc_expr(e, rel, NULL);
        } else {
            ct_sb_append_cstr(e->out, "0");
        }
        ct_sb_append_cstr(e->out, ", .len = ");
        append_bits(e, usize, len);
        ct_sb_append_cstr(e->out, "}");
        return;
    }
    case JOG_TY_BYTES_N:
        if (type->as.bytes_n.n != size) {
            emit_fail(e, "C backend: global bytes have a bad size");
            return;
        }
        ct_sb_append_cstr(e->out, "{.b = {");
        for (size_t i = 0; i < size; i++) {
            if (i > 0) {
                ct_sb_append_cstr(e->out, ", ");
            }
            ct_sb_appendf(e->out, "%u", (unsigned)data[i]);
        }
        ct_sb_append_cstr(e->out, "}}");
        return;
    case JOG_TY_ARRAY: {
        JogType *elem = type->as.array.elem;
        size_t stride;
        size_t count;
        if (!elem || elem->size == 0 || type->as.array.len == 0 || type->as.array.len > SIZE_MAX ||
            elem->size > SIZE_MAX / type->as.array.len) {
            emit_fail(e, "C backend: global array has a bad size");
            return;
        }
        stride = (size_t)elem->size;
        count = (size_t)type->as.array.len;
        if (stride * count != size) {
            emit_fail(e, "C backend: global array has a bad size");
            return;
        }
        ct_sb_append_cstr(e->out, "{.v = {");
        for (size_t i = 0; i < count; i++) {
            if (i > 0) {
                ct_sb_append_cstr(e->out, ", ");
            }
            append_data_init(e, global, abs_off + i * stride, elem, data + i * stride);
            if (e->failed) {
                return;
            }
        }
        ct_sb_append_cstr(e->out, "}}");
        return;
    }
    case JOG_TY_OPTION: {
        JogType *elem = type->as.elem.elem;
        uint32_t align;
        size_t value_off;
        size_t value_end;
        if (jog_type_option_is_niche(type)) {
            append_data_init(e, global, abs_off, elem, data);
            return;
        }
        if (!elem) {
            emit_fail(e, "C backend: option global has no element");
            return;
        }
        align = elem->align > 1 ? elem->align : 1;
        value_off = (size_t)(((uint64_t)1 + align - 1) / align * align);
        if (size < 1 || value_off > size) {
            emit_fail(e, "C backend: option global has a bad size");
            return;
        }
        if (data[0] > 1) {
            emit_fail(e, "C backend: bool global data is not 0 or 1");
            return;
        }
        value_end = value_off;
        if (!skip_value(elem) && elem->size > 0) {
            if (elem->size > (uint64_t)size - value_off) {
                emit_fail(e, "C backend: option global has a bad size");
                return;
            }
            value_end = value_off + (size_t)elem->size;
        }
        if (!range_zero(e, data, 1, value_off) || !range_zero(e, data, value_end, size)) {
            return;
        }
        ct_sb_append_cstr(e->out, "{.has = ");
        ct_sb_append_cstr(e->out, data[0] ? "1" : "0");
        if (value_end > value_off) {
            ct_sb_append_cstr(e->out, ", .value = ");
            append_data_init(e, global, abs_off + value_off, elem, data + value_off);
        }
        ct_sb_append_cstr(e->out, "}");
        return;
    }
    case JOG_TY_TUPLE:
        append_tuple_init(e, global, abs_off, type, data, size);
        return;
    case JOG_TY_STRUCT:
        append_named_fields(e, global, abs_off, type->as.nominal.fields, type->as.nominal.field_count, data, size);
        return;
    case JOG_TY_ENUM: {
        JogType *tag = type->as.nominal.base;
        uint64_t tag_bits = 0;
        uint64_t width;
        uint64_t mask;
        const JogVariant *active = NULL;
        size_t active_index = 0;
        size_t tag_size;
        size_t payload_off;
        uint64_t span;
        if (!tag || !tag->has_layout || tag->size == 0 || tag->size > 8 || tag->size > size ||
            !read_le(data, (size_t)tag->size, &tag_bits)) {
            emit_fail(e, "C backend: global enum tag has a bad width");
            return;
        }
        width = tag->size * 8;
        if (width >= 64) {
            mask = ~(uint64_t)0;
        } else {
            mask = ((uint64_t)1 << width) - 1;
        }
        tag_bits &= mask;
        for (size_t i = 0; i < type->as.nominal.variant_count; i++) {
            uint64_t want;
            if (!type->as.nominal.variants) {
                emit_fail(e, "C backend: global is missing fields");
                return;
            }
            want = (uint64_t)type->as.nominal.variants[i].tag & mask;
            if (want == tag_bits) {
                active = &type->as.nominal.variants[i];
                active_index = i;
                break;
            }
        }
        if (!active) {
            emit_fail(e, "C backend: global enum tag is not a variant");
            return;
        }
        if (active->field_count > 0 && !active->fields) {
            emit_fail(e, "C backend: global is missing fields");
            return;
        }
        tag_size = (size_t)tag->size;
        payload_off = (size_t)type->as.nominal.payload_offset;
        span = fields_span(active->fields, active->field_count);
        if (payload_off < tag_size || payload_off > size || span > SIZE_MAX || (size_t)span > size - payload_off) {
            emit_fail(e, "C backend: global enum has a bad layout");
            return;
        }
        if (!range_zero(e, data, tag_size, payload_off) ||
            !range_zero(e, data, payload_off + (size_t)span, size)) {
            return;
        }
        ct_sb_append_cstr(e->out, "{.tag = ");
        append_bits(e, tag, tag_bits);
        if (variant_has_payload(active) && span > 0 &&
            (!bytes_are_zero(data + payload_off, (size_t)span) ||
             range_has_reloc(global, abs_off + payload_off, (size_t)span))) {
            ct_sb_append_cstr(e->out, ", .payload = {.");
            append_variant_ident(e->out, type->as.nominal.variants, active_index);
            ct_sb_append_cstr(e->out, " = ");
            append_named_fields(e, global, abs_off + payload_off, active->fields, active->field_count, data + payload_off,
                                (size_t)span);
            ct_sb_append_cstr(e->out, "}");
        }
        ct_sb_append_cstr(e->out, "}");
        return;
    }
    default:
        emit_fail(e, "C backend: global type has no C object");
        ct_sb_append_cstr(e->out, "0");
        return;
    }
}

static bool global_pos(JogPos pos) {
    return pos.file && pos.file[0] != '\0' && pos.line != 0;
}

/* A const global that init (or any function) stores is a writable object.
   Folded data with no store stays const. */
static bool global_is_stored(const JirModule *module, uint32_t index) {
    for (size_t fi = 0; fi < module->funcs.count; fi++) {
        JirFunc *func = jir_func(module, (uint32_t)fi);
        if (!func) {
            continue;
        }
        for (size_t bi = 0; bi < func->blocks.count; bi++) {
            JirBlock *block = jir_block_at(func, (uint32_t)bi);
            if (!block) {
                continue;
            }
            for (size_t ii = 0; ii < block->insts.count; ii++) {
                JirInst *inst = ct_da_at(&block->insts, ii);
                if (inst && inst->op == JIR_STORE && inst->a.kind == JIR_OPND_GLOBAL &&
                    inst->a.as.global == index) {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool emit_global(Emit *e, const JirGlobal *global, uint32_t index) {
    size_t size;
    bool zero;
    bool lined = false;
    if (!global || !global->type) {
        emit_fail(e, "C backend: missing global");
        return false;
    }
    if (!global_is_object(global)) {
        if (global->data_len != 0) {
            emit_fail(e, "C backend: global type has no C object");
            return false;
        }
        return true;
    }
    if (!c_ident(global->name)) {
        emit_fail(e, "C backend: global name is not a C identifier");
        return false;
    }
    if (!global->type->has_layout || global->type->size > SIZE_MAX) {
        emit_fail(e, "C backend: global type has no layout");
        return false;
    }
    size = (size_t)global->type->size;
    if (global->data_len == 0) {
        zero = true;
    } else if (!global->data) {
        emit_fail(e, "C backend: global data is missing");
        return false;
    } else if (global->data_len != size) {
        emit_fail(e, "C backend: global data length does not match its type");
        return false;
    } else {
        zero = bytes_are_zero(global->data, size) && global->reloc_count == 0;
    }
    if (global_pos(global->pos)) {
        ct_sb_appendf(e->out, "#line %u ", global->pos.line);
        append_c_literal(e->out, global->pos.file);
        ct_sb_putc(e->out, '\n');
        lined = true;
    }
    ct_sb_append_cstr(e->out, "static ");
    if (global->is_const && !global_is_stored(e->module, index)) {
        ct_sb_append_cstr(e->out, "const ");
    }
    append_type_name(e, global->type);
    ct_sb_appendf(e->out, " %s = ", global->name);
    if (zero) {
        ct_sb_append_cstr(e->out, spelled_aggregate(global->type) ? "{0}" : "0");
    } else {
        append_data_init(e, global, 0, global->type, global->data);
    }
    ct_sb_append_cstr(e->out, ";\n");
    if (lined) {
        ct_sb_append_cstr(e->out, "#line 1 \"jog_emit.c\"\n");
    }
    return !e->failed;
}

static int is_vtable_dummy(const Emit *e, uint32_t index) {
    size_t i;
    if (!e || !e->module) {
        return 0;
    }
    for (i = 0; i < e->module->iface_table_count; i++) {
        if (e->module->iface_tables[i].global == (int32_t)index) {
            return 1;
        }
    }
    return 0;
}

static bool emit_vtables(Emit *e) {
    size_t i;
    size_t s;
    if (!e->module || e->module->iface_table_count == 0) {
        return true;
    }
    for (i = 0; i < e->module->iface_table_count && !e->failed; i++) {
        JirIfaceTable *table = &e->module->iface_tables[i];
        size_t j;
        int seen = 0;
        uint32_t id = table->iface ? table->iface->id : 0;
        for (j = 0; j < i; j++) {
            if (e->module->iface_tables[j].iface == table->iface) {
                seen = 1;
                break;
            }
        }
        if (seen) {
            continue;
        }
        ct_sb_appendf(e->out, "\nstruct jog_it_%u {\n    const void *type_id;\n    size_t size;\n    size_t align;\n", id);
        for (s = 0; s < table->slot_count; s++) {
            ct_sb_append_cstr(e->out, "    ");
            append_type_name(e, table->slots ? table->slots[s].fn_type : NULL);
            ct_sb_appendf(e->out, " m%u;\n", (unsigned)s);
        }
        for (s = 0; s < table->offset_count; s++) {
            ct_sb_appendf(e->out, "    size_t off%u;\n", (unsigned)s);
        }
        ct_sb_append_cstr(e->out, "};\n");
    }
    for (i = 0; i < e->module->iface_table_count && !e->failed; i++) {
        JirIfaceTable *table = &e->module->iface_tables[i];
        uint32_t id = table->iface ? table->iface->id : 0;
        if (!table->name || !c_ident(table->name) || !table->concrete || !table->type_id_name) {
            emit_fail(e, "C backend: interface table is incomplete");
            return false;
        }
        ct_sb_appendf(e->out, "static const struct jog_it_%u %s = {\n    &%s,\n    sizeof(", id, table->name,
                      table->type_id_name);
        append_type_name(e, table->concrete);
        ct_sb_append_cstr(e->out, "),\n    _Alignof(");
        append_type_name(e, table->concrete);
        ct_sb_append_cstr(e->out, ")");
        for (s = 0; s < table->slot_count; s++) {
            JirFunc *thunk = table->slots ? jir_func(e->module, (uint32_t)table->slots[s].func) : NULL;
            if (!thunk || !thunk->name) {
                emit_fail(e, "C backend: interface method is missing");
                return false;
            }
            ct_sb_appendf(e->out, ",\n    %s", thunk->name);
        }
        if (table->offset_count > 0 && !table->offsets) {
            emit_fail(e, "C backend: interface property offset is missing");
            return false;
        }
        for (s = 0; s < table->offset_count; s++) {
            ct_sb_appendf(e->out, ",\n    %llu", (unsigned long long)table->offsets[s]);
        }
        ct_sb_append_cstr(e->out, "\n};\n");
    }
    return !e->failed;
}

static void emit_thunk_proto(Emit *e, const JirFunc *func, uint32_t index) {
    bool any = false;
    uint32_t i;
    uint32_t limit;
    if (!func || !func->name) {
        emit_fail(e, "C backend: missing function");
        return;
    }
    ct_sb_append_cstr(e->out, "static ");
    append_result_type(e, func);
    ct_sb_appendf(e->out, " jog_fnthunk_%u(", index);
    limit = func->param_count;
    if (limit > func->locals.count) {
        limit = (uint32_t)func->locals.count;
    }
    for (i = 0; i < limit; i++) {
        JirLocal *local = jir_local_at(func, i);
        if (!local || skip_value(local->type)) {
            continue;
        }
        if (any) {
            ct_sb_append_cstr(e->out, ", ");
        }
        append_type_name(e, local->type);
        ct_sb_appendf(e->out, " l%u", i);
        any = true;
    }
    if (any) {
        ct_sb_append_cstr(e->out, ", ");
    }
    ct_sb_append_cstr(e->out, "void *env);\n");
}

/* A capturing lambda's param_count is the Jog arity plus the env pointer.
   Its code pointer is the function, so it must not grow a thunk. */
static int func_is_closure_code(const JirFunc *func) {
    size_t declared;
    if (!func || !func->type || func->type->kind != JOG_TY_FN || func->type->as.fn.is_extern ||
        func->linkage == JIR_LINK_EXTERN) {
        return 0;
    }
    declared = func->type->as.fn.param_count;
    return func->param_count == (uint32_t)declared + 1;
}

static int reloc_needs_thunk(const JirModule *module, const JirReloc *rel) {
    JirFunc *func;
    if (!module || !rel || rel->kind != JIR_RELOC_FUNC || rel->index >= module->funcs.count) {
        return 0;
    }
    func = jir_func(module, rel->index);
    if (!func || !func->type || func->type->kind != JOG_TY_FN || func->type->as.fn.is_extern ||
        func->type->as.fn.variadic || func->linkage == JIR_LINK_EXTERN || func_is_closure_code(func)) {
        return 0;
    }
    return 1;
}

static void emit_reloc_thunk_protos(Emit *e) {
    const JirModule *module = e->module;
    uint8_t *need;
    size_t i;
    size_t r;
    int any = 0;
    if (!module || module->funcs.count == 0) {
        return;
    }
    need = ct_arena_alloc(e->arena, module->funcs.count, 1);
    if (!need) {
        emit_fail(e, "C backend: out of memory");
        return;
    }
    memset(need, 0, module->funcs.count);
    for (i = 0; i < module->globals.count; i++) {
        JirGlobal *global = jir_global(module, (uint32_t)i);
        if (!global) {
            continue;
        }
        for (r = 0; r < global->reloc_count; r++) {
            if (reloc_needs_thunk(module, &global->relocs[r])) {
                need[global->relocs[r].index] = 1;
                any = 1;
            }
        }
    }
    if (!any) {
        return;
    }
    ct_sb_putc(e->out, '\n');
    for (i = 0; i < module->funcs.count; i++) {
        if (need[i]) {
            emit_thunk_proto(e, jir_func(module, (uint32_t)i), (uint32_t)i);
        }
    }
}

static bool emit_ordered(Emit *e, uint8_t *color, bool *any, uint32_t index) {
    JirGlobal *global;
    size_t r;
    if (!e->module || index >= e->module->globals.count) {
        emit_fail(e, "C backend: global relocation target was not emitted");
        return false;
    }
    if (color[index] == 2) {
        return true;
    }
    if (color[index] == 1) {
        emit_fail(e, "C backend: global relocation cycle");
        return false;
    }
    color[index] = 1;
    global = jir_global(e->module, index);
    if (global) {
        for (r = 0; r < global->reloc_count && !e->failed; r++) {
            if (global->relocs[r].kind == JIR_RELOC_GLOBAL &&
                !emit_ordered(e, color, any, global->relocs[r].index)) {
                return false;
            }
        }
    }
    if (e->failed) {
        return false;
    }
    color[index] = 2;
    if (is_vtable_dummy(e, index)) {
        return true;
    }
    if (!global_is_object(global)) {
        return emit_global(e, global, index);
    }
    if (!*any) {
        ct_sb_putc(e->out, '\n');
        *any = true;
    }
    return emit_global(e, global, index);
}

static bool emit_globals(Emit *e) {
    size_t n = e->module ? e->module->globals.count : 0;
    uint8_t *color;
    bool any = false;
    size_t i;
    emit_reloc_thunk_protos(e);
    if (e->failed) {
        return false;
    }
    if (n == 0) {
        return emit_vtables(e);
    }
    color = ct_arena_alloc(e->arena, n, 1);
    if (!color) {
        emit_fail(e, "C backend: out of memory");
        return false;
    }
    memset(color, 0, n);
    for (i = 0; i < n; i++) {
        if (!emit_ordered(e, color, &any, (uint32_t)i)) {
            return false;
        }
    }
    return emit_vtables(e);
}

static void emit_global_uses(Emit *e) {
    for (size_t i = 0; i < e->module->globals.count; i++) {
        JirGlobal *global = jir_global(e->module, (uint32_t)i);
        if (!global_is_object(global) || !c_ident(global->name)) {
            continue;
        }
        ct_sb_appendf(e->out, "    (void)&%s;\n", global->name);
    }
}

static void emit_entry(Emit *e, const char *name, int32_t index) {
    ct_sb_appendf(e->out, "void %s(void) {\n", name);
    if (strcmp(name, "jog_entry_init") == 0) {
        emit_global_uses(e);
    }
    if (index >= 0) {
        JirFunc *func = jir_func(e->module, (uint32_t)index);
        if (!func || !func->name) {
            emit_fail(e, "C backend: missing entry function");
        } else if (kept_func_params(func) != 0 || (func->type && func->type->kind == JOG_TY_FN && func->type->as.fn.variadic)) {
            emit_fail(e, "C backend: entry function must take no parameters");
        } else {
            ct_sb_appendf(e->out, "    %s();\n", func->name);
        }
    }
    ct_sb_append_cstr(e->out, "}\n");
}

static bool sched_extern(const JirFunc *func) {
    static const char *names[] = {
        "jogrt_sched_boot", "jogrt_scope_root", "jogrt_scope_current", "jogrt_scope_enter", "jogrt_scope_leave",
        "jogrt_scope_cancel", "jogrt_scope_join", "jogrt_scope_fail", "jogrt_go", "jogrt_yield", "jogrt_task_set",
        "jogrt_task_join", "jogrt_chan_init", "jogrt_chan_free", "jogrt_chan_close", "jogrt_chan_send",
        "jogrt_chan_recv", "jogrt_select_begin", "jogrt_select_add_recv", "jogrt_select_add_send",
        "jogrt_select_add_default", "jogrt_select_add_timeout", "jogrt_select_wait", "jogrt_select_status",
        "jogrt_sleep", "jogrt_err_cancelled", "jogrt_err_closed", "jogrt_cap_new", "jogrt_cap_set", "jogrt_cap_get",
    };
    size_t i;
    if (!func || func->linkage != JIR_LINK_EXTERN || !func->name) {
        return false;
    }
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(func->name, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool module_needs_fmod(const JirModule *module) {
    size_t fi;
    if (!module) {
        return false;
    }
    for (fi = 0; fi < module->funcs.count; fi++) {
        JirFunc *func = jir_func(module, (uint32_t)fi);
        size_t bi;
        if (!func) {
            continue;
        }
        for (bi = 0; bi < func->blocks.count; bi++) {
            JirBlock *block = jir_block_at(func, (uint32_t)bi);
            size_t ii;
            if (!block) {
                continue;
            }
            for (ii = 0; ii < block->insts.count; ii++) {
                JirInst *inst = ct_da_at(&block->insts, ii);
                if (inst && inst->op == JIR_BINARY && (JirBinOp)inst->aux == JIR_REM && type_is_float(inst->a.type)) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* Truncates the quotient toward zero. The remainder keeps the dividend's sign.
   Generated only when a float remainder exists, so the program does not need libm. */
static void emit_fmod_helpers(Emit *e) {
    ct_sb_append_cstr(e->out,
                      "static double jog_fmod(double a, double b) {\n"
                      "    double q;\n"
                      "    double t;\n"
                      "    if (b == 0) {\n"
                      "        return a;\n"
                      "    }\n"
                      "    q = a / b;\n"
                      "    if (q != q || (q != 0 && q + q == q) || q >= 9223372036854775808.0 || q < -9223372036854775808.0) {\n"
                      "        t = q;\n"
                      "    } else if (q >= 0) {\n"
                      "        t = (double)(long long)q;\n"
                      "    } else {\n"
                      "        t = -(double)(long long)(-q);\n"
                      "    }\n"
                      "    return a - b * t;\n"
                      "}\n"
                      "static float jog_fmodf(float a, float b) {\n"
                      "    float q;\n"
                      "    float t;\n"
                      "    if (b == 0) {\n"
                      "        return a;\n"
                      "    }\n"
                      "    q = a / b;\n"
                      "    if (q != q || (q != 0 && q + q == q) || q >= 9223372036854775808.0f || q < -9223372036854775808.0f) {\n"
                      "        t = q;\n"
                      "    } else if (q >= 0) {\n"
                      "        t = (float)(long long)q;\n"
                      "    } else {\n"
                      "        t = -(float)(long long)(-q);\n"
                      "    }\n"
                      "    return a - b * t;\n"
                      "}\n");
}

/* A value use of a non-extern function. A direct JIR_CALL callee is not one. */
static void mark_func_value(const JirModule *module, uint8_t *need, const JirOperand *op) {
    JirFunc *func;
    if (!module || !need || !op || op->kind != JIR_OPND_FUNC || op->as.func >= module->funcs.count) {
        return;
    }
    func = jir_func(module, op->as.func);
    if (!func || !func->type || func->type->kind != JOG_TY_FN || func->type->as.fn.is_extern ||
        func->type->as.fn.variadic || func->linkage == JIR_LINK_EXTERN || func_is_closure_code(func)) {
        return;
    }
    need[op->as.func] = 1;
}

static void mark_func_inst(const JirModule *module, uint8_t *need, const JirInst *inst) {
    size_t i;
    if (!inst) {
        return;
    }
    if (inst->op != JIR_CALL) {
        mark_func_value(module, need, &inst->a);
    }
    mark_func_value(module, need, &inst->b);
    if (!inst->args) {
        return;
    }
    for (i = 0; i < inst->arg_count; i++) {
        mark_func_value(module, need, &inst->args[i]);
    }
}

/* jog_fnthunk_N(params, void *env) calls the real function and drops env.
   Emitted only for functions that appear as values. */
static void emit_one_fn_thunk(Emit *e, const JirFunc *func, uint32_t index) {
    bool any = false;
    bool ret_void;
    uint32_t i;
    uint32_t limit;
    if (!func || !func->name) {
        emit_fail(e, "C backend: missing function");
        return;
    }
    ret_void = true;
    if (func->type && func->type->kind == JOG_TY_FN && !skip_value(func->type->as.fn.ret)) {
        ret_void = false;
    }
    ct_sb_append_cstr(e->out, "static ");
    append_result_type(e, func);
    ct_sb_appendf(e->out, " jog_fnthunk_%u(", index);
    limit = func->param_count;
    if (limit > func->locals.count) {
        limit = (uint32_t)func->locals.count;
    }
    for (i = 0; i < limit; i++) {
        JirLocal *local = jir_local_at(func, i);
        if (!local || skip_value(local->type)) {
            continue;
        }
        if (any) {
            ct_sb_append_cstr(e->out, ", ");
        }
        append_type_name(e, local->type);
        ct_sb_appendf(e->out, " l%u", i);
        any = true;
    }
    if (any) {
        ct_sb_append_cstr(e->out, ", ");
    }
    ct_sb_append_cstr(e->out, "void *env) {\n    (void)env;\n    ");
    if (!ret_void) {
        ct_sb_append_cstr(e->out, "return ");
    }
    ct_sb_append_cstr(e->out, func->name);
    ct_sb_putc(e->out, '(');
    any = false;
    for (i = 0; i < limit; i++) {
        JirLocal *local = jir_local_at(func, i);
        if (!local || skip_value(local->type)) {
            continue;
        }
        if (any) {
            ct_sb_append_cstr(e->out, ", ");
        }
        ct_sb_appendf(e->out, "l%u", i);
        any = true;
    }
    ct_sb_append_cstr(e->out, ");\n}\n");
}

static void emit_fn_thunks(Emit *e) {
    const JirModule *module = e->module;
    uint8_t *need;
    size_t fi;
    if (!module || module->funcs.count == 0) {
        return;
    }
    need = ct_arena_alloc(e->arena, module->funcs.count, 1);
    if (!need) {
        emit_fail(e, "out of memory");
        return;
    }
    memset(need, 0, module->funcs.count);
    for (fi = 0; fi < module->globals.count; fi++) {
        JirGlobal *global = jir_global(module, (uint32_t)fi);
        size_t r;
        if (!global) {
            continue;
        }
        for (r = 0; r < global->reloc_count; r++) {
            if (reloc_needs_thunk(module, &global->relocs[r])) {
                need[global->relocs[r].index] = 1;
            }
        }
    }
    for (fi = 0; fi < module->funcs.count; fi++) {
        JirFunc *func = jir_func(module, (uint32_t)fi);
        size_t bi;
        if (!func) {
            continue;
        }
        for (bi = 0; bi < func->blocks.count; bi++) {
            JirBlock *block = jir_block_at(func, (uint32_t)bi);
            size_t ii;
            if (!block) {
                continue;
            }
            for (ii = 0; ii < block->insts.count; ii++) {
                mark_func_inst(module, need, ct_da_at(&block->insts, ii));
            }
            if (block->term.kind == JIR_TERM_RET || block->term.kind == JIR_TERM_COND_BR ||
                block->term.kind == JIR_TERM_SWITCH) {
                mark_func_value(module, need, &block->term.value);
            }
        }
    }
    for (fi = 0; fi < module->funcs.count; fi++) {
        if (!need[fi]) {
            continue;
        }
        ct_sb_putc(e->out, '\n');
        emit_one_fn_thunk(e, jir_func(module, (uint32_t)fi), (uint32_t)fi);
    }
}

static bool emit_functions(Emit *e) {
    note_strings(e);
    if (e->failed) {
        return false;
    }
    const JirModule *module = e->module;
    if (module->funcs.count > 0) {
        ct_sb_putc(e->out, '\n');
        for (size_t i = 0; i < module->funcs.count; i++) {
            JirFunc *func = jir_func(module, (uint32_t)i);
            if (!func) {
                emit_fail(e, "C backend: missing function");
                return false;
            }
            if (sched_extern(func)) {
                continue;
            }
            emit_signature(e, func, true);
            ct_sb_append_cstr(e->out, ";\n");
        }
    }
    if (e->strings.count > 0) {
        ct_sb_putc(e->out, '\n');
        emit_string_data(e);
    }
    if (!emit_globals(e)) {
        return false;
    }
    if (module_needs_fmod(module)) {
        ct_sb_putc(e->out, '\n');
        emit_fmod_helpers(e);
    }
    emit_fn_thunks(e);
    if (e->failed) {
        return false;
    }
    if (module->funcs.count > 0) {
        ct_sb_putc(e->out, '\n');
        bool any_body = false;
        for (size_t i = 0; i < module->funcs.count; i++) {
            JirFunc *func = jir_func(module, (uint32_t)i);
            if (!func || func->linkage == JIR_LINK_EXTERN) {
                continue;
            }
            if (any_body) {
                ct_sb_putc(e->out, '\n');
            }
            emit_body(e, func);
            any_body = true;
            if (e->failed) {
                return false;
            }
        }
    }
    ct_sb_putc(e->out, '\n');
    emit_entry(e, "jog_entry_init", module->init_func);
    ct_sb_putc(e->out, '\n');
    emit_entry(e, "jog_entry_main", module->main_func);
    return !e->failed;
}

static void emit_prelude(CtSb *out) {
    ct_sb_append_cstr(out, "#include <stdint.h>\n");
    ct_sb_append_cstr(out, "#include <stddef.h>\n");
    ct_sb_append_cstr(out, "#include <stdbool.h>\n");
    ct_sb_append_cstr(out, "#include \"jogrt.h\"\n");
}

static void emit_close(Emit *e) {
    ct_da_free(&e->types);
    ct_da_free(&e->id_index);
    ct_da_free(&e->strings);
    ct_sb_free(&e->scratch);
    ct_arena_destroy(e->arena);
}

bool jog_emit_c(const JirModule *module, JogBuildMode mode, CtSb *out, CtDiag *diag) {
    if (!out) {
        CtPos pos = {0};
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "C backend: no output");
        return false;
    }
    if (!module) {
        CtPos pos = {0};
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "C backend: no module");
        return false;
    }

    Emit emit;
    memset(&emit, 0, sizeof emit);
    emit.module = module;
    emit.out = out;
    emit.diag = diag;
    emit.mode = mode;
    emit.arena = ct_arena_create();
    ct_sb_init(&emit.scratch);
    ct_da_init(&emit.types, sizeof(JogType *));
    ct_da_init(&emit.id_index, sizeof(int32_t));
    ct_da_init(&emit.strings, sizeof(EmitStr));
    if (!emit.arena) {
        emit_fail(&emit, "C backend: out of memory");
        emit_close(&emit);
        return false;
    }

    emit_prelude(out);
    if (!reach_module(&emit) || !assign_names(&emit) || !order_defs(&emit)) {
        emit_close(&emit);
        return false;
    }

    bool forwarded = false;
    bool any_def = emit.order_n > 0;
    bool any_forward = false;
    for (size_t id = 0; id < emit.id_index.count && !any_forward; id++) {
        int32_t index = *(int32_t *)ct_da_at(&emit.id_index, id);
        if (index < 0) {
            continue;
        }
        JogType *type = type_at(&emit, index);
        any_forward = type_is_struct_form(type) || type_is_incomplete(type);
    }
    if (any_forward || any_def) {
        ct_sb_putc(out, '\n');
    }
    forwarded = emit_forwards(&emit);
    if (emit.failed) {
        emit_close(&emit);
        return false;
    }
    if (forwarded && any_def) {
        ct_sb_putc(out, '\n');
    }
    for (size_t i = 0; i < emit.order_n; i++) {
        if (i > 0) {
            ct_sb_putc(out, '\n');
        }
        emit_def(&emit, type_at(&emit, emit.order[i]));
        if (emit.failed) {
            emit_close(&emit);
            return false;
        }
    }
    if (!emit_functions(&emit)) {
        emit_close(&emit);
        return false;
    }
    bool ok = !emit.failed && !out->failed;
    emit_close(&emit);
    return ok;
}
