#include "check_string.h"

#include "check_context.h"
#include "check_struct.h"
#include "resolve_package.h"

#include <stdio.h>
#include <string.h>

_Static_assert((int)JOG_STRING_FORMAT_PATH == (int)JOG_STRUCT_PATH, "format embed path");

/* fmt(buf, template) returns (str, error?). The template is a string
   literal or an interpolated string, checked the same way as a value
   interpolation except that fmt does not allocate. A user function
   named fmt is left to the ordinary call checker. */

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static bool is_error(const JogType *type) {
    return type && type->kind == JOG_TY_ERROR;
}

static void type_text(const JogType *type, char *buf, size_t cap) {
    CtSb sb;
    const char *text;
    if (!buf || cap == 0) {
        return;
    }
    buf[0] = '\0';
    ct_sb_init(&sb);
    if (type) {
        jog_type_print(type, &sb);
    }
    text = ct_sb_cstr(&sb);
    snprintf(buf, cap, "%s", text && text[0] != '\0' ? text : "?");
    ct_sb_free(&sb);
}

static JogType *poison(Check *ck, JogNode *node) {
    JogType *error = jog_check_error_type(ck);
    if (node) {
        node->type = error;
    }
    return error;
}

static const char *pkg_path(const JogPackage *pkg) {
    if (!pkg || !pkg->path) {
        return "";
    }
    return pkg->path;
}

static bool type_owned_by(const JogType *type, const JogPackage *pkg) {
    const char *tp;
    if (!type || !pkg) {
        return false;
    }
    if (type->kind != JOG_TY_STRUCT && type->kind != JOG_TY_ENUM && type->kind != JOG_TY_DISTINCT) {
        return false;
    }
    tp = type->as.nominal.package;
    if (!tp) {
        return false;
    }
    return strcmp(tp, pkg_path(pkg)) == 0;
}

static bool imports_pkg(const JogPackage *from, const char *file, const JogPackage *target) {
    size_t i;
    size_t n;
    JogNode *ast = NULL;
    if (!from || !target || !file || !from->files) {
        return false;
    }
    for (i = 0; i < from->file_count; i++) {
        JogSourceFile *source = from->files[i];
        if (!source) {
            continue;
        }
        if ((source->file.path && strcmp(source->file.path, file) == 0) ||
            (source->ast && source->ast->pos.file && strcmp(source->ast->pos.file, file) == 0)) {
            ast = source->ast;
            break;
        }
    }
    if (!ast || ast->kind != JOG_NODE_FILE) {
        return false;
    }
    for (n = 0; n < ast->as.file.imports.count; n++) {
        JogNode *import = list_at(ast->as.file.imports, n);
        if (import && import->symbol && import->symbol->package == target) {
            return true;
        }
    }
    return false;
}

static bool method_visible(const JogSymbol *method, const JogPackage *pkg, const char *file) {
    if (!method || !jog_symbol_visible_from(method, pkg, file)) {
        return false;
    }
    if (!method->package || method->package == pkg) {
        return true;
    }
    return imports_pkg(pkg, file, method->package);
}

static JogType *universe_error(JogProgram *program) {
    size_t i;
    if (!program) {
        return NULL;
    }
    for (i = 0; i < program->package_count; i++) {
        JogPackage *pkg = program->packages[i];
        JogPackageScope *scope = pkg ? pkg->scope : NULL;
        JogSymbol *sym;
        if (!scope || !scope->universe) {
            continue;
        }
        sym = jog_package_scope_lookup(scope->universe, "error");
        if (sym && sym->type) {
            return sym->type;
        }
    }
    return NULL;
}

static bool is_str_type(const JogType *type) {
    return type && type->kind == JOG_TY_DISTINCT && type->as.nominal.package == NULL && type->as.nominal.name &&
           strcmp(type->as.nominal.name, "str") == 0;
}

static bool is_mut_bytes(const JogType *type) {
    return type && type->kind == JOG_TY_BYTES && !type->is_const;
}

