#include "commands.h"
#include "phases.h"
#include "wasm_host.h"

#include "da.h"
#include "fs.h"
#include "proc.h"
#include "str.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* interp.c. Zero keeps the budget lowering passes. A positive value is
   --comptime-steps and replaces that budget for this process. */
extern void jog_interp_set_step_budget(uint64_t budget);

#if defined(_WIN32)
#define JOG_EXE_SUFFIX ".exe"
#else
#define JOG_EXE_SUFFIX ""
#endif

/* build, run, check, emit-c, and emit-ir share one pipeline:
   jog_load, jog_check, jog_lower, jog_emit_c, jog_cc_build.
   Each command stops after the phase it exists to show. A phase that
   reports an error is the last one. At most JOG_DIAG_LIMIT diagnostics
   are printed. See docs/plans/V1Compiler.md. */

enum { JOG_DIAG_LIMIT = 50 };

typedef enum PipelineStop {
    STOP_CHECK = 0,
    STOP_IR,
    STOP_C,
    STOP_BUILD,
    STOP_RUN
} PipelineStop;

typedef struct CmdRequest {
    PipelineStop stop;
    const char *package; /* NULL means the current directory */
    const char *output;  /* NULL means <module root>/out/<name> */
    JogBuildMode mode;
    int keep_c;
    int saw_mode;
    int saw_output;
    int saw_keep;
    char **run_argv;
    int run_argc;
    int include_tests; /* load *.test.jog */
    int match_expects; /* compare stdout with // expect: lines in test files */
    int target_wasm;   /* --target wasm32; output ends in .wasm */
    int saw_target;
    uint64_t comptime_steps; /* 0 when --comptime-steps was omitted */
    int saw_comptime;
    int diag_json; /* jog check --diag=json writes one JSON object per line */
    int saw_diag;
    const char **modules; /* --module values, "<name>=<dir>", in order; malloc */
    int module_count;
} CmdRequest;

static const char *usage_line(PipelineStop stop) {
    switch (stop) {
    case STOP_BUILD:
        return "usage: jog build [package] [-o file] [--release | --release-fast] [--keep-c] [--target wasm32] [--comptime-steps N] [--module name=dir ...]\n";
    case STOP_RUN:
        return "usage: jog run [package] [--target wasm32] [--comptime-steps N] [--module name=dir ...] [-- args...]\n";
    case STOP_CHECK:
        return "usage: jog check [package] [--diag=text|json] [--module name=dir ...]\n";
    case STOP_C:
        return "usage: jog emit-c [package] [--module name=dir ...]\n";
    case STOP_IR:
        return "usage: jog emit-ir [package] [--module name=dir ...]\n";
    }
    return "usage: jog <command> [arguments]\n";
}

static int usage(PipelineStop stop) {
    fputs(usage_line(stop), stderr);
    return 2;
}

static int parse_step_budget(const char *text, uint64_t *out) {
    uint64_t n = 0;
    const char *p;
    if (!text || text[0] == '\0' || !out) {
        return 0;
    }
    for (p = text; *p; p++) {
        unsigned digit;
        if (*p < '0' || *p > '9') {
            return 0;
        }
        digit = (unsigned)(*p - '0');
        if (n > (UINT64_MAX - digit) / 10) {
            return 0;
        }
        n = n * 10u + digit;
    }
    if (n == 0) {
        return 0;
    }
    *out = n;
    return 1;
}

/* "<name>=<dir>" with both sides non-empty. The loader checks the name and
   the folder. */
static int module_value_ok(const char *value) {
    const char *eq = value ? strchr(value, '=') : NULL;
    return eq && eq != value && eq[1] != '\0';
}

/* --module <name>=<dir> or --module=<name>=<dir> at argv[*i]. 1 when arg
   is not --module, 0 when it was consumed, 2 on bad usage. */
int jog_parse_module_flag(int argc, char **argv, int *i, const char **out) {
    const char *arg = argv[*i];
    const char *value = NULL;
    if (strcmp(arg, "--module") == 0) {
        if (*i + 1 >= argc) {
            fputs("jog: --module expects <name>=<dir>\n", stderr);
            return 2;
        }
        value = argv[++*i];
    } else if (strncmp(arg, "--module=", 9) == 0) {
        value = arg + 9;
    } else {
        return 1;
    }
    if (!module_value_ok(value)) {
        fprintf(stderr, "jog: --module expects <name>=<dir>, got '%s'\n", value);
        return 2;
    }
    *out = value;
    return 0;
}

