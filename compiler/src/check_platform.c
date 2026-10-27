#include "check_platform.h"

#include "os_suffix.h"

#include "str.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One actual per expect per target. The comparison is the type AST as
   written, so a per-target alias such as c.long matches by name. */

static CtPos as_pos(JogPos pos) {
    CtPos out;
    out.file = pos.file;
    out.line = pos.line;
    out.column = pos.column;
    return out;
}

static void fail(JogProgram *program, JogPos pos, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    ct_diag_emit(program->diag, CT_SEVERITY_ERROR, as_pos(pos), "%s", buf);
}

static const char *fn_name(const JogNode *fn) {
    if (!fn || !fn->as.fn.name || fn->as.fn.name[0] == '\0') {
        return "?";
    }
    return fn->as.fn.name;
}

static int path_is_os(const char *path) {
    return jog_file_os_tag(jog_path_base(path), NULL, NULL, NULL);
}

static int same_text(const char *a, const char *b) {
    if (a == b) {
        return 1;
    }
    if (!a || !b) {
        return 0;
    }
    return strcmp(a, b) == 0;
}

static int same_names(JogNames a, JogNames b) {
    size_t i;
    if (a.count != b.count) {
        return 0;
    }
    for (i = 0; i < a.count; i++) {
        const char *left = a.items ? a.items[i] : NULL;
        const char *right = b.items ? b.items[i] : NULL;
        if (!same_text(left, right)) {
            return 0;
        }
    }
    return 1;
}

static int same_expr(const JogNode *a, const JogNode *b);

static int same_expr_list(JogList a, JogList b) {
    size_t i;
    if (a.count != b.count) {
        return 0;
    }
    for (i = 0; i < a.count; i++) {
        const JogNode *left = a.items ? a.items[i] : NULL;
        const JogNode *right = b.items ? b.items[i] : NULL;
        if (!same_expr(left, right)) {
            return 0;
        }
    }
    return 1;
}

static int same_expr(const JogNode *a, const JogNode *b) {
    if (a == b) {
        return 1;
    }
    if (!a || !b || a->kind != b->kind) {
        return 0;
    }
    switch (a->kind) {
    case JOG_NODE_INT_LIT:
    case JOG_NODE_FLOAT_LIT:
    case JOG_NODE_STRING_LIT:
    case JOG_NODE_CHAR_LIT:
        return a->as.literal.len == b->as.literal.len &&
               (a->as.literal.len == 0 ||
                (a->as.literal.text && b->as.literal.text &&
                 memcmp(a->as.literal.text, b->as.literal.text, a->as.literal.len) == 0));
    case JOG_NODE_BOOL_LIT:
        return a->as.literal.value == b->as.literal.value;
    case JOG_NODE_NULL_LIT:
        return 1;
    case JOG_NODE_IDENT:
        return same_text(a->as.ident.name, b->as.ident.name) && same_expr_list(a->as.ident.type_args, b->as.ident.type_args);
    case JOG_NODE_UNARY:
        return a->as.unary.op == b->as.unary.op && same_expr(a->as.unary.operand, b->as.unary.operand);
    case JOG_NODE_BINARY:
        return a->as.binary.op == b->as.binary.op && same_expr(a->as.binary.lhs, b->as.binary.lhs) &&
               same_expr(a->as.binary.rhs, b->as.binary.rhs);
    default:
        return 0;
    }
}

