#include "check_struct.h"

#include "check_enum.h"
#include "check_constraint.h"
#include "check_context.h"
#include "check_error.h"
#include "check_generic.h"
#include "check_interface.h"
#include "check_memory.h"
#include "check_package.h"

#include "resolve_package.h"

#include <stdio.h>
#include <string.h>

/* Structs, fields, literals, methods, self, embedding, and extensions.
   A method and an extension are the same declaration. The method is the
   one declared in the nominal type's package. Anything else is an
   extension and sees only public fields. Value receivers are read-only
   and are passed by value. Pointer receivers take the address when the
   receiver is addressable. */

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static bool is_error(const JogType *type) {
    return type && type->kind == JOG_TY_ERROR;
}

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

static bool nominal_kind(JogTypeKind kind) {
    return kind == JOG_TY_STRUCT || kind == JOG_TY_ENUM || kind == JOG_TY_INTERFACE || kind == JOG_TY_DISTINCT;
}

static void type_text(Check *ck, const JogType *type, char *buf, size_t cap) {
    CtSb sb;
    const char *text;
    if (cap == 0) {
        return;
    }
    buf[0] = '\0';
    if (!type) {
        snprintf(buf, cap, "?");
        return;
    }
    ct_sb_init(&sb);
    jog_type_print(type, &sb);
    text = ct_sb_cstr(&sb);
    snprintf(buf, cap, "%s", text ? text : "?");
    ct_sb_free(&sb);
    (void)ck;
}

static JogSymbol *lookup_name(CheckJob *job, JogBlockScope *scope, const char *name) {
    JogSymbol *found;
    if (!name) {
        return NULL;
    }
    found = jog_block_scope_lookup(scope, name);
    if (found) {
        return found;
    }
    if (!job) {
        return NULL;
    }
    return jog_check_lookup_file(job, name);
}

static const char *here_file(const CheckJob *job) {
    if (job && job->decl) {
        return job->decl->pos.file;
    }
    return NULL;
}

static JogPackage *here_pkg(const CheckJob *job) {
    return job ? job->package : NULL;
}

/* One pointer level, matching field and method auto-deref. */
static JogType *peel_ptr(JogType *type, int *was_ptr) {
    if (was_ptr) {
        *was_ptr = 0;
    }
    if (type && type->kind == JOG_TY_PTR && type->as.elem.elem) {
        if (was_ptr) {
            *was_ptr = 1;
        }
        return type->as.elem.elem;
    }
    return type;
}

static bool type_in_package(const JogType *type, const JogPackage *pkg) {
    const char *tp;
    const char *pp;
    if (!type || !pkg || !nominal_kind(type->kind)) {
        return false;
    }
    tp = type->as.nominal.package;
    pp = pkg->path ? pkg->path : "";
    if (!tp) {
        return false;
    }
    return strcmp(tp, pp) == 0;
}

static bool is_extension(const JogType *on, const JogPackage *pkg) {
    return !type_in_package(on, pkg);
}

static bool same_file(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    return strcmp(a, b) == 0;
}

static bool file_imports(const CheckJob *job, const JogPackage *target) {
    JogSourceFile *source;
    JogNode *file;
    size_t i;
    if (!job || !job->package || !target || job->file_index >= job->package->file_count) {
        return false;
    }
    source = job->package->files[job->file_index];
    if (!source || !source->ast || source->ast->kind != JOG_NODE_FILE) {
        return false;
    }
    file = source->ast;
    for (i = 0; i < file->as.file.imports.count; i++) {
        JogNode *import = list_at(file->as.file.imports, i);
        if (import && import->symbol && import->symbol->package == target) {
            return true;
        }
    }
    return false;
}

static bool method_visible(const CheckJob *job, const JogSymbol *method) {
    const char *file = here_file(job);
    JogPackage *pkg = here_pkg(job);
    if (!method || !jog_symbol_visible_from(method, pkg, file)) {
        return false;
    }
    if (!method->package || method->package == pkg) {
        return true;
    }
    /* A method declared in the type's package is visible wherever the type
       is. An extension still needs its package imported in this file. */
    if (method->decl && method->decl->kind == JOG_NODE_FN) {
        JogType *on = NULL;
        jog_method_receiver(method->decl, &on, NULL);
        if (type_in_package(on, method->package)) {
            return true;
        }
    }
    return file_imports(job, method->package);
}

bool jog_struct_has_zero(const JogType *type) {
    size_t i;
    if (!type || type->kind != JOG_TY_STRUCT || !type->has_layout || is_universe_void(type)) {
        return false;
    }
    for (i = 0; i < type->as.nominal.field_count; i++) {
        JogField *field = type->as.nominal.fields ? &type->as.nominal.fields[i] : NULL;
        if (!field || !jog_check_type_has_zero(field->type)) {
            return false;
        }
    }
    return true;
}

static bool scope_method_sees(const JogNode *here, const JogPackage *pkg, JogType *owner) {
    JogType *on = NULL;
    int is_ptr = 0;
    if (!here || here->kind != JOG_NODE_FN || !here->as.fn.receiver || !here->type || here->type->kind != JOG_TY_FN) {
        return false;
    }
    if (!here->type->as.fn.params || here->type->as.fn.param_count == 0) {
        return false;
    }
    jog_method_receiver(here, &on, &is_ptr);
    if (on != owner) {
        return false;
    }
    return !is_extension(on, pkg);
}

static bool field_visible(const JogNode *here, const JogPackage *pkg, const char *file, JogType *owner, const JogField *field) {
    JogNode *decl;
    JogVisibility vis;
    if (!field) {
        return false;
    }
    decl = field->decl;
    vis = decl ? decl->visibility : JOG_VIS_PUBLIC;
    if (vis == JOG_VIS_PUBLIC) {
        return true;
    }
    if (vis == JOG_VIS_SCOPE) {
        return scope_method_sees(here, pkg, owner);
    }
    if (!decl) {
        return true;
    }
    if (vis == JOG_VIS_FILE) {
        return same_file(decl->pos.file, file);
    }
    /* private and private(package) */
    if (decl->symbol && decl->symbol->package) {
        return decl->symbol->package == pkg;
    }
    return pkg && decl->pos.file && file && same_file(decl->pos.file, file);
}

/* declared is the method's open receiver (every type argument is a type
   parameter). concrete is a value or embedded field of that same nominal. */
static int same_generic_receiver(const JogType *declared, const JogType *concrete) {
    size_t i;
    if (!declared || !concrete || declared->kind != concrete->kind) {
        return 0;
    }
    if (declared->kind != JOG_TY_STRUCT && declared->kind != JOG_TY_ENUM) {
        return 0;
    }
    if (!declared->as.nominal.decl || declared->as.nominal.decl != concrete->as.nominal.decl) {
        return 0;
    }
    if (declared->as.nominal.type_arg_count == 0 ||
        declared->as.nominal.type_arg_count != concrete->as.nominal.type_arg_count ||
        !declared->as.nominal.type_args) {
        return 0;
    }
    for (i = 0; i < declared->as.nominal.type_arg_count; i++) {
        JogType *arg = declared->as.nominal.type_args[i];
        if (!arg || arg->kind != JOG_TY_TYPE_PARAM) {
            return 0;
        }
    }
    return 1;
}

void jog_method_receiver(const JogNode *fn, JogType **on, int *is_ptr) {
    JogType *self;
    if (on) {
        *on = NULL;
    }
    if (is_ptr) {
        *is_ptr = 0;
    }
    if (!fn || fn->kind != JOG_NODE_FN || !fn->type || fn->type->kind != JOG_TY_FN) {
        return;
    }
    if (!fn->type->as.fn.params || fn->type->as.fn.param_count == 0) {
        return;
    }
    self = fn->type->as.fn.params[0];
    if (self && self->kind == JOG_TY_PTR && self->as.elem.elem) {
        if (is_ptr) {
            *is_ptr = 1;
        }
        if (on) {
            *on = self->as.elem.elem;
        }
        return;
    }
    if (on) {
        *on = self;
    }
}

static void consider_field(JogStructPath *best, int *count, const uint32_t *path, uint8_t len, JogField *field) {
    if (!best->found || len < best->len) {
        memset(best, 0, sizeof(*best));
        if (len > JOG_STRUCT_PATH) {
            return;
        }
        memcpy(best->index, path, len * sizeof(uint32_t));
        best->len = len;
        best->field = field;
        best->found = true;
        *count = 1;
        return;
    }
    if (len == best->len) {
        (*count)++;
    }
}