static int parse_args(PipelineStop stop, int argc, char **argv, CmdRequest *req) {
    int i = 0;
    memset(req, 0, sizeof(*req));
    req->stop = stop;
    req->mode = JOG_BUILD_DEBUG;
    req->modules = malloc(((size_t)argc + 1) * sizeof(const char *));
    if (!req->modules) {
        fputs("jog: out of memory\n", stderr);
        return 1;
    }
    for (i = 0; i < argc; i++) {
        const char *arg = argv[i];
        const char *module = NULL;
        int flag = jog_parse_module_flag(argc, argv, &i, &module);
        if (flag == 2) {
            return 2;
        }
        if (flag == 0) {
            req->modules[req->module_count++] = module;
            continue;
        }
        if (strcmp(arg, "--") == 0) {
            if (stop == STOP_RUN) {
                req->run_argv = argv + i + 1;
                req->run_argc = argc - i - 1;
                return 0;
            }
            if (i + 1 != argc) {
                return usage(stop);
            }
            return 0;
        }
        if (arg[0] != '-') {
            if (req->package) {
                return usage(stop);
            }
            req->package = arg;
            continue;
        }
        if (strcmp(arg, "--target") == 0 || strncmp(arg, "--target=", 9) == 0) {
            const char *value = NULL;
            if (stop != STOP_BUILD && stop != STOP_RUN) {
                return usage(stop);
            }
            if (req->saw_target) {
                return usage(stop);
            }
            if (arg[8] == '=') {
                value = arg + 9;
            } else {
                if (i + 1 >= argc) {
                    return usage(stop);
                }
                value = argv[++i];
            }
            if (!value || strcmp(value, "wasm32") != 0) {
                fprintf(stderr, "jog: unknown target '%s'\n", value ? value : "");
                return 2;
            }
            req->saw_target = 1;
            req->target_wasm = 1;
            continue;
        }
        if (strcmp(arg, "--comptime-steps") == 0 || strncmp(arg, "--comptime-steps=", 17) == 0) {
            const char *value = NULL;
            if (req->saw_comptime) {
                return usage(stop);
            }
            if (arg[0] != '\0' && strncmp(arg, "--comptime-steps=", 17) == 0) {
                value = arg + 17;
            } else {
                if (i + 1 >= argc) {
                    return usage(stop);
                }
                value = argv[++i];
            }
            if (!parse_step_budget(value, &req->comptime_steps)) {
                fprintf(stderr, "jog: --comptime-steps expects a positive integer\n");
                return 2;
            }
            req->saw_comptime = 1;
            continue;
        }
        if (strncmp(arg, "--diag=", 7) == 0) {
            const char *value = arg + 7;
            if (stop != STOP_CHECK || req->saw_diag) {
                return usage(stop);
            }
            req->saw_diag = 1;
            if (strcmp(value, "json") == 0) {
                req->diag_json = 1;
            } else if (strcmp(value, "text") == 0) {
                req->diag_json = 0;
            } else {
                fprintf(stderr, "jog: unknown diagnostic format '%s'\n", value);
                return 2;
            }
            continue;
        }
        if (stop != STOP_BUILD) {
            return usage(stop);
        }
        if (strcmp(arg, "--release") == 0) {
            if (req->saw_mode) {
                return usage(stop);
            }
            req->saw_mode = 1;
            req->mode = JOG_BUILD_RELEASE;
            continue;
        }
        if (strcmp(arg, "--release-fast") == 0) {
            if (req->saw_mode) {
                return usage(stop);
            }
            req->saw_mode = 1;
            req->mode = JOG_BUILD_RELEASE_FAST;
            continue;
        }
        if (strcmp(arg, "--keep-c") == 0) {
            if (req->saw_keep) {
                return usage(stop);
            }
            req->saw_keep = 1;
            req->keep_c = 1;
            continue;
        }
        if (strcmp(arg, "-o") == 0) {
            if (req->saw_output || i + 1 >= argc) {
                return usage(stop);
            }
            req->saw_output = 1;
            req->output = argv[++i];
            continue;
        }
        return usage(stop);
    }
    return 0;
}

static int ensure_dir(const char *path) {
    if (!path || path[0] == '\0') {
        return 0;
    }
    if (ct_fs_is_dir(path)) {
        return 1;
    }
    if (ct_fs_mkdir(path)) {
        return 1;
    }
    return ct_fs_is_dir(path);
}

/* Create dir and any missing parents. dir itself is not modified. */
static int mkdir_p(CtArena *arena, const char *dir) {
    char *copy = NULL;
    size_t len = 0;
    size_t i = 0;
    if (!dir || dir[0] == '\0') {
        return 0;
    }
    if (ct_fs_is_dir(dir)) {
        return 1;
    }
    len = strlen(dir);
    copy = ct_arena_alloc(arena, len + 1, 1);
    if (!copy) {
        return 0;
    }
    memcpy(copy, dir, len + 1);
    if (len >= 2 && copy[1] == ':') {
        i = 2;
    }
    while (i < len && (copy[i] == '/' || copy[i] == '\\')) {
        i++;
    }
    for (; i < len; i++) {
        if (copy[i] == '/' || copy[i] == '\\') {
            copy[i] = '\0';
            if (copy[0] != '\0' && !ensure_dir(copy)) {
                return 0;
            }
            copy[i] = '/';
        }
    }
    return ensure_dir(dir);
}

