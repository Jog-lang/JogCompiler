#include "phases.h"

#include "fs.h"
#include "proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Find clang, then gcc, and compile the program together with the runtime.

   JOG_CC, when set and non-empty, replaces that search. A path (anything
   with a separator) is used as given; a bare name is looked up on PATH.
   An empty JOG_CC is treated as unset. ct_proc_run does not search PATH,
   so the compiler is always an existing file before it is spawned.

   debug uses -O0 -g. release and release-fast both use -O2. Leaving bounds
   checks out of release-fast is the C backend's job; it already receives
   JogBuildMode, so this command does not add a preprocessor define.

   The mismatch flag follows the compiler's file name. Clang gets
   -Wno-incompatible-library-redeclaration. Every other compiler gets gcc's
   -Wno-builtin-declaration-mismatch.

   An output path ending in .wasm is wasm32, not the host. JogCcRequest has
   no target field, so the suffix is the signal. That link is clang
   --target=wasm32-unknown-unknown plus runtime/wasm/jogrt_wasm.c and
   wasm-ld. gcc cannot build it. The driver only uses the suffix for
   --target wasm32. */

#if defined(_WIN32)
#define CC_PATH_SEP ';'
#else
#define CC_PATH_SEP ':'
#endif

static bool is_separator(char c) {
    return c == '/' || c == '\\';
}

static const char *base_name(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (is_separator(*p)) {
            base = p + 1;
        }
    }
    return base;
}

static bool has_separator(const char *path) {
    for (const char *p = path; *p; p++) {
        if (is_separator(*p)) {
            return true;
        }
    }
    return false;
}

