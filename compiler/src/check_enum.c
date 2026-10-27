#include "check_enum.h"

#include "check_constraint.h"
#include "check_generic.h"
#include "check_interface.h"
#include "const_eval.h"
#include "resolve_package.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Enums, when, patterns, and the Option operators.
   T? is the structural option from check_error.c. null, comparisons with
   null, and niche *T? stay there. T converts implicitly to T?, and Some
   and Option name that option. A when on an enum or on T? must cover
   every variant, including through nested enum, option, and struct
   patterns. Any other when used as a value needs an else. in a..b is
   half-open, matching for i in a..b. An or-pattern (comma) does not bind
   names. A variant field default is checked here and may be omitted at
   the construction site. */

enum { JOG_ENUM_DEPTH = 32 };

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static bool is_error(const JogType *type) {
    return type && type->kind == JOG_TY_ERROR;
}

static JogType *peel(JogType *type) {
    return jog_type_underlying(type);
}

static JogType *error_type(Check *ck) {
    return jog_check_error_type(ck);
}

static JogType *bool_type(Check *ck) {
    JogType *type = jog_type_builtin(ck->program->types, JOG_BUILTIN_BOOL);
    if (!type) {
        jog_check_oom(ck);
    }
    return type;
}

static void type_text(const JogType *type, char *buf, size_t cap) {
    CtSb sb;
    const char *text;
    if (cap == 0) {
        return;
    }
    buf[0] = '\0';
    ct_sb_init(&sb);
    jog_type_print(type, &sb);
    text = ct_sb_cstr(&sb);
    snprintf(buf, cap, "%s", text ? text : "?");
    ct_sb_free(&sb);
}

static const char *type_label(const JogType *type) {
    if (type && type->as.nominal.name && type->as.nominal.name[0] != '\0') {
        return type->as.nominal.name;
    }
    return "?";
}

static bool same_type(const JogType *a, const JogType *b) {
    return a == b;
}

static void mismatch(Check *ck, JogPos pos, const JogType *want, const JogType *got) {
    char a[160];
    char b[160];
    type_text(want, a, sizeof(a));
    type_text(got, b, sizeof(b));
    jog_check_fail(ck, pos, "type mismatch: expected %s, got %s", a, b);
}

static const char *here_file(const CheckJob *job) {
    if (job && job->decl) {
        return job->decl->pos.file;
    }
    return NULL;
}

static bool is_discard(const char *name) {
    return !name || name[0] == '\0' || (name[0] == '_' && name[1] == '\0');
}

static bool tag_fits(const JogType *tag, int64_t value) {
    uint8_t bits;
    uint64_t half;
    if (!tag || tag->kind != JOG_TY_INT || tag->as.integer.bits == 0 || tag->as.integer.bits > 64) {
        return false;
    }
    bits = tag->as.integer.bits;
    if (bits >= 64) {
        return tag->as.integer.is_signed || value >= 0;
    }
    if (!tag->as.integer.is_signed) {
        if (value < 0) {
            return false;
        }
        return (uint64_t)value < ((uint64_t)1 << bits);
    }
    half = (uint64_t)1 << (bits - 1);
    if (value < 0) {
        return value >= -(int64_t)half;
    }
    return (uint64_t)value < half;
}

static bool eval_tag(Check *ck, JogNode *expr, JogType *tag, int64_t *out) {
    JogConst value;
    memset(&value, 0, sizeof(value));
    if (!jog_const_eval(ck->program->arena, ck->program->diag, ck->program->types, expr, tag && tag->kind == JOG_TY_INT ? tag : NULL,
                        &value) ||
        value.kind != JOG_CONST_INT) {
        ck->failed = true;
        return false;
    }
    if (tag && tag->kind == JOG_TY_INT && !tag_fits(tag, (int64_t)value.bits)) {
        jog_check_fail(ck, expr->pos, "tag value does not fit");
        return false;
    }
    if ((!tag || tag->kind != JOG_TY_INT) && (int64_t)value.bits < 0) {
        jog_check_fail(ck, expr->pos, "tag value does not fit");
        return false;
    }
    *out = (int64_t)value.bits;
    return true;
}

static bool contains_self(const JogType *self, const JogType *type, int depth) {
    size_t i;
    if (!type || depth > JOG_ENUM_DEPTH) {
        return true;
    }
    if (type == self) {
        return true;
    }
    switch (type->kind) {
    case JOG_TY_PTR:
    case JOG_TY_SLICE:
    case JOG_TY_FN:
    case JOG_TY_INTERFACE:
        return false;
    case JOG_TY_ARRAY:
        return contains_self(self, type->as.array.elem, depth + 1);
    case JOG_TY_OPTION:
        return contains_self(self, type->as.elem.elem, depth + 1);
    case JOG_TY_DISTINCT:
        return contains_self(self, type->as.nominal.base, depth + 1);
    case JOG_TY_TUPLE:
        for (i = 0; i < type->as.tuple.count; i++) {
            if (type->as.tuple.elems && contains_self(self, type->as.tuple.elems[i], depth + 1)) {
                return true;
            }
        }
        return false;
    case JOG_TY_STRUCT:
        for (i = 0; i < type->as.nominal.field_count; i++) {
            if (type->as.nominal.fields && contains_self(self, type->as.nominal.fields[i].type, depth + 1)) {
                return true;
            }
        }
        return false;
    case JOG_TY_ENUM:
        /* Result<T> by value inside enum Result is the same recursive enum.
           The instance is a different JogType from the template. */
        if (self && self->kind == JOG_TY_ENUM && self->as.nominal.decl &&
            type->as.nominal.decl == self->as.nominal.decl) {
            return true;
        }
        for (i = 0; i < type->as.nominal.variant_count; i++) {
            size_t f;
            const JogVariant *variant = type->as.nominal.variants ? &type->as.nominal.variants[i] : NULL;
            if (!variant) {
                return true;
            }
            for (f = 0; f < variant->field_count; f++) {
                if (variant->fields && contains_self(self, variant->fields[f].type, depth + 1)) {
                    return true;
                }
            }
        }
        return false;
    default:
        return false;
    }
}

static bool await_layout(Check *ck, CheckJob *job, const JogType *self, JogType *type, int depth) {
    size_t i;
    if (!type || ck->stop || depth > JOG_ENUM_DEPTH) {
        return !ck->stop;
    }
    switch (type->kind) {
    case JOG_TY_PTR:
    case JOG_TY_SLICE:
    case JOG_TY_FN:
    case JOG_TY_INTERFACE:
        return true;
    case JOG_TY_ARRAY:
        return await_layout(ck, job, self, type->as.array.elem, depth + 1);
    case JOG_TY_OPTION:
        return await_layout(ck, job, self, type->as.elem.elem, depth + 1);
    case JOG_TY_DISTINCT:
        return await_layout(ck, job, self, type->as.nominal.base, depth + 1);
    case JOG_TY_TUPLE:
        for (i = 0; i < type->as.tuple.count; i++) {
            if (type->as.tuple.elems && !await_layout(ck, job, self, type->as.tuple.elems[i], depth + 1)) {
                return false;
            }
        }
        return !ck->stop;
    case JOG_TY_STRUCT:
    case JOG_TY_ENUM:
        if (type == self || type->has_layout) {
            return true;
        }
        /* An instance of the enum being checked is not a different
           declaration. Waiting on it would deadlock. */
        if (self && type->as.nominal.decl && type->as.nominal.decl == self->as.nominal.decl) {
            return true;
        }
        if (type->as.nominal.decl && type->as.nominal.decl->symbol) {
            if (!jog_check_wait_finished(ck, job, type->as.nominal.decl->symbol)) {
                return false;
            }
        }
        return !ck->stop;
    default:
        return true;
    }
}