static int ensure_parent(CtArena *arena, const char *path) {
    size_t len = 0;
    size_t slash = 0;
    char *parent = NULL;
    if (!path || path[0] == '\0') {
        return 0;
    }
    len = strlen(path);
    slash = len;
    while (slash > 0 && path[slash - 1] != '/' && path[slash - 1] != '\\') {
        slash--;
    }
    if (slash <= 1) {
        return 1;
    }
    parent = ct_arena_alloc(arena, slash, 1);
    if (!parent) {
        return 0;
    }
    memcpy(parent, path, slash - 1);
    parent[slash - 1] = '\0';
    return mkdir_p(arena, parent);
}

static int path_ends_with(const char *path, const char *suffix) {
    size_t nlen = 0;
    size_t slen = 0;
    if (!path || !suffix) {
        return 0;
    }
    nlen = strlen(path);
    slen = strlen(suffix);
    if (nlen < slen) {
        return 0;
    }
    return memcmp(path + nlen - slen, suffix, slen) == 0;
}

static char *suffix_name(CtArena *arena, const char *name, const char *suffix) {
    size_t nlen = 0;
    size_t slen = 0;
    char *out = NULL;
    if (!arena || !name || !suffix) {
        return NULL;
    }
    nlen = strlen(name);
    slen = strlen(suffix);
    out = ct_arena_alloc(arena, nlen + slen + 1, 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, name, nlen);
    memcpy(out + nlen, suffix, slen + 1);
    return out;
}

static int write_file(const char *path, const char *data, size_t len) {
    FILE *fp = NULL;
    if (!path || !data) {
        return 0;
    }
    fp = ct_fs_fopen(path, "wb");
    if (!fp) {
        return 0;
    }
    if (fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        return 0;
    }
    return fclose(fp) == 0;
}

static int read_line(FILE *fp, char **buf, size_t *cap) {
    size_t n = 0;
    int ch = 0;
    while ((ch = fgetc(fp)) != EOF) {
        if (n + 2 > *cap) {
            size_t next = *cap == 0 ? 128 : *cap * 2;
            char *grown = NULL;
            if (next < *cap) {
                return -1;
            }
            grown = realloc(*buf, next);
            if (!grown) {
                return -1;
            }
            *buf = grown;
            *cap = next;
        }
        (*buf)[n++] = (char)ch;
        if (ch == '\n') {
            break;
        }
    }
    if (n == 0) {
        return 0;
    }
    (*buf)[n] = '\0';
    return 1;
}

/* ct_diag_print writes every message. The driver shows the first
   JOG_DIAG_LIMIT lines and counts the rest. */
static void print_diagnostics(const CtDiag *diag, FILE *out) {
    FILE *tmp = NULL;
    char *line = NULL;
    size_t cap = 0;
    size_t shown = 0;
    size_t hidden = 0;
    int rc = 0;
    if (!diag || !out) {
        return;
    }
    tmp = tmpfile();
    if (!tmp) {
        ct_diag_print(diag, out);
        return;
    }
    ct_diag_print(diag, tmp);
    if (fflush(tmp) != 0 || fseek(tmp, 0, SEEK_SET) != 0) {
        fclose(tmp);
        ct_diag_print(diag, out);
        return;
    }
    while ((rc = read_line(tmp, &line, &cap)) == 1) {
        if (shown < JOG_DIAG_LIMIT) {
            fputs(line, out);
            shown++;
        } else {
            hidden++;
        }
    }
    free(line);
    fclose(tmp);
    if (rc < 0) {
        fputs("error: could not print every diagnostic\n", out);
    } else if (hidden > 0) {
        fprintf(out, "error: %zu more diagnostic%s not shown\n", hidden, hidden == 1 ? "" : "s");
    }
    fflush(out);
}

static void free_package_tokens(JogPackage *pkg) {
    size_t i = 0;
    if (!pkg) {
        return;
    }
    for (i = 0; i < pkg->file_count; i++) {
        if (pkg->files && pkg->files[i]) {
            ct_da_free(&pkg->files[i]->tokens);
        }
    }
    for (i = 0; i < pkg->platform_file_count; i++) {
        if (pkg->platform_files && pkg->platform_files[i]) {
            ct_da_free(&pkg->platform_files[i]->tokens);
        }
    }
}