static bool is_error_iface(JogType *type, const JogType *universe) {
    JogType *base = jog_type_underlying(type);
    if (!base) {
        return false;
    }
    if (universe && base == universe) {
        return true;
    }
    return base->kind == JOG_TY_INTERFACE && base->as.nominal.name && strcmp(base->as.nominal.name, "error") == 0 &&
           !base->as.nominal.package;
}

static bool format_signature(const JogNode *fn, const JogType *error) {
    const JogType *ty;
    const JogType *ret;
    const JogType *opt;
    if (!fn || fn->kind != JOG_NODE_FN || !fn->type || fn->type->kind != JOG_TY_FN) {
        return false;
    }
    if (fn->as.fn.type_params.count > 0 || fn->as.fn.variadic) {
        return false;
    }
    ty = fn->type;
    if (ty->as.fn.context_count != 0 || ty->as.fn.param_count != 2 || !ty->as.fn.params) {
        return false;
    }
    if (!is_mut_bytes(ty->as.fn.params[1])) {
        return false;
    }
    ret = ty->as.fn.ret;
    if (!ret || ret->kind != JOG_TY_TUPLE || ret->as.tuple.count != 2 || !ret->as.tuple.elems) {
        return false;
    }
    if (!is_str_type(ret->as.tuple.elems[0])) {
        return false;
    }
    opt = ret->as.tuple.elems[1];
    if (!opt || opt->kind != JOG_TY_OPTION) {
        return false;
    }
    return is_error_iface(opt->as.elem.elem, error);
}

static JogType *format_owner(JogType *type) {
    if (type && type->kind == JOG_TY_PTR && type->as.elem.elem) {
        return type->as.elem.elem;
    }
    return type;
}

static bool nominal_user(const JogType *type) {
    return type && (type->kind == JOG_TY_STRUCT || type->kind == JOG_TY_ENUM || type->kind == JOG_TY_DISTINCT);
}

static void take_format(JogStringFormat *best, int *count, uint8_t *best_len, JogNode *fn, int is_ptr,
                        const uint32_t *path, uint8_t path_len) {
    if (*count == 0 || path_len < *best_len) {
        memset(best, 0, sizeof(*best));
        best->fn = fn;
        best->is_ptr = is_ptr;
        best->path_len = path_len;
        if (path_len > 0 && path) {
            memcpy(best->index, path, path_len * sizeof(uint32_t));
        }
        *best_len = path_len;
        *count = 1;
        return;
    }
    if (path_len == *best_len) {
        (*count)++;
    }
}

int jog_string_format_method(Check *ck, CheckJob *job, JogProgram *program, JogType *type, const JogPackage *pkg,
                             const char *file, JogStringFormat *out) {
    JogType *owner;
    JogType *error;
    JogStringFormat best;
    int count = 0;
    uint8_t best_len = 0;
    bool bad = false;
    bool direct_bad = false;
    size_t p;
    size_t f;
    size_t d;
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    owner = format_owner(type);
    if (!program || !nominal_user(owner)) {
        return 0;
    }
    error = universe_error(program);
    memset(&best, 0, sizeof(best));
    for (p = 0; p < program->package_count; p++) {
        JogPackage *mpkg = program->packages[p];
        if (!mpkg) {
            continue;
        }
        for (f = 0; f < mpkg->file_count; f++) {
            JogSourceFile *source = mpkg->files ? mpkg->files[f] : NULL;
            JogNode *src;
            if (!source || !source->ast || source->ast->kind != JOG_NODE_FILE) {
                continue;
            }
            src = source->ast;
            for (d = 0; d < src->as.file.decls.count; d++) {
                JogNode *decl = list_at(src->as.file.decls, d);
                JogType *on = NULL;
                int is_ptr = 0;
                uint32_t path[JOG_STRUCT_PATH];
                uint8_t path_len = 0;
                bool matched = false;
                if (!decl || decl->kind != JOG_NODE_FN || !decl->as.fn.receiver || !decl->as.fn.name ||
                    strcmp(decl->as.fn.name, "format") != 0 || !decl->symbol) {
                    continue;
                }
                if (!method_visible(decl->symbol, pkg, file)) {
                    continue;
                }
                if (ck && job && !jog_check_wait(ck, job, decl->symbol)) {
                    return -1;
                }
                if (!decl->type || decl->type->kind != JOG_TY_FN) {
                    continue;
                }
                jog_method_receiver(decl, &on, &is_ptr);
                if (!nominal_user(on) || on->as.nominal.type_arg_count > 0 ||
                    !type_owned_by(on, decl->symbol->package)) {
                    continue;
                }
                if (on == owner) {
                    matched = true;
                    path_len = 0;
                } else if (owner->kind == JOG_TY_STRUCT &&
                           jog_struct_embed_path(owner, on, path, &path_len) && path_len > 0) {
                    matched = true;
                }
                if (!matched) {
                    continue;
                }
                if (!format_signature(decl, error)) {
                    if (path_len == 0) {
                        direct_bad = true;
                    } else {
                        bad = true;
                    }
                    continue;
                }
                take_format(&best, &count, &best_len, decl, is_ptr, path, path_len);
            }
        }
    }
    if (ck && ck->stop) {
        return -1;
    }
    if (count > 1) {
        return 3;
    }
    if (direct_bad && (count == 0 || best_len > 0)) {
        return 2;
    }
    if (count == 1) {
        if (out) {
            *out = best;
        }
        return 1;
    }
    if (bad || direct_bad) {
        return 2;
    }
    return 0;
}