static int same_type(const JogNode *a, const JogNode *b) {
    size_t i;
    if (a == b) {
        return 1;
    }
    if (!a || !b || a->kind != b->kind) {
        return 0;
    }
    switch (a->kind) {
    case JOG_NODE_TYPE_NAME:
        if (!same_names(a->as.type_name.path, b->as.type_name.path) ||
            a->as.type_name.args.count != b->as.type_name.args.count) {
            return 0;
        }
        for (i = 0; i < a->as.type_name.args.count; i++) {
            const JogNode *left = a->as.type_name.args.items ? a->as.type_name.args.items[i] : NULL;
            const JogNode *right = b->as.type_name.args.items ? b->as.type_name.args.items[i] : NULL;
            if (!same_type(left, right)) {
                return 0;
            }
        }
        return 1;
    case JOG_NODE_TYPE_PTR:
    case JOG_NODE_TYPE_SLICE:
    case JOG_NODE_TYPE_OPTION:
        return a->as.type_elem.is_const == b->as.type_elem.is_const && same_type(a->as.type_elem.elem, b->as.type_elem.elem);
    case JOG_NODE_TYPE_ARRAY:
        return a->as.type_array.infer_len == b->as.type_array.infer_len &&
               same_expr(a->as.type_array.len, b->as.type_array.len) &&
               same_type(a->as.type_array.elem, b->as.type_array.elem);
    case JOG_NODE_TYPE_BYTES:
        return a->as.type_bytes.is_const == b->as.type_bytes.is_const &&
               same_expr(a->as.type_bytes.len, b->as.type_bytes.len);
    case JOG_NODE_TYPE_TUPLE:
        if (a->as.type_tuple.elems.count != b->as.type_tuple.elems.count) {
            return 0;
        }
        for (i = 0; i < a->as.type_tuple.elems.count; i++) {
            const JogNode *left = a->as.type_tuple.elems.items ? a->as.type_tuple.elems.items[i] : NULL;
            const JogNode *right = b->as.type_tuple.elems.items ? b->as.type_tuple.elems.items[i] : NULL;
            if (!same_type(left, right)) {
                return 0;
            }
        }
        return 1;
    case JOG_NODE_TYPE_FN:
        if (a->as.type_fn.escaping != b->as.type_fn.escaping || a->as.type_fn.is_extern != b->as.type_fn.is_extern) {
            return 0;
        }
        if (a->as.type_fn.params.count != b->as.type_fn.params.count ||
            a->as.type_fn.contexts.count != b->as.type_fn.contexts.count) {
            return 0;
        }
        for (i = 0; i < a->as.type_fn.params.count; i++) {
            const JogNode *left = a->as.type_fn.params.items ? a->as.type_fn.params.items[i] : NULL;
            const JogNode *right = b->as.type_fn.params.items ? b->as.type_fn.params.items[i] : NULL;
            if (!same_type(left, right)) {
                return 0;
            }
        }
        for (i = 0; i < a->as.type_fn.contexts.count; i++) {
            const JogNode *left = a->as.type_fn.contexts.items ? a->as.type_fn.contexts.items[i] : NULL;
            const JogNode *right = b->as.type_fn.contexts.items ? b->as.type_fn.contexts.items[i] : NULL;
            if (!same_type(left, right)) {
                return 0;
            }
        }
        return same_type(a->as.type_fn.ret, b->as.type_fn.ret);
    case JOG_NODE_TYPE_UNIT:
        return 1;
    default:
        return 0;
    }
}

static void append_expr(CtSb *sb, const JogNode *node) {
    if (!node) {
        ct_sb_append_cstr(sb, "?");
        return;
    }
    switch (node->kind) {
    case JOG_NODE_INT_LIT:
    case JOG_NODE_FLOAT_LIT:
        ct_sb_append_cstr(sb, node->as.literal.text ? node->as.literal.text : "?");
        return;
    case JOG_NODE_IDENT:
        ct_sb_append_cstr(sb, node->as.ident.name ? node->as.ident.name : "?");
        return;
    default:
        ct_sb_append_cstr(sb, "?");
        return;
    }
}