static char lower_ascii(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static bool base_contains(const char *path, const char *needle) {
    const char *base = base_name(path);
    size_t nlen = strlen(needle);
    if (nlen == 0) {
        return true;
    }
    for (const char *p = base; *p; p++) {
        size_t j = 0;
        while (j < nlen && p[j] && lower_ascii(p[j]) == lower_ascii(needle[j])) {
            j++;
        }
        if (j == nlen) {
            return true;
        }
    }
    return false;
}

static const char *mismatch_flag(const char *compiler) {
    if (base_contains(compiler, "clang")) {
        return "-Wno-incompatible-library-redeclaration";
    }
    return "-Wno-builtin-declaration-mismatch";
}

static bool push_arg(CtDa *args, const char *arg) {
    if (!arg) {
        return false;
    }
    return ct_da_push(args, &arg);
}

/* Clang also warns when an extern fopen/fread/fwrite is missing stdio.h.
   Generated C does not include that header. */
static bool push_builtin_flags(CtDa *args, const char *compiler) {
    if (!push_arg(args, mismatch_flag(compiler))) {
        return false;
    }
    if (base_contains(compiler, "clang") && !push_arg(args, "-Wno-builtin-requires-header")) {
        return false;
    }
    return true;
}

static bool push_mode_flags(CtDa *args, JogBuildMode mode) {
    switch (mode) {
    case JOG_BUILD_DEBUG:
        return push_arg(args, "-O0") && push_arg(args, "-g");
    case JOG_BUILD_RELEASE:
    case JOG_BUILD_RELEASE_FAST:
        return push_arg(args, "-O2");
    }
    return false;
}

/* dir + name, then dir + name.exe on Windows when name has no extension.
   An empty dir uses name unchanged. */
static const char *file_in_dir(CtArena *arena, const char *dir, const char *name, bool *oom) {
    char *path = ct_path_join(arena, dir, name);
    if (!path) {
        *oom = true;
        return NULL;
    }
    if (ct_fs_is_file(path)) {
        return path;
    }
#if defined(_WIN32)
    if (!strchr(base_name(name), '.')) {
        size_t len = strlen(path);
        char *exe = ct_arena_alloc(arena, len + 5, 1);
        if (!exe) {
            *oom = true;
            return NULL;
        }
        memcpy(exe, path, len);
        memcpy(exe + len, ".exe", 5);
        if (ct_fs_is_file(exe)) {
            return exe;
        }
    }
#endif
    return NULL;
}

static const char *search_path(CtArena *arena, const char *name, bool *oom) {
    const char *path = ct_fs_getenv(arena, "PATH");
    if (!path) {
        return NULL;
    }
    const char *start = path;
    for (;;) {
        const char *end = start;
        while (*end && *end != CC_PATH_SEP) {
            end++;
        }
        char *dir = NULL;
        if (end == start) {
            dir = ct_arena_strdup(arena, ".");
        } else {
            size_t len = (size_t)(end - start);
            dir = ct_arena_alloc(arena, len + 1, 1);
            if (dir) {
                memcpy(dir, start, len);
                dir[len] = '\0';
            }
        }
        if (!dir) {
            *oom = true;
            return NULL;
        }
        const char *found = file_in_dir(arena, dir, name, oom);
        if (*oom || found) {
            return found;
        }
        if (*end == '\0') {
            break;
        }
        start = end + 1;
    }
    return NULL;
}

/* *override_out is the JOG_CC text when that variable selects the compiler,
   and NULL when clang/gcc were searched. The text is the environment's. */
static const char *find_compiler(CtArena *arena, const char **override_out, bool *oom) {
    const char *override = ct_fs_getenv(arena, "JOG_CC");
    if (override && override[0] != '\0') {
        *override_out = override;
        if (has_separator(override)) {
            return file_in_dir(arena, "", override, oom);
        }
        return search_path(arena, override, oom);
    }
    *override_out = NULL;
    const char *clang = search_path(arena, "clang", oom);
    if (*oom || clang) {
        return clang;
    }
    return search_path(arena, "gcc", oom);
}

static const char *link_flag(CtArena *arena, const char *lib) {
    size_t len = strlen(lib);
    char *flag = ct_arena_alloc(arena, len + 3, 1);
    if (!flag) {
        return NULL;
    }
    flag[0] = '-';
    flag[1] = 'l';
    memcpy(flag + 2, lib, len + 1);
    return flag;
}

static void print_command(const char *const *argv);

static bool path_ends_with(const char *path, const char *suffix) {
    size_t nlen;
    size_t slen;
    if (!path || !suffix) {
        return false;
    }
    nlen = strlen(path);
    slen = strlen(suffix);
    if (nlen < slen) {
        return false;
    }
    return memcmp(path + nlen - slen, suffix, slen) == 0;
}

/* Leading jog-wasm-link directives, one symbol and one module per line,
   name the Wasm import module for that extern. The driver writes them
   from @link. An extern with no directive stays an env import. */
enum { CC_WASM_TOKEN = 128 };

typedef struct CcWasmLink {
    char symbol[CC_WASM_TOKEN];
    char module[CC_WASM_TOKEN];
    int found;
} CcWasmLink;

static int cc_ident_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static int cc_ident_token(const char *text) {
    const char *p;
    if (!text || text[0] == '\0') {
        return 0;
    }
    if (!((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z') || text[0] == '_')) {
        return 0;
    }
    for (p = text + 1; *p; p++) {
        if (!cc_ident_char(*p)) {
            return 0;
        }
    }
    return 1;
}

static int cc_module_token(const char *text) {
    const char *p;
    size_t n = 0;
    if (!text || text[0] == '\0') {
        return 0;
    }
    for (p = text; *p; p++) {
        char c = *p;
        int ok = cc_ident_char(c) || c == '.' || c == '-';
        if (!ok) {
            return 0;
        }
        n++;
    }
    return n < CC_WASM_TOKEN;
}

static int cc_starts_with(const char *s, size_t n, const char *prefix) {
    size_t i;
    for (i = 0; prefix[i] != '\0'; i++) {
        if (i >= n || s[i] != prefix[i]) {
            return 0;
        }
    }
    return 1;
}

/* 1 parsed, 0 not a directive, -1 a directive that is not usable. */
static int cc_parse_link_directive(const char *s, size_t n, char *sym, char *mod) {
    const char *prefix = "/* jog-wasm-link ";
    size_t i;
    size_t k;
    size_t prefix_len;
    if (!cc_starts_with(s, n, prefix)) {
        return 0;
    }
    prefix_len = strlen(prefix);
    i = prefix_len;
    k = 0;
    while (i < n && s[i] != ' ' && s[i] != '\t') {
        if (k + 1 >= CC_WASM_TOKEN) {
            return -1;
        }
        sym[k++] = s[i++];
    }
    sym[k] = '\0';
    if (k == 0 || i >= n || s[i] != ' ') {
        return -1;
    }
    i++;
    k = 0;
    while (i < n && s[i] != ' ' && s[i] != '\t') {
        if (k + 1 >= CC_WASM_TOKEN) {
            return -1;
        }
        mod[k++] = s[i++];
    }
    mod[k] = '\0';
    if (k == 0) {
        return -1;
    }
    while (i < n && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    if (i + 1 >= n || s[i] != '*' || s[i + 1] != '/') {
        return -1;
    }
    i += 2;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) {
        i++;
    }
    if (i != n || !cc_ident_token(sym) || !cc_module_token(mod)) {
        return -1;
    }
    return 1;
}

static int cc_line_is_extern(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    if (n - i < 7 || memcmp(s + i, "extern", 6) != 0) {
        return 0;
    }
    return s[i + 6] == ' ' || s[i + 6] == '\t';
}

static int cc_extern_name(const char *s, size_t n, char *name) {
    size_t paren = 0;
    size_t i;
    size_t start;
    size_t len;
    int saw = 0;
    if (!cc_line_is_extern(s, n)) {
        return 0;
    }
    for (paren = 0; paren < n; paren++) {
        if (s[paren] == '(') {
            saw = 1;
            break;
        }
    }
    if (!saw || paren == 0) {
        return 0;
    }
    i = paren;
    while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t')) {
        i--;
    }
    start = i;
    while (start > 0 && cc_ident_char(s[start - 1])) {
        start--;
    }
    len = i - start;
    if (len == 0 || len + 1 > CC_WASM_TOKEN) {
        return 0;
    }
    memcpy(name, s + start, len);
    name[len] = '\0';
    return cc_ident_token(name);
}

static char *cc_read_file(const char *path, size_t *out_len) {
    FILE *fp;
    long length;
    char *buf;
    size_t got;
    fp = ct_fs_fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    length = ftell(fp);
    if (length < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = malloc((size_t)length + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    got = fread(buf, 1, (size_t)length, fp);
    fclose(fp);
    if (got != (size_t)length) {
        free(buf);
        return NULL;
    }
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

static int cc_same_link(const CtDa *links, const char *sym, const char *mod) {
    size_t i;
    for (i = 0; i < links->count; i++) {
        const CcWasmLink *have = ct_da_at((CtDa *)links, i);
        if (!have || strcmp(have->symbol, sym) != 0) {
            continue;
        }
        return strcmp(have->module, mod) == 0 ? 1 : -1;
    }
    return 0;
}

static int cc_write_all(const char *path, const char *data, size_t len);

static bool apply_wasm_import_modules(const char *path, CtDiag *diag) {
    CtPos pos = {0};
    char *text = NULL;
    size_t n = 0;
    size_t at = 0;
    CtDa links;
    CtSb out;
    int links_ready;
    int out_ready = 0;
    int ok = 0;
    pos.file = path;
    links_ready = ct_da_init(&links, sizeof(CcWasmLink)) ? 1 : 0;
    if (!links_ready) {
        ct_da_free(&links);
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        return false;
    }
    text = cc_read_file(path, &n);
    if (!text) {
        ct_da_free(&links);
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot read C input '%s'", path ? path : "");
        return false;
    }
    while (at < n) {
        size_t end = at;
        size_t len;
        int parsed;
        CcWasmLink link;
        int same;
        while (end < n && text[end] != '\n') {
            end++;
        }
        len = end - at;
        if (len > 0 && text[at + len - 1] == '\r') {
            len--;
        }
        memset(&link, 0, sizeof link);
        parsed = cc_parse_link_directive(text + at, len, link.symbol, link.module);
        if (parsed == 0) {
            break;
        }
        if (parsed < 0) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "malformed wasm import directive");
            goto done;
        }
        same = cc_same_link(&links, link.symbol, link.module);
        if (same < 0) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "extern function '%s' is linked as two Wasm import modules",
                         link.symbol);
            goto done;
        }
        if (same == 0 && !ct_da_push(&links, &link)) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
            goto done;
        }
        if (end >= n) {
            at = n;
            break;
        }
        at = end + 1;
    }
    if (links.count == 0) {
        ok = 1;
        goto done;
    }
    ct_sb_init(&out);
    out_ready = 1;
    while (at <= n) {
        size_t end = at;
        size_t len;
        int has_nl = 0;
        char name[CC_WASM_TOKEN];
        size_t i;
        if (at == n) {
            break;
        }
        while (end < n && text[end] != '\n') {
            end++;
        }
        if (end < n && text[end] == '\n') {
            has_nl = 1;
        }
        len = end - at;
        if (len > 0 && text[at + len - 1] == '\r') {
            len--;
        }
        if (cc_extern_name(text + at, len, name)) {
            for (i = 0; i < links.count; i++) {
                CcWasmLink *link = ct_da_at(&links, i);
                if (!link || strcmp(link->symbol, name) != 0) {
                    continue;
                }
                if (link->found) {
                    ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "wasm import '%s' has two extern prototypes", name);
                    goto done;
                }
                if (!ct_sb_appendf(&out, "__attribute__((import_module(\"%s\")))\n", link->module) ||
                    !ct_sb_appendf(&out, "__attribute__((import_name(\"%s\")))\n", link->symbol)) {
                    ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
                    goto done;
                }
                link->found = 1;
            }
        }
        if ((len > 0 && !ct_sb_append(&out, text + at, len)) || (has_nl && !ct_sb_putc(&out, '\n'))) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
            goto done;
        }
        if (!has_nl) {
            break;
        }
        at = end + 1;
    }
    if (out.failed) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        goto done;
    }
    for (at = 0; at < links.count; at++) {
        CcWasmLink *link = ct_da_at(&links, at);
        if (!link || !link->found) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "wasm import '%s' was not emitted as an extern",
                         link && link->symbol[0] ? link->symbol : "");
            goto done;
        }
    }
    if (!out.data || !cc_write_all(path, out.data, out.len)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot write C input '%s'", path ? path : "");
        goto done;
    }
    ok = 1;

