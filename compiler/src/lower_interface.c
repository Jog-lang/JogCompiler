#include "lower_interface.h"

#include "check_constraint.h"
#include "check_struct.h"
#include "iface_table.h"
#include "lower_context.h"
#include "lower_error.h"
#include "lower_generic.h"

#include "intern.h"

#include <stdio.h>
#include <string.h>

/* A method table is one static per (concrete type, interface): a type id,
   size, align, then one thunk per direct method, then one size_t per
   property. The type id is the address of a unique static byte. Thunks are
   internal functions with an extern function type so the slot is one C
   function pointer. Property offsets follow the thunks so method indexes
   stay 3 + slot. */

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static const char *intern_buf(Lower *L, const char *buf) {
    const char *name = ct_intern_cstr(L->program->intern, buf);
    if (!name) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
    }
    return name;
}

static JogType *universe_void(Lower *L) {
    const char *name;
    JogPackageScope *scope;
    JogSymbol *sym;
    if (!L->program->root || !L->program->root->scope) {
        return NULL;
    }
    scope = L->program->root->scope;
    if (!scope->universe) {
        return NULL;
    }
    name = ct_intern_cstr(L->program->intern, "void");
    if (!name) {
        return NULL;
    }
    sym = jog_package_scope_lookup(scope->universe, name);
    return sym ? sym->type : NULL;
}

static JogType *void_ptr(Lower *L) {
    JogType *v = universe_void(L);
    if (!v) {
        jog_lower_fail(L, (JogPos){0}, "missing void");
        return NULL;
    }
    return jog_type_ptr(L->program->types, v, false);
}

static void term_now(Lower *L, JirTerm term) {
    if (L->failed || L->terminated) {
        return;
    }
    jir_terminate(L->func, L->block, term);
    L->terminated = true;
}

static void term_ret(Lower *L, JirOperand value, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_RET;
    term.value = value;
    term.pos = pos;
    term_now(L, term);
}

static void term_unreachable(Lower *L, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_UNREACHABLE;
    term.pos = pos;
    term_now(L, term);
}

static void term_cond(Lower *L, JirOperand cond, uint32_t yes, uint32_t no, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_COND_BR;
    term.value = cond;
    term.target = yes;
    term.else_target = no;
    term.pos = pos;
    term_now(L, term);
}

static void use_block(Lower *L, uint32_t block) {
    L->block = block;
    L->terminated = false;
}

static JirOperand emit_unary(Lower *L, JirOp op, JirOperand src, JogType *type, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (L->failed) {
        return jir_none();
    }
    if (op == JIR_BITCAST && src.type == type) {
        return src;
    }
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, op, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = src;
    return jog_lower_local_op(L, dst);
}