static void search_fields(JogType *type, const char *name, uint32_t *prefix, uint8_t prefix_len, const JogNode *here,
                          const JogPackage *pkg, const char *file, JogStructPath *best, int *count, int *hidden) {
    size_t i;
    if (!type || type->kind != JOG_TY_STRUCT || !name || prefix_len >= JOG_STRUCT_PATH) {
        return;
    }
    for (i = 0; i < type->as.nominal.field_count; i++) {
        JogField *field = type->as.nominal.fields ? &type->as.nominal.fields[i] : NULL;
        uint32_t path[JOG_STRUCT_PATH];
        uint8_t len;
        if (!field) {
            continue;
        }
        memcpy(path, prefix, prefix_len * sizeof(uint32_t));
        path[prefix_len] = (uint32_t)i;
        len = (uint8_t)(prefix_len + 1);
        if (field->name && strcmp(field->name, name) == 0) {
            if (!field_visible(here, pkg, file, type, field)) {
                *hidden = 1;
            } else {
                consider_field(best, count, path, len, field);
            }
        }
        if (field->embedded && field->type && field->type->kind == JOG_TY_STRUCT) {
            search_fields(field->type, name, path, len, here, pkg, file, best, count, hidden);
        }
    }
}

void jog_struct_lookup_field(JogType *type, const char *name, const JogNode *here, const JogPackage *pkg,
                             const char *file, JogStructPath *out) {
    uint32_t prefix[JOG_STRUCT_PATH];
    int count = 0;
    int hidden = 0;
    memset(out, 0, sizeof(*out));
    search_fields(type, name, prefix, 0, here, pkg, file, out, &count, &hidden);
    if (count > 1) {
        out->ambiguous = true;
        out->field = NULL;
    }
    if (!out->found && hidden) {
        out->hidden = true;
    }
}

static bool embed_search(JogType *type, JogType *inner, uint32_t *prefix, uint8_t prefix_len, uint32_t *index, uint8_t *len,
                         int *count) {
    size_t i;
    if (!type || type->kind != JOG_TY_STRUCT || prefix_len >= JOG_STRUCT_PATH) {
        return false;
    }
    if (type == inner || same_generic_receiver(inner, type)) {
        if (*count == 0) {
            memcpy(index, prefix, prefix_len * sizeof(uint32_t));
            *len = prefix_len;
        }
        (*count)++;
        return true;
    }
    for (i = 0; i < type->as.nominal.field_count; i++) {
        JogField *field = type->as.nominal.fields ? &type->as.nominal.fields[i] : NULL;
        uint32_t path[JOG_STRUCT_PATH];
        if (!field || !field->embedded || !field->type || field->type->kind != JOG_TY_STRUCT) {
            continue;
        }
        memcpy(path, prefix, prefix_len * sizeof(uint32_t));
        path[prefix_len] = (uint32_t)i;
        embed_search(field->type, inner, path, (uint8_t)(prefix_len + 1), index, len, count);
    }
    return *count == 1;
}

bool jog_struct_embed_path(JogType *outer, JogType *inner, uint32_t *index, uint8_t *len) {
    uint32_t prefix[JOG_STRUCT_PATH];
    int count = 0;
    if (!outer || !inner || !index || !len) {
        return false;
    }
    *len = 0;
    if (outer == inner || same_generic_receiver(inner, outer)) {
        return true;
    }
    embed_search(outer, inner, prefix, 0, index, len, &count);
    return count == 1;
}

static bool value_contains(const JogType *self, const JogType *type, int depth);

static bool value_contains(const JogType *self, const JogType *type, int depth) {
    size_t i;
    if (!type || depth > 64) {
        return true;
    }
    if (type->kind == JOG_TY_DISTINCT) {
        return value_contains(self, type->as.nominal.base, depth + 1);
    }
    if (type->kind == JOG_TY_ARRAY) {
        return value_contains(self, type->as.array.elem, depth + 1);
    }
    if (type->kind == JOG_TY_TUPLE) {
        for (i = 0; i < type->as.tuple.count; i++) {
            if (!type->as.tuple.elems || value_contains(self, type->as.tuple.elems[i], depth + 1)) {
                return true;
            }
        }
        return false;
    }
    if (type->kind != JOG_TY_STRUCT) {
        return false;
    }
    if (type == self) {
        return true;
    }
    /* Box<T> by value inside struct Box is the same recursive struct.
       The instance is a different JogType from the template. */
    if (self && self->as.nominal.decl && type->as.nominal.decl == self->as.nominal.decl) {
        return true;
    }
    if (type->has_layout || is_universe_void(type)) {
        return false;
    }
    if (!type->as.nominal.fields || type->as.nominal.field_count == 0) {
        return false;
    }
    if (type->as.nominal.field_count == 1 && type->as.nominal.fields[0].type == NULL && type->as.nominal.fields[0].name == NULL) {
        return true;
    }
    for (i = 0; i < type->as.nominal.field_count; i++) {
        if (value_contains(self, type->as.nominal.fields[i].type, depth + 1)) {
            return true;
        }
    }
    return false;
}

static void publish_struct(CheckJob *job, JogType *type) {
    size_t i;
    if (job->decl) {
        job->decl->type = type;
    }
    for (i = 0; i < job->symbol_count; i++) {
        if (job->symbols[i]) {
            job->symbols[i]->type = type;
        }
    }
}

static bool hold_open(Check *ck, JogType *type) {
    JogField *hold = ct_arena_alloc(ck->program->arena, sizeof(*hold), _Alignof(JogField));
    if (!hold) {
        jog_check_oom(ck);
        return false;
    }
    memset(hold, 0, sizeof(*hold));
    type->as.nominal.fields = hold;
    type->as.nominal.field_count = 1;
    return true;
}

static const char *embed_name(const JogType *type) {
    if (type && nominal_kind(type->kind) && type->as.nominal.name && type->as.nominal.name[0] != '\0') {
        return type->as.nominal.name;
    }
    return NULL;
}

/* 1 ready, 0 stopped, -1 a value field's struct failed. A published but
   unfinished struct is waited on so a pointer-only neighbor is laid out
   before this value field. Waiting on self would deadlock; value_contains
   reports that cycle. */
static int await_value(Check *ck, CheckJob *job, JogType *self, JogType *type, int depth) {
    size_t i;
    if (!type || ck->stop) {
        return ck->stop ? 0 : 1;
    }
    if (depth > 64) {
        return -1;
    }
    if (type->kind == JOG_TY_DISTINCT) {
        return await_value(ck, job, self, type->as.nominal.base, depth + 1);
    }
    if (type->kind == JOG_TY_ARRAY) {
        return await_value(ck, job, self, type->as.array.elem, depth + 1);
    }
    if (type->kind == JOG_TY_TUPLE) {
        if (!type->as.tuple.elems) {
            return 1;
        }
        for (i = 0; i < type->as.tuple.count; i++) {
            int ready = await_value(ck, job, self, type->as.tuple.elems[i], depth + 1);
            if (ready != 1) {
                return ready;
            }
        }
        return 1;
    }
    if (type->kind != JOG_TY_STRUCT || type == self || type->has_layout || is_universe_void(type)) {
        return 1;
    }
    if (job && job->decl && type->as.nominal.decl == job->decl) {
        return 1;
    }
    {
        JogNode *decl = type->as.nominal.decl;
        JogSymbol *sym = decl ? decl->symbol : NULL;
        if (!sym) {
            return 1;
        }
        if (is_error(sym->type)) {
            return -1;
        }
        if (!jog_check_wait_finished(ck, job, sym)) {
            return 0;
        }
        if (is_error(sym->type)) {
            return -1;
        }
    }
    return 1;
}

static bool add_field_symbol(Check *ck, CheckJob *job, JogNode *decl, const char *name, JogType *field_type) {
    JogSymbol *symbol = jog_symbol_new(ck->program->arena, JOG_SYM_FIELD, name, decl, field_type, job->package);
    if (!symbol) {
        jog_check_oom(ck);
        return false;
    }
    decl->symbol = symbol;
    decl->type = field_type;
    return true;
}