static void append_type(CtSb *sb, const JogNode *node) {
    size_t i;
    if (!node) {
        ct_sb_append_cstr(sb, "()");
        return;
    }
    switch (node->kind) {
    case JOG_NODE_TYPE_NAME:
        if (node->as.type_name.path.count == 0 || !node->as.type_name.path.items) {
            ct_sb_append_cstr(sb, "?");
        }
        for (i = 0; i < node->as.type_name.path.count; i++) {
            if (i > 0) {
                ct_sb_putc(sb, '.');
            }
            ct_sb_append_cstr(sb, node->as.type_name.path.items[i] ? node->as.type_name.path.items[i] : "?");
        }
        if (node->as.type_name.args.count > 0) {
            ct_sb_putc(sb, '<');
            for (i = 0; i < node->as.type_name.args.count; i++) {
                if (i > 0) {
                    ct_sb_append_cstr(sb, ", ");
                }
                append_type(sb, node->as.type_name.args.items ? node->as.type_name.args.items[i] : NULL);
            }
            ct_sb_putc(sb, '>');
        }
        return;
    case JOG_NODE_TYPE_PTR:
        ct_sb_putc(sb, '*');
        if (node->as.type_elem.is_const) {
            ct_sb_append_cstr(sb, "const ");
        }
        append_type(sb, node->as.type_elem.elem);
        return;
    case JOG_NODE_TYPE_SLICE:
        ct_sb_append_cstr(sb, "[]");
        if (node->as.type_elem.is_const) {
            ct_sb_append_cstr(sb, "const ");
        }
        append_type(sb, node->as.type_elem.elem);
        return;
    case JOG_NODE_TYPE_OPTION:
        append_type(sb, node->as.type_elem.elem);
        ct_sb_putc(sb, '?');
        return;
    case JOG_NODE_TYPE_ARRAY:
        ct_sb_putc(sb, '[');
        if (node->as.type_array.infer_len) {
            ct_sb_putc(sb, '_');
        } else {
            append_expr(sb, node->as.type_array.len);
        }
        ct_sb_putc(sb, ']');
        append_type(sb, node->as.type_array.elem);
        return;
    case JOG_NODE_TYPE_BYTES:
        if (node->as.type_bytes.is_const) {
            ct_sb_append_cstr(sb, "const ");
        }
        ct_sb_append_cstr(sb, "bytes");
        if (node->as.type_bytes.len) {
            ct_sb_putc(sb, '[');
            append_expr(sb, node->as.type_bytes.len);
            ct_sb_putc(sb, ']');
        }
        return;
    case JOG_NODE_TYPE_TUPLE:
        ct_sb_putc(sb, '(');
        for (i = 0; i < node->as.type_tuple.elems.count; i++) {
            if (i > 0) {
                ct_sb_append_cstr(sb, ", ");
            }
            append_type(sb, node->as.type_tuple.elems.items ? node->as.type_tuple.elems.items[i] : NULL);
        }
        ct_sb_putc(sb, ')');
        return;
    case JOG_NODE_TYPE_UNIT:
        ct_sb_append_cstr(sb, "()");
        return;
    case JOG_NODE_TYPE_FN:
        ct_sb_append_cstr(sb, "fn");
        return;
    default:
        ct_sb_append_cstr(sb, "?");
        return;
    }
}

static void type_text(const JogNode *node, int pack, char *buf, size_t cap) {
    CtSb sb;
    const char *text;
    ct_sb_init(&sb);
    if (pack) {
        ct_sb_append_cstr(&sb, "...");
    }
    append_type(&sb, node);
    text = ct_sb_cstr(&sb);
    if (sb.failed || !text) {
        snprintf(buf, cap, "?");
    } else {
        snprintf(buf, cap, "%s", text);
    }
    ct_sb_free(&sb);
}

static const char *list_at_name(JogList list, size_t index) {
    JogNode *node;
    if (!list.items || index >= list.count) {
        return NULL;
    }
    node = list.items[index];
    if (!node) {
        return NULL;
    }
    if (node->kind == JOG_NODE_PARAM) {
        return node->as.param.name;
    }
    if (node->kind == JOG_NODE_TYPE_PARAM) {
        return node->as.type_param.name;
    }
    return NULL;
}

static JogNode *param_type(JogList list, size_t index) {
    JogNode *node;
    if (!list.items || index >= list.count) {
        return NULL;
    }
    node = list.items[index];
    if (!node || node->kind != JOG_NODE_PARAM) {
        return NULL;
    }
    return node->as.param.type;
}

static int param_pack(JogList list, size_t index) {
    JogNode *node;
    if (!list.items || index >= list.count) {
        return 0;
    }
    node = list.items[index];
    return node && node->kind == JOG_NODE_PARAM && node->as.param.pack;
}