static void publish_enum(CheckJob *job, JogType *type) {
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

static bool add_variant_symbol(Check *ck, CheckJob *job, JogNode *decl, const char *name, JogType *type) {
    JogSymbol *symbol = jog_symbol_new(ck->program->arena, JOG_SYM_VARIANT, name, decl, type, job->package);
    if (!symbol) {
        jog_check_oom(ck);
        return false;
    }
    decl->symbol = symbol;
    decl->type = type;
    return true;
}

bool jog_enum_has_zero(const JogType *type) {
    size_t i;
    const JogVariant *variant;
    if (!type || type->kind != JOG_TY_ENUM || !type->has_layout || !type->as.nominal.variants) {
        return false;
    }
    variant = NULL;
    for (i = 0; i < type->as.nominal.variant_count; i++) {
        if (type->as.nominal.variants[i].tag == 0) {
            variant = &type->as.nominal.variants[i];
            break;
        }
    }
    if (!variant) {
        return false;
    }
    for (i = 0; i < variant->field_count; i++) {
        if (!variant->fields || !jog_check_type_has_zero(variant->fields[i].type)) {
            return false;
        }
    }
    return true;
}

JogType *jog_check_enum(Check *ck, CheckJob *job) {
    JogNode *node;
    JogType *type;
    JogType *error;
    JogType *tag = NULL;
    JogVariant *variants;
    size_t count;
    size_t i;
    size_t filled;
    bool bad = false;
    bool any_payload = false;
    bool have_prev = false;
    int64_t prev = 0;
    const char *ename;
    if (!ck || !job || !job->decl) {
        return NULL;
    }
    node = job->decl;
    error = error_type(ck);
    ename = node->as.aggregate.name ? node->as.aggregate.name : "?";
    if (!error) {
        return NULL;
    }
    type = jog_type_new_nominal(ck->program->types, JOG_TY_ENUM, node->as.aggregate.name,
                                job->package ? job->package->path : NULL, node);
    if (!type) {
        jog_check_oom(ck);
        return NULL;
    }
    publish_enum(job, type);
    if (!jog_generic_bind_struct_params(ck, job, &bad)) {
        return NULL;
    }
    if (node->as.aggregate.tag_type) {
        tag = jog_check_resolve_type(ck, job, node->as.aggregate.tag_type);
        if (ck->stop) {
            return NULL;
        }
        if (!tag || is_error(tag)) {
            bad = true;
            tag = NULL;
        } else if (tag->kind != JOG_TY_INT) {
            jog_check_fail(ck, node->as.aggregate.tag_type->pos, "enum tag must be an integer");
            bad = true;
            tag = NULL;
        } else {
            type->as.nominal.base = tag;
        }
    }
    count = node->as.aggregate.members.count;
    if (count == 0) {
        jog_check_fail(ck, node->pos, "enum '%s' needs a variant", ename);
        return error;
    }
    variants = ct_arena_alloc(ck->program->arena, count * sizeof(*variants), _Alignof(JogVariant));
    if (!variants) {
        jog_check_oom(ck);
        return NULL;
    }
    memset(variants, 0, count * sizeof(*variants));
    filled = 0;
    for (i = 0; i < count && !ck->stop; i++) {
        JogNode *member = list_at(node->as.aggregate.members, i);
        if (member && member->kind == JOG_NODE_VARIANT && member->as.variant.fields.count > 0) {
            any_payload = true;
        }
    }
    for (i = 0; i < count && !ck->stop; i++) {
        JogNode *member = list_at(node->as.aggregate.members, i);
        JogField *fields = NULL;
        size_t field_count;
        size_t f;
        size_t nfields = 0;
        int64_t tag_value = 0;
        const char *vname;
        bool fields_ok = true;
        if (!member || member->kind != JOG_NODE_VARIANT) {
            jog_check_fail(ck, member ? member->pos : node->pos, "unsupported enum member");
            bad = true;
            continue;
        }
        vname = member->as.variant.name;
        if (!vname || vname[0] == '\0') {
            jog_check_fail(ck, member->pos, "missing variant name");
            bad = true;
            continue;
        }
        for (f = 0; f < filled; f++) {
            if (variants[f].name && strcmp(variants[f].name, vname) == 0) {
                jog_check_fail(ck, member->pos, "duplicate variant '%s'", vname);
                bad = true;
                vname = NULL;
                break;
            }
        }
        if (!vname) {
            continue;
        }
        field_count = member->as.variant.fields.count;
        if (field_count > 0) {
            fields = ct_arena_alloc(ck->program->arena, field_count * sizeof(*fields), _Alignof(JogField));
            if (!fields) {
                jog_check_oom(ck);
                return NULL;
            }
            memset(fields, 0, field_count * sizeof(*fields));
        }
        for (f = 0; f < field_count && !ck->stop; f++) {
            JogNode *param = list_at(member->as.variant.fields, f);
            JogType *field_type;
            size_t prev_f;
            if (!param || param->kind != JOG_NODE_PARAM) {
                jog_check_fail(ck, param ? param->pos : member->pos, "expected a variant field");
                fields_ok = false;
                continue;
            }
            if (!param->as.param.name || param->as.param.name[0] == '\0') {
                jog_check_fail(ck, param->pos, "missing field name");
                fields_ok = false;
                continue;
            }
            if (!param->as.param.type) {
                jog_check_fail(ck, param->pos, "missing type");
                fields_ok = false;
                continue;
            }
            field_type = jog_check_resolve_type(ck, job, param->as.param.type);
            if (ck->stop) {
                return NULL;
            }
            if (!field_type || is_error(field_type)) {
                fields_ok = false;
                continue;
            }
            for (prev_f = 0; prev_f < nfields; prev_f++) {
                if (fields[prev_f].name && strcmp(fields[prev_f].name, param->as.param.name) == 0) {
                    jog_check_fail(ck, param->pos, "duplicate field '%s'", param->as.param.name);
                    fields_ok = false;
                    field_type = NULL;
                    break;
                }
            }
            if (!field_type) {
                continue;
            }
            if (!await_layout(ck, job, type, field_type, 0)) {
                return NULL;
            }
            if (param->as.param.default_value) {
                if (jog_generic_has_param(field_type)) {
                    jog_check_fail(ck, param->pos, "a type parameter cannot have a default value yet");
                    fields_ok = false;
                } else {
                    JogType *got = jog_check_expr(ck, job, NULL, param->as.param.default_value, field_type);
                    if (ck->stop) {
                        return NULL;
                    }
                    if (!got || is_error(got)) {
                        fields_ok = false;
                    }
                }
            }
            param->type = field_type;
            fields[nfields].name = param->as.param.name;
            fields[nfields].type = field_type;
            fields[nfields].decl = param;
            nfields++;
        }
        if (ck->stop) {
            return NULL;
        }
        if (member->as.variant.value) {
            if (any_payload) {
                jog_check_fail(ck, member->pos, "a variant cannot have both a payload and a tag value");
                bad = true;
            } else if (!eval_tag(ck, member->as.variant.value, tag, &tag_value)) {
                bad = true;
                tag_value = 0;
            }
        } else if (have_prev) {
            if (prev == INT64_MAX) {
                jog_check_fail(ck, member->pos, "tag value does not fit");
                bad = true;
            } else {
                tag_value = prev + 1;
            }
        }
        if (tag && !tag_fits(tag, tag_value)) {
            jog_check_fail(ck, member->pos, "tag value does not fit");
            bad = true;
        }
        if (!tag && tag_value < 0) {
            jog_check_fail(ck, member->pos, "tag value does not fit");
            bad = true;
        }
        if (!fields_ok) {
            bad = true;
        }
        variants[filled].name = vname;
        variants[filled].fields = fields;
        variants[filled].field_count = nfields;
        variants[filled].tag = tag_value;
        variants[filled].decl = member;
        if (!add_variant_symbol(ck, job, member, vname, type)) {
            return NULL;
        }
        prev = tag_value;
        have_prev = true;
        filled++;
    }
    if (ck->stop) {
        return NULL;
    }
    if (bad || filled == 0) {
        return error;
    }
    for (i = 0; i < filled; i++) {
        size_t j;
        for (j = i + 1; j < filled; j++) {
            if (variants[i].tag == variants[j].tag) {
                jog_check_fail(ck, variants[j].decl ? variants[j].decl->pos : node->pos, "duplicate tag for '%s'",
                               variants[j].name ? variants[j].name : "?");
                return error;
            }
        }
    }
    type->as.nominal.variants = variants;
    type->as.nominal.variant_count = filled;
    for (i = 0; i < filled; i++) {
        size_t f;
        for (f = 0; f < variants[i].field_count; f++) {
            if (contains_self(type, variants[i].fields[f].type, 0)) {
                jog_check_fail(ck, node->pos, "enum '%s' recursively contains itself", ename);
                return error;
            }
        }
    }
    if (node->as.aggregate.type_params.count > 0) {
        /* The template is not a value type. Concrete lists are laid out
           when they are built. */
        if (!jog_generic_enum_finish(ck->program->types, ck->program->arena, job->symbol)) {
            jog_check_oom(ck);
            return NULL;
        }
        return type;
    }
    if (!jog_type_layout(ck->program->types, type)) {
        jog_check_fail(ck, node->pos, "enum '%s' recursively contains itself", ename);
        return error;
    }
    return type;
}

static bool finish_enum(Check *ck, CheckJob *job, JogType *type) {
    JogSymbol *symbol;
    if (!type || type->kind != JOG_TY_ENUM) {
        return false;
    }
    symbol = type->as.nominal.decl ? type->as.nominal.decl->symbol : NULL;
    if (!symbol) {
        return true;
    }
    return jog_check_wait_finished(ck, job, symbol);
}

static int find_variant(const JogType *type, const char *name) {
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

static const JogVariant *variant_at(const JogType *type, int index) {
    if (!type || index < 0 || (size_t)index >= type->as.nominal.variant_count || !type->as.nominal.variants) {
        return NULL;
    }
    return &type->as.nominal.variants[index];
}

static JogType *lookup_enum_type(Check *ck, CheckJob *job, JogBlockScope *scope, const char *name, JogNode *at) {
    JogSymbol *symbol;
    JogType *type;
    if (!name) {
        return NULL;
    }
    symbol = jog_block_scope_lookup(scope, name);
    if (!symbol && job) {
        symbol = jog_check_lookup_file(job, name);
    }
    if (!symbol || symbol->kind != JOG_SYM_TYPE) {
        return NULL;
    }
    if (!jog_check_wait_finished(ck, job, symbol)) {
        return NULL;
    }
    if (symbol->decl && symbol->decl->kind == JOG_NODE_TYPE_DECL && symbol->type_param_count > 0) {
        JogList args;
        memset(&args, 0, sizeof(args));
        if (at && at->kind == JOG_NODE_IDENT) {
            args = at->as.ident.type_args;
        }
        type = jog_generic_alias_apply(ck, job, symbol, args, at ? at->pos : (JogPos){0});
        if (!type) {
            return NULL;
        }
        if (type->kind == JOG_TY_ERROR) {
            if (at) {
                at->symbol = symbol;
                at->type = type;
            }
            return type;
        }
        type = peel(type);
        if (!type || type->kind != JOG_TY_ENUM) {
            return NULL;
        }
        if (at) {
            at->symbol = symbol;
            at->type = type;
        }
        return type;
    }
    /* A generic struct is not an enum. Struct<T>.method is a method call;
       inventing an enum instance here would hide that method. */
    if (symbol->decl && symbol->decl->kind == JOG_NODE_ENUM && symbol->type_param_count > 0) {
        JogList args;
        memset(&args, 0, sizeof(args));
        if (at && at->kind == JOG_NODE_IDENT) {
            args = at->as.ident.type_args;
        }
        type = jog_generic_enum_apply(ck, job, symbol, args, at ? at->pos : (JogPos){0});
        if (!type) {
            return NULL;
        }
        if (at) {
            at->symbol = symbol;
            at->type = type;
        }
        return type;
    }
    if (symbol->type_param_count > 0) {
        return NULL;
    }
    type = peel(symbol->type);
    if (!type || type->kind != JOG_TY_ENUM) {
        /* Chan<T>.init and other non-enum type calls are not variants. */
        return NULL;
    }
    if (at && at->kind == JOG_NODE_IDENT && at->as.ident.type_args.count > 0) {
        jog_check_fail(ck, at->pos, "type arguments are not checked yet");
        at->type = error_type(ck);
        return at->type;
    }
    if (at) {
        at->symbol = symbol;
        at->type = symbol->type;
    }
    return type;
}

static JogType *enum_from_expr_type(Check *ck, CheckJob *job, JogType *type) {
    type = peel(type);
    if (!type || type->kind != JOG_TY_ENUM) {
        return NULL;
    }
    if (!finish_enum(ck, job, type)) {
        return NULL;
    }
    return type;
}

static bool payload_free(const JogVariant *variant) {
    return variant && variant->field_count == 0;
}

static JogType *variant_value(Check *ck, JogNode *node, JogType *type, const JogVariant *variant, bool allow_payload) {
    JogType *error = error_type(ck);
    if (!payload_free(variant) && !allow_payload) {
        jog_check_fail(ck, node->pos, "variant '%s' needs arguments", variant && variant->name ? variant->name : "?");
        node->type = error;
        node->symbol = variant ? variant->decl->symbol : NULL;
        return error;
    }
    node->type = type;
    node->symbol = variant && variant->decl ? variant->decl->symbol : NULL;
    return type;
}

/* Option<T> is the structural option, unless this file already has a type
   named Option. Some and None name that option. A user method named Some
   is not this path: the base has to be the name Option. */
static JogType *builtin_option_type(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *base) {
    JogSymbol *symbol;
    JogType *elem;
    JogType *built;
    JogNode *arg;
    if (!ck || !base || base->kind != JOG_NODE_IDENT || !base->as.ident.name || strcmp(base->as.ident.name, "Option") != 0) {
        return NULL;
    }
    symbol = scope ? jog_block_scope_lookup(scope, "Option") : NULL;
    if (!symbol && job) {
        symbol = jog_check_lookup_file(job, "Option");
    }
    if (symbol && symbol->kind == JOG_SYM_TYPE) {
        return NULL;
    }
    if (base->as.ident.type_args.count != 1) {
        jog_check_fail(ck, base->pos, "Option takes one type argument");
        return error_type(ck);
    }
    arg = list_at(base->as.ident.type_args, 0);
    elem = jog_check_resolve_type(ck, job, arg);
    if (!elem || ck->stop) {
        return NULL;
    }
    if (elem->kind == JOG_TY_ERROR) {
        return elem;
    }
    built = jog_type_option(ck->program->types, elem);
    if (!built) {
        jog_check_fail(ck, base->pos, "out of memory");
        return NULL;
    }
    base->type = built;
    return built;
}

static int finish_option_name(Check *ck, JogNode *node, JogType *opt, const char *name, JogType **out) {
    JogType *error;
    if (!opt) {
        return 0;
    }
    if (opt->kind == JOG_TY_ERROR) {
        node->type = opt;
        if (out) {
            *out = opt;
        }
        return 1;
    }
    if (opt->kind != JOG_TY_OPTION) {
        return 0;
    }
    error = error_type(ck);
    if (name && strcmp(name, "None") == 0) {
        node->type = opt;
        if (out) {
            *out = opt;
        }
        return 1;
    }
    if (name && strcmp(name, "Some") == 0) {
        jog_check_fail(ck, node->pos, "variant 'Some' needs arguments");
        node->type = error;
        if (out) {
            *out = error;
        }
        return 1;
    }
    jog_check_fail(ck, node->pos, "unknown variant '%s'", name ? name : "?");
    node->type = error;
    if (out) {
        *out = error;
    }
    return 1;
}

static JogType *check_option_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogType *opt, const char *name) {
    JogType *error = error_type(ck);
    JogNode *arg;
    JogNode *value;
    JogType *got;
    if (!opt || opt->kind != JOG_TY_OPTION) {
        return NULL;
    }
    if (name && strcmp(name, "None") == 0) {
        if (call->as.call.args.count != 0) {
            jog_check_fail(ck, call->pos, "variant 'None' does not take arguments");
            call->type = error;
            return error;
        }
        call->type = opt;
        return opt;
    }
    if (!name || strcmp(name, "Some") != 0) {
        jog_check_fail(ck, call->pos, "unknown variant '%s'", name ? name : "?");
        call->type = error;
        return error;
    }
    if (call->as.call.args.count != 1) {
        jog_check_fail(ck, call->pos, "variant 'Some' takes one argument");
        call->type = error;
        return error;
    }
    arg = list_at(call->as.call.args, 0);
    value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : arg;
    got = jog_check_expr(ck, job, scope, value, opt->as.elem.elem);
    if (!got || ck->stop) {
        return NULL;
    }
    if (got->kind == JOG_TY_ERROR) {
        call->type = got;
        return got;
    }
    call->type = opt;
    return opt;
}

int jog_check_enum_member(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *member, JogType **out) {
    JogNode *base;
    JogType *type;
    const JogVariant *variant = NULL;
    int index;
    if (!ck || !member || member->kind != JOG_NODE_MEMBER || member->as.member.safe || !member->as.member.name) {
        return 0;
    }
    base = member->as.member.base;
    if (!base || base->kind != JOG_NODE_IDENT) {
        return 0;
    }
    type = builtin_option_type(ck, job, scope, base);
    if (ck->stop) {
        return -1;
    }
    if (type) {
        return finish_option_name(ck, member, type, member->as.member.name, out);
    }
    type = lookup_enum_type(ck, job, scope, base->as.ident.name, base);
    if (ck->stop) {
        return -1;
    }
    if (!type) {
        return 0;
    }
    if (type->kind == JOG_TY_ERROR) {
        member->type = type;
        if (out) {
            *out = type;
        }
        return 1;
    }
    index = find_variant(type, member->as.member.name);
    if (index < 0) {
        jog_check_fail(ck, member->pos, "unknown variant '%s'", member->as.member.name);
        member->type = error_type(ck);
        if (out) {
            *out = member->type;
        }
        return 1;
    }
    variant = variant_at(type, index);
    if (out) {
        *out = variant_value(ck, member, type, variant, false);
    } else {
        (void)variant_value(ck, member, type, variant, false);
    }
    return 1;
}

JogType *jog_check_enum_dot(Check *ck, CheckJob *job, JogNode *node, JogType *target) {
    JogType *type;
    JogType *error;
    const JogVariant *variant = NULL;
    int index;
    const char *name;
    if (!ck || !node) {
        return NULL;
    }
    error = error_type(ck);
    name = node->as.ident.name;
    {
        JogType *opt = peel(target);
        if (opt && opt->kind == JOG_TY_OPTION) {
            JogType *got = NULL;
            if (!finish_option_name(ck, node, opt, name, &got)) {
                return NULL;
            }
            return node->type;
        }
    }
    type = enum_from_expr_type(ck, job, target);
    if (ck->stop) {
        return NULL;
    }
    if (!type) {
        jog_check_fail(ck, node->pos, "enum variant needs a known type");
        node->type = error;
        return error;
    }
    index = find_variant(type, name);
    if (index < 0) {
        jog_check_fail(ck, node->pos, "unknown variant '%s'", name ? name : "?");
        node->type = error;
        return error;
    }
    variant = variant_at(type, index);
    return variant_value(ck, node, type, variant, false);
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

static JogType *check_variant_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogType *type,
                                   const JogVariant *variant) {
    JogType *error = error_type(ck);
    unsigned char *filled = NULL;
    size_t positional = 0;
    size_t i;
    bool bad = false;
    if (call->as.call.trailing || call->as.call.contexts.count > 0) {
        jog_check_fail(ck, call->pos, "variant arguments cannot be a lambda or a context");
        call->type = error;
        return error;
    }
    if (variant->field_count == 0) {
        if (call->as.call.args.count > 0) {
            jog_check_fail(ck, call->pos, "variant '%s' has no fields", variant->name ? variant->name : "?");
            bad = true;
        }
        call->symbol = variant->decl ? variant->decl->symbol : NULL;
        call->type = bad ? error : type;
        return call->type;
    }
    filled = ct_arena_alloc(ck->program->arena, variant->field_count, 1);
    if (!filled) {
        jog_check_oom(ck);
        return NULL;
    }
    memset(filled, 0, variant->field_count);
    for (i = 0; i < call->as.call.args.count && !ck->stop; i++) {
        JogNode *arg = list_at(call->as.call.args, i);
        int slot;
        JogType *got;
        if (!arg || arg->kind != JOG_NODE_ARG || !arg->as.arg.value) {
            jog_check_fail(ck, arg ? arg->pos : call->pos, "expected an argument");
            bad = true;
            continue;
        }
        slot = place_arg(variant, arg->as.arg.name, &positional, filled);
        if (slot == -2) {
            jog_check_fail(ck, arg->pos, "duplicate argument '%s'", arg->as.arg.name);
            bad = true;
            continue;
        }
        if (slot < 0) {
            jog_check_fail(ck, arg->pos, slot == -3 ? "unknown field '%s'" : "too many arguments",
                           arg->as.arg.name ? arg->as.arg.name : "?");
            bad = true;
            (void)jog_check_expr(ck, job, scope, arg->as.arg.value, NULL);
            continue;
        }
        got = jog_check_expr(ck, job, scope, arg->as.arg.value, variant->fields[slot].type);
        if (ck->stop) {
            return NULL;
        }
        if (!got || is_error(got)) {
            bad = true;
        }
    }
    for (i = 0; i < variant->field_count; i++) {
        JogNode *decl;
        if (!filled[i]) {
            decl = variant->fields[i].decl;
            if (decl && decl->kind == JOG_NODE_PARAM && decl->as.param.default_value) {
                continue;
            }
            jog_check_fail(ck, call->pos, "missing field '%s'", variant->fields[i].name ? variant->fields[i].name : "?");
            bad = true;
        }
    }
    call->symbol = variant->decl ? variant->decl->symbol : NULL;
    call->type = bad ? error : type;
    return call->type;
}

int jog_check_enum_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogType *target, JogType **out) {
    JogNode *callee;
    JogType *type = NULL;
    const JogVariant *variant = NULL;
    int index;
    if (!ck || !call || call->kind != JOG_NODE_CALL) {
        return 0;
    }
    callee = call->as.call.callee;
    if (!callee) {
        return 0;
    }
    if (callee->kind == JOG_NODE_MEMBER && !callee->as.member.safe && callee->as.member.name && callee->as.member.base &&
        callee->as.member.base->kind == JOG_NODE_IDENT) {
        type = builtin_option_type(ck, job, scope, callee->as.member.base);
        if (ck->stop) {
            return -1;
        }
        if (type) {
            JogType *built;
            if (type->kind == JOG_TY_ERROR) {
                call->type = type;
                if (out) {
                    *out = type;
                }
                return 1;
            }
            callee->type = type;
            built = check_option_call(ck, job, scope, call, type, callee->as.member.name);
            if (!built || ck->stop) {
                return -1;
            }
            if (out) {
                *out = built;
            }
            return 1;
        }
        type = lookup_enum_type(ck, job, scope, callee->as.member.base->as.ident.name, callee->as.member.base);
        if (ck->stop) {
            return -1;
        }
        if (!type) {
            return 0;
        }
        if (type->kind == JOG_TY_ERROR) {
            call->type = type;
            if (out) {
                *out = type;
            }
            return 1;
        }
        index = find_variant(type, callee->as.member.name);
        if (index < 0) {
            jog_check_fail(ck, callee->pos, "unknown variant '%s'", callee->as.member.name);
            call->type = error_type(ck);
            if (out) {
                *out = call->type;
            }
            return 1;
        }
        variant = variant_at(type, index);
        callee->symbol = variant && variant->decl ? variant->decl->symbol : NULL;
        callee->type = type;
    } else if (callee->kind == JOG_NODE_DOT_VARIANT) {
        JogType *opt = peel(target);
        JogType *built;
        if (!opt || opt->kind != JOG_TY_OPTION) {
            return 0;
        }
        built = check_option_call(ck, job, scope, call, opt, callee->as.ident.name);
        if (!built || ck->stop) {
            return -1;
        }
        if (out) {
            *out = built;
        }
        return 1;
    } else {
        return 0;
    }
    if (out) {
        *out = check_variant_call(ck, job, scope, call, type, variant);
    }
    if (ck->stop) {
        return -1;
    }
    return 1;
}