static bool formattable(JogType *type) {
    JogType *base = jog_type_underlying(type);
    if (!base) {
        return false;
    }
    if (base->kind == JOG_TY_BOOL || base->kind == JOG_TY_BYTES || base->kind == JOG_TY_INT ||
        base->kind == JOG_TY_UNTYPED_INT) {
        return true;
    }
    return false;
}

static JogType *check_hole(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node) {
    JogType *got;
    char buf[160];
    if (!node) {
        return NULL;
    }
    got = jog_check_expr(ck, job, scope, node, NULL);
    if (!got || ck->stop) {
        return got;
    }
    if (is_error(got)) {
        return got;
    }
    if (formattable(got)) {
        return got;
    }
    {
        const char *file = job && job->decl ? job->decl->pos.file : NULL;
        const JogPackage *pkg = job ? job->package : NULL;
        int status = jog_string_format_method(ck, job, ck->program, got, pkg, file, NULL);
        if (status < 0) {
            return NULL;
        }
        if (status == 1) {
            return got;
        }
        if (status == 2) {
            jog_check_fail(ck, node->pos, "format method must be fn format(buf: bytes): (str, error?)");
            return poison(ck, node);
        }
        if (status == 3) {
            jog_check_fail(ck, node->pos, "ambiguous format method");
            return poison(ck, node);
        }
    }
    type_text(got, buf, sizeof(buf));
    jog_check_fail(ck, node->pos, "type %s cannot be formatted", buf);
    return poison(ck, node);
}

static JogType *str_type(Check *ck) {
    return jog_type_core_str(ck->program->types);
}

static JogType *fmt_result(Check *ck, CheckJob *job) {
    JogSymbol *err_sym;
    JogType *str;
    JogType *err;
    JogType *opt;
    JogType *elems[2];
    if (!ck || !job || !job->package || !job->package->scope) {
        return NULL;
    }
    str = str_type(ck);
    err_sym = jog_file_lookup(job->package->scope, job->file_index, "error");
    err = err_sym ? err_sym->type : NULL;
    if (!str || !err) {
        jog_check_oom(ck);
        return NULL;
    }
    opt = jog_type_option(ck->program->types, err);
    if (!opt) {
        jog_check_oom(ck);
        return NULL;
    }
    elems[0] = str;
    elems[1] = opt;
    return jog_type_tuple(ck->program->types, elems, 2);
}

static bool is_fmt_name(const JogNode *callee) {
    return callee && callee->kind == JOG_NODE_IDENT && callee->as.ident.name &&
           callee->as.ident.type_args.count == 0 && strcmp(callee->as.ident.name, "fmt") == 0;
}