JogType *jog_check_struct(Check *ck, CheckJob *job) {
    JogNode *node;
    JogType *type;
    JogType *error;
    JogField *fields;
    size_t count;
    size_t i;
    size_t filled;
    bool bad = false;
    const char *sname;
    if (!ck || !job || !job->decl) {
        return NULL;
    }
    node = job->decl;
    error = jog_check_error_type(ck);
    sname = node->as.aggregate.name ? node->as.aggregate.name : "?";
    type = jog_type_new_nominal(ck->program->types, JOG_TY_STRUCT, node->as.aggregate.name,
                                job->package ? job->package->path : NULL, node);
    if (!type || !error) {
        jog_check_oom(ck);
        return NULL;
    }
    if (!hold_open(ck, type)) {
        return NULL;
    }
    publish_struct(job, type);
    if (!jog_generic_bind_struct_params(ck, job, &bad)) {
        return NULL;
    }
    count = node->as.aggregate.members.count;
    fields = NULL;
    filled = 0;
    if (count > 0) {
        fields = ct_arena_alloc(ck->program->arena, count * sizeof(*fields), _Alignof(JogField));
        if (!fields) {
            jog_check_oom(ck);
            return NULL;
        }
        memset(fields, 0, count * sizeof(*fields));
    }
    for (i = 0; i < count && !ck->stop; i++) {
        JogNode *member = list_at(node->as.aggregate.members, i);
        JogType *field_type = NULL;
        const char *fname = NULL;
        bool embedded = false;
        size_t prev;
        if (!member) {
            bad = true;
            continue;
        }
        if (member->kind == JOG_NODE_FIELD) {
            fname = member->as.field.name;
            if (!member->as.field.type) {
                jog_check_fail(ck, member->pos, "missing type");
                bad = true;
                continue;
            }
            field_type = jog_check_resolve_type(ck, job, member->as.field.type);
            if (ck->stop) {
                return NULL;
            }
            if (!field_type || is_error(field_type)) {
                bad = true;
                continue;
            }
            if (member->as.field.default_value && jog_generic_has_param(field_type)) {
                jog_check_fail(ck, member->pos, "a type parameter cannot have a default value yet");
                bad = true;
            } else if (member->as.field.default_value) {
                JogType *got = jog_check_expr(ck, job, NULL, member->as.field.default_value, field_type);
                if (ck->stop) {
                    return NULL;
                }
                if (!got || is_error(got)) {
                    bad = true;
                }
            }
        } else if (member->kind == JOG_NODE_EMBED) {
            embedded = true;
            if (!member->as.embed.type) {
                jog_check_fail(ck, member->pos, "missing type");
                bad = true;
                continue;
            }
            field_type = jog_check_resolve_type(ck, job, member->as.embed.type);
            if (ck->stop) {
                return NULL;
            }
            if (!field_type || is_error(field_type)) {
                bad = true;
                continue;
            }
            if (field_type->kind != JOG_TY_STRUCT) {
                jog_check_fail(ck, member->pos, "embedded field must be a struct");
                bad = true;
                continue;
            }
            fname = embed_name(field_type);
            if (!fname) {
                jog_check_fail(ck, member->pos, "embedded field must be a struct");
                bad = true;
                continue;
            }
        } else {
            jog_check_fail(ck, member->pos, "unsupported struct member");
            bad = true;
            continue;
        }
        if (!fname || fname[0] == '\0') {
            jog_check_fail(ck, member->pos, "missing field name");
            bad = true;
            continue;
        }
        for (prev = 0; prev < filled; prev++) {
            if (fields[prev].name && strcmp(fields[prev].name, fname) == 0) {
                jog_check_fail(ck, member->pos, "duplicate field '%s'", fname);
                bad = true;
                fname = NULL;
                break;
            }
        }
        if (!fname) {
            continue;
        }
        if (is_universe_void(field_type)) {
            jog_check_fail(ck, member->pos, "field '%s' has no layout", fname);
            bad = true;
            continue;
        }
        fields[filled].name = fname;
        fields[filled].type = field_type;
        fields[filled].embedded = embedded;
        fields[filled].decl = member;
        if (!add_field_symbol(ck, job, member, fname, field_type)) {
            return NULL;
        }
        filled++;
    }
    if (ck->stop) {
        return NULL;
    }
    if (bad) {
        return error;
    }
    type->as.nominal.fields = fields;
    type->as.nominal.field_count = filled;
    for (i = 0; i < filled; i++) {
        int ready = await_value(ck, job, type, fields[i].type, 0);
        if (ready == 0) {
            return NULL;
        }
        if (ready < 0) {
            return error;
        }
    }
    for (i = 0; i < filled; i++) {
        if (value_contains(type, fields[i].type, 0)) {
            jog_check_fail(ck, node->pos, "struct '%s' recursively contains itself", sname);
            return error;
        }
    }
    if (node->as.aggregate.type_params.count > 0) {
        /* The template is not a value type. Concrete lists are laid out
           when they are built. */
        if (!jog_generic_struct_finish(ck->program->types, ck->program->arena, job->symbol)) {
            jog_check_oom(ck);
            return NULL;
        }
        return type;
    }
    if (!jog_type_layout(ck->program->types, type)) {
        jog_check_fail(ck, node->pos, "struct '%s' recursively contains itself", sname);
        return error;
    }
    return type;
}

static bool classify_receiver(Check *ck, CheckJob *job, JogType **on, int *is_ptr, JogType **self_ty) {
    JogNode *fn = job->decl;
    JogType *recv;
    JogType *self;
    int ptr;
    if (!fn->as.fn.receiver || !fn->as.fn.receiver->type) {
        jog_check_fail(ck, fn->pos, "missing receiver");
        return false;
    }
    recv = fn->as.fn.receiver->type;
    if (is_error(recv)) {
        return false;
    }
    ptr = fn->as.fn.receiver_is_ptr ? 1 : 0;
    if (!ptr && recv->kind == JOG_TY_PTR && recv->as.elem.elem) {
        ptr = 1;
        recv = recv->as.elem.elem;
    }
    if (ptr) {
        self = jog_type_ptr(ck->program->types, recv, false);
        if (!self) {
            jog_check_oom(ck);
            return false;
        }
    } else {
        self = recv;
    }
    *on = recv;
    *is_ptr = ptr;
    *self_ty = self;
    return !ck->stop;
}

JogType *jog_check_method_type(Check *ck, CheckJob *job, JogType *fn_type) {
    JogType *on = NULL;
    JogType *self = NULL;
    JogType **params;
    JogType *built;
    int is_ptr = 0;
    size_t n;
    size_t i;
    if (!fn_type || fn_type->kind != JOG_TY_FN) {
        return fn_type;
    }
    if (!classify_receiver(ck, job, &on, &is_ptr, &self)) {
        return ck->stop ? NULL : jog_check_error_type(ck);
    }
    n = fn_type->as.fn.param_count + 1;
    params = ct_arena_alloc(ck->program->arena, n * sizeof(*params), _Alignof(JogType *));
    if (!params) {
        jog_check_oom(ck);
        return NULL;
    }
    params[0] = self;
    for (i = 0; i < fn_type->as.fn.param_count; i++) {
        params[i + 1] = fn_type->as.fn.params ? fn_type->as.fn.params[i] : NULL;
    }
    built = jog_type_fn(ck->program->types, params, n, fn_type->as.fn.ret, fn_type->as.fn.contexts,
                        fn_type->as.fn.context_count, fn_type->as.fn.variadic, fn_type->as.fn.escaping, fn_type->as.fn.is_extern);
    if (!built) {
        jog_check_oom(ck);
        return NULL;
    }
    (void)on;
    (void)is_ptr;
    return built;
}

void jog_check_bind_self(Check *ck, CheckJob *job, JogBlockScope *scope) {
    JogNode *fn;
    JogType *self_ty;
    JogSymbol *symbol;
    const char *name;
    JogSymbolKind kind;
    if (!ck || !job || !scope || ck->stop) {
        return;
    }
    fn = job->decl;
    if (!fn || fn->kind != JOG_NODE_FN || !fn->as.fn.receiver || !fn->type || fn->type->kind != JOG_TY_FN) {
        return;
    }
    if (!fn->type->as.fn.params || fn->type->as.fn.param_count == 0) {
        jog_check_fail(ck, fn->pos, "missing receiver");
        return;
    }
    self_ty = fn->type->as.fn.params[0];
    name = ct_intern(ck->program->intern, "self", 4);
    if (!name) {
        jog_check_oom(ck);
        return;
    }
    kind = (self_ty && self_ty->kind == JOG_TY_PTR) ? JOG_SYM_PARAMETER : JOG_SYM_CONSTANT;
    symbol = jog_symbol_new(ck->program->arena, kind, name, fn, self_ty, job->package);
    if (!symbol || !jog_block_scope_bind(scope, symbol)) {
        if (!ck->stop && !symbol) {
            jog_check_oom(ck);
        } else if (!ck->stop) {
            jog_check_fail(ck, fn->pos, "duplicate declaration of 'self'");
        }
    }
}