static void free_program_tokens(JogProgram *program) {
    size_t i = 0;
    if (!program) {
        return;
    }
    if (program->packages) {
        for (i = 0; i < program->package_count; i++) {
            free_package_tokens(program->packages[i]);
        }
    }
    free_package_tokens(program->root);
}

static int phase_ok(const CtDiag *diag, size_t before, int succeeded) {
    return succeeded && ct_diag_error_count(diag) == before;
}

static int print_text(CtSb *sb) {
    const char *text = NULL;
    if (!sb || sb->failed) {
        return 0;
    }
    text = ct_sb_cstr(sb);
    if (fputs(text, stdout) < 0 || fflush(stdout) != 0) {
        return 0;
    }
    return 1;
}

typedef struct WasmImportNote {
    const char *symbol;
    const char *module;
} WasmImportNote;

static CtPos pos_of(JogPos pos) {
    CtPos out;
    out.file = pos.file;
    out.line = pos.line;
    out.column = pos.column;
    return out;
}

static int c_ident_token(const char *text) {
    const char *p;
    if (!text || text[0] == '\0') {
        return 0;
    }
    if (!((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z') || text[0] == '_')) {
        return 0;
    }
    for (p = text + 1; *p; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
    }
    return 1;
}

static int wasm_module_token(const char *text) {
    const char *p;
    size_t n = 0;
    if (!text || text[0] == '\0') {
        return 0;
    }
    for (p = text; *p; p++) {
        char c = *p;
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                 c == '-';
        if (!ok) {
            return 0;
        }
        n++;
    }
    return n < 128;
}

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

/* The module named by one @link("..."), or NULL when the node has none.
   *bad is set when an @link is present and not one safe string literal. */
static const char *attr_link_module(CtDiag *diag, JogNode *node, int *bad) {
    size_t i;
    const char *found = NULL;
    if (!node || !bad) {
        return NULL;
    }
    for (i = 0; i < node->attrs.count; i++) {
        JogNode *attr = list_at(node->attrs, i);
        JogNode *arg;
        JogNode *value;
        const char *text;
        if (!attr || attr->kind != JOG_NODE_ATTRIBUTE || !attr->as.attribute.name ||
            strcmp(attr->as.attribute.name, "link") != 0) {
            continue;
        }
        if (found) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos_of(attr->pos), "@link is set more than once");
            *bad = 1;
            return NULL;
        }
        arg = attr->as.attribute.args.count == 1 ? list_at(attr->as.attribute.args, 0) : NULL;
        value = arg && arg->kind == JOG_NODE_ARG && arg->as.arg.name == NULL ? arg->as.arg.value : NULL;
        text = value && value->kind == JOG_NODE_STRING_LIT ? value->as.literal.text : NULL;
        if (!attr->as.attribute.has_parens || !text) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos_of(attr->pos), "@link expects one string literal");
            *bad = 1;
            return NULL;
        }
        if (!wasm_module_token(text)) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos_of(attr->pos),
                         "@link module '%s' is not a Wasm import name", text);
            *bad = 1;
            return NULL;
        }
        found = text;
    }
    return found;
}

static int note_import(CtDiag *diag, CtDa *notes, const char *symbol, const char *module, JogPos pos) {
    size_t i;
    WasmImportNote note;
    if (!c_ident_token(symbol)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos_of(pos), "extern function '%s' cannot be a Wasm import",
                     symbol ? symbol : "");
        return 0;
    }
    for (i = 0; i < notes->count; i++) {
        WasmImportNote *have = ct_da_at(notes, i);
        if (!have || !have->symbol || strcmp(have->symbol, symbol) != 0) {
            continue;
        }
        if (!have->module || strcmp(have->module, module) != 0) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos_of(pos), "extern function '%s' is linked as '%s' and '%s'", symbol,
                         have->module ? have->module : "", module);
            return 0;
        }
        return 1;
    }
    note.symbol = symbol;
    note.module = module;
    if (!ct_da_push(notes, &note)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos_of(pos), "out of memory");
        return 0;
    }
    return 1;
}