static const char *single_type_name(const JogNode *type) {
    if (!type || type->kind != JOG_NODE_TYPE_NAME || type->as.type_name.path.count != 1 || !type->as.type_name.path.items) {
        return NULL;
    }
    return type->as.type_name.path.items[0];
}

/* -2 when the checker stopped. -1 when the path is not a variant of en.
   0 and up is the variant index. A bare name, Enum.Variant, and
   pkg.Enum.Variant all name a variant of the expression's enum. */
static int is_variant_index(Check *ck, CheckJob *job, JogNode *type_node, JogType *en, const char **name_out) {
    const char *const *items;
    size_t count;
    JogSymbol *sym;
    const char *file;
    *name_out = NULL;
    if (!type_node || type_node->kind != JOG_NODE_TYPE_NAME || !type_node->as.type_name.path.items) {
        return -1;
    }
    items = type_node->as.type_name.path.items;
    count = type_node->as.type_name.path.count;
    if (count == 0 || !items[count - 1]) {
        return -1;
    }
    *name_out = items[count - 1];
    if (count == 1) {
        return find_variant(en, items[0]);
    }
    file = job && job->decl ? job->decl->pos.file : NULL;
    if (count == 2) {
        sym = jog_check_lookup_file(job, items[0]);
        if (!sym || sym->kind != JOG_SYM_TYPE) {
            return -1;
        }
        if (!jog_check_wait(ck, job, sym)) {
            return -2;
        }
        if (sym->type != en) {
            return -1;
        }
        type_node->symbol = sym;
        return find_variant(en, items[1]);
    }
    if (count == 3) {
        JogSymbol *pkg = jog_check_lookup_file(job, items[0]);
        if (!pkg || pkg->kind != JOG_SYM_PACKAGE || !pkg->package || !pkg->package->scope) {
            return -1;
        }
        sym = jog_package_scope_lookup(pkg->package->scope, items[1]);
        if (!sym || sym->kind != JOG_SYM_TYPE) {
            return -1;
        }
        if (!jog_check_wait(ck, job, sym)) {
            return -2;
        }
        if (sym->type != en || !jog_symbol_visible_from(sym, job ? job->package : NULL, file)) {
            return -1;
        }
        type_node->symbol = sym;
        return find_variant(en, items[2]);
    }
    return -1;
}