typedef struct MethodHit {
    JogSymbol *symbol;
    JogNode *decl;
    JogType *on;
    uint32_t index[JOG_STRUCT_PATH];
    uint8_t path_len;
    int depth;
} MethodHit;

static void each_fn(JogProgram *program, void (*visit)(JogNode *fn, JogPackage *pkg, void *ud), void *ud) {
    size_t p;
    size_t f;
    size_t d;
    if (!program) {
        return;
    }
    for (p = 0; p < program->package_count; p++) {
        JogPackage *pkg = program->packages[p];
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count; f++) {
            JogSourceFile *source = pkg->files ? pkg->files[f] : NULL;
            JogNode *file;
            if (!source || !source->ast || source->ast->kind != JOG_NODE_FILE) {
                continue;
            }
            file = source->ast;
            for (d = 0; d < file->as.file.decls.count; d++) {
                JogNode *decl = list_at(file->as.file.decls, d);
                if (decl && decl->kind == JOG_NODE_FN && decl->as.fn.receiver) {
                    visit(decl, pkg, ud);
                }
            }
        }
    }
}

typedef struct SeenMethod {
    JogNode *fn;
    struct SeenMethod *next;
} SeenMethod;

typedef struct DupWalk {
    Check *ck;
    SeenMethod *seen;
} DupWalk;

static void dup_visit(JogNode *fn, JogPackage *pkg, void *ud) {
    DupWalk *walk = ud;
    SeenMethod *prev;
    SeenMethod *cell;
    JogType *on = NULL;
    int is_ptr = 0;
    if (!fn->symbol || !fn->type || fn->type->kind != JOG_TY_FN || !fn->as.fn.name) {
        return;
    }
    jog_method_receiver(fn, &on, &is_ptr);
    if (!on) {
        return;
    }
    for (prev = walk->seen; prev; prev = prev->next) {
        JogNode *earlier = prev->fn;
        JogType *prev_on = NULL;
        int prev_ptr = 0;
        if (!earlier->symbol || earlier->symbol->package != pkg || !earlier->as.fn.name ||
            strcmp(earlier->as.fn.name, fn->as.fn.name) != 0) {
            continue;
        }
        jog_method_receiver(earlier, &prev_on, &prev_ptr);
        if (prev_on == on || same_generic_receiver(prev_on, on) || same_generic_receiver(on, prev_on)) {
            jog_check_fail(walk->ck, fn->pos, "duplicate method '%s'", fn->as.fn.name);
            break;
        }
    }
    cell = ct_arena_alloc(walk->ck->program->arena, sizeof(*cell), _Alignof(SeenMethod));
    if (!cell) {
        jog_check_oom(walk->ck);
        return;
    }
    cell->fn = fn;
    cell->next = walk->seen;
    walk->seen = cell;
}

void jog_check_method_duplicates(Check *ck) {
    DupWalk walk;
    if (!ck || !ck->program || ck->stop) {
        return;
    }
    memset(&walk, 0, sizeof(walk));
    walk.ck = ck;
    each_fn(ck->program, dup_visit, &walk);
}

typedef struct FindWalk {
    Check *ck;
    CheckJob *job;
    const char *name;
    MethodHit *best;
    int count;
    bool waiting;
} FindWalk;

static void find_on_type(FindWalk *find, JogType *type, uint32_t *prefix, uint8_t prefix_len, int depth);

static void take_method(FindWalk *find, JogNode *fn, const uint32_t *path, uint8_t path_len, int depth) {
    if (!find->best->symbol || depth < find->best->depth) {
        memset(find->best, 0, sizeof(*find->best));
        if (path_len > 0 && path) {
            memcpy(find->best->index, path, path_len * sizeof(uint32_t));
        }
        find->best->path_len = path_len;
        find->best->decl = fn;
        find->best->symbol = fn->symbol;
        find->best->depth = depth;
        find->count = 1;
        return;
    }
    if (depth == find->best->depth) {
        find->count++;
    }
}

static void scan_methods(FindWalk *find, JogType *type, const uint32_t *path, uint8_t path_len, int depth) {
    size_t p;
    size_t f;
    size_t d;
    JogProgram *program = find->ck->program;
    for (p = 0; p < program->package_count && !find->waiting; p++) {
        JogPackage *pkg = program->packages[p];
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count && !find->waiting; f++) {
            JogSourceFile *source = pkg->files ? pkg->files[f] : NULL;
            JogNode *file;
            if (!source || !source->ast || source->ast->kind != JOG_NODE_FILE) {
                continue;
            }
            file = source->ast;
            for (d = 0; d < file->as.file.decls.count && !find->waiting; d++) {
                JogNode *decl = list_at(file->as.file.decls, d);
                JogType *on = NULL;
                int is_ptr = 0;
                if (!decl || decl->kind != JOG_NODE_FN || !decl->as.fn.receiver || !decl->symbol || !decl->as.fn.name) {
                    continue;
                }
                if (strcmp(decl->as.fn.name, find->name) != 0) {
                    continue;
                }
                /* A method in another package needs its signature before
                   visibility can tell a method from an extension. */
                if (decl->symbol->package && find->job && decl->symbol->package != find->job->package) {
                    if (!jog_check_wait(find->ck, find->job, decl->symbol)) {
                        find->waiting = true;
                        return;
                    }
                }
                if (!method_visible(find->job, decl->symbol)) {
                    continue;
                }
                if (!jog_check_wait(find->ck, find->job, decl->symbol)) {
                    find->waiting = true;
                    return;
                }
                if (!decl->type || decl->type->kind != JOG_TY_FN) {
                    continue;
                }
                jog_method_receiver(decl, &on, &is_ptr);
                if (on != type && !same_generic_receiver(on, type)) {
                    continue;
                }
                take_method(find, decl, path, path_len, depth);
                (void)is_ptr;
            }
        }
    }
}

static void find_on_type(FindWalk *find, JogType *type, uint32_t *prefix, uint8_t prefix_len, int depth) {
    size_t i;
    int saved;
    if (!type || find->waiting || depth > JOG_STRUCT_PATH) {
        return;
    }
    saved = find->count;
    scan_methods(find, type, prefix, prefix_len, depth);
    if (find->waiting) {
        return;
    }
    /* A method declared on this type shadows promoted methods. */
    if (find->count > saved && find->best->symbol && find->best->depth == depth) {
        return;
    }
    if (type->kind != JOG_TY_STRUCT) {
        return;
    }
    for (i = 0; i < type->as.nominal.field_count && !find->waiting; i++) {
        JogField *field = type->as.nominal.fields ? &type->as.nominal.fields[i] : NULL;
        uint32_t path[JOG_STRUCT_PATH];
        if (!field || !field->embedded || !field->type || prefix_len >= JOG_STRUCT_PATH) {
            continue;
        }
        memcpy(path, prefix, prefix_len * sizeof(uint32_t));
        path[prefix_len] = (uint32_t)i;
        find_on_type(find, field->type, path, (uint8_t)(prefix_len + 1), depth + 1);
    }
}

static int lookup_method(Check *ck, CheckJob *job, JogType *outer, const char *name, MethodHit *hit, bool *ambiguous) {
    FindWalk find;
    uint32_t prefix[JOG_STRUCT_PATH];
    memset(hit, 0, sizeof(*hit));
    *ambiguous = false;
    if (!outer || !name) {
        return 0;
    }
    memset(&find, 0, sizeof(find));
    find.ck = ck;
    find.job = job;
    find.name = name;
    find.best = hit;
    find_on_type(&find, outer, prefix, 0, 0);
    if (find.waiting || ck->stop) {
        return -1;
    }
    if (find.count > 1) {
        *ambiguous = true;
        return 1;
    }
    return hit->symbol ? 1 : 0;
}