static JogNode *type_param_constraint(JogList list, size_t index) {
    JogNode *node;
    if (!list.items || index >= list.count) {
        return NULL;
    }
    node = list.items[index];
    if (!node || node->kind != JOG_NODE_TYPE_PARAM) {
        return NULL;
    }
    return node->as.type_param.constraint;
}

/* 1 when the signatures differ and a diagnostic was emitted. */
static int signatures_differ(JogProgram *program, const JogNode *expect, const JogNode *actual) {
    char got[128];
    char want[128];
    size_t i;
    if (expect->as.fn.type_params.count != actual->as.fn.type_params.count) {
        fail(program, actual->pos, "actual fn '%s' does not match its expect: type parameter count is %zu, expected %zu",
             fn_name(actual), actual->as.fn.type_params.count, expect->as.fn.type_params.count);
        return 1;
    }
    for (i = 0; i < expect->as.fn.type_params.count; i++) {
        const char *left = list_at_name(expect->as.fn.type_params, i);
        const char *right = list_at_name(actual->as.fn.type_params, i);
        JogNode *lc = type_param_constraint(expect->as.fn.type_params, i);
        JogNode *rc = type_param_constraint(actual->as.fn.type_params, i);
        if (!same_text(left, right)) {
            fail(program, actual->pos, "actual fn '%s' does not match its expect: type parameter %zu is '%s', expected '%s'",
                 fn_name(actual), i + 1, right ? right : "?", left ? left : "?");
            return 1;
        }
        if (!same_type(lc, rc)) {
            type_text(rc, 0, got, sizeof got);
            type_text(lc, 0, want, sizeof want);
            fail(program, actual->pos,
                 "actual fn '%s' does not match its expect: type parameter %zu constraint is '%s', expected '%s'",
                 fn_name(actual), i + 1, rc ? got : "any", lc ? want : "any");
            return 1;
        }
    }
    if ((expect->as.fn.receiver != NULL) != (actual->as.fn.receiver != NULL) ||
        expect->as.fn.receiver_is_ptr != actual->as.fn.receiver_is_ptr ||
        !same_type(expect->as.fn.receiver, actual->as.fn.receiver)) {
        type_text(actual->as.fn.receiver, 0, got, sizeof got);
        type_text(expect->as.fn.receiver, 0, want, sizeof want);
        fail(program, actual->pos, "actual fn '%s' does not match its expect: receiver is '%s%s', expected '%s%s'",
             fn_name(actual), actual->as.fn.receiver_is_ptr ? "*" : "", actual->as.fn.receiver ? got : "none",
             expect->as.fn.receiver_is_ptr ? "*" : "", expect->as.fn.receiver ? want : "none");
        return 1;
    }
    if (expect->as.fn.params.count != actual->as.fn.params.count) {
        fail(program, actual->pos, "actual fn '%s' does not match its expect: parameter count is %zu, expected %zu",
             fn_name(actual), actual->as.fn.params.count, expect->as.fn.params.count);
        return 1;
    }
    for (i = 0; i < expect->as.fn.params.count; i++) {
        const char *left = list_at_name(expect->as.fn.params, i);
        const char *right = list_at_name(actual->as.fn.params, i);
        int lpack = param_pack(expect->as.fn.params, i);
        int rpack = param_pack(actual->as.fn.params, i);
        JogNode *lt = param_type(expect->as.fn.params, i);
        JogNode *rt = param_type(actual->as.fn.params, i);
        if (!same_text(left, right)) {
            fail(program, actual->pos, "actual fn '%s' does not match its expect: parameter %zu is named '%s', expected '%s'",
                 fn_name(actual), i + 1, right ? right : "?", left ? left : "?");
            return 1;
        }
        if (lpack != rpack || !same_type(lt, rt)) {
            type_text(rt, rpack, got, sizeof got);
            type_text(lt, lpack, want, sizeof want);
            fail(program, actual->pos, "actual fn '%s' does not match its expect: parameter %zu is '%s', expected '%s'",
                 fn_name(actual), i + 1, got, want);
            return 1;
        }
    }
    if (expect->as.fn.variadic != actual->as.fn.variadic) {
        fail(program, actual->pos, "actual fn '%s' does not match its expect: %s", fn_name(actual),
             actual->as.fn.variadic ? "it is variadic, expected a fixed parameter list"
                                    : "it has a fixed parameter list, expected variadic");
        return 1;
    }
    if (!same_type(expect->as.fn.ret, actual->as.fn.ret)) {
        type_text(actual->as.fn.ret, 0, got, sizeof got);
        type_text(expect->as.fn.ret, 0, want, sizeof want);
        fail(program, actual->pos, "actual fn '%s' does not match its expect: return type is '%s', expected '%s'",
             fn_name(actual), got, want);
        return 1;
    }
    if (expect->as.fn.contexts.count != actual->as.fn.contexts.count) {
        fail(program, actual->pos, "actual fn '%s' does not match its expect: context count is %zu, expected %zu",
             fn_name(actual), actual->as.fn.contexts.count, expect->as.fn.contexts.count);
        return 1;
    }
    for (i = 0; i < expect->as.fn.contexts.count; i++) {
        const char *left = list_at_name(expect->as.fn.contexts, i);
        const char *right = list_at_name(actual->as.fn.contexts, i);
        JogNode *lt = param_type(expect->as.fn.contexts, i);
        JogNode *rt = param_type(actual->as.fn.contexts, i);
        if (!same_text(left, right)) {
            fail(program, actual->pos, "actual fn '%s' does not match its expect: context %zu is named '%s', expected '%s'",
                 fn_name(actual), i + 1, right ? right : "?", left ? left : "?");
            return 1;
        }
        if (!same_type(lt, rt)) {
            type_text(rt, 0, got, sizeof got);
            type_text(lt, 0, want, sizeof want);
            fail(program, actual->pos, "actual fn '%s' does not match its expect: context %zu is '%s', expected '%s'",
                 fn_name(actual), i + 1, got, want);
            return 1;
        }
    }
    return 0;
}