static JogType *check_is_type(Check *ck, CheckJob *job, JogNode *node, JogNode *expr, JogNode *type_node) {
    JogType *got;
    JogType *boolean;
    JogType *error;
    JogType *en;
    const char *name;
    const JogVariant *variant = NULL;
    int index;
    error = error_type(ck);
    boolean = bool_type(ck);
    if (!boolean) {
        return NULL;
    }
    got = expr && expr->type ? expr->type : (expr ? jog_check_expr(ck, job, jog_check_body_scope(job), expr, NULL) : NULL);
    if (ck->stop) {
        return NULL;
    }
    if (!got || is_error(got)) {
        node->type = error;
        return error;
    }
    en = enum_from_expr_type(ck, job, got);
    if (ck->stop) {
        return NULL;
    }
    if (!en) {
        jog_check_fail(ck, node->pos, "is needs an enum");
        node->type = error;
        return error;
    }
    name = NULL;
    index = is_variant_index(ck, job, type_node, en, &name);
    if (index == -2) {
        return NULL;
    }
    if (index < 0) {
        jog_check_fail(ck, type_node ? type_node->pos : node->pos, "'%s' is not a variant of '%s'", name ? name : "?",
                       type_label(en));
        node->type = error;
        return error;
    }
    variant = variant_at(en, index);
    node->symbol = variant && variant->decl ? variant->decl->symbol : NULL;
    node->type = boolean;
    if (type_node) {
        type_node->symbol = node->symbol;
        type_node->type = en;
    }
    return boolean;
}

JogType *jog_check_enum_is(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node) {
    (void)scope;
    if (!ck || !node) {
        return NULL;
    }
    return check_is_type(ck, job, node, node->as.is.expr, node->as.is.type);
}

JogType *jog_check_enum_force(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node) {
    JogType *got;
    JogType *inner;
    JogType *error;
    if (!ck || !node) {
        return NULL;
    }
    error = error_type(ck);
    got = jog_check_expr(ck, job, scope, node->as.unary.operand, NULL);
    if (ck->stop) {
        return NULL;
    }
    if (!got || is_error(got)) {
        node->type = error;
        return error;
    }
    got = peel(got);
    if (!got || got->kind != JOG_TY_OPTION) {
        jog_check_fail(ck, node->pos, "operator '!!' needs an option");
        node->type = error;
        return error;
    }
    inner = got->as.elem.elem;
    node->type = inner ? inner : error;
    return node->type;
}

JogType *jog_check_enum_elvis(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node) {
    JogType *left;
    JogType *inner;
    JogType *right;
    JogType *error;
    if (!ck || !node) {
        return NULL;
    }
    error = error_type(ck);
    left = jog_check_expr(ck, job, scope, node->as.binary.lhs, NULL);
    if (ck->stop) {
        return NULL;
    }
    if (!left || is_error(left)) {
        if (node->as.binary.rhs) {
            (void)jog_check_expr(ck, job, scope, node->as.binary.rhs, NULL);
        }
        node->type = error;
        return error;
    }
    left = peel(left);
    if (!left || left->kind != JOG_TY_OPTION) {
        jog_check_fail(ck, node->pos, "operator '?:' needs an option");
        if (node->as.binary.rhs) {
            (void)jog_check_expr(ck, job, scope, node->as.binary.rhs, NULL);
        }
        node->type = error;
        return error;
    }
    inner = left->as.elem.elem;
    right = jog_check_expr(ck, job, scope, node->as.binary.rhs, inner);
    if (ck->stop) {
        return NULL;
    }
    if (!right || is_error(right)) {
        node->type = error;
        return error;
    }
    node->type = inner;
    return inner;
}

static JogType *peel_ptr(JogType *type) {
    if (type && type->kind == JOG_TY_PTR) {
        return type->as.elem.elem;
    }
    return type;
}

JogType *jog_check_enum_safe(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node) {
    JogType *base;
    JogType *inner;
    JogType *owner;
    JogType *error;
    JogType *result;
    const JogField *field = NULL;
    size_t i;
    if (!ck || !node) {
        return NULL;
    }
    error = error_type(ck);
    if (!node->as.member.name) {
        jog_check_fail(ck, node->pos, "operator '?.' needs a field");
        node->type = error;
        return error;
    }
    base = jog_check_expr(ck, job, scope, node->as.member.base, NULL);
    if (ck->stop) {
        return NULL;
    }
    if (!base || is_error(base)) {
        node->type = error;
        return error;
    }
    base = peel(base);
    if (!base || base->kind != JOG_TY_OPTION) {
        jog_check_fail(ck, node->pos, "operator '?.' needs an option");
        node->type = error;
        return error;
    }
    inner = peel(base->as.elem.elem);
    owner = peel(peel_ptr(inner));
    if (!owner || owner->kind != JOG_TY_STRUCT || !owner->has_layout) {
        jog_check_fail(ck, node->pos, "operator '?.' needs a struct field");
        node->type = error;
        return error;
    }
    for (i = 0; i < owner->as.nominal.field_count; i++) {
        const JogField *item = &owner->as.nominal.fields[i];
        if (item->name && strcmp(item->name, node->as.member.name) == 0) {
            field = item;
            break;
        }
    }
    if (!field) {
        jog_check_fail(ck, node->pos, "unknown field '%s'", node->as.member.name);
        node->type = error;
        return error;
    }
    if (field->decl && field->decl->symbol &&
        !jog_symbol_visible_from(field->decl->symbol, job ? job->package : NULL, here_file(job))) {
        jog_check_fail(ck, node->pos, "field '%s' is not visible", node->as.member.name);
        node->type = error;
        return error;
    }
    result = jog_type_option(ck->program->types, field->type);
    if (!result) {
        jog_check_oom(ck);
        return NULL;
    }
    node->symbol = field->decl ? field->decl->symbol : NULL;
    node->type = result;
    return result;
}