done:
    if (out_ready) {
        ct_sb_free(&out);
    }
    ct_da_free(&links);
    free(text);
    return ok ? true : false;
}

static int cc_write_all(const char *path, const char *data, size_t len) {
    FILE *fp;
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

/* Freestanding wasm32. Undefined symbols become env imports, including
   hello's puts, unless a jog-wasm-link directive selects another module.
   The native runtime is not compiled in. */
static bool jog_cc_build_wasm(const JogCcRequest *request, CtDiag *diag) {
    CtPos pos = {0};
    CtArena *arena = NULL;
    CtDa args;
    bool args_ready = ct_da_init(&args, sizeof(const char *));
    bool ok = false;
    bool oom = false;
    const char *override = NULL;
    const char *compiler = NULL;
    const char *wasm_rt = NULL;
    const char *jogrt_main = NULL;
    const char *end = NULL;
    int exit_code = 0;

    if (!request) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot invoke the C compiler: missing request");
        goto done;
    }
    pos.file = request->c_path;
    if (!request->c_path || request->c_path[0] == '\0' || !request->exe_path || request->exe_path[0] == '\0' ||
        !request->runtime_root || request->runtime_root[0] == '\0') {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot invoke the C compiler: missing input, output, or runtime path");
        goto done;
    }
    if (request->mode != JOG_BUILD_DEBUG && request->mode != JOG_BUILD_RELEASE &&
        request->mode != JOG_BUILD_RELEASE_FAST) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot invoke the C compiler: unknown build mode");
        goto done;
    }
    if (request->extra_lib_count > 0) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "wasm32 does not link host libraries; extern functions are env imports");
        goto done;
    }

    arena = ct_arena_create();
    if (!arena || !args_ready) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        goto done;
    }

    compiler = find_compiler(arena, &override, &oom);
    if (oom) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        goto done;
    }
    if (!compiler) {
        if (override) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "C compiler '%s' from JOG_CC was not found", override);
        } else {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "C compiler not found; looked for clang, then gcc, on PATH");
        }
        goto done;
    }
    if (!base_contains(compiler, "clang")) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "wasm32 needs clang; '%s' cannot target wasm32",
                     override ? override : compiler);
        goto done;
    }

    wasm_rt = ct_path_join(arena, request->runtime_root, "wasm/jogrt_wasm.c");
    jogrt_main = ct_path_join(arena, request->runtime_root, "jogrt_main.c");
    if (!wasm_rt || !jogrt_main) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        goto done;
    }
    if (!ct_fs_is_file(request->c_path)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "C input '%s' was not found", request->c_path);
        goto done;
    }
    if (!ct_fs_is_file(wasm_rt) || !ct_fs_is_file(jogrt_main)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "runtime '%s' is missing wasm/jogrt_wasm.c or jogrt_main.c",
                     request->runtime_root);
        goto done;
    }
    if (!apply_wasm_import_modules(request->c_path, diag)) {
        goto done;
    }

    if (!push_arg(&args, compiler) || !push_arg(&args, "--target=wasm32-unknown-unknown") || !push_arg(&args, "-std=gnu17") ||
        !push_mode_flags(&args, request->mode) || !push_builtin_flags(&args, compiler) ||
        !push_arg(&args, "-ffreestanding") || !push_arg(&args, "-fno-builtin") || !push_arg(&args, "-nostdlib") ||
        !push_arg(&args, "-I") || !push_arg(&args, request->runtime_root) || !push_arg(&args, request->c_path) ||
        !push_arg(&args, wasm_rt) || !push_arg(&args, jogrt_main) || !push_arg(&args, "-Wl,--no-entry") ||
        !push_arg(&args, "-Wl,--export=main") || !push_arg(&args, "-Wl,--export=memory") ||
        !push_arg(&args, "-Wl,--allow-undefined") || !push_arg(&args, "-Wl,-z,stack-size=1048576") ||
        !push_arg(&args, "-o") || !push_arg(&args, request->exe_path)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        goto done;
    }
    if (!ct_da_push(&args, &end) || !args.data) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        goto done;
    }
    if (request->verbose) {
        print_command(args.data);
    }
    if (!ct_proc_run(args.data, NULL, NULL, &exit_code)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot run the C compiler '%s'", compiler);
        goto done;
    }
    if (exit_code != 0) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "the C compiler exited with status %d", exit_code);
        goto done;
    }
    ok = true;