static int check_template(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, bool need_alloc,
                          bool *ok) {
    size_t i;
    bool good = true;
    if (!node) {
        jog_check_fail(ck, (JogPos){0}, "fmt template must be a string");
        *ok = false;
        return ck->stop ? -1 : 0;
    }
    if (node->kind == JOG_NODE_STRING_LIT) {
        JogType *got = jog_check_expr(ck, job, scope, node, NULL);
        if (!got || ck->stop) {
            return -1;
        }
        *ok = !is_error(got);
        return 0;
    }
    if (node->kind != JOG_NODE_INTERP) {
        jog_check_fail(ck, node->pos, "fmt template must be a string");
        *ok = false;
        return ck->stop ? -1 : 0;
    }
    for (i = 0; i < node->as.interp.parts.count && !ck->stop; i++) {
        JogType *got = check_hole(ck, job, scope, list_at(node->as.interp.parts, i));
        if (!got || ck->stop) {
            return -1;
        }
        if (is_error(got)) {
            good = false;
        }
    }
    if (ck->stop) {
        return -1;
    }
    if (need_alloc && job && !job->infer_collecting && !jog_check_context_has_named(job, "Allocator")) {
        jog_check_fail(ck, node->pos, "string interpolation needs an Allocator context");
        good = false;
    }
    *ok = good;
    return 0;
}

JogType *jog_check_string_interp(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node) {
    bool ok = false;
    JogType *str;
    if (!ck || !node || ck->stop) {
        return NULL;
    }
    if (check_template(ck, job, scope, node, true, &ok) < 0) {
        return NULL;
    }
    if (!ok) {
        return poison(ck, node);
    }
    str = str_type(ck);
    if (!str) {
        jog_check_oom(ck);
        return NULL;
    }
    node->type = str;
    return str;
}

int jog_check_string_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call) {
    JogNode *callee;
    JogNode *buf_arg;
    JogNode *tmpl_arg;
    JogNode *buf;
    JogNode *tmpl;
    JogType *buf_ty;
    JogType *base;
    JogType *result;
    bool tmpl_ok = false;
    size_t i;
    if (!ck || !call || call->kind != JOG_NODE_CALL || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    callee = call->as.call.callee;
    if (!is_fmt_name(callee)) {
        return 0;
    }
    if (scope && jog_block_scope_lookup(scope, "fmt")) {
        return 0;
    }
    if (call->as.call.trailing || call->as.call.args.count != 2) {
        jog_check_fail(ck, call->pos, "fmt expects a buffer and a template");
        poison(ck, call);
        return ck->stop ? -1 : 1;
    }
    for (i = 0; i < call->as.call.args.count; i++) {
        JogNode *arg = list_at(call->as.call.args, i);
        if (!arg || arg->kind != JOG_NODE_ARG || arg->as.arg.name) {
            jog_check_fail(ck, arg ? arg->pos : call->pos, "fmt expects a buffer and a template");
            poison(ck, call);
            return ck->stop ? -1 : 1;
        }
    }
    buf_arg = list_at(call->as.call.args, 0);
    tmpl_arg = list_at(call->as.call.args, 1);
    buf = buf_arg->as.arg.value;
    tmpl = tmpl_arg->as.arg.value;
    buf_ty = jog_check_expr(ck, job, scope, buf, NULL);
    if (!buf_ty || ck->stop) {
        return -1;
    }
    if (!is_error(buf_ty)) {
        base = jog_type_underlying(buf_ty);
        if (!base || base->kind != JOG_TY_BYTES || base->is_const) {
            jog_check_fail(ck, buf->pos, "fmt buffer must be bytes");
            buf_ty = poison(ck, buf);
        }
    }
    if (check_template(ck, job, scope, tmpl, false, &tmpl_ok) < 0) {
        return -1;
    }
    if (is_error(buf_ty) || !tmpl_ok) {
        return poison(ck, call) ? (ck->stop ? -1 : 1) : -1;
    }
    result = fmt_result(ck, job);
    if (!result || ck->stop) {
        return -1;
    }
    call->type = result;
    return 1;
}