static JogNode *fn_param(JogNode *fn, size_t index) {
    if (!fn || fn->kind != JOG_NODE_FN) {
        return NULL;
    }
    return list_at(fn->as.fn.params, index);
}

static int named_param(JogNode *fn, const char *name, size_t count) {
    size_t i;
    if (!name) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        JogNode *param = fn_param(fn, i);
        if (param && param->kind == JOG_NODE_PARAM && param->as.param.name && strcmp(param->as.param.name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool addressable_node(const JogNode *node) {
    if (!node) {
        return false;
    }
    if (node->kind == JOG_NODE_IDENT && node->symbol) {
        return node->symbol->kind == JOG_SYM_VARIABLE || node->symbol->kind == JOG_SYM_PARAMETER ||
               node->symbol->kind == JOG_SYM_CONTEXT;
    }
    if (node->kind == JOG_NODE_MEMBER && node->symbol && node->symbol->kind == JOG_SYM_FIELD) {
        JogType *base_ty = node->as.member.base ? node->as.member.base->type : NULL;
        if (base_ty && base_ty->kind == JOG_TY_PTR) {
            return !base_ty->is_const;
        }
        return addressable_node(node->as.member.base);
    }
    if (node->kind == JOG_NODE_UNARY && node->as.unary.op == JOG_TOK_STAR) {
        JogType *op_ty = node->as.unary.operand ? node->as.unary.operand->type : NULL;
        return op_ty && op_ty->kind == JOG_TY_PTR && !op_ty->is_const;
    }
    if (node->kind == JOG_NODE_INDEX) {
        JogType *base_ty = node->as.index.base ? node->as.index.base->type : NULL;
        if (!base_ty) {
            return false;
        }
        if (base_ty->kind == JOG_TY_PTR || base_ty->kind == JOG_TY_SLICE || base_ty->kind == JOG_TY_BYTES) {
            return !base_ty->is_const;
        }
        if (base_ty->kind == JOG_TY_ARRAY || base_ty->kind == JOG_TY_BYTES_N) {
            return addressable_node(node->as.index.base);
        }
        return false;
    }
    return false;
}

/* Last value parameter when it is a pack. Methods prepend the receiver,
   so the pack is the last function-type parameter. */
static int method_pack_slot(JogNode *fn, const JogType *fn_ty) {
    JogNode *param;
    size_t ast_n;
    size_t nparam;
    if (!fn || fn->kind != JOG_NODE_FN || !fn_ty || fn_ty->kind != JOG_TY_FN) {
        return -1;
    }
    nparam = fn_ty->as.fn.param_count;
    ast_n = fn->as.fn.params.count;
    if (nparam == 0 || ast_n == 0 || nparam != ast_n + 1) {
        return -1;
    }
    param = list_at(fn->as.fn.params, ast_n - 1);
    if (!param || param->kind != JOG_NODE_PARAM || !param->as.param.pack) {
        return -1;
    }
    return (int)nparam - 1;
}

static JogType *method_pack_elem(const JogType *fn_ty, int pack) {
    const JogType *slot;
    if (!fn_ty || fn_ty->kind != JOG_TY_FN || pack < 0 || (size_t)pack >= fn_ty->as.fn.param_count || !fn_ty->as.fn.params) {
        return NULL;
    }
    slot = fn_ty->as.fn.params[pack];
    if (!slot || slot->kind != JOG_TY_SLICE) {
        return NULL;
    }
    return slot->as.elem.elem;
}

static int check_method_args(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogNode *fn, JogType *fn_ty,
                             int implicit) {
    JogType *error;
    size_t user_count;
    size_t i;
    size_t positional = 0;
    bool seen_named = false;
    unsigned char *filled;
    int pack;
    int saw_elems;
    error = jog_check_error_type(ck);
    if (!fn_ty) {
        fn_ty = fn ? fn->type : NULL;
    }
    if (!fn_ty || fn_ty->kind != JOG_TY_FN || fn_ty->as.fn.param_count == 0) {
        jog_check_fail(ck, call->pos, "missing receiver");
        call->type = error;
        return 1;
    }
    /* Slot 0 is self. Implicit calls do not pass it. Explicit calls do. */
    user_count = fn_ty->as.fn.param_count - (implicit ? 1 : 0);
    pack = method_pack_slot(fn, fn_ty);
    saw_elems = 0;
    filled = NULL;
    if (fn_ty->as.fn.param_count > 0) {
        filled = ct_arena_alloc(ck->program->arena, fn_ty->as.fn.param_count, 1);
        if (!filled) {
            jog_check_oom(ck);
            return -1;
        }
        memset(filled, 0, fn_ty->as.fn.param_count);
    }
    if (implicit) {
        filled[0] = 1;
    }
    for (i = 0; i < call->as.call.args.count && !ck->stop; i++) {
        JogNode *arg = list_at(call->as.call.args, i);
        JogNode *value;
        int slot = -1;
        JogType *want = NULL;
        if (!arg || arg->kind != JOG_NODE_ARG) {
            jog_check_fail(ck, arg ? arg->pos : call->pos, "expected an argument");
            continue;
        }
        value = arg->as.arg.value;
        if (arg->as.arg.name) {
            int user = named_param(fn, arg->as.arg.name, fn->as.fn.params.count);
            seen_named = true;
            if (user < 0) {
                jog_check_fail(ck, arg->pos, "unknown parameter '%s'", arg->as.arg.name);
                if (value) {
                    (void)jog_check_expr(ck, job, scope, value, NULL);
                }
                continue;
            }
            slot = user + 1;
            if (filled[slot]) {
                jog_check_fail(ck, arg->pos, "duplicate argument '%s'", arg->as.arg.name);
                continue;
            }
            if (pack >= 0 && slot == pack && saw_elems) {
                jog_check_fail(ck, arg->pos, "duplicate argument '%s'", arg->as.arg.name);
                continue;
            }
            filled[slot] = 1;
            want = fn_ty->as.fn.params[slot];
        } else {
            size_t slot_index;
            if (seen_named) {
                jog_check_fail(ck, arg->pos, "positional argument after a named argument");
                if (value) {
                    (void)jog_check_expr(ck, job, scope, value, NULL);
                }
                continue;
            }
            slot_index = implicit ? positional + 1 : positional;
            if (pack >= 0 && slot_index >= (size_t)pack) {
                JogType *elem = method_pack_elem(fn_ty, pack);
                if (!elem) {
                    jog_check_fail(ck, arg->pos, "pack is not a slice");
                    if (value) {
                        (void)jog_check_expr(ck, job, scope, value, NULL);
                    }
                    continue;
                }
                saw_elems = 1;
                want = elem;
                positional++;
            } else if (slot_index >= fn_ty->as.fn.param_count) {
                jog_check_fail(ck, arg->pos, "too many arguments");
                if (value) {
                    (void)jog_check_expr(ck, job, scope, value, NULL);
                }
                continue;
            } else {
                slot = (int)slot_index;
                positional++;
                filled[slot] = 1;
                want = fn_ty->as.fn.params[slot];
            }
        }
        if (value) {
            arg->type = jog_check_expr(ck, job, scope, value, want);
        }
    }
    if (pack >= 0 && filled) {
        filled[pack] = 1;
    }
    for (i = 0; i < fn_ty->as.fn.param_count && !ck->stop; i++) {
        JogNode *param;
        if (!filled || filled[i]) {
            continue;
        }
        if (i == 0) {
            jog_check_fail(ck, call->pos, "missing argument");
            continue;
        }
        param = fn_param(fn, i - 1);
        if (param && param->kind == JOG_NODE_PARAM && param->as.param.default_value) {
            continue;
        }
        jog_check_fail(ck, call->pos, "missing argument '%s'",
                       param && param->as.param.name ? param->as.param.name : "?");
    }
    if (ck->stop) {
        return -1;
    }
    if (jog_check_context_resolve(ck, job, call) < 0) {
        return -1;
    }
    call->symbol = fn->symbol;
    call->type = fn_ty->as.fn.ret ? fn_ty->as.fn.ret : error;
    (void)user_count;
    return 1;
}

static int check_struct_lit(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogType *type, JogSymbol *symbol) {
    JogType *error = jog_check_error_type(ck);
    size_t n;
    size_t i;
    size_t positional = 0;
    bool seen_named = false;
    unsigned char *filled;
    JogNode *callee = call->as.call.callee;
    n = type->as.nominal.field_count;
    if (callee) {
        callee->symbol = symbol;
        callee->type = type;
    }
    filled = NULL;
    if (n > 0) {
        filled = ct_arena_alloc(ck->program->arena, n, 1);
        if (!filled) {
            jog_check_oom(ck);
            return -1;
        }
        memset(filled, 0, n);
    }
    for (i = 0; i < call->as.call.args.count && !ck->stop; i++) {
        JogNode *arg = list_at(call->as.call.args, i);
        JogNode *value;
        int slot = -1;
        if (!arg || arg->kind != JOG_NODE_ARG) {
            jog_check_fail(ck, arg ? arg->pos : call->pos, "expected an argument");
            continue;
        }
        value = arg->as.arg.value;
        if (arg->as.arg.name) {
            seen_named = true;
            for (slot = 0; slot < (int)n; slot++) {
                JogField *field = &type->as.nominal.fields[slot];
                if (field->name && strcmp(field->name, arg->as.arg.name) == 0) {
                    break;
                }
            }
            if (slot < 0 || slot >= (int)n) {
                jog_check_fail(ck, arg->pos, "unknown field '%s'", arg->as.arg.name);
                if (value) {
                    (void)jog_check_expr(ck, job, scope, value, NULL);
                }
                continue;
            }
            if (!field_visible(job ? job->decl : NULL, here_pkg(job), here_file(job), type, &type->as.nominal.fields[slot])) {
                jog_check_fail(ck, arg->pos, "field '%s' is not visible", arg->as.arg.name);
            }
            if (filled[slot]) {
                jog_check_fail(ck, arg->pos, "duplicate field '%s'", arg->as.arg.name);
                continue;
            }
            filled[slot] = 1;
        } else {
            if (seen_named) {
                jog_check_fail(ck, arg->pos, "positional field after a named field");
                if (value) {
                    (void)jog_check_expr(ck, job, scope, value, NULL);
                }
                continue;
            }
            if (positional >= n) {
                jog_check_fail(ck, arg->pos, "too many fields");
                if (value) {
                    (void)jog_check_expr(ck, job, scope, value, NULL);
                }
                continue;
            }
            slot = (int)positional;
            positional++;
            filled[slot] = 1;
            if (!field_visible(job ? job->decl : NULL, here_pkg(job), here_file(job), type, &type->as.nominal.fields[slot])) {
                jog_check_fail(ck, arg->pos, "field '%s' is not visible",
                               type->as.nominal.fields[slot].name ? type->as.nominal.fields[slot].name : "?");
            }
        }
        if (value && slot >= 0 && slot < (int)n) {
            arg->type = jog_check_expr(ck, job, scope, value, type->as.nominal.fields[slot].type);
        }
    }
    for (i = 0; i < n && !ck->stop; i++) {
        JogField *field;
        JogNode *decl;
        if (!filled || filled[i]) {
            continue;
        }
        field = &type->as.nominal.fields[i];
        decl = field->decl;
        if (decl && decl->kind == JOG_NODE_FIELD && decl->as.field.default_value) {
            continue;
        }
        if (jog_check_type_has_zero(field->type)) {
            continue;
        }
        jog_check_fail(ck, call->pos, "missing field '%s'", field->name ? field->name : "?");
    }
    if (ck->stop) {
        return -1;
    }
    call->symbol = symbol;
    call->type = type ? type : error;
    return 1;
}

typedef struct MethodEnv {
    Check *ck;
    CheckJob *job;
    JogBlockScope *scope;
} MethodEnv;

/* The call's base name is a parameter, variable, or constant, and the
   same file imported a package under that name. 1 after the diagnostic. */
static int report_hidden_package(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *base) {
    JogSymbol *local;
    JogSymbol *hidden;
    const char *what;
    if (!base || base->kind != JOG_NODE_IDENT || !base->as.ident.name) {
        return 0;
    }
    local = lookup_name(job, scope, base->as.ident.name);
    if (!local || (local->kind != JOG_SYM_PARAMETER && local->kind != JOG_SYM_VARIABLE &&
                   local->kind != JOG_SYM_CONSTANT && local->kind != JOG_SYM_CONTEXT)) {
        return 0;
    }
    hidden = jog_check_lookup_file(job, base->as.ident.name);
    if (!hidden || hidden->kind != JOG_SYM_PACKAGE || hidden == local) {
        return 0;
    }
    what = "parameter";
    if (local->kind == JOG_SYM_VARIABLE) {
        what = "variable";
    } else if (local->kind == JOG_SYM_CONSTANT) {
        what = "constant";
    } else if (local->kind == JOG_SYM_CONTEXT) {
        what = "context";
    }
    jog_check_fail(ck, base->pos, "%s '%s' hides imported package '%s'", what, base->as.ident.name, base->as.ident.name);
    return 1;
}

static JogType *method_probe(void *env, JogNode *node, JogType *target) {
    MethodEnv *method = env;
    return jog_check_expr(method->ck, method->job, method->scope, node, target);
}

static int method_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogNode *base, JogType *base_ty,
                       const char *name, int implicit) {
    JogType *outer;
    JogType *error;
    MethodHit hit;
    bool ambiguous = false;
    int was_ptr = 0;
    int found;
    JogNode *callee = call->as.call.callee;
    error = jog_check_error_type(ck);
    outer = peel_ptr(base_ty, &was_ptr);
    found = lookup_method(ck, job, outer, name, &hit, &ambiguous);
    if (found < 0) {
        return -1;
    }
    if (!found) {
        return 0;
    }
    if (ambiguous) {
        jog_check_fail(ck, call->pos, "ambiguous method '%s'", name);
        call->type = error;
        return 1;
    }
    if (callee) {
        callee->symbol = hit.symbol;
        callee->type = hit.decl->type;
    }
    if (implicit) {
        JogType *self = hit.decl->type->as.fn.params[0];
        int need_ptr = self && self->kind == JOG_TY_PTR;
        if (need_ptr) {
            int base_is_ptr = 0;
            peel_ptr(base_ty, &base_is_ptr);
            if (!base_is_ptr && !addressable_node(base)) {
                jog_check_fail(ck, call->pos, "cannot call pointer method '%s' on this value", name);
            }
        }
    }
    if (hit.symbol && hit.symbol->type_param_count > 0) {
        MethodEnv menv;
        JogType *inst;
        menv.ck = ck;
        menv.job = job;
        menv.scope = scope;
        inst = jog_check_generic_method(ck, job, &menv, call, callee, outer, implicit, method_probe);
        if (!inst) {
            return -1;
        }
        if (inst->kind == JOG_TY_ERROR) {
            call->type = inst;
            return 1;
        }
        if (callee) {
            callee->type = inst;
        }
        return check_method_args(ck, job, scope, call, hit.decl, inst, implicit);
    }
    return check_method_args(ck, job, scope, call, hit.decl, hit.decl->type, implicit);
}

/* List<i32>.get is a method of that instance. A generic alias is its target. */
static JogType *named_receiver_type(Check *ck, CheckJob *job, JogNode *ident, JogSymbol *sym) {
    if (!sym) {
        return jog_check_error_type(ck);
    }
    if (sym->decl && sym->decl->kind == JOG_NODE_TYPE_DECL && sym->type_param_count > 0 &&
        !sym->decl->as.type_decl.distinct) {
        return jog_generic_alias_apply(ck, job, sym, ident->as.ident.type_args, ident->pos);
    }
    if (sym->decl && sym->decl->kind == JOG_NODE_ENUM && sym->type_param_count > 0) {
        return jog_generic_enum_apply(ck, job, sym, ident->as.ident.type_args, ident->pos);
    }
    if (sym->decl && sym->decl->kind == JOG_NODE_STRUCT && sym->type_param_count > 0) {
        return jog_generic_struct_apply(ck, job, sym, ident->as.ident.type_args, ident->pos);
    }
    if (ident && ident->as.ident.type_args.count > 0) {
        jog_check_fail(ck, ident->pos, "type arguments are not checked yet");
        return jog_check_error_type(ck);
    }
    return sym->type;
}

int jog_check_struct_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogType **out) {
    JogNode *callee;
    JogType *error;
    if (!call || call->kind != JOG_NODE_CALL || !out) {
        return 0;
    }
    *out = NULL;
    callee = call->as.call.callee;
    error = jog_check_error_type(ck);
    if (!callee || ck->stop) {
        return ck->stop ? -1 : 0;
    }
    if (callee->kind == JOG_NODE_IDENT) {
        JogSymbol *symbol = lookup_name(job, scope, callee->as.ident.name);
        if (!symbol || symbol->kind != JOG_SYM_TYPE) {
            return 0;
        }
        if (!jog_check_wait(ck, job, symbol)) {
            return -1;
        }
        callee->symbol = symbol;
        /* Boxed<i32>(value: 1) is Box<i32>(value: 1): the alias is that struct. */
        if (symbol->decl && symbol->decl->kind == JOG_NODE_TYPE_DECL && symbol->type_param_count > 0 &&
            !symbol->decl->as.type_decl.distinct) {
            JogType *inst = jog_generic_alias_apply(ck, job, symbol, callee->as.ident.type_args, callee->pos);
            JogSymbol *owner;
            if (!inst) {
                return -1;
            }
            callee->type = inst;
            if (inst->kind == JOG_TY_ERROR) {
                call->type = inst;
                *out = inst;
                return 1;
            }
            if (inst->kind != JOG_TY_STRUCT) {
                return 0;
            }
            owner = inst->as.nominal.decl && inst->as.nominal.decl->symbol ? inst->as.nominal.decl->symbol : symbol;
            if (check_struct_lit(ck, job, scope, call, inst, owner) < 0) {
                return -1;
            }
            *out = call->type ? call->type : error;
            return 1;
        }
        callee->type = symbol->type;
        if (!symbol->type || symbol->type->kind != JOG_TY_STRUCT) {
            return 0;
        }
        if (symbol->type_param_count > 0) {
            JogType *inst = jog_generic_struct_apply(ck, job, symbol, callee->as.ident.type_args, callee->pos);
            if (!inst) {
                return -1;
            }
            if (inst->kind == JOG_TY_ERROR) {
                call->type = inst;
                *out = inst;
                return 1;
            }
            callee->type = inst;
            if (check_struct_lit(ck, job, scope, call, inst, symbol) < 0) {
                return -1;
            }
            *out = call->type ? call->type : error;
            return 1;
        }
        if (callee->as.ident.type_args.count > 0) {
            jog_check_fail(ck, callee->pos, "type arguments are not checked yet");
            call->type = error;
            *out = error;
            return 1;
        }
        if (check_struct_lit(ck, job, scope, call, symbol->type, symbol) < 0) {
            return -1;
        }
        *out = call->type ? call->type : error;
        return 1;
    }
    if (callee->kind == JOG_NODE_MEMBER && callee->as.member.name && !callee->as.member.safe) {
        JogNode *base = callee->as.member.base;
        JogSymbol *base_sym = NULL;
        JogType *base_ty;
        int implicit;
        int rc;
        if (base && base->kind == JOG_NODE_IDENT) {
            base_sym = lookup_name(job, scope, base->as.ident.name);
            if (base_sym && base_sym->kind == JOG_SYM_PACKAGE) {
                return 0;
            }
            if (base_sym && base_sym->kind == JOG_SYM_TYPE) {
                JogType *on;
                if (!jog_check_wait(ck, job, base_sym)) {
                    return -1;
                }
                base->symbol = base_sym;
                if (base_sym->type_param_count > 0 || base->as.ident.type_args.count > 0) {
                    on = named_receiver_type(ck, job, base, base_sym);
                    if (!on) {
                        return -1;
                    }
                    if (on->kind == JOG_TY_ERROR) {
                        base->type = on;
                        call->type = on;
                        *out = on;
                        return 1;
                    }
                } else {
                    on = base_sym->type;
                }
                base->type = on;
                implicit = 0;
                rc = method_call(ck, job, scope, call, base, on, callee->as.member.name, implicit);
                if (rc <= 0) {
                    return rc;
                }
                *out = call->type ? call->type : error;
                return 1;
            }
        }
        base_ty = base ? jog_check_expr(ck, job, scope, base, NULL) : NULL;
        if (ck->stop) {
            return -1;
        }
        if (!base_ty || is_error(base_ty)) {
            call->type = error;
            *out = error;
            return 1;
        }
        {
            int iface = jog_check_interface_call(ck, job, scope, call, base_ty, out);
            if (iface != 0) {
                return iface < 0 ? -1 : 1;
            }
        }
        {
            JogStructPath field;
            JogType *outer = peel_ptr(base_ty, NULL);
            jog_struct_lookup_field(outer, callee->as.member.name, job ? job->decl : NULL, here_pkg(job), here_file(job), &field);
            if (field.found && !field.ambiguous) {
                return 0;
            }
        }
        rc = method_call(ck, job, scope, call, base, base_ty, callee->as.member.name, 1);
        if (rc == 0) {
            JogType *outer = peel_ptr(base_ty, NULL);
            if (outer && outer->kind == JOG_TY_TYPE_PARAM) {
                int constrained = jog_constraint_call(ck, job, scope, call, base_ty, out);
                if (constrained < 0) {
                    return -1;
                }
                if (constrained > 0) {
                    return 1;
                }
            }
            if (report_hidden_package(ck, job, scope, base)) {
                call->type = error;
                *out = error;
                return 1;
            }
            return 0;
        }
        if (rc < 0) {
            return -1;
        }
        *out = call->type ? call->type : error;
        return 1;
    }
    return 0;
}

int jog_check_struct_member(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *member, JogType **out) {
    JogNode *base;
    JogType *base_ty;
    JogType *outer;
    JogType *error;
    JogStructPath field;
    MethodHit hit;
    bool ambiguous = false;
    int found;
    if (!member || member->kind != JOG_NODE_MEMBER || !out) {
        return 0;
    }
    *out = NULL;
    if (member->as.member.safe || !member->as.member.name) {
        return 0;
    }
    base = member->as.member.base;
    error = jog_check_error_type(ck);
    if (base && base->kind == JOG_NODE_IDENT) {
        JogSymbol *symbol = lookup_name(job, scope, base->as.ident.name);
        if (symbol && symbol->kind == JOG_SYM_PACKAGE) {
            return 0;
        }
        if (symbol && symbol->kind == JOG_SYM_TYPE) {
            JogType *on;
            if (!jog_check_wait(ck, job, symbol)) {
                return -1;
            }
            base->symbol = symbol;
            if (symbol->type_param_count > 0 || base->as.ident.type_args.count > 0) {
                on = named_receiver_type(ck, job, base, symbol);
                if (!on) {
                    return -1;
                }
                if (on->kind == JOG_TY_ERROR) {
                    base->type = on;
                    member->type = on;
                    *out = on;
                    return 1;
                }
            } else {
                on = symbol->type;
            }
            base->type = on;
            found = lookup_method(ck, job, on, member->as.member.name, &hit, &ambiguous);
            if (found < 0) {
                return -1;
            }
            if (!found) {
                jog_check_fail(ck, member->pos, "undefined method '%s'", member->as.member.name);
                member->type = error;
                *out = error;
                return 1;
            }
            if (ambiguous) {
                jog_check_fail(ck, member->pos, "ambiguous method '%s'", member->as.member.name);
                member->type = error;
                *out = error;
                return 1;
            }
            member->symbol = hit.symbol;
            member->type = hit.decl->type;
            *out = member->type;
            return 1;
        }
    }
    base_ty = base ? jog_check_expr(ck, job, scope, base, NULL) : NULL;
    if (ck->stop) {
        return -1;
    }
    if (!base_ty || is_error(base_ty)) {
        member->type = error;
        *out = error;
        return 1;
    }
    /* bytes and slices have no declared fields. .ptr and .len are views. */
    {
        JogType *direct = jog_type_underlying(base_ty);
        if (direct && (direct->kind == JOG_TY_BYTES || direct->kind == JOG_TY_SLICE)) {
            int view = jog_check_package_view(ck, job, member, out);
            if (view == 0) {
                return 0;
            }
            return view > 0 ? 1 : -1;
        }
    }
    outer = peel_ptr(base_ty, NULL);
    jog_struct_lookup_field(outer, member->as.member.name, job ? job->decl : NULL, here_pkg(job), here_file(job), &field);
    if (field.ambiguous) {
        jog_check_fail(ck, member->pos, "ambiguous field '%s'", member->as.member.name);
        member->type = error;
        *out = error;
        return 1;
    }
    if (field.found && field.field) {
        member->symbol = field.field->decl ? field.field->decl->symbol : NULL;
        member->type = field.field->type;
        *out = member->type ? member->type : error;
        return 1;
    }
    {
        int named = jog_check_enum_name(ck, member, outer);
        if (named < 0) {
            return -1;
        }
        if (named > 0) {
            *out = member->type;
            return 1;
        }
    }
    found = lookup_method(ck, job, outer, member->as.member.name, &hit, &ambiguous);
    if (found < 0) {
        return -1;
    }
    if (ambiguous) {
        jog_check_fail(ck, member->pos, "ambiguous method '%s'", member->as.member.name);
        member->type = error;
        *out = error;
        return 1;
    }
    if (found && hit.symbol) {
        jog_check_fail(ck, member->pos, "method '%s' must be called", member->as.member.name);
        member->symbol = hit.symbol;
        member->type = error;
        *out = error;
        return 1;
    }
    if (field.hidden) {
        jog_check_fail(ck, member->pos, "field '%s' is not visible", member->as.member.name);
        member->type = error;
        *out = error;
        return 1;
    }
    if (outer && outer->kind == JOG_TY_INTERFACE) {
        int iface = jog_check_interface_member(ck, job, member, outer, out);
        if (iface != 0) {
            return iface < 0 ? -1 : 1;
        }
    }
    if (outer && (outer->kind == JOG_TY_STRUCT || nominal_kind(outer->kind) || outer->kind == JOG_TY_INT ||
                  outer->kind == JOG_TY_BOOL || outer->kind == JOG_TY_BYTES)) {
        jog_check_fail(ck, member->pos, "unknown field '%s'", member->as.member.name);
        member->type = error;
        *out = error;
        return 1;
    }
    if (outer && outer->kind == JOG_TY_TYPE_PARAM) {
        return jog_constraint_member(ck, job, member, outer, out);
    }
    return 0;
}

/* 1 when node can be stored through, 0 when it is a const view, -1 otherwise.
   One pointer, matching a struct field: p.0 is (*p).0. */
static int tuple_place(const JogNode *node) {
    JogType *base_ty;
    if (!node) {
        return -1;
    }
    if (node->kind == JOG_NODE_IDENT && node->symbol) {
        if (node->symbol->kind == JOG_SYM_VARIABLE || node->symbol->kind == JOG_SYM_PARAMETER) {
            return 1;
        }
        if (node->symbol->kind == JOG_SYM_CONSTANT) {
            return 0;
        }
        return -1;
    }
    if (node->kind == JOG_NODE_MEMBER) {
        base_ty = node->as.member.base ? node->as.member.base->type : NULL;
        if (base_ty && base_ty->kind == JOG_TY_PTR) {
            return base_ty->is_const ? 0 : 1;
        }
        return tuple_place(node->as.member.base);
    }
    if (node->kind == JOG_NODE_INDEX) {
        base_ty = node->as.index.base ? node->as.index.base->type : NULL;
        if (!base_ty) {
            return -1;
        }
        if (base_ty->kind == JOG_TY_PTR || base_ty->kind == JOG_TY_SLICE || base_ty->kind == JOG_TY_BYTES) {
            return base_ty->is_const ? 0 : 1;
        }
        if (base_ty->kind == JOG_TY_ARRAY || base_ty->kind == JOG_TY_BYTES_N) {
            return tuple_place(node->as.index.base);
        }
        return -1;
    }
    if (node->kind == JOG_NODE_UNARY && node->as.unary.op == JOG_TOK_STAR) {
        base_ty = node->as.unary.operand ? node->as.unary.operand->type : NULL;
        if (base_ty && base_ty->kind == JOG_TY_PTR) {
            return base_ty->is_const ? 0 : 1;
        }
    }
    return -1;
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

static void check_field_value(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *assign, JogType *field_ty) {
    JogType *unit;
    JogTokenKind base;
    unit = jog_check_unit_type(ck);
    base = compound_base(assign->as.assign.op);
    if (base != JOG_TOK_EOF) {
        if ((base == JOG_TOK_PLUS || base == JOG_TOK_MINUS) && field_ty && field_ty->kind == JOG_TY_PTR) {
            if (jog_check_memory_ptr_compound(ck, job, scope, assign->as.assign.value, field_ty) < 0) {
                return;
            }
            assign->type = unit;
            return;
        }
        if (!field_ty || field_ty->kind != JOG_TY_INT) {
            char buf[160];
            type_text(ck, field_ty, buf, sizeof(buf));
            jog_check_fail(ck, assign->pos, "operator '%s' does not apply to %s", jog_token_spelling(base), buf);
        }
        (void)jog_check_expr(ck, job, scope, assign->as.assign.value, field_ty);
    } else if (assign->as.assign.op == JOG_TOK_ASSIGN) {
        (void)jog_check_expr(ck, job, scope, assign->as.assign.value, field_ty);
    } else {
        jog_check_fail(ck, assign->pos, "operator '%s' is not checked yet", jog_token_spelling(assign->as.assign.op));
        return;
    }
    assign->type = unit;
}

void jog_check_field_assign(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *assign) {
    JogNode *target;
    JogType *field_ty = NULL;
    JogType *unit;
    int rc;
    if (!assign) {
        return;
    }
    target = assign->as.assign.target;
    unit = jog_check_unit_type(ck);
    if (target && target->kind == JOG_NODE_MEMBER && !target->as.member.name) {
        int place;
        rc = jog_check_error_field(ck, job, scope, target, &field_ty);
        if (rc < 0 || ck->stop) {
            return;
        }
        if (rc == 0 || is_error(field_ty)) {
            if (rc == 0) {
                jog_check_fail(ck, assign->pos, "assignment target is not checked yet");
            }
            if (assign->as.assign.value) {
                (void)jog_check_expr(ck, job, scope, assign->as.assign.value, NULL);
            }
            return;
        }
        place = tuple_place(target);
        if (place != 1) {
            if (place == 0) {
                jog_check_fail(ck, target->pos, "cannot assign through a const view");
            } else {
                jog_check_fail(ck, target->pos, "cannot assign to this value");
            }
            if (assign->as.assign.value) {
                (void)jog_check_expr(ck, job, scope, assign->as.assign.value, field_ty);
            }
            assign->type = unit;
            return;
        }
        check_field_value(ck, job, scope, assign, field_ty);
        return;
    }
    rc = jog_check_struct_member(ck, job, scope, target, &field_ty);
    if (rc < 0 || ck->stop) {
        return;
    }
    if (rc > 0 && target && target->symbol && target->symbol->kind == JOG_SYM_FIELD && target->as.member.base) {
        JogType *outer = peel_ptr(target->as.member.base->type, NULL);
        JogNode *decl = target->symbol->decl;
        if (outer && outer->kind == JOG_TY_TYPE_PARAM && decl && decl->kind == JOG_NODE_FIELD && !decl->as.field.is_var) {
            jog_check_fail(ck, target->pos, "cannot assign to property '%s'",
                           target->as.member.name ? target->as.member.name : "?");
            if (assign->as.assign.value) {
                (void)jog_check_expr(ck, job, scope, assign->as.assign.value, field_ty);
            }
            assign->type = unit;
            return;
        }
    }
    {
        int st = jog_check_interface_store(ck, job, scope, assign);
        if (st < 0 || ck->stop) {
            return;
        }
        if (st > 0) {
            return;
        }
    }
    if (rc == 0 || !target || !target->symbol || target->symbol->kind != JOG_SYM_FIELD) {
        jog_check_fail(ck, assign->pos, "assignment target is not checked yet");
        if (assign->as.assign.value) {
            (void)jog_check_expr(ck, job, scope, assign->as.assign.value, NULL);
        }
        return;
    }
    if (!addressable_node(target)) {
        jog_check_fail(ck, target->pos, "cannot assign to field '%s'", target->as.member.name ? target->as.member.name : "?");
        if (assign->as.assign.value) {
            (void)jog_check_expr(ck, job, scope, assign->as.assign.value, field_ty);
        }
        assign->type = unit;
        return;
    }
    check_field_value(ck, job, scope, assign, field_ty);
}