static int marked(CtDa *seen, const JogNode *node) {
    size_t i;
    for (i = 0; i < seen->count; i++) {
        if (*(JogNode **)ct_da_at(seen, i) == node) {
            return 1;
        }
    }
    return 0;
}

static int mark(CtDa *seen, const JogNode *node) {
    return ct_da_push(seen, &node);
}

static int name_eq(const JogNode *fn, const char *name) {
    return fn && fn->kind == JOG_NODE_FN && fn->as.fn.name && name && strcmp(fn->as.fn.name, name) == 0;
}

typedef struct FnList {
    JogNode **items;
    size_t count;
} FnList;

static int push_fn(CtDa *da, JogNode *fn) {
    return ct_da_push(da, &fn);
}

static void collect_fns(JogSourceFile *source, int want_expect, CtDa *out) {
    JogNode *file;
    size_t i;
    if (!source || !source->ast || source->ast->kind != JOG_NODE_FILE) {
        return;
    }
    file = source->ast;
    for (i = 0; i < file->as.file.decls.count; i++) {
        JogNode *decl = file->as.file.decls.items ? file->as.file.decls.items[i] : NULL;
        int flag;
        if (!decl || decl->kind != JOG_NODE_FN || decl->as.fn.receiver) {
            continue;
        }
        flag = want_expect ? decl->as.fn.is_expect : decl->as.fn.is_actual;
        if (flag) {
            push_fn(out, decl);
        }
    }
}