static int collect_wasm_imports(JogProgram *program, CtDa *notes) {
    size_t p;
    size_t f;
    size_t d;
    size_t n;
    if (!program || !notes) {
        return 0;
    }
    for (p = 0; p < program->package_count; p++) {
        JogPackage *pkg = program->packages ? program->packages[p] : NULL;
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count; f++) {
            JogSourceFile *file = pkg->files ? pkg->files[f] : NULL;
            JogNode *root = file ? file->ast : NULL;
            if (!root || root->kind != JOG_NODE_FILE) {
                continue;
            }
            for (d = 0; d < root->as.file.decls.count; d++) {
                JogNode *decl = list_at(root->as.file.decls, d);
                const char *block_mod;
                int bad = 0;
                if (!decl || decl->kind != JOG_NODE_EXTERN_BLOCK) {
                    continue;
                }
                block_mod = attr_link_module(program->diag, decl, &bad);
                if (bad) {
                    return 0;
                }
                for (n = 0; n < decl->as.extern_block.fns.count; n++) {
                    JogNode *fn = list_at(decl->as.extern_block.fns, n);
                    const char *fn_mod;
                    const char *mod;
                    bad = 0;
                    if (!fn || fn->kind != JOG_NODE_FN) {
                        continue;
                    }
                    fn_mod = attr_link_module(program->diag, fn, &bad);
                    if (bad) {
                        return 0;
                    }
                    mod = fn_mod ? fn_mod : block_mod;
                    if (!mod) {
                        continue;
                    }
                    if (!note_import(program->diag, notes, fn->as.fn.name, mod, fn->pos)) {
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}

static int prepend_wasm_imports(JogProgram *program, CtSb *c_text) {
    CtDa notes;
    CtSb header;
    CtPos pos = {0};
    size_t i;
    int ready;
    if (!program || !c_text) {
        return 0;
    }
    ready = ct_da_init(&notes, sizeof(WasmImportNote)) ? 1 : 0;
    if (!ready) {
        ct_da_free(&notes);
        ct_diag_emit(program->diag, CT_SEVERITY_ERROR, pos, "out of memory");
        return 0;
    }
    if (!collect_wasm_imports(program, &notes)) {
        ct_da_free(&notes);
        return 0;
    }
    if (notes.count == 0) {
        ct_da_free(&notes);
        return 1;
    }
    ct_sb_init(&header);
    for (i = 0; i < notes.count; i++) {
        WasmImportNote *note = ct_da_at(&notes, i);
        if (!note || !ct_sb_appendf(&header, "/* jog-wasm-link %s %s */\n", note->symbol, note->module)) {
            ct_sb_free(&header);
            ct_da_free(&notes);
            ct_diag_emit(program->diag, CT_SEVERITY_ERROR, pos, "out of memory");
            return 0;
        }
    }
    if (c_text->len > 0 && c_text->data && !ct_sb_append(&header, c_text->data, c_text->len)) {
        ct_sb_free(&header);
        ct_da_free(&notes);
        ct_diag_emit(program->diag, CT_SEVERITY_ERROR, pos, "out of memory");
        return 0;
    }
    if (header.failed || !header.data) {
        ct_sb_free(&header);
        ct_da_free(&notes);
        ct_diag_emit(program->diag, CT_SEVERITY_ERROR, pos, "out of memory");
        return 0;
    }
    ct_sb_free(c_text);
    *c_text = header;
    ct_da_free(&notes);
    return 1;
}

static int link_program(JogProgram *program, const CmdRequest *req, CtSb *c_text, const char **exe_out) {
    CtArena *arena = program->arena;
    CtDiag *diag = program->diag;
    CtPos pos = {0};
    const char *out_dir = NULL;
    const char *c_name = NULL;
    const char *c_path = NULL;
    const char *exe_name = NULL;
    const char *exe_path = NULL;
    JogCcRequest request;
    if (!program->module_root || !program->module_name) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "missing module root");
        return 0;
    }
    out_dir = ct_path_join(arena, program->module_root, "out");
    c_name = suffix_name(arena, program->module_name, ".c");
    c_path = out_dir && c_name ? ct_path_join(arena, out_dir, c_name) : NULL;
    if (req->output) {
        exe_path = req->output;
    } else {
        exe_name = suffix_name(arena, program->module_name, req->target_wasm ? ".wasm" : JOG_EXE_SUFFIX);
        exe_path = out_dir && exe_name ? ct_path_join(arena, out_dir, exe_name) : NULL;
    }
    if (!out_dir || !c_path || !exe_path) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        return 0;
    }
    if (req->target_wasm && !path_ends_with(exe_path, ".wasm")) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "wasm32 output '%s' must end in .wasm", exe_path);
        return 0;
    }
    if (!req->target_wasm && path_ends_with(exe_path, ".wasm")) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "output '%s' ends in .wasm; pass --target wasm32", exe_path);
        return 0;
    }
    pos.file = c_path;
    if (!mkdir_p(arena, out_dir) || (req->output && !ensure_parent(arena, exe_path))) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot create output directory");
        return 0;
    }
    if (req->target_wasm && !prepend_wasm_imports(program, c_text)) {
        return 0;
    }
    if (!write_file(c_path, c_text->data ? c_text->data : "", c_text->len)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot write '%s'", c_path);
        return 0;
    }
    memset(&request, 0, sizeof request);
    request.c_path = c_path;
    request.exe_path = exe_path;
    request.runtime_root = program->runtime_root;
    request.mode = req->mode;
    request.extra_libs = (const char *const *)program->link_libs;
    request.extra_lib_count = program->link_lib_count;
    request.verbose = false;
    if (!jog_cc_build(&request, diag)) {
        return 0;
    }
    if (!req->keep_c) {
        ct_fs_remove(c_path);
    }
    *exe_out = exe_path;
    return 1;
}