static bool is_arith_tok(JogTokenKind op) {
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

static bool int_lit_tree(const JogNode *node) {
    if (!node) {
        return false;
    }
    if (node->kind == JOG_NODE_INT_LIT) {
        return true;
    }
    if (node->kind == JOG_NODE_UNARY) {
        JogTokenKind op = node->as.unary.op;
        return (op == JOG_TOK_MINUS || op == JOG_TOK_TILDE) && int_lit_tree(node->as.unary.operand);
    }
    if (node->kind != JOG_NODE_BINARY || !is_arith_tok(node->as.binary.op)) {
        return false;
    }
    return int_lit_tree(node->as.binary.lhs) && int_lit_tree(node->as.binary.rhs);
}

static bool field_is_unit(const JogType *type) {
    const JogType *underlying;
    if (!type) {
        return false;
    }
    underlying = jog_type_underlying((JogType *)type);
    return underlying && underlying->kind == JOG_TY_UNIT;
}

/* Every variant is only a tag. A unit field is not a payload. */
static bool enum_payload_free(const JogType *en) {
    size_t i;
    if (!en || en->kind != JOG_TY_ENUM || !en->as.nominal.base || !en->as.nominal.variants || en->as.nominal.variant_count == 0) {
        return false;
    }
    for (i = 0; i < en->as.nominal.variant_count; i++) {
        const JogVariant *variant = &en->as.nominal.variants[i];
        size_t f;
        if (variant->field_count > 0 && !variant->fields) {
            return false;
        }
        for (f = 0; f < variant->field_count; f++) {
            if (!field_is_unit(variant->fields[f].type)) {
                return false;
            }
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

static int variant_by_tag(const JogType *en, uint64_t bits) {
    size_t i;
    if (!en || !en->as.nominal.variants) {
        return -1;
    }
    for (i = 0; i < en->as.nominal.variant_count; i++) {
        if ((uint64_t)en->as.nominal.variants[i].tag == bits) {
            return (int)i;
        }
    }
    return -1;
}

/* Integer to a payload-free enum. 0 when this cast is not that. */
static int check_tag_cast(Check *ck, CheckJob *job, JogNode *node, JogType *got, JogType *dest, JogType **out) {
    JogType *en;
    JogType *src;
    JogType *error;
    if (!got || is_error(got)) {
        return 0;
    }
    en = enum_from_expr_type(ck, job, dest);
    if (ck->stop) {
        return -1;
    }
    if (!en || !enum_payload_free(en)) {
        return 0;
    }
    src = peel(got);
    if (!src || (src->kind != JOG_TY_INT && src->kind != JOG_TY_UNTYPED_INT)) {
        return 0;
    }
    error = error_type(ck);
    if (int_lit_tree(node->as.cast.expr)) {
        JogConst value;
        memset(&value, 0, sizeof(value));
        if (!jog_const_eval(ck->program->arena, ck->program->diag, ck->program->types, node->as.cast.expr,
                            en->as.nominal.base, &value) ||
            value.kind != JOG_CONST_INT) {
            ck->failed = true;
            node->type = error;
            if (out) {
                *out = error;
            }
            return 1;
        }
        if (variant_by_tag(en, value.bits) < 0) {
            jog_check_fail(ck, node->pos, "constant does not fit");
            node->type = error;
            if (out) {
                *out = error;
            }
            return 1;
        }
    } else if (src->kind != JOG_TY_INT) {
        return 0;
    }
    node->type = dest;
    if (out) {
        *out = dest;
    }
    return 1;
}

int jog_check_enum_name(Check *ck, JogNode *member, JogType *outer) {
    JogType *en;
    JogType *str;
    if (!ck || !member || member->kind != JOG_NODE_MEMBER || !member->as.member.name || strcmp(member->as.member.name, "name") != 0) {
        return 0;
    }
    en = peel(outer);
    if (en && en->kind == JOG_TY_PTR) {
        en = peel(en->as.elem.elem);
    }
    if (!en || en->kind != JOG_TY_ENUM || enum_has_field(en, "name")) {
        return 0;
    }
    str = jog_type_builtin(ck->program->types, JOG_BUILTIN_CONST_BYTES);
    if (!str) {
        jog_check_oom(ck);
        return -1;
    }
    member->symbol = NULL;
    member->type = str;
    return 1;
}

int jog_check_enum_cast(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType **out) {
    JogType *dest;
    JogType *got;
    JogType *en;
    JogType *error;
    char a[160];
    char b[160];
    if (!node || node->kind != JOG_NODE_CAST || node->as.cast.op != JOG_TOK_KW_AS) {
        return 0;
    }
    dest = jog_check_resolve_type(ck, job, node->as.cast.type);
    if (ck->stop) {
        return -1;
    }
    if (!dest) {
        return 0;
    }
    error = error_type(ck);
    if (is_error(dest)) {
        node->type = dest;
        if (out) {
            *out = dest;
        }
        return 1;
    }
    got = jog_check_expr(ck, job, scope, node->as.cast.expr, NULL);
    if (ck->stop) {
        return -1;
    }
    en = enum_from_expr_type(ck, job, got);
    if (ck->stop) {
        return -1;
    }
    if (!en) {
        return check_tag_cast(ck, job, node, got, dest, out);
    }
    if (en->as.nominal.base && same_type(dest, en->as.nominal.base)) {
        node->type = dest;
        if (out) {
            *out = dest;
        }
        return 1;
    }
    type_text(got, a, sizeof(a));
    type_text(dest, b, sizeof(b));
    jog_check_fail(ck, node->pos, "cannot cast %s to %s", a, b);
    node->type = error;
    if (out) {
        *out = error;
    }
    return 1;
}

static bool pat_covers_type(const JogNode *pat, const JogType *type);

static bool fields_cover(const JogNode *pat, const JogVariant *variant) {
    unsigned char filled[64];
    size_t positional = 0;
    size_t i;
    if (!pat || pat->kind != JOG_NODE_PAT_VARIANT || !variant) {
        return false;
    }
    if (variant->field_count > 64) {
        return false;
    }
    if (variant->field_count == 0) {
        return pat->as.pat_variant.fields.count == 0;
    }
    if (!variant->fields) {
        return false;
    }
    memset(filled, 0, variant->field_count);
    for (i = 0; i < pat->as.pat_variant.fields.count; i++) {
        JogNode *field = list_at(pat->as.pat_variant.fields, i);
        int slot = -1;
        JogNode *inner;
        if (!field || field->kind != JOG_NODE_PAT_FIELD) {
            return false;
        }
        if (field->as.pat_field.name) {
            size_t f;
            for (f = 0; f < variant->field_count; f++) {
                if (variant->fields[f].name && strcmp(variant->fields[f].name, field->as.pat_field.name) == 0) {
                    slot = (int)f;
                    break;
                }
            }
        } else {
            while (positional < variant->field_count && filled[positional]) {
                positional++;
            }
            if (positional < variant->field_count) {
                slot = (int)positional++;
            }
        }
        if (slot < 0 || filled[slot]) {
            return false;
        }
        filled[slot] = 1;
        inner = field->as.pat_field.pattern;
        if (!pat_covers_type(inner, variant->fields[slot].type)) {
            return false;
        }
    }
    for (i = 0; i < variant->field_count; i++) {
        if (!filled[i]) {
            return false;
        }
    }
    return true;
}

static bool pat_covers_type(const JogNode *pat, const JogType *type) {
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
    if (!type) {
        return false;
    }
    if (type->kind == JOG_TY_ENUM && pat->kind == JOG_NODE_PAT_VARIANT && type->as.nominal.variant_count == 1) {
        int index = find_variant(type, pat->as.pat_variant.name);
        if (index != 0) {
            return false;
        }
        return fields_cover(pat, variant_at(type, 0));
    }
    if (type->kind == JOG_TY_OPTION && pat->kind == JOG_NODE_PAT_VARIANT && pat->as.pat_variant.name &&
        strcmp(pat->as.pat_variant.name, "Some") == 0 && pat->as.pat_variant.fields.count == 1) {
        JogNode *field = list_at(pat->as.pat_variant.fields, 0);
        JogNode *inner = field && field->kind == JOG_NODE_PAT_FIELD ? field->as.pat_field.pattern : NULL;
        return pat_covers_type(inner, type->as.elem.elem);
    }
    return false;
}

static bool check_pattern(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *pat, JogType *subject, bool no_subject,
                          int depth);

/* Positional and named payload fields. `field: pattern` checks `pattern`
   against that field. A missing field is an error; there is no `..`. */
static bool check_payload_fields(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *pat, const char *ctor,
                                 const JogField *fields, size_t field_count, int depth) {
    unsigned char *filled;
    size_t positional = 0;
    size_t i;
    bool ok = true;
    if (!pat || pat->kind != JOG_NODE_PAT_VARIANT) {
        return false;
    }
    if (field_count == 0) {
        if (pat->as.pat_variant.fields.count > 0) {
            jog_check_fail(ck, pat->pos, "variant '%s' has no fields", ctor ? ctor : "?");
            return false;
        }
        return true;
    }
    if (!fields) {
        return false;
    }
    filled = ct_arena_alloc(ck->program->arena, field_count, 1);
    if (!filled) {
        jog_check_oom(ck);
        return false;
    }
    memset(filled, 0, field_count);
    for (i = 0; i < pat->as.pat_variant.fields.count && !ck->stop; i++) {
        JogNode *field = list_at(pat->as.pat_variant.fields, i);
        int slot = -1;
        if (!field || field->kind != JOG_NODE_PAT_FIELD) {
            ok = false;
            continue;
        }
        if (field->as.pat_field.name) {
            size_t f;
            for (f = 0; f < field_count; f++) {
                if (fields[f].name && strcmp(fields[f].name, field->as.pat_field.name) == 0) {
                    slot = (int)f;
                    break;
                }
            }
            if (slot < 0) {
                jog_check_fail(ck, field->pos, "unknown field '%s'", field->as.pat_field.name);
                ok = false;
                continue;
            }
            if (filled[slot]) {
                jog_check_fail(ck, field->pos, "duplicate field '%s'", field->as.pat_field.name);
                ok = false;
                continue;
            }
        } else {
            while (positional < field_count && filled[positional]) {
                positional++;
            }
            if (positional >= field_count) {
                jog_check_fail(ck, field->pos, "too many fields");
                ok = false;
                continue;
            }
            slot = (int)positional++;
        }
        filled[slot] = 1;
        if (!check_pattern(ck, job, scope, field->as.pat_field.pattern, fields[slot].type, false, depth + 1)) {
            ok = false;
        }
    }
    for (i = 0; i < field_count; i++) {
        if (!filled[i]) {
            jog_check_fail(ck, pat->pos, "missing field '%s'", fields[i].name ? fields[i].name : "?");
            ok = false;
        }
    }
    return ok;
}

/* `code: in 1..10` binds `code`. `h: height` binds only `height`. `_` and a
   wildcard field bind nothing. */
static bool field_name_binds(const JogNode *field) {
    const char *name;
    const JogNode *inner;
    if (!field || field->kind != JOG_NODE_PAT_FIELD) {
        return false;
    }
    name = field->as.pat_field.name;
    inner = field->as.pat_field.pattern;
    if (!name || is_discard(name) || !inner) {
        return false;
    }
    if (inner->kind == JOG_NODE_PAT_WILDCARD || inner->kind == JOG_NODE_PAT_BIND) {
        return false;
    }
    return true;
}

static bool arm_has_value_bind(const JogNode *pat) {
    size_t i;
    if (!pat) {
        return false;
    }
    if (pat->kind == JOG_NODE_PAT_BIND && (!pat->symbol || pat->symbol->kind != JOG_SYM_VARIANT)) {
        if (pat->as.ident.name && strcmp(pat->as.ident.name, "None") == 0 && pat->type && peel(pat->type) &&
            peel(pat->type)->kind == JOG_TY_OPTION) {
            return false;
        }
        return true;
    }
    if (pat->kind == JOG_NODE_PAT_VARIANT) {
        for (i = 0; i < pat->as.pat_variant.fields.count; i++) {
            JogNode *field = list_at(pat->as.pat_variant.fields, i);
            if (!field || field->kind != JOG_NODE_PAT_FIELD) {
                continue;
            }
            if (field_name_binds(field) || arm_has_value_bind(field->as.pat_field.pattern)) {
                return true;
            }
        }
    }
    return false;
}

static void bind_pattern(Check *ck, CheckJob *job, JogNode *pat) {
    size_t i;
    if (!pat || ck->stop) {
        return;
    }
    if (pat->kind == JOG_NODE_PAT_BIND && (!pat->symbol || pat->symbol->kind != JOG_SYM_VARIANT)) {
        if (pat->as.ident.name && strcmp(pat->as.ident.name, "None") == 0 && pat->type && peel(pat->type) &&
            peel(pat->type)->kind == JOG_TY_OPTION) {
            return;
        }
        if (!is_discard(pat->as.ident.name)) {
            JogSymbol *symbol = jog_check_body_bind(job, pat, JOG_SYM_CONSTANT, pat->as.ident.name, pat->type);
            if (symbol) {
                pat->symbol = symbol;
                jog_check_body_assigned(job, symbol);
            }
        }
        return;
    }
    if (pat->kind == JOG_NODE_PAT_VARIANT) {
        for (i = 0; i < pat->as.pat_variant.fields.count; i++) {
            JogNode *field = list_at(pat->as.pat_variant.fields, i);
            JogNode *inner;
            if (!field || field->kind != JOG_NODE_PAT_FIELD) {
                continue;
            }
            inner = field->as.pat_field.pattern;
            bind_pattern(ck, job, inner);
            if (!field_name_binds(field) || !inner || !inner->type || is_error(inner->type)) {
                continue;
            }
            {
                JogSymbol *symbol = jog_check_body_bind(job, field, JOG_SYM_CONSTANT, field->as.pat_field.name, inner->type);
                if (symbol) {
                    field->symbol = symbol;
                    jog_check_body_assigned(job, symbol);
                }
            }
        }
    }
}

static bool check_pattern(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *pat, JogType *subject, bool no_subject,
                          int depth) {
    JogType *error;
    JogType *type;
    if (!pat || ck->stop) {
        return false;
    }
    if (depth > JOG_ENUM_DEPTH) {
        jog_check_fail(ck, pat->pos, "pattern is too deep");
        return false;
    }
    error = error_type(ck);
    type = peel(subject);
    if (pat->kind == JOG_NODE_PAT_WILDCARD) {
        if (no_subject) {
            jog_check_fail(ck, pat->pos, "wildcard needs a subject");
            return false;
        }
        pat->type = type;
        return true;
    }
    if (no_subject) {
        if (pat->kind == JOG_NODE_PAT_EXPR) {
            JogType *boolean = bool_type(ck);
            JogType *got = jog_check_expr(ck, job, scope, pat->as.pat_expr.expr, boolean);
            pat->type = boolean;
            return got && !is_error(got) && got->kind == JOG_TY_BOOL;
        }
        if (pat->kind == JOG_NODE_PAT_BIND) {
            JogNodeKind saved = pat->kind;
            JogType *boolean = bool_type(ck);
            JogType *got;
            pat->kind = JOG_NODE_IDENT;
            got = jog_check_expr(ck, job, scope, pat, boolean);
            pat->kind = saved;
            return got && !is_error(got);
        }
        jog_check_fail(ck, pat->pos, "variant pattern needs a subject");
        pat->type = error;
        return false;
    }
    if (!type || is_error(type)) {
        pat->type = error;
        return false;
    }
    if (pat->kind == JOG_NODE_PAT_BIND) {
        if (type->kind == JOG_TY_ENUM) {
            int index = find_variant(type, pat->as.ident.name);
            const JogVariant *variant = variant_at(type, index);
            if (index >= 0 && payload_free(variant)) {
                pat->symbol = variant->decl ? variant->decl->symbol : NULL;
                pat->type = type;
                return true;
            }
        }
        if (type->kind == JOG_TY_OPTION && pat->as.ident.name && strcmp(pat->as.ident.name, "None") == 0) {
            pat->type = subject;
            return true;
        }
        pat->type = subject;
        return true;
    }
    if (pat->kind == JOG_NODE_PAT_EXPR) {
        JogType *got = jog_check_expr(ck, job, scope, pat->as.pat_expr.expr, subject);
        if (ck->stop) {
            return false;
        }
        pat->type = subject;
        return got && !is_error(got);
    }
    if (pat->kind == JOG_NODE_PAT_IN) {
        JogNode *range = pat->as.pat_expr.expr;
        JogType *lo;
        JogType *hi;
        if (type->kind != JOG_TY_INT) {
            jog_check_fail(ck, pat->pos, "in pattern needs an integer");
            pat->type = error;
            return false;
        }
        if (!range || range->kind != JOG_NODE_BINARY || range->as.binary.op != JOG_TOK_DOTDOT) {
            jog_check_fail(ck, pat->pos, "in pattern needs a range");
            pat->type = error;
            return false;
        }
        lo = jog_check_expr(ck, job, scope, range->as.binary.lhs, subject);
        hi = jog_check_expr(ck, job, scope, range->as.binary.rhs, subject);
        pat->type = subject;
        return lo && hi && !is_error(lo) && !is_error(hi);
    }
    if (pat->kind == JOG_NODE_PAT_IS) {
        const char *name = single_type_name(pat->as.pat_is.type);
        int index;
        const JogVariant *variant;
        if (type && type->kind == JOG_TY_INTERFACE && !jog_constraint_is_typeset(type)) {
            int iface = jog_check_interface_pat_is(ck, job, pat, subject);
            if (iface < 0 || ck->stop) {
                return false;
            }
            return iface > 0;
        }
        if (!type || type->kind != JOG_TY_ENUM) {
            jog_check_fail(ck, pat->pos, "is needs an enum");
            pat->type = error;
            return false;
        }
        index = name ? find_variant(type, name) : -1;
        variant = variant_at(type, index);
        if (!variant) {
            jog_check_fail(ck, pat->as.pat_is.type ? pat->as.pat_is.type->pos : pat->pos, "'%s' is not a variant of '%s'",
                           name ? name : "?", type_label(type));
            pat->type = error;
            return false;
        }
        pat->symbol = variant->decl ? variant->decl->symbol : NULL;
        pat->type = subject;
        if (pat->as.pat_is.type) {
            pat->as.pat_is.type->symbol = pat->symbol;
            pat->as.pat_is.type->type = (JogType *)type;
        }
        return true;
    }
    if (pat->kind == JOG_NODE_PAT_VARIANT) {
        const char *name = pat->as.pat_variant.name;
        if (type->kind == JOG_TY_OPTION && name && strcmp(name, "Some") == 0) {
            JogNode *field;
            if (pat->as.pat_variant.fields.count != 1) {
                jog_check_fail(ck, pat->pos, "Some takes one field");
                pat->type = error;
                return false;
            }
            field = list_at(pat->as.pat_variant.fields, 0);
            if (!field || field->kind != JOG_NODE_PAT_FIELD) {
                jog_check_fail(ck, field ? field->pos : pat->pos, "Some takes one field");
                pat->type = error;
                return false;
            }
            if (field->as.pat_field.name && strcmp(field->as.pat_field.name, "value") != 0) {
                jog_check_fail(ck, field->pos, "unknown field '%s'", field->as.pat_field.name);
                pat->type = error;
                return false;
            }
            pat->type = subject;
            return check_pattern(ck, job, scope, field->as.pat_field.pattern, type->as.elem.elem, false, depth + 1);
        }
        if (type->kind == JOG_TY_OPTION && name && strcmp(name, "None") == 0) {
            if (pat->as.pat_variant.fields.count != 0) {
                jog_check_fail(ck, pat->pos, "None has no fields");
                pat->type = error;
                return false;
            }
            pat->type = subject;
            return true;
        }
        if (type->kind == JOG_TY_ENUM) {
            int index = find_variant(type, name);
            const JogVariant *variant = variant_at(type, index);
            if (index < 0 || !variant) {
                jog_check_fail(ck, pat->pos, "unknown variant '%s'", name ? name : "?");
                pat->type = error;
                return false;
            }
            pat->symbol = variant->decl ? variant->decl->symbol : NULL;
            pat->type = subject;
            return check_payload_fields(ck, job, scope, pat, variant->name, variant->fields, variant->field_count, depth);
        }
        if (type->kind == JOG_TY_STRUCT && name && type->as.nominal.name && strcmp(name, type->as.nominal.name) == 0) {
            pat->type = subject;
            return check_payload_fields(ck, job, scope, pat, type->as.nominal.name, type->as.nominal.fields,
                                        type->as.nominal.field_count, depth);
        }
        jog_check_fail(ck, pat->pos, "unknown variant '%s'", name ? name : "?");
        pat->type = error;
        return false;
    }
    jog_check_fail(ck, pat->pos, "pattern is not checked yet");
    pat->type = error;
    return false;
}

/* Rows of a constructor matrix. A column is NULL when that position is a
   wildcard. One fully-wild row covers the product, including a recursive
   payload, so a wildcard does not unfold the type forever. Other rows are
   split by constructor: A(One, Two) does not help A(Two, One). */
typedef struct CovRow {
    JogNode **cols;
    size_t n;
} CovRow;

static void *cov_alloc(Check *ck, size_t bytes, size_t align) {
    void *p;
    if (!ck || ck->stop || bytes == 0) {
        return NULL;
    }
    p = ct_arena_alloc(ck->program->arena, bytes, align);
    if (!p) {
        jog_check_oom(ck);
    }
    return p;
}

static bool col_wild(const JogNode *pat, const JogType *type) {
    type = peel((JogType *)type);
    if (!pat || pat->kind == JOG_NODE_PAT_WILDCARD) {
        return true;
    }
    if (pat->kind != JOG_NODE_PAT_BIND) {
        return false;
    }
    if (pat->symbol && pat->symbol->kind == JOG_SYM_VARIANT) {
        return false;
    }
    if (type && type->kind == JOG_TY_OPTION && pat->as.ident.name && strcmp(pat->as.ident.name, "None") == 0) {
        return false;
    }
    return true;
}

static int variant_by_symbol(const JogType *type, const JogSymbol *symbol) {
    size_t i;
    if (!type || !symbol || type->kind != JOG_TY_ENUM || !type->as.nominal.variants) {
        return -1;
    }
    for (i = 0; i < type->as.nominal.variant_count; i++) {
        JogNode *decl = type->as.nominal.variants[i].decl;
        if (decl && (symbol->decl == decl || decl->symbol == symbol)) {
            return (int)i;
        }
    }
    return -1;
}

/* Payload-free `Empty` and `Shape.Empty` name one variant. A payload
   constructor expression is one value, not a cover of that variant. */
static int pat_ctor_index(const JogNode *pat, const JogType *en) {
    int index;
    if (!pat || !en || en->kind != JOG_TY_ENUM) {
        return -1;
    }
    if (pat->kind == JOG_NODE_PAT_VARIANT) {
        index = find_variant(en, pat->as.pat_variant.name);
        if (index < 0) {
            index = variant_by_symbol(en, pat->symbol);
        }
        return index;
    }
    if (pat->kind == JOG_NODE_PAT_IS && pat->symbol) {
        return variant_by_symbol(en, pat->symbol);
    }
    if (pat->kind == JOG_NODE_PAT_BIND && pat->symbol && pat->symbol->kind == JOG_SYM_VARIANT) {
        return variant_by_symbol(en, pat->symbol);
    }
    if (pat->kind == JOG_NODE_PAT_EXPR && pat->as.pat_expr.expr && pat->as.pat_expr.expr->symbol &&
        pat->as.pat_expr.expr->symbol->kind == JOG_SYM_VARIANT) {
        index = variant_by_symbol(en, pat->as.pat_expr.expr->symbol);
        if (index < 0 || !payload_free(variant_at(en, index))) {
            return -1;
        }
        return index;
    }
    return -1;
}

static bool fill_pat_slots(Check *ck, const JogNode *pat, const JogField *fields, size_t count, JogNode **out) {
    unsigned char *filled;
    size_t positional = 0;
    size_t i;
    if (!pat || pat->kind != JOG_NODE_PAT_VARIANT) {
        return false;
    }
    if (count == 0) {
        return pat->as.pat_variant.fields.count == 0;
    }
    if (!fields || !out) {
        return false;
    }
    filled = cov_alloc(ck, count, 1);
    if (!filled) {
        return false;
    }
    memset(filled, 0, count);
    memset(out, 0, count * sizeof(*out));
    for (i = 0; i < pat->as.pat_variant.fields.count; i++) {
        JogNode *field = list_at(pat->as.pat_variant.fields, i);
        int slot = -1;
        if (!field || field->kind != JOG_NODE_PAT_FIELD) {
            return false;
        }
        if (field->as.pat_field.name) {
            size_t f;
            for (f = 0; f < count; f++) {
                if (fields[f].name && strcmp(fields[f].name, field->as.pat_field.name) == 0) {
                    slot = (int)f;
                    break;
                }
            }
        } else {
            while (positional < count && filled[positional]) {
                positional++;
            }
            if (positional < count) {
                slot = (int)positional++;
            }
        }
        if (slot < 0 || filled[slot]) {
            return false;
        }
        filled[slot] = 1;
        out[slot] = field->as.pat_field.pattern;
    }
    for (i = 0; i < count; i++) {
        if (!filled[i]) {
            return false;
        }
    }
    return true;
}

static JogNode **row_cols(Check *ck, JogNode **prefix, size_t nprefix, const CovRow *row) {
    size_t ntail = row && row->n > 0 ? row->n - 1 : 0;
    size_t n = nprefix + ntail;
    JogNode **cols;
    if (n == 0 || !ck || ck->stop) {
        return NULL;
    }
    cols = cov_alloc(ck, n * sizeof(*cols), _Alignof(JogNode *));
    if (!cols) {
        return NULL;
    }
    if (nprefix > 0) {
        if (prefix) {
            memcpy(cols, prefix, nprefix * sizeof(*cols));
        } else {
            memset(cols, 0, nprefix * sizeof(*cols));
        }
    }
    if (ntail > 0 && row && row->cols) {
        memcpy(cols + nprefix, row->cols + 1, ntail * sizeof(*cols));
    } else if (ntail > 0) {
        memset(cols + nprefix, 0, ntail * sizeof(*cols));
    }
    return cols;
}

static bool rows_cover(Check *ck, CovRow *rows, size_t nrows, JogType **types, size_t ncols, int depth);

static bool row_covers_all(const CovRow *row, JogType **types, size_t ncols) {
    size_t c;
    if (!row || row->n < ncols) {
        return false;
    }
    if (ncols == 0) {
        return true;
    }
    if (!row->cols) {
        return false;
    }
    for (c = 0; c < ncols; c++) {
        if (!col_wild(row->cols[c], types ? types[c] : NULL)) {
            return false;
        }
    }
    return true;
}

static bool rows_cover_variant(Check *ck, CovRow *rows, size_t nrows, const JogType *en, size_t index, JogType **tail_types,
                               size_t ntail, int depth) {
    const JogVariant *variant;
    CovRow *out;
    JogType **next_types;
    JogNode **slots = NULL;
    size_t nout = 0;
    size_t i;
    size_t nf;
    size_t ncols2;
    size_t t;
    if (!ck || ck->stop || !en || en->kind != JOG_TY_ENUM || index >= en->as.nominal.variant_count) {
        return false;
    }
    if (depth > JOG_ENUM_DEPTH) {
        return false;
    }
    variant = variant_at(en, (int)index);
    if (!variant) {
        return false;
    }
    nf = variant->field_count;
    if (nf > 0) {
        slots = cov_alloc(ck, nf * sizeof(*slots), _Alignof(JogNode *));
        if (!slots) {
            return false;
        }
    }
    out = cov_alloc(ck, (nrows > 0 ? nrows : 1) * sizeof(*out), _Alignof(CovRow));
    if (!out) {
        return false;
    }
    for (i = 0; i < nrows && !ck->stop; i++) {
        JogNode *pat;
        JogNode **prefix = NULL;
        size_t nprefix = 0;
        size_t nt;
        if (!rows || rows[i].n == 0 || !rows[i].cols) {
            continue;
        }
        pat = rows[i].cols[0];
        nt = rows[i].n - 1;
        if (col_wild(pat, en)) {
            nprefix = nf;
        } else {
            int ctor = pat_ctor_index(pat, en);
            if (ctor != (int)index) {
                continue;
            }
            if (nf == 0 || pat->kind != JOG_NODE_PAT_VARIANT) {
                nprefix = nf;
            } else if (!fill_pat_slots(ck, pat, variant->fields, nf, slots)) {
                if (ck->stop) {
                    return false;
                }
                continue;
            } else {
                nprefix = nf;
                prefix = slots;
            }
        }
        out[nout].n = nprefix + nt;
        out[nout].cols = row_cols(ck, prefix, nprefix, &rows[i]);
        if (out[nout].n > 0 && !out[nout].cols) {
            return false;
        }
        nout++;
    }
    if (ck->stop) {
        return false;
    }
    ncols2 = nf + ntail;
    if (ncols2 == 0) {
        return nout > 0;
    }
    next_types = cov_alloc(ck, ncols2 * sizeof(*next_types), _Alignof(JogType *));
    if (!next_types) {
        return false;
    }
    for (t = 0; t < nf; t++) {
        next_types[t] = variant->fields ? variant->fields[t].type : NULL;
    }
    for (t = 0; t < ntail; t++) {
        next_types[nf + t] = tail_types ? tail_types[t] : NULL;
    }
    return rows_cover(ck, out, nout, next_types, ncols2, depth + 1);
}

static bool rows_cover(Check *ck, CovRow *rows, size_t nrows, JogType **types, size_t ncols, int depth) {
    JogType *type;
    size_t r;
    if (!ck || ck->stop) {
        return false;
    }
    if (ncols == 0) {
        return nrows > 0;
    }
    if (depth > JOG_ENUM_DEPTH) {
        return false;
    }
    if (nrows == 0 || !rows || !types || !types[0]) {
        return false;
    }
    for (r = 0; r < nrows; r++) {
        if (row_covers_all(&rows[r], types, ncols)) {
            return true;
        }
    }
    type = peel(types[0]);
    if (!type) {
        return false;
    }
    if (type->kind == JOG_TY_ENUM) {
        size_t v;
        if (type->as.nominal.variant_count == 0) {
            return false;
        }
        for (v = 0; v < type->as.nominal.variant_count; v++) {
            if (!rows_cover_variant(ck, rows, nrows, type, v, types + 1, ncols - 1, depth)) {
                return false;
            }
        }
        return !ck->stop;
    }
    if (type->kind == JOG_TY_OPTION) {
        CovRow *some_rows;
        CovRow *none_rows;
        JogType **some_types;
        size_t some_n = 0;
        size_t none_n = 0;
        size_t i;
        size_t t;
        some_rows = cov_alloc(ck, nrows * sizeof(*some_rows), _Alignof(CovRow));
        none_rows = cov_alloc(ck, nrows * sizeof(*none_rows), _Alignof(CovRow));
        if (!some_rows || !none_rows) {
            return false;
        }
        for (i = 0; i < nrows && !ck->stop; i++) {
            JogNode *pat = rows[i].n > 0 && rows[i].cols ? rows[i].cols[0] : NULL;
            size_t nt = rows[i].n > 0 ? rows[i].n - 1 : 0;
            if (col_wild(pat, type)) {
                JogNode *wild = NULL;
                some_rows[some_n].n = 1 + nt;
                some_rows[some_n].cols = row_cols(ck, &wild, 1, &rows[i]);
                if (!some_rows[some_n].cols) {
                    return false;
                }
                some_n++;
                none_rows[none_n].n = nt;
                none_rows[none_n].cols = nt ? row_cols(ck, NULL, 0, &rows[i]) : NULL;
                if (nt > 0 && !none_rows[none_n].cols) {
                    return false;
                }
                none_n++;
                continue;
            }
            if (pat && pat->kind == JOG_NODE_PAT_VARIANT && pat->as.pat_variant.name &&
                strcmp(pat->as.pat_variant.name, "Some") == 0 && pat->as.pat_variant.fields.count == 1) {
                JogNode *field = list_at(pat->as.pat_variant.fields, 0);
                JogNode *inner = field && field->kind == JOG_NODE_PAT_FIELD ? field->as.pat_field.pattern : NULL;
                some_rows[some_n].n = 1 + nt;
                some_rows[some_n].cols = row_cols(ck, &inner, 1, &rows[i]);
                if (!some_rows[some_n].cols) {
                    return false;
                }
                some_n++;
                continue;
            }
            if (pat && ((pat->kind == JOG_NODE_PAT_BIND && pat->as.ident.name && strcmp(pat->as.ident.name, "None") == 0 &&
                         !(pat->symbol && pat->symbol->kind == JOG_SYM_VARIANT)) ||
                        (pat->kind == JOG_NODE_PAT_EXPR && pat->as.pat_expr.expr &&
                         pat->as.pat_expr.expr->kind == JOG_NODE_NULL_LIT) ||
                        (pat->kind == JOG_NODE_PAT_VARIANT && pat->as.pat_variant.name &&
                         strcmp(pat->as.pat_variant.name, "None") == 0 && pat->as.pat_variant.fields.count == 0))) {
                none_rows[none_n].n = nt;
                none_rows[none_n].cols = nt ? row_cols(ck, NULL, 0, &rows[i]) : NULL;
                if (nt > 0 && !none_rows[none_n].cols) {
                    return false;
                }
                none_n++;
            }
        }
        if (ck->stop) {
            return false;
        }
        some_types = cov_alloc(ck, ncols * sizeof(*some_types), _Alignof(JogType *));
        if (!some_types) {
            return false;
        }
        some_types[0] = type->as.elem.elem;
        for (t = 1; t < ncols; t++) {
            some_types[t] = types[t];
        }
        if (!rows_cover(ck, some_rows, some_n, some_types, ncols, depth + 1)) {
            return false;
        }
        return rows_cover(ck, none_rows, none_n, ncols > 1 ? types + 1 : NULL, ncols - 1, depth + 1);
    }
    if (type->kind == JOG_TY_STRUCT) {
        CovRow *out;
        JogType **next_types;
        JogNode **slots = NULL;
        size_t nout = 0;
        size_t i;
        size_t nf = type->as.nominal.field_count;
        size_t ncols2;
        size_t t;
        const char *sname = type->as.nominal.name;
        if (nf > 0) {
            slots = cov_alloc(ck, nf * sizeof(*slots), _Alignof(JogNode *));
            if (!slots) {
                return false;
            }
        }
        out = cov_alloc(ck, nrows * sizeof(*out), _Alignof(CovRow));
        if (!out) {
            return false;
        }
        for (i = 0; i < nrows && !ck->stop; i++) {
            JogNode *pat = rows[i].n > 0 && rows[i].cols ? rows[i].cols[0] : NULL;
            JogNode **prefix = NULL;
            size_t nprefix = 0;
            size_t nt;
            if (col_wild(pat, type)) {
                nprefix = nf;
            } else if (pat && pat->kind == JOG_NODE_PAT_VARIANT && sname && pat->as.pat_variant.name &&
                       strcmp(pat->as.pat_variant.name, sname) == 0) {
                if (!fill_pat_slots(ck, pat, type->as.nominal.fields, nf, slots)) {
                    if (ck->stop) {
                        return false;
                    }
                    continue;
                }
                nprefix = nf;
                prefix = nf ? slots : NULL;
            } else {
                continue;
            }
            nt = rows[i].n > 0 ? rows[i].n - 1 : 0;
            out[nout].n = nprefix + nt;
            out[nout].cols = row_cols(ck, prefix, nprefix, &rows[i]);
            if (out[nout].n > 0 && !out[nout].cols) {
                return false;
            }
            nout++;
        }
        if (ck->stop) {
            return false;
        }
        ncols2 = nf + ncols - 1;
        if (ncols2 == 0) {
            return nout > 0;
        }
        next_types = cov_alloc(ck, ncols2 * sizeof(*next_types), _Alignof(JogType *));
        if (!next_types) {
            return false;
        }
        for (t = 0; t < nf; t++) {
            next_types[t] = type->as.nominal.fields ? type->as.nominal.fields[t].type : NULL;
        }
        for (t = 1; t < ncols; t++) {
            next_types[nf + t - 1] = types[t];
        }
        return rows_cover(ck, out, nout, next_types, ncols2, depth + 1);
    }
    {
        CovRow *out = cov_alloc(ck, nrows * sizeof(*out), _Alignof(CovRow));
        size_t nout = 0;
        size_t i;
        if (!out) {
            return false;
        }
        for (i = 0; i < nrows; i++) {
            JogNode *pat = rows[i].n > 0 && rows[i].cols ? rows[i].cols[0] : NULL;
            size_t nt;
            if (!col_wild(pat, type)) {
                continue;
            }
            nt = rows[i].n > 0 ? rows[i].n - 1 : 0;
            out[nout].n = nt;
            out[nout].cols = nt ? row_cols(ck, NULL, 0, &rows[i]) : NULL;
            if (nt > 0 && !out[nout].cols) {
                return false;
            }
            nout++;
        }
        return rows_cover(ck, out, nout, ncols > 1 ? types + 1 : NULL, ncols - 1, depth);
    }
}

static bool rows_from_pats(Check *ck, JogNode **pats, size_t n, CovRow **out) {
    CovRow *rows;
    size_t i;
    *out = NULL;
    if (n == 0) {
        return true;
    }
    rows = cov_alloc(ck, n * sizeof(*rows), _Alignof(CovRow));
    if (!rows) {
        return false;
    }
    for (i = 0; i < n; i++) {
        JogNode **slot = cov_alloc(ck, sizeof(*slot), _Alignof(JogNode *));
        if (!slot) {
            return false;
        }
        slot[0] = pats ? pats[i] : NULL;
        rows[i].cols = slot;
        rows[i].n = 1;
    }
    *out = rows;
    return true;
}

static bool patterns_cover(Check *ck, JogNode **pats, size_t n, JogType *type) {
    CovRow *rows = NULL;
    JogType *types[1];
    if (!type || n == 0) {
        return false;
    }
    if (!rows_from_pats(ck, pats, n, &rows) || ck->stop) {
        return false;
    }
    types[0] = type;
    return rows_cover(ck, rows, n, types, 1, 0);
}

static bool patterns_cover_variant(Check *ck, JogNode **pats, size_t n, JogType *en, size_t index) {
    CovRow *rows = NULL;
    if (n == 0) {
        return false;
    }
    if (!rows_from_pats(ck, pats, n, &rows) || ck->stop) {
        return false;
    }
    return rows_cover_variant(ck, rows, n, en, index, NULL, 0, 0);
}

static bool when_has_else(const JogNode *node) {
    size_t a;
    if (!node) {
        return false;
    }
    for (a = 0; a < node->as.when.arms.count; a++) {
        JogNode *arm = list_at(node->as.when.arms, a);
        if (arm && arm->as.when_arm.conds.count == 0) {
            return true;
        }
    }
    return false;
}

static JogNode **when_patterns(Check *ck, const JogNode *node, size_t *n) {
    size_t a;
    size_t total = 0;
    size_t k = 0;
    JogNode **pats;
    *n = 0;
    if (!node) {
        return NULL;
    }
    for (a = 0; a < node->as.when.arms.count; a++) {
        JogNode *arm = list_at(node->as.when.arms, a);
        if (arm) {
            total += arm->as.when_arm.conds.count;
        }
    }
    if (total == 0) {
        return NULL;
    }
    pats = cov_alloc(ck, total * sizeof(*pats), _Alignof(JogNode *));
    if (!pats) {
        return NULL;
    }
    for (a = 0; a < node->as.when.arms.count; a++) {
        JogNode *arm = list_at(node->as.when.arms, a);
        size_t c;
        if (!arm) {
            continue;
        }
        for (c = 0; c < arm->as.when_arm.conds.count; c++) {
            pats[k++] = list_at(arm->as.when_arm.conds, c);
        }
    }
    *n = k;
    return pats;
}

static void report_enum_gaps(Check *ck, JogNode *node, JogType *en, bool *bad) {
    JogNode **pats;
    size_t n = 0;
    size_t v;
    if (!ck || !node || !en || ck->stop || when_has_else(node)) {
        return;
    }
    pats = when_patterns(ck, node, &n);
    if (ck->stop) {
        return;
    }
    for (v = 0; v < en->as.nominal.variant_count; v++) {
        const JogVariant *variant;
        if (patterns_cover_variant(ck, pats, n, en, v)) {
            continue;
        }
        if (ck->stop) {
            return;
        }
        variant = variant_at(en, (int)v);
        jog_check_fail(ck, node->pos, "when does not cover '%s'", variant && variant->name ? variant->name : "?");
        if (bad) {
            *bad = true;
        }
    }
}

static bool patterns_cover_some(Check *ck, JogNode **pats, size_t n, JogType *option) {
    JogNode **inners;
    size_t i;
    size_t m = 0;
    if (!option) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (pats && col_wild(pats[i], option)) {
            return true;
        }
    }
    if (n == 0) {
        return false;
    }
    inners = cov_alloc(ck, n * sizeof(*inners), _Alignof(JogNode *));
    if (!inners) {
        return false;
    }
    for (i = 0; i < n; i++) {
        JogNode *pat = pats ? pats[i] : NULL;
        JogNode *field;
        JogNode *inner;
        if (!pat || pat->kind != JOG_NODE_PAT_VARIANT || !pat->as.pat_variant.name ||
            strcmp(pat->as.pat_variant.name, "Some") != 0 || pat->as.pat_variant.fields.count != 1) {
            continue;
        }
        field = list_at(pat->as.pat_variant.fields, 0);
        inner = field && field->kind == JOG_NODE_PAT_FIELD ? field->as.pat_field.pattern : NULL;
        if (!inner) {
            continue;
        }
        inners[m++] = inner;
    }
    if (m == 0) {
        return false;
    }
    return patterns_cover(ck, inners, m, option->as.elem.elem);
}

static bool patterns_cover_none(JogNode **pats, size_t n, const JogType *option) {
    size_t i;
    for (i = 0; i < n; i++) {
        JogNode *pat = pats ? pats[i] : NULL;
        if (col_wild(pat, option)) {
            return true;
        }
        if (!pat) {
            continue;
        }
        if (pat->kind == JOG_NODE_PAT_BIND && pat->as.ident.name && strcmp(pat->as.ident.name, "None") == 0 &&
            !(pat->symbol && pat->symbol->kind == JOG_SYM_VARIANT)) {
            return true;
        }
        if (pat->kind == JOG_NODE_PAT_EXPR && pat->as.pat_expr.expr && pat->as.pat_expr.expr->kind == JOG_NODE_NULL_LIT) {
            return true;
        }
        if (pat->kind == JOG_NODE_PAT_VARIANT && pat->as.pat_variant.name && strcmp(pat->as.pat_variant.name, "None") == 0 &&
            pat->as.pat_variant.fields.count == 0) {
            return true;
        }
    }
    return false;
}

static void report_option_gaps(Check *ck, JogNode *node, JogType *option, bool *bad) {
    JogNode **pats;
    size_t n = 0;
    if (!ck || !node || !option || ck->stop || when_has_else(node)) {
        return;
    }
    pats = when_patterns(ck, node, &n);
    if (ck->stop) {
        return;
    }
    if (!patterns_cover_some(ck, pats, n, option)) {
        if (ck->stop) {
            return;
        }
        jog_check_fail(ck, node->pos, "when does not cover 'Some'");
        if (bad) {
            *bad = true;
        }
    }
    if (!patterns_cover_none(pats, n, option)) {
        jog_check_fail(ck, node->pos, "when does not cover 'None'");
        if (bad) {
            *bad = true;
        }
    }
}


static bool arm_is_total(const JogNode *arm, const JogType *subject, bool no_subject) {
    size_t i;
    if (!arm) {
        return false;
    }
    if (arm->as.when_arm.conds.count == 0) {
        return true;
    }
    if (no_subject) {
        return false;
    }
    for (i = 0; i < arm->as.when_arm.conds.count; i++) {
        JogNode *pat = list_at(arm->as.when_arm.conds, i);
        if (pat && pat->kind == JOG_NODE_PAT_WILDCARD) {
            return true;
        }
        if (pat && pat_covers_type(pat, subject) && pat->kind == JOG_NODE_PAT_BIND &&
            (!pat->symbol || pat->symbol->kind != JOG_SYM_VARIANT)) {
            if (!(subject && peel((JogType *)subject)->kind == JOG_TY_OPTION && pat->as.ident.name &&
                  strcmp(pat->as.ident.name, "None") == 0)) {
                return true;
            }
        }
    }
    return false;
}

static void unify_arm(Check *ck, JogPos pos, JogType **result, JogType *got, bool *bad) {
    if (!got || is_error(got)) {
        *bad = true;
        return;
    }
    if (!*result || is_error(*result)) {
        *result = got;
        return;
    }
    if (!same_type(*result, got)) {
        mismatch(ck, pos, *result, got);
        *bad = true;
    }
}

JogType *jog_check_enum_when(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType *target, bool as_value) {
    JogType *error;
    JogType *subject = NULL;
    JogType *result = NULL;
    JogBlockScope *parent;
    bool bad = false;
    bool no_subject;
    bool all_end = true;
    bool saw_else = false;
    size_t i;
    if (!ck || !node) {
        return NULL;
    }
    error = error_type(ck);
    if (!error) {
        return NULL;
    }
    if (node->as.when.arms.count == 0) {
        jog_check_fail(ck, node->pos, "when needs an arm");
        node->type = error;
        return error;
    }
    no_subject = node->as.when.subject == NULL;
    if (!no_subject) {
        subject = jog_check_expr(ck, job, scope, node->as.when.subject, NULL);
        if (ck->stop) {
            return NULL;
        }
        if (!subject || is_error(subject)) {
            bad = true;
            subject = NULL;
        }
    }
    parent = scope ? scope : jog_check_body_scope(job);
    for (i = 0; i < node->as.when.arms.count && !ck->stop; i++) {
        JogNode *arm = list_at(node->as.when.arms, i);
        JogBlockScope *child;
        JogType *arm_ty;
        bool reaches = true;
        bool is_else;
        size_t c;
        bool or_bind = false;
        if (!arm || arm->kind != JOG_NODE_WHEN_ARM) {
            bad = true;
            continue;
        }
        is_else = arm->as.when_arm.conds.count == 0;
        if (is_else) {
            if (saw_else || i + 1 != node->as.when.arms.count) {
                jog_check_fail(ck, arm->pos, "else must be the last arm");
                bad = true;
            }
            saw_else = true;
        }
        child = jog_block_scope_new(ck->program->arena, parent, job && job->package ? job->package->scope : NULL);
        if (!child) {
            jog_check_oom(ck);
            return NULL;
        }
        for (c = 0; c < arm->as.when_arm.conds.count && !ck->stop; c++) {
            JogNode *pat = list_at(arm->as.when_arm.conds, c);
            if (!check_pattern(ck, job, parent, pat, subject, no_subject, 0)) {
                bad = true;
            }
            if (arm->as.when_arm.conds.count > 1 && arm_has_value_bind(pat)) {
                or_bind = true;
            }
        }
        if (or_bind) {
            jog_check_fail(ck, arm->pos, "or-pattern cannot bind a name");
            bad = true;
        }
        jog_check_body_set_scope(job, child);
        if (!or_bind && arm->as.when_arm.conds.count == 1) {
            bind_pattern(ck, job, list_at(arm->as.when_arm.conds, 0));
        }
        {
            JogIfaceNarrow narrow;
            JogNode *only = NULL;
            if (!or_bind && arm->as.when_arm.conds.count == 1) {
                only = list_at(arm->as.when_arm.conds, 0);
            }
            jog_iface_narrow_pat(ck, node->as.when.subject, only, &narrow);
            arm_ty = jog_check_when_arm(ck, job, child, arm->as.when_arm.body, as_value ? (result ? result : target) : NULL,
                                        as_value, &reaches);
            jog_iface_narrow_leave(&narrow);
        }
        jog_check_body_set_scope(job, parent);
        if (ck->stop) {
            return NULL;
        }
        if (as_value) {
            unify_arm(ck, arm->pos, &result, arm_ty, &bad);
        }
        arm->type = as_value ? arm_ty : jog_check_unit_type(ck);
        if (reaches) {
            all_end = false;
        }
    }
    if (ck->stop) {
        return NULL;
    }
    if (subject && peel(subject) && peel(subject)->kind == JOG_TY_ENUM) {
        report_enum_gaps(ck, node, peel(subject), &bad);
        if (ck->stop) {
            return NULL;
        }
    } else if (subject && peel(subject) && peel(subject)->kind == JOG_TY_OPTION) {
        report_option_gaps(ck, node, peel(subject), &bad);
        if (ck->stop) {
            return NULL;
        }
    } else if (as_value) {
        bool total = false;
        for (i = 0; i < node->as.when.arms.count; i++) {
            if (arm_is_total(list_at(node->as.when.arms, i), subject, no_subject)) {
                total = true;
                break;
            }
        }
        if (!total) {
            jog_check_fail(ck, node->pos, "when needs an else");
            bad = true;
        }
    }
    if (bad || !as_value) {
        node->type = as_value ? error : jog_check_unit_type(ck);
        if (!as_value && !bad && all_end) {
            bool exhaustive = true;
            if (!(subject && peel(subject) && (peel(subject)->kind == JOG_TY_ENUM || peel(subject)->kind == JOG_TY_OPTION))) {
                exhaustive = false;
                for (i = 0; i < node->as.when.arms.count; i++) {
                    if (arm_is_total(list_at(node->as.when.arms, i), subject, no_subject)) {
                        exhaustive = true;
                        break;
                    }
                }
            }
            if (exhaustive) {
                jog_check_body_unreachable(job);
            }
        }
        return node->type;
    }
    if (subject && peel(subject) && (peel(subject)->kind == JOG_TY_ENUM || peel(subject)->kind == JOG_TY_OPTION) && all_end) {
        jog_check_body_unreachable(job);
    } else if (all_end) {
        jog_check_body_unreachable(job);
    }
    node->type = result ? result : error;
    return node->type;
}