static void place_file(JogProgram *program, JogSourceFile *source, int *bad) {
    JogNode *file;
    size_t i;
    int is_os;
    if (!source || !source->ast || source->ast->kind != JOG_NODE_FILE) {
        return;
    }
    is_os = path_is_os(source->file.path);
    file = source->ast;
    for (i = 0; i < file->as.file.decls.count; i++) {
        JogNode *decl = file->as.file.decls.items ? file->as.file.decls.items[i] : NULL;
        if (!decl || decl->kind != JOG_NODE_FN) {
            continue;
        }
        if (decl->as.fn.is_expect && is_os) {
            fail(program, decl->pos, "expect fn '%s' is only allowed in a file with no OS suffix", fn_name(decl));
            *bad = 1;
        }
        if (decl->as.fn.is_actual && !is_os) {
            fail(program, decl->pos, "actual fn '%s' is only allowed in an OS-suffixed file", fn_name(decl));
            *bad = 1;
        }
    }
}

static int expect_named(CtDa *expects, const char *name) {
    size_t i;
    for (i = 0; i < expects->count; i++) {
        JogNode *fn = *(JogNode **)ct_da_at(expects, i);
        if (name_eq(fn, name)) {
            return 1;
        }
    }
    return 0;
}

static int cmp_actual_file(const void *a, const void *b) {
    const JogNode *fa = *(JogNode *const *)a;
    const JogNode *fb = *(JogNode *const *)b;
    const char *sa = jog_path_base(fa && fa->pos.file ? fa->pos.file : "");
    const char *sb = jog_path_base(fb && fb->pos.file ? fb->pos.file : "");
    return strcmp(sa, sb);
}

static void report_twice(JogProgram *program, const JogNode *expect, const char *os, CtDa *actuals) {
    char list[512];
    size_t i;
    size_t at = 0;
    if (actuals->count == 0 || !actuals->data) {
        return;
    }
    qsort(actuals->data, actuals->count, sizeof(JogNode *), cmp_actual_file);
    list[0] = '\0';
    for (i = 0; i < actuals->count; i++) {
        JogNode *fn = *(JogNode **)ct_da_at(actuals, i);
        const char *base = jog_path_base(fn && fn->pos.file ? fn->pos.file : "");
        size_t n = strlen(base);
        size_t extra = i > 0 ? 2 : 0;
        if (at + extra + n + 1 > sizeof list) {
            break;
        }
        if (i > 0) {
            list[at++] = ',';
            list[at++] = ' ';
        }
        memcpy(list + at, base, n);
        at += n;
        list[at] = '\0';
    }
    fail(program, expect->pos, "expect fn '%s' has %zu actuals for target '%s' (%s)", fn_name(expect), actuals->count,
         os, list);
}

static void check_target(JogProgram *program, const char *os, JogSourceFile **files, size_t file_count, CtDa *expects,
                         CtDa *compared, CtDa *orphans, int *bad) {
    CtDa actuals;
    size_t e;
    size_t a;
    size_t f;
    if (!ct_da_init(&actuals, sizeof(JogNode *))) {
        fail(program, (JogPos){0}, "out of memory");
        *bad = 1;
        return;
    }
    for (f = 0; f < file_count; f++) {
        collect_fns(files ? files[f] : NULL, 0, &actuals);
    }
    for (e = 0; e < expects->count; e++) {
        JogNode *expect = *(JogNode **)ct_da_at(expects, e);
        CtDa matches = {0};
        if (!expect) {
            continue;
        }
        if (!ct_da_init(&matches, sizeof(JogNode *))) {
            fail(program, expect->pos, "out of memory");
            *bad = 1;
            ct_da_free(&actuals);
            return;
        }
        for (a = 0; a < actuals.count; a++) {
            JogNode *actual = *(JogNode **)ct_da_at(&actuals, a);
            if (name_eq(actual, expect->as.fn.name) && !ct_da_push(&matches, &actual)) {
                fail(program, expect->pos, "out of memory");
                *bad = 1;
                ct_da_free(&matches);
                ct_da_free(&actuals);
                return;
            }
        }
        if (matches.count == 0) {
            fail(program, expect->pos, "expect fn '%s' has no actual for target '%s'", fn_name(expect), os);
            *bad = 1;
        } else if (matches.count > 1) {
            report_twice(program, expect, os, &matches);
            *bad = 1;
        } else {
            JogNode *actual = *(JogNode **)ct_da_at(&matches, 0);
            if (actual && !marked(compared, actual)) {
                if (!mark(compared, actual)) {
                    fail(program, actual->pos, "out of memory");
                    *bad = 1;
                    ct_da_free(&matches);
                    ct_da_free(&actuals);
                    return;
                }
                if (signatures_differ(program, expect, actual)) {
                    *bad = 1;
                }
            }
        }
        ct_da_free(&matches);
    }
    for (a = 0; a < actuals.count; a++) {
        JogNode *actual = *(JogNode **)ct_da_at(&actuals, a);
        if (!actual || expect_named(expects, actual->as.fn.name) || marked(orphans, actual)) {
            continue;
        }
        if (!mark(orphans, actual)) {
            fail(program, actual->pos, "out of memory");
            *bad = 1;
            break;
        }
        fail(program, actual->pos, "actual fn '%s' has no expect fn", fn_name(actual));
        *bad = 1;
    }
    ct_da_free(&actuals);
}