static int run_linked(const char *exe, char **args, int nargs, const char *stdout_path, int *code) {
    CtDa argv;
    const char *slot = NULL;
    int i = 0;
    int ok = 0;
    if (!ct_da_init(&argv, sizeof(const char *))) {
        return 0;
    }
    slot = exe;
    if (!ct_da_push(&argv, &slot)) {
        ct_da_free(&argv);
        return 0;
    }
    for (i = 0; i < nargs; i++) {
        slot = args[i];
        if (!ct_da_push(&argv, &slot)) {
            ct_da_free(&argv);
            return 0;
        }
    }
    slot = NULL;
    if (!ct_da_push(&argv, &slot)) {
        ct_da_free(&argv);
        return 0;
    }
    ok = ct_proc_run(argv.data, stdout_path, NULL, code);
    ct_da_free(&argv);
    return ok;
}

static int line_is_expect(const char *line, size_t len, const char **text, size_t *text_len) {
    const char *prefix = "// expect:";
    size_t plen = strlen(prefix);
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (len - i < plen || memcmp(line + i, prefix, plen) != 0) {
        return 0;
    }
    i += plen;
    if (i < len && line[i] == ' ') {
        i++;
    }
    *text = line + i;
    *text_len = len - i;
    return 1;
}

static char *dup_range(CtArena *arena, const char *text, size_t len) {
    char *out = ct_arena_alloc(arena, len + 1, 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

/* // expect: lines from *.test.jog, in package and file order. */
static int collect_expects(JogProgram *program, CtDa *out) {
    size_t p;
    size_t f;
    if (!program || !out) {
        return 0;
    }
    for (p = 0; p < program->package_count; p++) {
        JogPackage *pkg = program->packages ? program->packages[p] : NULL;
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count; f++) {
            JogSourceFile *file = pkg->files ? pkg->files[f] : NULL;
            const char *data;
            size_t size;
            size_t i = 0;
            if (!file || !file->is_test || !file->file.data) {
                continue;
            }
            data = file->file.data;
            size = file->file.size;
            while (i < size) {
                size_t start = i;
                size_t len;
                const char *text = NULL;
                size_t text_len = 0;
                char *copy;
                while (i < size && data[i] != '\n' && data[i] != '\r') {
                    i++;
                }
                len = i - start;
                if (line_is_expect(data + start, len, &text, &text_len)) {
                    copy = dup_range(program->arena, text, text_len);
                    if (!copy || !ct_da_push(out, &copy)) {
                        return 0;
                    }
                }
                if (i < size && data[i] == '\r') {
                    i++;
                }
                if (i < size && data[i] == '\n') {
                    i++;
                }
            }
        }
    }
    return 1;
}

static char *read_whole(const char *path) {
    FILE *fp;
    long n;
    char *buf;
    if (!path) {
        return NULL;
    }
    fp = ct_fs_fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    n = ftell(fp);
    if (n < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (n > 0 && fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    buf[n] = '\0';
    fclose(fp);
    return buf;
}

static void strip_cr(char *text) {
    char *read;
    char *write;
    if (!text) {
        return;
    }
    read = text;
    write = text;
    while (*read) {
        if (*read != '\r') {
            *write++ = *read;
        }
        read++;
    }
    *write = '\0';
}

/* A trailing newline ends the last line and does not add an empty one. */
static int split_lines(char *text, CtDa *out) {
    size_t n;
    size_t i = 0;
    if (!text) {
        return 0;
    }
    n = strlen(text);
    while (i < n) {
        size_t start = i;
        char *line;
        while (i < n && text[i] != '\n') {
            i++;
        }
        if (i < n) {
            text[i] = '\0';
        }
        line = text + start;
        if (!ct_da_push(out, &line)) {
            return 0;
        }
        if (i < n) {
            i++;
        }
    }
    return 1;
}

static void print_lines(FILE *fp, const char *title, CtDa *lines) {
    size_t i;
    fprintf(fp, "%s (%zu)\n", title, lines->count);
    for (i = 0; i < lines->count; i++) {
        const char *line = *(const char **)ct_da_at(lines, i);
        fprintf(fp, "  |%s|\n", line ? line : "");
    }
}

static int expects_match(CtDa *expect, char *stdout_text) {
    CtDa got;
    size_t i;
    int ready;
    int same;
    ready = ct_da_init(&got, sizeof(char *)) ? 1 : 0;
    if (!ready || !split_lines(stdout_text, &got)) {
        if (ready) {
            ct_da_free(&got);
        }
        return 0;
    }
    same = got.count == expect->count;
    for (i = 0; same && i < got.count; i++) {
        const char *want = *(const char **)ct_da_at(expect, i);
        const char *have = *(char **)ct_da_at(&got, i);
        if (!want || !have || strcmp(want, have) != 0) {
            same = 0;
        }
    }
    if (!same) {
        print_lines(stderr, "expected", expect);
        print_lines(stderr, "actual", &got);
    }
    ct_da_free(&got);
    return same;
}

static int decl_is_test(const JogNode *fn) {
    size_t i;
    if (!fn || fn->kind != JOG_NODE_FN) {
        return 0;
    }
    for (i = 0; i < fn->attrs.count; i++) {
        const JogNode *attr = fn->attrs.items ? fn->attrs.items[i] : NULL;
        if (attr && attr->kind == JOG_NODE_ATTRIBUTE && attr->as.attribute.name &&
            strcmp(attr->as.attribute.name, "test") == 0) {
            return 1;
        }
    }
    return 0;
}

static int program_has_test(const JogProgram *program) {
    size_t p;
    size_t f;
    size_t d;
    if (!program) {
        return 0;
    }
    for (p = 0; p < program->package_count; p++) {
        JogPackage *pkg = program->packages ? program->packages[p] : NULL;
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count; f++) {
            JogSourceFile *source = pkg->files ? pkg->files[f] : NULL;
            JogNode *root = source ? source->ast : NULL;
            if (!root || root->kind != JOG_NODE_FILE) {
                continue;
            }
            for (d = 0; d < root->as.file.decls.count; d++) {
                const JogNode *decl = root->as.file.decls.items ? root->as.file.decls.items[d] : NULL;
                if (decl_is_test(decl)) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int compile_pipeline(const CmdRequest *req) {
    CtArena *arena = NULL;
    CtDiag *diag = NULL;
    CtIntern *intern = NULL;
    JogProgram program;
    JirModule *module = NULL;
    CtSb emitted;
    const char *start = NULL;
    const char *exe_path = NULL;
    const char *capture_path = NULL;
    size_t before = 0;
    int status = 1;
    int ran = 0;
    int run_code = 0;
    int expects_ready = 0;
    CtDa expects;
    char *captured = NULL;

    memset(&program, 0, sizeof program);
    ct_sb_init(&emitted);
    expects_ready = ct_da_init(&expects, sizeof(char *)) ? 1 : 0;
    if (!expects_ready) {
        fprintf(stderr, "jog: out of memory\n");
        goto cleanup;
    }
    arena = ct_arena_create();
    diag = arena ? ct_diag_create(arena) : NULL;
    intern = arena ? ct_intern_create(arena) : NULL;
    if (!arena || !diag || !intern) {
        fprintf(stderr, "jog: out of memory\n");
        goto cleanup;
    }
    program.arena = arena;
    program.diag = diag;
    program.intern = intern;
    program.options.mode = req->mode;
    program.options.include_tests = req->include_tests ? true : false;
    program.options.ptr_size = req->target_wasm ? 4u : 8u;
    program.options.start_package = req->stop == STOP_CHECK && req->package != NULL;
    program.types = jog_types_create(arena, program.options.ptr_size);
    if (!program.types) {
        fprintf(stderr, "jog: out of memory\n");
        goto cleanup;
    }

    if (req->module_count > 0) {
        JogModuleArg *args = ct_arena_alloc(arena, (size_t)req->module_count * sizeof(JogModuleArg),
                                            _Alignof(JogModuleArg));
        int m;
        if (!args) {
            fprintf(stderr, "jog: out of memory\n");
            goto cleanup;
        }
        for (m = 0; m < req->module_count; m++) {
            const char *eq = strchr(req->modules[m], '=');
            args[m].name = dup_range(arena, req->modules[m], (size_t)(eq - req->modules[m]));
            args[m].dir = eq + 1;
            if (!args[m].name) {
                fprintf(stderr, "jog: out of memory\n");
                goto cleanup;
            }
        }
        program.module_args = args;
        program.module_arg_count = (size_t)req->module_count;
    }

    start = req->package ? req->package : ".";
    before = ct_diag_error_count(diag);
    if (!phase_ok(diag, before, jog_load(&program, start))) {
        goto report;
    }
    if (program.is_library && (req->stop == STOP_BUILD || (req->stop == STOP_RUN && !req->include_tests))) {
        CtPos pos = {0};
        pos.file = program.module_root;
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "module '%s' is a library; use jog check or jog test",
                     program.module_name);
        goto report;
    }

    before = ct_diag_error_count(diag);
    if (!phase_ok(diag, before, jog_check(&program))) {
        goto report;
    }
    if (req->stop == STOP_CHECK) {
        status = 0;
        goto report;
    }
    if (req->include_tests && !program_has_test(&program)) {
        fputs("no tests\n", stdout);
        fflush(stdout);
        status = 0;
        goto report;
    }

    jog_interp_set_step_budget(req->comptime_steps);
    before = ct_diag_error_count(diag);
    module = jog_lower(&program);
    if (!phase_ok(diag, before, module != NULL)) {
        goto report;
    }
    if (req->stop == STOP_IR) {
        jir_dump(module, &emitted);
        if (!print_text(&emitted)) {
            goto report;
        }
        status = 0;
        goto report;
    }

    before = ct_diag_error_count(diag);
    if (!phase_ok(diag, before, jog_emit_c(module, req->mode, &emitted, diag)) || emitted.failed) {
        goto report;
    }
    if (req->stop == STOP_C) {
        if (!print_text(&emitted)) {
            goto report;
        }
        status = 0;
        goto report;
    }

    if (!link_program(&program, req, &emitted, &exe_path)) {
        goto report;
    }
    if (req->stop == STOP_RUN) {
        CtPos pos = {0};
        const char *stdout_path = NULL;
        if (req->match_expects) {
            const char *out_dir = ct_path_join(arena, program.module_root, "out");
            const char *cap_name = suffix_name(arena, program.module_name ? program.module_name : "jog", ".testout");
            capture_path = out_dir && cap_name ? ct_path_join(arena, out_dir, cap_name) : NULL;
            if (!capture_path) {
                ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
                goto report;
            }
            stdout_path = capture_path;
            if (!collect_expects(&program, &expects)) {
                ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
                goto report;
            }
        }
        if (req->target_wasm) {
            if (!jog_wasm_run(exe_path, program.runtime_root, req->run_argv, req->run_argc, stdout_path, &run_code,
                              diag)) {
                goto report;
            }
        } else if (!run_linked(exe_path, req->run_argv, req->run_argc, stdout_path, &run_code)) {
            pos.file = exe_path;
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot run '%s'", exe_path ? exe_path : "");
            goto report;
        }
        ran = 1;
        status = run_code;
        if (req->match_expects) {
            captured = read_whole(capture_path);
            if (capture_path) {
                ct_fs_remove(capture_path);
            }
            if (!captured) {
                ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot read test output");
                status = 1;
                goto report;
            }
            strip_cr(captured);
            if (fputs(captured, stdout) < 0 || fflush(stdout) != 0) {
                status = 1;
                goto report;
            }
            if (run_code != 0) {
                ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "test exited with status %d", run_code);
                status = run_code;
            }
            if (expects.count > 0 && !expects_match(&expects, captured)) {
                ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "test output did not match // expect: lines");
                if (status == 0) {
                    status = 1;
                }
            }
        }
        goto report;
    }
    status = 0;

report:
    if (req->diag_json) {
        ct_diag_print_json(diag, stdout);
        if (fflush(stdout) != 0) {
            status = 1;
        }
    } else {
        print_diagnostics(diag, stderr);
    }
    if (status != 0 && !ran && ct_diag_error_count(diag) == 0) {
        fputs("jog: compilation failed\n", stderr);
    }
cleanup:
    free(captured);
    if (expects_ready) {
        ct_da_free(&expects);
    }
    free_program_tokens(&program);
    jir_module_destroy(module);
    jog_types_release(program.types);
    ct_sb_free(&emitted);
    ct_arena_destroy(arena);
    return status;
}

/* jog test shares the build pipeline. commands.h stays unchanged.
   modules are --module values, "<name>=<dir>". */
int jog_drive_test(const char *start, const char **modules, int module_count) {
    CmdRequest req;
    memset(&req, 0, sizeof req);
    req.stop = STOP_RUN;
    req.package = start && start[0] ? start : NULL;
    req.mode = JOG_BUILD_DEBUG;
    req.include_tests = 1;
    req.match_expects = 1;
    req.modules = modules;
    req.module_count = module_count;
    return compile_pipeline(&req);
}

static int jog_command(PipelineStop stop, int argc, char **argv) {
    CmdRequest req;
    int status = parse_args(stop, argc, argv, &req);
    if (status == 0) {
        status = compile_pipeline(&req);
    }
    free((void *)req.modules);
    return status;
}

int jog_cmd_check(int argc, char **argv) {
    return jog_command(STOP_CHECK, argc, argv);
}

int jog_cmd_emit_ir(int argc, char **argv) {
    return jog_command(STOP_IR, argc, argv);
}

int jog_cmd_emit_c(int argc, char **argv) {
    return jog_command(STOP_C, argc, argv);
}

int jog_cmd_build(int argc, char **argv) {
    return jog_command(STOP_BUILD, argc, argv);
}

int jog_cmd_run(int argc, char **argv) {
    return jog_command(STOP_RUN, argc, argv);
}