static JirOperand emit_extract(Lower *L, JirOperand src, uint32_t aux, JogType *field, JogPos pos) {
    JirOperand local;
    uint32_t dst;
    JirInst *inst;
    if (L->failed) {
        return jir_none();
    }
    local = jog_lower_as_local(L, src, pos);
    if (L->failed) {
        return jir_none();
    }
    if (!jog_lower_new_local(L, NULL, field, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_EXTRACT, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = local;
    inst->aux = aux;
    return jog_lower_local_op(L, dst);
}

static JirOperand emit_eq(Lower *L, JirOperand lhs, JirOperand rhs, JogPos pos) {
    JogType *boolean = jog_type_builtin(L->program->types, JOG_BUILTIN_BOOL);
    uint32_t dst;
    JirInst *inst;
    if (!boolean) {
        jog_lower_fail(L, pos, "out of memory");
        return jir_none();
    }
    if (!jog_lower_new_local(L, NULL, boolean, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_BINARY, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->aux = JIR_EQ;
    inst->a = lhs;
    inst->b = rhs;
    return jog_lower_local_op(L, dst);
}

static int32_t find_global_name(Lower *L, const char *name) {
    size_t i;
    if (!name) {
        return -1;
    }
    for (i = 0; i < L->module->globals.count; i++) {
        JirGlobal *global = jir_global(L->module, (uint32_t)i);
        if (global && global->name && strcmp(global->name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t ensure_tid(Lower *L, JogType *concrete, const char **out_name) {
    const char *name;
    int32_t index;
    JogType *u8;
    if (out_name) {
        *out_name = NULL;
    }
    if (!concrete) {
        jog_lower_fail(L, (JogPos){0}, "missing type");
        return -1;
    }
    {
        char buf[80];
        snprintf(buf, sizeof(buf), "jog_tid_%u", concrete->id);
        name = intern_buf(L, buf);
    }
    if (!name) {
        return -1;
    }
    if (out_name) {
        *out_name = name;
    }
    index = find_global_name(L, name);
    if (index >= 0) {
        return index;
    }
    u8 = jog_type_builtin(L->program->types, JOG_BUILTIN_U8);
    if (!u8) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
        return -1;
    }
    index = jir_global_add(L->module, name, u8, true);
    if (index < 0) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
        return -1;
    }
    return index;
}

static int same_pkg(const JogType *type, const JogSymbol *sym) {
    const char *want = type && type->as.nominal.package ? type->as.nominal.package : "";
    const char *have = sym && sym->package && sym->package->path ? sym->package->path : "";
    return strcmp(want, have) == 0;
}

static JogNode *find_method(Lower *L, JogType *concrete, JogNode *req) {
    size_t p;
    if (!concrete || !req || !req->as.fn.name || !L->program) {
        return NULL;
    }
    for (p = 0; p < L->program->package_count; p++) {
        JogPackage *pkg = L->program->packages[p];
        size_t f;
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count; f++) {
            JogSourceFile *source = pkg->files ? pkg->files[f] : NULL;
            JogNode *file;
            size_t d;
            if (!source || !source->ast || source->ast->kind != JOG_NODE_FILE) {
                continue;
            }
            file = source->ast;
            for (d = 0; d < file->as.file.decls.count; d++) {
                JogNode *fn = list_at(file->as.file.decls, d);
                JogType *self;
                JogType *on;
                int is_ptr;
                if (!fn || fn->kind != JOG_NODE_FN || !fn->symbol || !fn->as.fn.name || !fn->as.fn.receiver) {
                    continue;
                }
                if (strcmp(fn->as.fn.name, req->as.fn.name) != 0 || !same_pkg(concrete, fn->symbol)) {
                    continue;
                }
                if (fn->symbol->type_param_count > 0 || !fn->type || fn->type->kind != JOG_TY_FN) {
                    continue;
                }
                if (!fn->type->as.fn.params || fn->type->as.fn.param_count == 0) {
                    continue;
                }
                self = fn->type->as.fn.params[0];
                is_ptr = self && self->kind == JOG_TY_PTR;
                on = is_ptr && self->as.elem.elem ? self->as.elem.elem : self;
                if (on != concrete) {
                    continue;
                }
                if ((is_ptr ? 1 : 0) != (req->as.fn.receiver_is_ptr ? 1 : 0)) {
                    continue;
                }
                return fn;
            }
        }
    }
    return NULL;
}

static JogType *thunk_type(Lower *L, JogType *method, JogType *data) {
    size_t n;
    size_t i;
    JogType **params;
    JogType *ret;
    if (!method || method->kind != JOG_TY_FN || method->as.fn.param_count == 0) {
        return NULL;
    }
    n = method->as.fn.param_count + method->as.fn.context_count;
    params = ct_arena_alloc(L->program->arena, n * sizeof(*params), _Alignof(JogType *));
    if (!params) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
        return NULL;
    }
    params[0] = data;
    for (i = 1; i < method->as.fn.param_count; i++) {
        params[i] = method->as.fn.params ? method->as.fn.params[i] : NULL;
    }
    for (i = 0; i < method->as.fn.context_count; i++) {
        params[method->as.fn.param_count + i] = method->as.fn.contexts ? method->as.fn.contexts[i] : NULL;
    }
    ret = method->as.fn.ret;
    return jog_type_fn(L->program->types, params, n, ret, NULL, 0, false, false, true);
}

static int32_t build_thunk(Lower *L, JogNode *method, JogType *fn_ty, const char *name, JogPos pos) {
    JirFunc *saved_func;
    JogNode *saved_ast;
    uint32_t saved_block;
    int saved_term;
    int32_t index;
    JirFunc *thunk;
    uint32_t entry;
    size_t before;
    JogType *self_ty;
    int is_ptr;
    JirOperand data;
    JirOperand self;
    JirOperand *args;
    JirInst *call;
    size_t n;
    size_t i;
    int32_t method_index;
    JogType *ret;
    int produces;
    uint32_t dst = 0;
    saved_func = L->func;
    saved_ast = L->ast_fn;
    saved_block = L->block;
    saved_term = L->terminated ? 1 : 0;
    index = jir_func_add(L->module, name, name, JIR_LINK_INTERNAL, fn_ty, NULL);
    if (index < 0) {
        jog_lower_fail(L, pos, "out of memory");
        return -1;
    }
    thunk = jir_func(L->module, (uint32_t)index);
    if (!thunk) {
        jog_lower_fail(L, pos, "out of memory");
        return -1;
    }
    thunk->pos = pos;
    before = thunk->blocks.count;
    entry = jir_block_add(thunk, "entry");
    if (thunk->blocks.count == before) {
        jog_lower_fail(L, pos, "out of memory");
        return -1;
    }
    L->func = thunk;
    L->ast_fn = NULL;
    L->block = entry;
    L->terminated = false;
    method_index = jog_lower_find_fn(L, method);
    if (method_index < 0) {
        jog_lower_fail(L, pos, "missing lowered function '%s'", method->as.fn.name ? method->as.fn.name : "?");
        goto restore;
    }
    self_ty = method->type->as.fn.params[0];
    is_ptr = self_ty && self_ty->kind == JOG_TY_PTR;
    data = jog_lower_local_op(L, 0);
    if (is_ptr) {
        self = emit_unary(L, JIR_BITCAST, data, self_ty, pos);
    } else {
        JogType *ptr_ty = self_ty ? jog_type_ptr(L->program->types, self_ty, false) : NULL;
        JirOperand ptr;
        if (!ptr_ty) {
            jog_lower_fail(L, pos, "out of memory");
            goto restore;
        }
        ptr = emit_unary(L, JIR_BITCAST, data, ptr_ty, pos);
        self = emit_unary(L, JIR_LOAD, ptr, self_ty, pos);
    }
    n = fn_ty->as.fn.param_count;
    args = NULL;
    if (n > 0) {
        args = ct_arena_alloc(L->program->arena, n * sizeof(*args), _Alignof(JirOperand));
        if (!args) {
            jog_lower_fail(L, pos, "out of memory");
            goto restore;
        }
        args[0] = self;
        for (i = 1; i < n; i++) {
            args[i] = jog_lower_local_op(L, (uint32_t)i);
        }
    }
    ret = fn_ty->as.fn.ret;
    produces = ret && ret->kind != JOG_TY_UNIT && ret->kind != JOG_TY_ERROR;
    if (produces) {
        if (!jog_lower_new_local(L, NULL, ret, &dst)) {
            goto restore;
        }
    }
    if (!L->failed) {
        call = jog_lower_emit(L, JIR_CALL, produces ? (int32_t)dst : -1, pos);
        if (call) {
            call->a = jir_func_ref(L->module, (uint32_t)method_index);
            if (n > 0 && !jir_set_args(L->module, call, args, n)) {
                jog_lower_fail(L, pos, "out of memory");
            }
        }
    }
    if (!L->failed && !L->terminated) {
        if (produces) {
            term_ret(L, jog_lower_local_op(L, dst), pos);
        } else {
            term_ret(L, jir_none(), pos);
        }
    }
restore:
    L->func = saved_func;
    L->ast_fn = saved_ast;
    L->block = saved_block;
    L->terminated = saved_term ? true : false;
    return L->failed ? -1 : index;
}

static JirIfaceTable *find_table(Lower *L, JogType *concrete, JogType *iface) {
    size_t i;
    for (i = 0; i < L->module->iface_table_count; i++) {
        JirIfaceTable *row = &L->module->iface_tables[i];
        if (row->concrete == concrete && row->iface == iface) {
            return row;
        }
    }
    return NULL;
}

static JirIfaceTable *push_table(Lower *L, JirIfaceTable row) {
    JirModule *module = L->module;
    JirIfaceTable *grown;
    grown = ct_arena_alloc(module->arena, (module->iface_table_count + 1) * sizeof(*grown), _Alignof(JirIfaceTable));
    if (!grown) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
        return NULL;
    }
    if (module->iface_table_count > 0 && module->iface_tables) {
        memcpy(grown, module->iface_tables, module->iface_table_count * sizeof(*grown));
    }
    grown[module->iface_table_count] = row;
    module->iface_tables = grown;
    module->iface_table_count++;
    return &module->iface_tables[module->iface_table_count - 1];
}

static int field_byte_offset(Lower *L, JogType *concrete, const char *name, uint64_t *out) {
    JogStructPath hit;
    JogType *cur;
    uint64_t off;
    uint8_t i;
    const char *file;
    JogPackage *pkg;
    if (!L || !concrete || !name || !out) {
        return 0;
    }
    if (!jog_type_layout(L->program->types, concrete)) {
        return 0;
    }
    file = L->ast_fn ? L->ast_fn->pos.file : NULL;
    pkg = L->ast_fn && L->ast_fn->symbol ? L->ast_fn->symbol->package : NULL;
    memset(&hit, 0, sizeof(hit));
    jog_struct_lookup_field(concrete, name, L->ast_fn, pkg, file, &hit);
    if (!hit.found || hit.ambiguous || hit.hidden || !hit.field || hit.len == 0) {
        return 0;
    }
    cur = concrete;
    off = 0;
    for (i = 0; i < hit.len; i++) {
        JogField *field;
        if (!cur || cur->kind != JOG_TY_STRUCT || !cur->as.nominal.fields ||
            hit.index[i] >= cur->as.nominal.field_count) {
            return 0;
        }
        field = &cur->as.nominal.fields[hit.index[i]];
        off += field->offset;
        cur = field->type;
    }
    *out = off;
    return 1;
}

/* Direct method slots, then properties in declaration order, including
   properties of an embedded interface. Matches the C object layout. */
static int walk_props(Lower *L, JogType *iface, JogType *concrete, uint64_t *offs, int *count, int depth) {
    JogNode *decl;
    size_t i;
    if (!iface || !iface->as.nominal.decl || depth > 16) {
        return 1;
    }
    decl = iface->as.nominal.decl;
    if (decl->kind != JOG_NODE_INTERFACE) {
        return 1;
    }
    for (i = 0; i < decl->as.aggregate.members.count && !L->failed; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        JogType *emb;
        if (!member) {
            continue;
        }
        if (member->kind == JOG_NODE_FIELD && member->as.field.name) {
            if (offs) {
                uint64_t off = 0;
                if (!field_byte_offset(L, concrete, member->as.field.name, &off)) {
                    jog_lower_fail(L, member->pos, "missing property '%s'", member->as.field.name);
                    return 0;
                }
                offs[*count] = off;
            }
            (*count)++;
            continue;
        }
        if (member->kind != JOG_NODE_EMBED || !member->as.embed.type) {
            continue;
        }
        emb = member->as.embed.type->type;
        if (!emb || emb->kind != JOG_TY_INTERFACE || jog_constraint_is_typeset(emb)) {
            continue;
        }
        if (!walk_props(L, emb, concrete, offs, count, depth + 1)) {
            return 0;
        }
    }
    return !L->failed;
}

static JirIfaceTable *ensure_table(Lower *L, JogType *concrete, JogType *iface, JogPos pos) {
    JirIfaceTable *found;
    JirIfaceTable row;
    JogNode *decl;
    JogType *data;
    const char *tid_name = NULL;
    int32_t tid;
    size_t nreq = 0;
    size_t i;
    size_t slot;
    found = find_table(L, concrete, iface);
    if (found) {
        return found;
    }
    if (!iface || !iface->as.nominal.decl || iface->as.nominal.decl->kind != JOG_NODE_INTERFACE) {
        jog_lower_fail(L, pos, "missing interface");
        return NULL;
    }
    decl = iface->as.nominal.decl;
    for (i = 0; i < decl->as.aggregate.members.count; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        if (member && member->kind == JOG_NODE_FN && member->type && member->type->kind == JOG_TY_FN) {
            nreq++;
        }
    }
    data = void_ptr(L);
    if (!data) {
        return NULL;
    }
    tid = ensure_tid(L, concrete, &tid_name);
    if (tid < 0) {
        return NULL;
    }
    memset(&row, 0, sizeof(row));
    row.type_id_name = tid_name;
    row.type_id_global = tid;
    row.concrete = concrete;
    row.iface = iface;
    row.slot_count = nreq;
    row.global = -1;
    if (nreq > 0) {
        row.slots = ct_arena_alloc(L->program->arena, nreq * sizeof(*row.slots), _Alignof(JirIfaceSlot));
        if (!row.slots) {
            jog_lower_fail(L, pos, "out of memory");
            return NULL;
        }
        memset(row.slots, 0, nreq * sizeof(*row.slots));
    }
    slot = 0;
    for (i = 0; i < decl->as.aggregate.members.count && !L->failed; i++) {
        JogNode *req = list_at(decl->as.aggregate.members, i);
        JogNode *method;
        JogType *fn_ty;
        const char *name;
        int32_t index;
        if (!req || req->kind != JOG_NODE_FN || !req->type || req->type->kind != JOG_TY_FN) {
            continue;
        }
        method = find_method(L, concrete, req);
        if (!method) {
            jog_lower_fail(L, pos, "missing method '%s'", req->as.fn.name ? req->as.fn.name : "?");
            return NULL;
        }
        fn_ty = thunk_type(L, method->type, data);
        if (!fn_ty) {
            jog_lower_fail(L, pos, "missing method '%s'", req->as.fn.name ? req->as.fn.name : "?");
            return NULL;
        }
        {
            char buf[80];
            snprintf(buf, sizeof(buf), "jog_ifn_%u_%u_%u", iface->id, concrete->id, (unsigned)slot);
            name = intern_buf(L, buf);
        }
        if (!name) {
            return NULL;
        }
        index = build_thunk(L, method, fn_ty, name, pos);
        if (index < 0) {
            return NULL;
        }
        row.slots[slot].func = index;
        row.slots[slot].fn_type = fn_ty;
        slot++;
    }
    if (L->failed) {
        return NULL;
    }
    {
        int nprop = 0;
        if (!walk_props(L, iface, concrete, NULL, &nprop, 0)) {
            return NULL;
        }
        row.offset_count = (size_t)nprop;
        if (nprop > 0) {
            int cursor = 0;
            row.offsets = ct_arena_alloc(L->program->arena, (size_t)nprop * sizeof(*row.offsets), _Alignof(uint64_t));
            if (!row.offsets) {
                jog_lower_fail(L, pos, "out of memory");
                return NULL;
            }
            memset(row.offsets, 0, (size_t)nprop * sizeof(*row.offsets));
            if (!walk_props(L, iface, concrete, row.offsets, &cursor, 0)) {
                return NULL;
            }
        }
    }
    if (L->failed) {
        return NULL;
    }
    {
        char buf[80];
        snprintf(buf, sizeof(buf), "jog_vt_%u_%u", iface->id, concrete->id);
        row.name = intern_buf(L, buf);
    }
    if (!row.name) {
        return NULL;
    }
    {
        JogType *u8 = jog_type_builtin(L->program->types, JOG_BUILTIN_U8);
        if (!u8) {
            jog_lower_fail(L, pos, "out of memory");
            return NULL;
        }
        row.global = jir_global_add(L->module, row.name, u8, true);
        if (row.global < 0) {
            jog_lower_fail(L, pos, "out of memory");
            return NULL;
        }
    }
    return push_table(L, row);
}

static JirOperand aggregate2(Lower *L, JogType *type, JirOperand a, JirOperand b, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    JirOperand args[2];
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_AGGREGATE, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    args[0] = a;
    args[1] = b;
    if (!jir_set_args(L->module, inst, args, 2)) {
        jog_lower_fail(L, pos, "out of memory");
        return jir_none();
    }
    return jog_lower_local_op(L, dst);
}

JirOperand jog_lower_iface_convert(Lower *L, JirOperand src, JogType *iface, JogPos pos) {
    JogType *concrete;
    JogType *data;
    JirIfaceTable *table;
    JirOperand ptr;
    JirOperand vt;
    iface = jog_lower_type(L, iface);
    if (L->failed || !iface || iface->kind != JOG_TY_INTERFACE) {
        return jir_none();
    }
    if (!src.type || src.type->kind != JOG_TY_PTR || !src.type->as.elem.elem) {
        jog_lower_fail(L, pos, "cannot convert to an interface");
        return jir_none();
    }
    concrete = src.type->as.elem.elem;
    table = ensure_table(L, concrete, iface, pos);
    data = void_ptr(L);
    if (!table || !data || L->failed) {
        return jir_none();
    }
    ptr = emit_unary(L, JIR_BITCAST, src, data, pos);
    vt = emit_unary(L, JIR_BITCAST, jir_global_ref(L->module, (uint32_t)table->global), data, pos);
    if (L->failed) {
        return jir_none();
    }
    return aggregate2(L, iface, ptr, vt, pos);
}

static JogType *call_fn_type(Lower *L, JogNode *req, JogType *data) {
    JogType *fn = req->type;
    size_t n;
    size_t i;
    JogType **params;
    if (!fn || fn->kind != JOG_TY_FN) {
        return NULL;
    }
    n = 1 + fn->as.fn.param_count + fn->as.fn.context_count;
    params = ct_arena_alloc(L->program->arena, n * sizeof(*params), _Alignof(JogType *));
    if (!params) {
        jog_lower_fail(L, req->pos, "out of memory");
        return NULL;
    }
    params[0] = data;
    for (i = 0; i < fn->as.fn.param_count; i++) {
        params[1 + i] = fn->as.fn.params ? fn->as.fn.params[i] : NULL;
    }
    for (i = 0; i < fn->as.fn.context_count; i++) {
        params[1 + fn->as.fn.param_count + i] = fn->as.fn.contexts ? fn->as.fn.contexts[i] : NULL;
    }
    return jog_type_fn(L->program->types, params, n, fn->as.fn.ret, NULL, 0, false, false, true);
}

static int slot_of(JogType *iface, JogNode *req) {
    JogNode *decl;
    size_t i;
    int slot = 0;
    if (!iface || !req || !iface->as.nominal.decl) {
        return -1;
    }
    decl = iface->as.nominal.decl;
    for (i = 0; i < decl->as.aggregate.members.count; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        if (!member || member->kind != JOG_NODE_FN || !member->type || member->type->kind != JOG_TY_FN) {
            continue;
        }
        if (member == req) {
            return slot;
        }
        slot++;
    }
    return -1;
}

static int fill_user_args(Lower *L, JogNode *node, JogNode *req, JogType *fn, JirOperand *args) {
    size_t n = fn->as.fn.param_count;
    size_t i;
    size_t positional = 0;
    int seen_named = 0;
    unsigned char *filled;
    filled = NULL;
    if (n > 0) {
        filled = ct_arena_alloc(L->program->arena, n, 1);
        if (!filled) {
            jog_lower_fail(L, node->pos, "out of memory");
            return 0;
        }
        memset(filled, 0, n);
    }
    for (i = 0; i < node->as.call.args.count && !L->failed && !L->terminated; i++) {
        JogNode *arg = list_at(node->as.call.args, i);
        JogNode *value;
        int index;
        if (!arg || arg->kind != JOG_NODE_ARG) {
            jog_lower_fail(L, node->pos, "expected an argument");
            return 0;
        }
        value = arg->as.arg.value;
        if (arg->as.arg.name) {
            size_t p;
            seen_named = 1;
            index = -1;
            for (p = 0; p < n; p++) {
                JogNode *param = list_at(req->as.fn.params, p);
                if (param && param->kind == JOG_NODE_PARAM && param->as.param.name &&
                    strcmp(param->as.param.name, arg->as.arg.name) == 0) {
                    index = (int)p;
                    break;
                }
            }
            if (index < 0) {
                jog_lower_fail(L, arg->pos, "unknown parameter '%s'", arg->as.arg.name);
                return 0;
            }
        } else {
            if (seen_named || positional >= n) {
                jog_lower_fail(L, arg->pos, "too many arguments");
                return 0;
            }
            index = (int)positional;
            positional++;
        }
        if (filled[index]) {
            jog_lower_fail(L, arg->pos, "duplicate argument");
            return 0;
        }
        filled[index] = 1;
        args[index] = jog_lower_expr(L, value);
    }
    if (node->as.call.trailing && !L->failed && !L->terminated) {
        JogNode *lam = node->as.call.trailing;
        JogType *want;
        JogType *inner;
        if (n == 0 || !fn->as.fn.params) {
            jog_lower_fail(L, lam->pos, "trailing lambda needs a function parameter");
            return 0;
        }
        if (filled && filled[n - 1]) {
            jog_lower_fail(L, lam->pos, "trailing lambda does not fit");
            return 0;
        }
        want = fn->as.fn.params[n - 1];
        inner = want ? jog_type_underlying(want) : NULL;
        if (!inner || inner->kind != JOG_TY_FN) {
            jog_lower_fail(L, lam->pos, "trailing lambda needs a function parameter");
            return 0;
        }
        if (filled) {
            filled[n - 1] = 1;
        }
        args[n - 1] = jog_lower_expr(L, lam);
    }
    for (i = 0; i < n && !L->failed; i++) {
        JogNode *param;
        if (filled && filled[i]) {
            continue;
        }
        param = list_at(req->as.fn.params, i);
        if (!param || param->kind != JOG_NODE_PARAM || !param->as.param.default_value) {
            jog_lower_fail(L, node->pos, "missing argument");
            return 0;
        }
        args[i] = jog_lower_expr(L, param->as.param.default_value);
    }
    return !L->failed && !L->terminated;
}

bool jog_lower_iface_call(Lower *L, JogNode *node, JirOperand *out) {
    JogNode *callee;
    JogNode *base;
    JogNode *req;
    JogType *base_ty;
    JogType *iface;
    int needs_load = 0;
    JirOperand recv;
    JirOperand data_op;
    JirOperand vt;
    JogType *data;
    JogType *fn_ty;
    int slot;
    JogType *slot_ptr_ty;
    JogType *usize;
    JirOperand table;
    JirOperand slot_ptr;
    JirOperand fn_ptr;
    JirOperand *args;
    size_t nparam;
    size_t nctx;
    size_t total;
    JirInst *call;
    JogType *ret;
    int produces;
    uint32_t dst = 0;
    JirInst *inst;
    if (out) {
        *out = jir_none();
    }
    if (!L || !node || !out || node->kind != JOG_NODE_CALL || L->failed) {
        return false;
    }
    callee = node->as.call.callee;
    if (!callee || callee->kind != JOG_NODE_MEMBER || !callee->symbol || callee->symbol->kind != JOG_SYM_METHOD) {
        return false;
    }
    req = callee->symbol->decl;
    if (!req || req->kind != JOG_NODE_FN || req->as.fn.receiver) {
        return false;
    }
    base = callee->as.member.base;
    base_ty = base && base->type ? jog_lower_type(L, base->type) : NULL;
    if (!base_ty) {
        return false;
    }
    if (base_ty->kind == JOG_TY_INTERFACE) {
        iface = base_ty;
    } else if (base_ty->kind == JOG_TY_PTR && base_ty->as.elem.elem && base_ty->as.elem.elem->kind == JOG_TY_INTERFACE) {
        iface = base_ty->as.elem.elem;
        needs_load = 1;
    } else {
        return false;
    }
    recv = jog_lower_expr(L, base);
    if (L->failed) {
        return true;
    }
    if (needs_load) {
        recv = emit_unary(L, JIR_LOAD, recv, iface, node->pos);
    }
    data = void_ptr(L);
    if (!data || L->failed) {
        return true;
    }
    data_op = emit_extract(L, recv, 0, data, node->pos);
    vt = emit_extract(L, recv, 1, data, node->pos);
    fn_ty = call_fn_type(L, req, data);
    slot = slot_of(iface, req);
    if (!fn_ty || slot < 0 || L->failed) {
        jog_lower_fail(L, node->pos, "missing method '%s'", req->as.fn.name ? req->as.fn.name : "?");
        return true;
    }
    slot_ptr_ty = jog_type_ptr(L->program->types, fn_ty, false);
    usize = jog_type_builtin(L->program->types, JOG_BUILTIN_USIZE);
    if (!slot_ptr_ty || !usize) {
        jog_lower_fail(L, node->pos, "out of memory");
        return true;
    }
    table = emit_unary(L, JIR_BITCAST, vt, slot_ptr_ty, node->pos);
    if (!jog_lower_new_local(L, NULL, slot_ptr_ty, &dst)) {
        return true;
    }
    inst = jog_lower_emit(L, JIR_ELEM_ADDR, (int32_t)dst, node->pos);
    if (!inst) {
        return true;
    }
    inst->a = table;
    inst->b = jir_int(usize, (uint64_t)(3 + slot));
    slot_ptr = jog_lower_local_op(L, dst);
    fn_ptr = emit_unary(L, JIR_LOAD, slot_ptr, fn_ty, node->pos);
    nparam = req->type->as.fn.param_count;
    nctx = node->context_arg_count;
    total = 1 + nparam + nctx;
    args = ct_arena_alloc(L->program->arena, total * sizeof(*args), _Alignof(JirOperand));
    if (!args) {
        jog_lower_fail(L, node->pos, "out of memory");
        return true;
    }
    memset(args, 0, total * sizeof(*args));
    args[0] = data_op;
    if ((nparam > 0 || node->as.call.args.count > 0) && !fill_user_args(L, node, req, req->type, args + 1)) {
        return true;
    }
    if (nctx > 0 && !jog_lower_context_args(L, node, args, 1 + nparam)) {
        return true;
    }
    if (L->failed || L->terminated) {
        return true;
    }
    ret = fn_ty->as.fn.ret;
    produces = ret && ret->kind != JOG_TY_UNIT && ret->kind != JOG_TY_ERROR;
    dst = 0;
    if (produces && !jog_lower_new_local(L, NULL, ret, &dst)) {
        return true;
    }
    call = jog_lower_emit(L, JIR_CALL, produces ? (int32_t)dst : -1, node->pos);
    if (!call) {
        return true;
    }
    call->a = fn_ptr;
    if (!jir_set_args(L->module, call, args, total)) {
        jog_lower_fail(L, node->pos, "out of memory");
        return true;
    }
    if (produces) {
        *out = jog_lower_local_op(L, dst);
    }
    return true;
}

static JirOperand load_type_id(Lower *L, JirOperand iface_val, JogPos pos) {
    JogType *data = void_ptr(L);
    JogType *data_ptr;
    JirOperand vt;
    JirOperand table;
    if (!data) {
        return jir_none();
    }
    data_ptr = jog_type_ptr(L->program->types, data, false);
    if (!data_ptr) {
        jog_lower_fail(L, pos, "out of memory");
        return jir_none();
    }
    vt = emit_extract(L, iface_val, 1, data, pos);
    table = emit_unary(L, JIR_BITCAST, vt, data_ptr, pos);
    return emit_unary(L, JIR_LOAD, table, data, pos);
}

static int add_block(Lower *L, uint32_t *out) {
    size_t before = L->func->blocks.count;
    *out = jir_block_add(L->func, NULL);
    if (L->func->blocks.count == before) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
        return 0;
    }
    return 1;
}

bool jog_lower_iface_cast(Lower *L, JogNode *node, JirOperand *out) {
    JogNode *expr;
    JogType *dest;
    JogType *src_ty;
    JirOperand src;
    if (out) {
        *out = jir_none();
    }
    if (!L || !node || !out || node->kind != JOG_NODE_CAST || L->failed) {
        return false;
    }
    expr = node->as.cast.expr;
    dest = node->type ? jog_lower_type(L, node->type) : NULL;
    src_ty = expr && expr->type ? jog_lower_type(L, expr->type) : NULL;
    if (node->as.cast.op != JOG_TOK_KW_AS) {
        JogType *elem;
        JogType *data;
        JirOperand have;
        JirOperand want;
        JirOperand cmp;
        JirOperand ptr;
        const char *tid_name = NULL;
        int32_t tid;
        if (!src_ty || src_ty->kind != JOG_TY_INTERFACE) {
            return false;
        }
        if (!dest || dest->kind != JOG_TY_OPTION || !dest->as.elem.elem || dest->as.elem.elem->kind != JOG_TY_PTR) {
            jog_lower_fail(L, node->pos, "as? expects a pointer to a struct or enum");
            return true;
        }
        elem = dest->as.elem.elem->as.elem.elem;
        src = jog_lower_expr(L, expr);
        data = void_ptr(L);
        tid = ensure_tid(L, elem, &tid_name);
        if (!data || tid < 0 || L->failed) {
            return true;
        }
        have = load_type_id(L, src, node->pos);
        want = emit_unary(L, JIR_BITCAST, jir_global_ref(L->module, (uint32_t)tid), data, node->pos);
        cmp = emit_eq(L, have, want, node->pos);
        ptr = emit_extract(L, src, 0, data, node->pos);
        ptr = emit_unary(L, JIR_BITCAST, ptr, dest->as.elem.elem, node->pos);
        if (L->failed) {
            return true;
        }
        *out = aggregate2(L, dest, cmp, ptr, node->pos);
        return true;
    }
    if (dest && dest->kind == JOG_TY_INTERFACE) {
        src = jog_lower_expr(L, expr);
        if (L->failed) {
            return true;
        }
        if (src.type && src.type->kind == JOG_TY_INTERFACE) {
            *out = src;
            return true;
        }
        *out = jog_lower_iface_convert(L, src, dest, node->pos);
        return true;
    }
    if (src_ty && src_ty->kind == JOG_TY_INTERFACE && dest && dest->kind == JOG_TY_PTR && dest->as.elem.elem) {
        JogType *elem = dest->as.elem.elem;
        JogType *data;
        JirOperand have;
        JirOperand want;
        JirOperand cmp;
        JirOperand ptr;
        const char *tid_name = NULL;
        int32_t tid;
        uint32_t ok_block;
        uint32_t bad_block;
        JogType *str;
        JirInst *panic;
        if (elem->kind != JOG_TY_STRUCT && elem->kind != JOG_TY_ENUM) {
            return false;
        }
        src = jog_lower_expr(L, expr);
        data = void_ptr(L);
        tid = ensure_tid(L, elem, &tid_name);
        if (!data || tid < 0 || L->failed) {
            return true;
        }
        have = load_type_id(L, src, node->pos);
        want = emit_unary(L, JIR_BITCAST, jir_global_ref(L->module, (uint32_t)tid), data, node->pos);
        cmp = emit_eq(L, have, want, node->pos);
        ptr = emit_extract(L, src, 0, data, node->pos);
        ptr = emit_unary(L, JIR_BITCAST, ptr, dest, node->pos);
        if (L->failed || L->terminated) {
            return true;
        }
        if (!add_block(L, &ok_block) || !add_block(L, &bad_block)) {
            return true;
        }
        term_cond(L, cmp, ok_block, bad_block, node->pos);
        use_block(L, bad_block);
        str = jog_type_builtin(L->program->types, JOG_BUILTIN_CONST_BYTES);
        panic = jog_lower_emit(L, JIR_PANIC, -1, node->pos);
        if (panic && str) {
            panic->a = jir_string(str, "as failed", 9);
        }
        term_unreachable(L, node->pos);
        use_block(L, ok_block);
        *out = ptr;
        return true;
    }
    return false;
}

static int user_iface_ty(const JogType *type) {
    return type && type->kind == JOG_TY_INTERFACE && type->as.nominal.decl && !jog_constraint_is_typeset(type);
}

static int base_iface(Lower *L, JogNode *base, JogType **iface, int *needs_load) {
    JogType *base_ty;
    if (iface) {
        *iface = NULL;
    }
    if (needs_load) {
        *needs_load = 0;
    }
    if (!L || !base || !base->type) {
        return 0;
    }
    base_ty = jog_lower_type(L, base->type);
    if (L->failed) {
        return -1;
    }
    if (!base_ty) {
        return 0;
    }
    if (user_iface_ty(base_ty)) {
        if (iface) {
            *iface = base_ty;
        }
        return 1;
    }
    if (base_ty->kind == JOG_TY_PTR && user_iface_ty(base_ty->as.elem.elem)) {
        if (needs_load) {
            *needs_load = 1;
        }
        if (iface) {
            *iface = base_ty->as.elem.elem;
        }
        return 1;
    }
    return 0;
}

static int direct_methods(const JogType *iface) {
    JogNode *decl;
    size_t i;
    int n = 0;
    if (!iface || !iface->as.nominal.decl) {
        return 0;
    }
    decl = iface->as.nominal.decl;
    if (decl->kind != JOG_NODE_INTERFACE) {
        return 0;
    }
    for (i = 0; i < decl->as.aggregate.members.count; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        if (member && member->kind == JOG_NODE_FN && member->type && member->type->kind == JOG_TY_FN) {
            n++;
        }
    }
    return n;
}

/* 1 when want is property number *slot. Same order as walk_props. */
static int prop_index(JogType *iface, JogNode *want, int *slot, int depth) {
    JogNode *decl;
    size_t i;
    if (!iface || !want || !slot || !iface->as.nominal.decl || depth > 16) {
        return 0;
    }
    decl = iface->as.nominal.decl;
    if (decl->kind != JOG_NODE_INTERFACE) {
        return 0;
    }
    for (i = 0; i < decl->as.aggregate.members.count; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        JogType *emb;
        if (!member) {
            continue;
        }
        if (member->kind == JOG_NODE_FIELD && member->as.field.name) {
            if (member == want) {
                return 1;
            }
            (*slot)++;
            continue;
        }
        if (member->kind != JOG_NODE_EMBED || !member->as.embed.type) {
            continue;
        }
        emb = member->as.embed.type->type;
        if (!emb || !user_iface_ty(emb)) {
            continue;
        }
        if (prop_index(emb, want, slot, depth + 1)) {
            return 1;
        }
    }
    return 0;
}

static JirOperand emit_elem(Lower *L, JirOperand ptr, JirOperand index, JogType *ptr_ty, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (L->failed) {
        return jir_none();
    }
    if (ptr.type != ptr_ty) {
        ptr = emit_unary(L, JIR_BITCAST, ptr, ptr_ty, pos);
    }
    if (!jog_lower_new_local(L, NULL, ptr_ty, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_ELEM_ADDR, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = ptr;
    inst->b = index;
    return jog_lower_local_op(L, dst);
}

static void emit_store(Lower *L, JirOperand ptr, JirOperand value, JogPos pos) {
    JirInst *inst;
    if (L->failed || L->terminated) {
        return;
    }
    inst = jog_lower_emit(L, JIR_STORE, -1, pos);
    if (!inst) {
        return;
    }
    inst->a = ptr;
    inst->b = value;
}

/* Address of the property: *(table + offset slot) added to the data pointer. */
static JirOperand prop_addr(Lower *L, JogNode *member, JogType *iface, int needs_load) {
    JogNode *decl;
    JogType *data;
    JogType *usize;
    JogType *u8;
    JogType *u8_ptr;
    JogType *usz_ptr;
    JogType *field_ty;
    JogType *field_ptr;
    JirOperand recv;
    JirOperand vt;
    JirOperand data_op;
    JirOperand slot_ptr;
    JirOperand off;
    JirOperand bytes;
    int slot = 0;
    int methods;
    if (!member || !iface || !member->as.member.base) {
        jog_lower_fail(L, member ? member->pos : (JogPos){0}, "missing property");
        return jir_none();
    }
    decl = member->symbol ? member->symbol->decl : NULL;
    methods = direct_methods(iface);
    if (!decl || decl->kind != JOG_NODE_FIELD || !prop_index(iface, decl, &slot, 0)) {
        jog_lower_fail(L, member->pos, "missing property '%s'", member->as.member.name ? member->as.member.name : "?");
        return jir_none();
    }
    field_ty = member->type ? jog_lower_type(L, member->type) : NULL;
    data = void_ptr(L);
    usize = jog_type_builtin(L->program->types, JOG_BUILTIN_USIZE);
    u8 = jog_type_builtin(L->program->types, JOG_BUILTIN_U8);
    if (!field_ty || field_ty->kind == JOG_TY_ERROR || !data || !usize || !u8) {
        jog_lower_fail(L, member->pos, "missing property '%s'", member->as.member.name ? member->as.member.name : "?");
        return jir_none();
    }
    field_ptr = jog_type_ptr(L->program->types, field_ty, false);
    u8_ptr = jog_type_ptr(L->program->types, u8, false);
    usz_ptr = jog_type_ptr(L->program->types, usize, false);
    if (!field_ptr || !u8_ptr || !usz_ptr) {
        jog_lower_fail(L, member->pos, "out of memory");
        return jir_none();
    }
    recv = jog_lower_expr(L, member->as.member.base);
    if (L->failed) {
        return jir_none();
    }
    if (needs_load) {
        recv = emit_unary(L, JIR_LOAD, recv, iface, member->pos);
    }
    data_op = emit_extract(L, recv, 0, data, member->pos);
    vt = emit_extract(L, recv, 1, data, member->pos);
    slot_ptr = emit_elem(L, vt, jir_int(usize, (uint64_t)(3 + methods + slot)), usz_ptr, member->pos);
    off = emit_unary(L, JIR_LOAD, slot_ptr, usize, member->pos);
    bytes = emit_elem(L, data_op, off, u8_ptr, member->pos);
    return emit_unary(L, JIR_BITCAST, bytes, field_ptr, member->pos);
}

bool jog_lower_iface_load(Lower *L, JogNode *node, JirOperand *out) {
    JogType *iface = NULL;
    JogType *field_ty;
    int needs_load = 0;
    int kind;
    JirOperand addr;
    if (out) {
        *out = jir_none();
    }
    if (!L || !node || !out || node->kind != JOG_NODE_MEMBER || L->failed) {
        return false;
    }
    if (!node->symbol || node->symbol->kind != JOG_SYM_FIELD) {
        return false;
    }
    kind = base_iface(L, node->as.member.base, &iface, &needs_load);
    if (kind < 0) {
        return true;
    }
    if (kind == 0) {
        return false;
    }
    addr = prop_addr(L, node, iface, needs_load);
    field_ty = node->type ? jog_lower_type(L, node->type) : NULL;
    if (L->failed || !field_ty || addr.kind == JIR_OPND_NONE) {
        return true;
    }
    *out = emit_unary(L, JIR_LOAD, addr, field_ty, node->pos);
    return true;
}

bool jog_lower_iface_store(Lower *L, JogNode *node) {
    JogNode *target;
    JogType *iface = NULL;
    int needs_load = 0;
    int kind;
    JirOperand addr;
    JirOperand value;
    if (!L || !node || node->kind != JOG_NODE_ASSIGN || L->failed) {
        return false;
    }
    target = node->as.assign.target;
    if (!target || target->kind != JOG_NODE_MEMBER || !target->symbol || target->symbol->kind != JOG_SYM_FIELD) {
        return false;
    }
    kind = base_iface(L, target->as.member.base, &iface, &needs_load);
    if (kind < 0) {
        return true;
    }
    if (kind == 0) {
        return false;
    }
    if (node->as.assign.op != JOG_TOK_ASSIGN) {
        jog_lower_fail(L, node->pos, "operator '%s' is not lowered yet", jog_token_spelling(node->as.assign.op));
        return true;
    }
    addr = prop_addr(L, target, iface, needs_load);
    value = jog_lower_expr(L, node->as.assign.value);
    if (L->failed || addr.kind == JIR_OPND_NONE) {
        return true;
    }
    emit_store(L, addr, value, node->pos);
    return true;
}

static JirOperand type_is(Lower *L, JirOperand iface_val, JogType *concrete, JogPos pos) {
    JogType *data;
    JirOperand have;
    JirOperand want;
    const char *tid_name = NULL;
    int32_t tid;
    data = void_ptr(L);
    concrete = concrete ? jog_lower_type(L, concrete) : NULL;
    tid = ensure_tid(L, concrete, &tid_name);
    if (!data || tid < 0 || L->failed) {
        return jir_none();
    }
    have = load_type_id(L, iface_val, pos);
    want = emit_unary(L, JIR_BITCAST, jir_global_ref(L->module, (uint32_t)tid), data, pos);
    return emit_eq(L, have, want, pos);
}

bool jog_lower_iface_is(Lower *L, JogNode *node, JirOperand *out) {
    JogNode *expr;
    JogType *src_ty;
    JogType *concrete;
    JirOperand src;
    if (out) {
        *out = jir_none();
    }
    if (!L || !node || !out || node->kind != JOG_NODE_IS || L->failed) {
        return false;
    }
    expr = node->as.is.expr;
    src_ty = expr && expr->type ? jog_lower_type(L, expr->type) : NULL;
    if (L->failed) {
        return true;
    }
    if (!user_iface_ty(src_ty)) {
        return false;
    }
    concrete = node->as.is.type && node->as.is.type->type ? node->as.is.type->type : NULL;
    if (!concrete || (concrete->kind != JOG_TY_STRUCT && concrete->kind != JOG_TY_ENUM)) {
        jog_lower_fail(L, node->pos, "is expects a struct or enum");
        return true;
    }
    src = jog_lower_expr(L, expr);
    if (L->failed) {
        return true;
    }
    *out = type_is(L, src, concrete, node->pos);
    return true;
}

bool jog_lower_iface_narrowed(Lower *L, JogNode *node, JirOperand raw, JirOperand *out) {
    JogType *want;
    JogType *have;
    JogType *elem;
    JogType *data;
    JirOperand ptr;
    if (out) {
        *out = jir_none();
    }
    if (!L || !node || !out || node->kind != JOG_NODE_IDENT || L->failed || raw.kind == JIR_OPND_NONE) {
        return false;
    }
    want = node->type ? jog_lower_type(L, node->type) : NULL;
    have = raw.type ? jog_lower_type(L, raw.type) : NULL;
    if (L->failed) {
        return true;
    }
    if (!want || want->kind != JOG_TY_PTR) {
        return false;
    }
    elem = want->as.elem.elem;
    if (!elem || (elem->kind != JOG_TY_STRUCT && elem->kind != JOG_TY_ENUM) || !elem->as.nominal.decl) {
        return false;
    }
    if (!user_iface_ty(have)) {
        return false;
    }
    data = void_ptr(L);
    if (!data || L->failed) {
        return true;
    }
    ptr = emit_extract(L, raw, 0, data, node->pos);
    ptr = emit_unary(L, JIR_BITCAST, ptr, want, node->pos);
    if (L->failed) {
        return true;
    }
    *out = ptr;
    return true;
}

static void term_br(Lower *L, uint32_t target, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_BR;
    term.target = target;
    term.pos = pos;
    term_now(L, term);
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

static void lower_iface_arm(Lower *L, JogNode *arm, bool as_value, uint32_t result, int have_result, uint32_t join) {
    size_t saved;
    JogNode *body;
    if (L->failed || L->terminated || !arm) {
        return;
    }
    saved = L->locals.count;
    body = arm->as.when_arm.body;
    if (!as_value) {
        if (body && body->kind == JOG_NODE_BLOCK) {
            jog_lower_stmt(L, body);
        } else if (body) {
            (void)jog_lower_expr(L, body);
        }
        if (!L->terminated && !L->failed) {
            term_br(L, join, arm->pos);
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
            term_br(L, join, arm->pos);
        }
    }
    L->locals.count = saved;
}

bool jog_lower_iface_when(Lower *L, JogNode *node, bool as_value, JirOperand *out) {
    JogNode *subject_node;
    JogType *sty;
    JirOperand subject;
    uint32_t *arm_blocks;
    uint32_t join = 0;
    uint32_t result = 0;
    int have_result = 0;
    size_t n;
    size_t i;
    if (out) {
        *out = jir_none();
    }
    if (!L || !node || node->kind != JOG_NODE_WHEN || L->failed || L->terminated) {
        return false;
    }
    subject_node = node->as.when.subject;
    sty = subject_node && subject_node->type ? jog_lower_type(L, subject_node->type) : NULL;
    if (L->failed) {
        return true;
    }
    if (!user_iface_ty(sty)) {
        return false;
    }
    n = node->as.when.arms.count;
    if (n == 0) {
        jog_lower_fail(L, node->pos, "when needs an arm");
        return true;
    }
    subject = jog_lower_as_local(L, jog_lower_expr(L, subject_node), node->pos);
    if (L->failed || L->terminated) {
        return true;
    }
    if (as_value && node->type && node->type->kind != JOG_TY_UNIT && node->type->kind != JOG_TY_ERROR) {
        JogType *ret = jog_lower_type(L, node->type);
        if (!ret || !jog_lower_new_local(L, NULL, ret, &result)) {
            return true;
        }
        have_result = 1;
    }
    if (!add_block(L, &join)) {
        return true;
    }
    arm_blocks = ct_arena_alloc(L->program->arena, n * sizeof(uint32_t), _Alignof(uint32_t));
    if (!arm_blocks) {
        jog_lower_fail(L, node->pos, "out of memory");
        return true;
    }
    for (i = 0; i < n; i++) {
        if (!add_block(L, &arm_blocks[i])) {
            return true;
        }
    }
    for (i = 0; i < n && !L->failed && !L->terminated; i++) {
        JogNode *arm = list_at(node->as.when.arms, i);
        size_t c;
        if (!arm) {
            jog_lower_fail(L, node->pos, "when needs an arm");
            return true;
        }
        if (arm->as.when_arm.conds.count == 0) {
            term_br(L, arm_blocks[i], arm->pos);
            break;
        }
        for (c = 0; c < arm->as.when_arm.conds.count && !L->failed; c++) {
            JogNode *pat = list_at(arm->as.when_arm.conds, c);
            JogType *concrete;
            JirOperand cmp;
            uint32_t next = 0;
            if (!pat || pat->kind != JOG_NODE_PAT_IS) {
                jog_lower_fail(L, pat ? pat->pos : arm->pos, "pattern is not lowered yet");
                return true;
            }
            concrete = pat->as.pat_is.type ? pat->as.pat_is.type->type : NULL;
            cmp = type_is(L, subject, concrete, pat->pos);
            if (L->failed || cmp.kind == JIR_OPND_NONE) {
                return true;
            }
            if (!add_block(L, &next)) {
                return true;
            }
            term_cond(L, cmp, arm_blocks[i], next, pat->pos);
            use_block(L, next);
        }
    }
    if (L->failed) {
        return true;
    }
    if (!L->terminated) {
        term_br(L, join, node->pos);
    }
    for (i = 0; i < n && !L->failed; i++) {
        use_block(L, arm_blocks[i]);
        lower_iface_arm(L, list_at(node->as.when.arms, i), as_value, result, have_result, join);
    }
    if (L->failed) {
        return true;
    }
    use_block(L, join);
    if (have_result && out) {
        *out = jog_lower_local_op(L, result);
    }
    return true;
}