static void current_os_files(JogPackage *pkg, const char *os, CtDa *out) {
    size_t i;
    for (i = 0; i < pkg->file_count; i++) {
        JogSourceFile *source = pkg->files ? pkg->files[i] : NULL;
        const char *tag = NULL;
        size_t tag_len = 0;
        if (!source || !source->file.path) {
            continue;
        }
        if (!jog_file_os_tag(jog_path_base(source->file.path), &tag, &tag_len, NULL)) {
            continue;
        }
        if (jog_os_tag_matches(tag, tag_len, os)) {
            ct_da_push(out, &source);
        }
    }
}

static int check_package(JogProgram *program, JogPackage *pkg) {
    const char *current = jog_target_os(program->options.ptr_size);
    CtDa expects = {0};
    CtDa compared = {0};
    CtDa orphans = {0};
    CtDa current_files = {0};
    size_t i;
    int bad = 0;
    int ready;
    if (!pkg) {
        return 1;
    }
    for (i = 0; i < pkg->file_count; i++) {
        place_file(program, pkg->files ? pkg->files[i] : NULL, &bad);
    }
    for (i = 0; i < pkg->platform_file_count; i++) {
        place_file(program, pkg->platform_files ? pkg->platform_files[i] : NULL, &bad);
    }
    ready = ct_da_init(&expects, sizeof(JogNode *)) && ct_da_init(&compared, sizeof(JogNode *)) &&
            ct_da_init(&orphans, sizeof(JogNode *)) && ct_da_init(&current_files, sizeof(JogSourceFile *));
    if (!ready) {
        fail(program, (JogPos){0}, "out of memory");
        ct_da_free(&expects);
        ct_da_free(&compared);
        ct_da_free(&orphans);
        ct_da_free(&current_files);
        return 0;
    }
    for (i = 0; i < pkg->file_count; i++) {
        JogSourceFile *source = pkg->files ? pkg->files[i] : NULL;
        if (!source || path_is_os(source->file.path)) {
            continue;
        }
        collect_fns(source, 1, &expects);
    }
    if (pkg->target_count == 0) {
        current_os_files(pkg, current, &current_files);
        check_target(program, current, (JogSourceFile **)current_files.data, current_files.count, &expects, &compared,
                     &orphans, &bad);
    } else {
        for (i = 0; i < pkg->target_count; i++) {
            JogTargetSources *group = pkg->target_sources ? &pkg->target_sources[i] : NULL;
            const char *os = group && group->os ? group->os : (pkg->targets ? pkg->targets[i] : current);
            check_target(program, os, group ? group->files : NULL, group ? group->file_count : 0, &expects, &compared,
                         &orphans, &bad);
        }
    }
    ct_da_free(&expects);
    ct_da_free(&compared);
    ct_da_free(&orphans);
    ct_da_free(&current_files);
    return !bad;
}

bool jog_check_platform(JogProgram *program) {
    size_t i;
    int ok = 1;
    if (!program || !program->diag) {
        return false;
    }
    for (i = 0; i < program->package_count; i++) {
        if (!check_package(program, program->packages ? program->packages[i] : NULL)) {
            ok = 0;
        }
    }
    return ok != 0;
}