done:
    ct_da_free(&args);
    ct_arena_destroy(arena);
    return ok;
}

static void print_command(const char *const *argv) {
    fputs("jog cc:", stderr);
    for (size_t i = 0; argv[i]; i++) {
        const char *arg = argv[i];
        int quote = arg[0] == '\0';
        for (const char *p = arg; *p; p++) {
            if (*p == ' ' || *p == '\t' || *p == '"') {
                quote = 1;
            }
        }
        fputc(' ', stderr);
        if (!quote) {
            fputs(arg, stderr);
            continue;
        }
        fputc('"', stderr);
        for (const char *p = arg; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '\\' || c == '"') {
                fputc('\\', stderr);
            }
            fputc(c, stderr);
        }
        fputc('"', stderr);
    }
    fputc('\n', stderr);
    fflush(stderr);
}

bool jog_cc_build(const JogCcRequest *request, CtDiag *diag) {
    CtPos pos = {0};
    if (request && path_ends_with(request->exe_path, ".wasm")) {
        return jog_cc_build_wasm(request, diag);
    }
    CtArena *arena = NULL;
    CtDa args;
    bool args_ready = ct_da_init(&args, sizeof(const char *));
    bool ok = false;
    bool oom = false;
    const char *override = NULL;
    const char *compiler = NULL;
    const char *jogrt_c = NULL;
    const char *jogrt_sched = NULL;
    const char *jogrt_switch = NULL;
    const char *jogrt_main = NULL;
    const char *end = NULL;
    int exit_code = 0;

    if (!request) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot invoke the C compiler: missing request");
        goto done;
    }
    pos.file = request->c_path;
    if (!request->c_path || request->c_path[0] == '\0' || !request->exe_path || request->exe_path[0] == '\0' ||
        !request->runtime_root || request->runtime_root[0] == '\0') {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot invoke the C compiler: missing input, output, or runtime path");
        goto done;
    }
    if (request->mode != JOG_BUILD_DEBUG && request->mode != JOG_BUILD_RELEASE &&
        request->mode != JOG_BUILD_RELEASE_FAST) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot invoke the C compiler: unknown build mode");
        goto done;
    }

    arena = ct_arena_create();
    if (!arena || !args_ready) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        goto done;
    }

    compiler = find_compiler(arena, &override, &oom);
    if (oom) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        goto done;
    }
    if (!compiler) {
        if (override) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "C compiler '%s' from JOG_CC was not found", override);
        } else {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "C compiler not found; looked for clang, then gcc, on PATH");
        }
        goto done;
    }

    jogrt_c = ct_path_join(arena, request->runtime_root, "jogrt.c");
    jogrt_sched = ct_path_join(arena, request->runtime_root, "jogrt_sched.c");
    jogrt_switch = ct_path_join(arena, request->runtime_root, "jogrt_switch.c");
    jogrt_main = ct_path_join(arena, request->runtime_root, "jogrt_main.c");
    if (!jogrt_c || !jogrt_sched || !jogrt_switch || !jogrt_main) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        goto done;
    }
    if (!ct_fs_is_file(request->c_path)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "C input '%s' was not found", request->c_path);
        goto done;
    }
    if (!ct_fs_is_file(jogrt_c) || !ct_fs_is_file(jogrt_sched) || !ct_fs_is_file(jogrt_switch) || !ct_fs_is_file(jogrt_main)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "runtime '%s' is missing jogrt.c, jogrt_sched.c, jogrt_switch.c, or jogrt_main.c",
                     request->runtime_root);
        goto done;
    }

    if (!push_arg(&args, compiler) || !push_arg(&args, "-std=gnu17") || !push_mode_flags(&args, request->mode) ||
        !push_builtin_flags(&args, compiler) || !push_arg(&args, "-I") || !push_arg(&args, request->runtime_root) ||
        !push_arg(&args, request->c_path) || !push_arg(&args, jogrt_c) || !push_arg(&args, jogrt_sched) ||
        !push_arg(&args, jogrt_switch) || !push_arg(&args, jogrt_main) ||
        !push_arg(&args, "-o") || !push_arg(&args, request->exe_path)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        goto done;
    }

    if (request->extra_lib_count > 0 && !request->extra_libs) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "missing link library list");
        goto done;
    }
    for (size_t i = 0; i < request->extra_lib_count; i++) {
        const char *lib = request->extra_libs[i];
        const char *flag = NULL;
        if (!lib || lib[0] == '\0') {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "empty link library");
            goto done;
        }
        flag = link_flag(arena, lib);
        if (!flag || !push_arg(&args, flag)) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
            goto done;
        }
    }

    if (!ct_da_push(&args, &end) || !args.data) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        goto done;
    }

    if (request->verbose) {
        print_command(args.data);
    }
    if (!ct_proc_run(args.data, NULL, NULL, &exit_code)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot run the C compiler '%s'", compiler);
        goto done;
    }
    if (exit_code != 0) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "the C compiler exited with status %d", exit_code);
        goto done;
    }
    ok = true;

done:
    ct_da_free(&args);
    ct_arena_destroy(arena);
    return ok;
}
