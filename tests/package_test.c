#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "diag.h"
#include "fs.h"
#include "proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#define pkg_mkdir(path) _mkdir(path)
#define pkg_rmdir(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#define pkg_mkdir(path) mkdir((path), 0777)
#define pkg_rmdir(path) rmdir(path)
#endif

/* Multi-package behavior the flat e2e files cannot stage: visibility,
   init order across an import, and jog test. Each case is copied to a
   temporary module so the compiler's out/ directory stays out of the tree. */

static int g_failures = 0;
static unsigned g_seq = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

static int env_set(const char *value) {
#if defined(_WIN32)
    return _putenv_s("JOG_ROOT", value ? value : "") == 0;
#else
    if (!value || value[0] == '\0') {
        unsetenv("JOG_ROOT");
        return 1;
    }
    return setenv("JOG_ROOT", value, 1) == 0;
#endif
}

static char *dup_cstr(const char *text) {
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, text, len + 1);
    return copy;
}

static char *exe_path(CtArena *arena) {
#if defined(_WIN32)
    DWORD size = 32768;
    char *buf = ct_arena_alloc(arena, size, 1);
    DWORD got = 0;
    if (!buf) {
        return NULL;
    }
    got = GetModuleFileNameA(NULL, buf, size);
    if (got == 0 || got >= size) {
        return NULL;
    }
    return buf;
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = (uint32_t)sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) {
        return NULL;
    }
    return ct_arena_strdup(arena, buf);
#else
    char buf[4096];
    ssize_t got = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (got < 0 || (size_t)got >= sizeof buf - 1) {
        return NULL;
    }
    buf[got] = '\0';
    return ct_arena_strdup(arena, buf);
#endif
}

static char *parent_dir(CtArena *arena, const char *path) {
    size_t len = 0;
    size_t slash = 0;
    char *out = NULL;
    if (!path) {
        return NULL;
    }
    len = strlen(path);
    slash = len;
    while (slash > 0 && path[slash - 1] != '/' && path[slash - 1] != '\\') {
        slash--;
    }
    if (slash <= 1) {
        return ct_arena_strdup(arena, ".");
    }
    out = ct_arena_alloc(arena, slash, 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, path, slash - 1);
    out[slash - 1] = '\0';
    return out;
}

static const char *jog_executable(CtArena *arena) {
    char *exe = exe_path(arena);
    char *dir = exe ? parent_dir(arena, exe) : NULL;
    const char *jog = NULL;
    if (!dir) {
        return NULL;
    }
#if defined(_WIN32)
    jog = ct_path_join(arena, dir, "jog.exe");
#else
    jog = ct_path_join(arena, dir, "jog");
#endif
    if (!jog || !ct_fs_is_file(jog)) {
        return NULL;
    }
    return jog;
}

static const char *temp_dir(CtArena *arena) {
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof buf, buf);
    if (n == 0 || n >= sizeof buf) {
        return ct_arena_strdup(arena, ".");
    }
    if (buf[n - 1] == '\\' || buf[n - 1] == '/') {
        buf[n - 1] = '\0';
    }
    return ct_arena_strdup(arena, buf);
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || dir[0] == '\0') {
        dir = "/tmp";
    }
    (void)arena;
    return dir;
#endif
}

static char *temp_path(CtArena *arena, const char *suffix) {
    char name[96];
    const char *dir = temp_dir(arena);
    unsigned pid = 0;
    int wrote = 0;
    if (!dir) {
        return NULL;
    }
#if defined(_WIN32)
    pid = GetCurrentProcessId();
#else
    pid = (unsigned)getpid();
#endif
    wrote = snprintf(name, sizeof name, "jog-pkg-%u-%u%s", pid, g_seq++, suffix);
    if (wrote < 0 || (size_t)wrote >= sizeof name) {
        return NULL;
    }
    return ct_path_join(arena, dir, name);
}

static int ensure_dir(const char *path) {
    if (ct_fs_is_dir(path)) {
        return 1;
    }
    if (pkg_mkdir(path) == 0) {
        return 1;
    }
    return ct_fs_is_dir(path);
}

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    FILE *out = fopen(to, "wb");
    char buf[4096];
    int ok = in && out;
    if (!ok) {
        if (in) {
            fclose(in);
        }
        if (out) {
            fclose(out);
        }
        return 0;
    }
    for (;;) {
        size_t n = fread(buf, 1, sizeof buf, in);
        if (n > 0 && fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
        if (n < sizeof buf) {
            if (ferror(in)) {
                ok = 0;
            }
            break;
        }
    }
    if (fclose(out) != 0) {
        ok = 0;
    }
    fclose(in);
    return ok;
}

static int copy_tree(CtArena *arena, CtDiag *diag, const char *src, const char *dst) {
    CtDa files;
    CtDa dirs;
    size_t i = 0;
    int files_ready = 0;
    int dirs_ready = 0;
    int ok = 0;
    files_ready = ct_da_init(&files, sizeof(const char *)) ? 1 : 0;
    dirs_ready = ct_da_init(&dirs, sizeof(const char *)) ? 1 : 0;
    if (!files_ready || !dirs_ready || !ensure_dir(dst)) {
        goto done;
    }
    if (!ct_fs_list(arena, diag, src, CT_FS_FILES, NULL, &files) ||
        !ct_fs_list(arena, diag, src, CT_FS_DIRS, NULL, &dirs)) {
        goto done;
    }
    for (i = 0; i < files.count; i++) {
        const char *name = *(const char **)ct_da_at(&files, i);
        const char *from = name ? ct_path_join(arena, src, name) : NULL;
        const char *to = name ? ct_path_join(arena, dst, name) : NULL;
        if (!from || !to || !copy_file(from, to)) {
            goto done;
        }
    }
    ok = 1;
    for (i = 0; i < dirs.count; i++) {
        const char *name = *(const char **)ct_da_at(&dirs, i);
        const char *from = name ? ct_path_join(arena, src, name) : NULL;
        const char *to = name ? ct_path_join(arena, dst, name) : NULL;
        if (!from || !to || !copy_tree(arena, diag, from, to)) {
            ok = 0;
            break;
        }
    }
done:
    if (files_ready) {
        ct_da_free(&files);
    }
    if (dirs_ready) {
        ct_da_free(&dirs);
    }
    return ok;
}

static void remove_tree(CtArena *arena, CtDiag *diag, const char *dir) {
    CtDa files;
    CtDa dirs;
    size_t i = 0;
    int files_ready = ct_da_init(&files, sizeof(const char *)) ? 1 : 0;
    int dirs_ready = ct_da_init(&dirs, sizeof(const char *)) ? 1 : 0;
    if (files_ready && ct_fs_list(arena, diag, dir, CT_FS_FILES, NULL, &files)) {
        for (i = 0; i < files.count; i++) {
            const char *name = *(const char **)ct_da_at(&files, i);
            const char *path = name ? ct_path_join(arena, dir, name) : NULL;
            if (path) {
                remove(path);
            }
        }
    }
    if (dirs_ready && ct_fs_list(arena, diag, dir, CT_FS_DIRS, NULL, &dirs)) {
        for (i = 0; i < dirs.count; i++) {
            const char *name = *(const char **)ct_da_at(&dirs, i);
            const char *path = name ? ct_path_join(arena, dir, name) : NULL;
            if (path) {
                remove_tree(arena, diag, path);
            }
        }
    }
    if (files_ready) {
        ct_da_free(&files);
    }
    if (dirs_ready) {
        ct_da_free(&dirs);
    }
    pkg_rmdir(dir);
}

static char *read_stripped(const char *path) {
    FILE *fp = fopen(path, "rb");
    long length = 0;
    char *buf = NULL;
    size_t got = 0;
    size_t written = 0;
    size_t i = 0;
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
    for (i = 0; i < got; i++) {
        if (buf[i] != '\r') {
            buf[written++] = buf[i];
        }
    }
    buf[written] = '\0';
    return buf;
}

static int run_jog_flag(const char *jog, const char *mod, const char *cmd, const char *flag, const char *out_path,
                        const char *err_path, int *code) {
    const char *argv[5];
    int n = 0;
    argv[n++] = jog;
    argv[n++] = cmd;
    if (flag && flag[0] != '\0') {
        argv[n++] = flag;
    }
    argv[n++] = mod;
    argv[n] = NULL;
    return ct_proc_run(argv, out_path, err_path, code) ? 1 : 0;
}

static int run_jog(const char *jog, const char *mod, const char *cmd, const char *out_path, const char *err_path,
                   int *code) {
    return run_jog_flag(jog, mod, cmd, NULL, out_path, err_path, code);
}

static const char *find_range(const char *start, const char *end, const char *needle) {
    size_t n = 0;
    const char *p = NULL;
    if (!start || !needle) {
        return NULL;
    }
    n = strlen(needle);
    if (!end) {
        end = start + strlen(start);
    }
    if (end < start || n == 0) {
        return NULL;
    }
    for (p = start; p + n <= end; p++) {
        if (memcmp(p, needle, n) == 0) {
            return p;
        }
    }
    return NULL;
}

/* The function line is "fn fN ... <name>". */
static int fn_id(const char *dump, const char *name, int *out) {
    const char *at = NULL;
    const char *p = NULL;
    int n = 0;
    if (!dump || !name || !out) {
        return 0;
    }
    at = strstr(dump, name);
    if (!at) {
        return 0;
    }
    p = at;
    while (p > dump && p[-1] != '\n') {
        p--;
    }
    if (strncmp(p, "fn f", 4) != 0) {
        return 0;
    }
    p += 4;
    if (*p < '0' || *p > '9') {
        return 0;
    }
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        p++;
    }
    *out = n;
    return 1;
}

/* disp's body calls first's function before second's. */
static int dispatcher_order(const char *dump, const char *first, const char *second, const char *disp) {
    int first_id = 0;
    int second_id = 0;
    const char *at = NULL;
    const char *body = NULL;
    const char *next = NULL;
    char call_a[32];
    char call_b[32];
    const char *pos_a = NULL;
    const char *pos_b = NULL;
    int wrote_a = 0;
    int wrote_b = 0;
    if (!fn_id(dump, first, &first_id) || !fn_id(dump, second, &second_id)) {
        return 0;
    }
    at = strstr(dump, disp);
    if (!at) {
        return 0;
    }
    body = strchr(at, '\n');
    if (!body) {
        return 0;
    }
    next = strstr(body + 1, "\nfn ");
    wrote_a = snprintf(call_a, sizeof call_a, "call f%d()", first_id);
    wrote_b = snprintf(call_b, sizeof call_b, "call f%d()", second_id);
    if (wrote_a < 0 || wrote_b < 0 || (size_t)wrote_a >= sizeof call_a || (size_t)wrote_b >= sizeof call_b) {
        return 0;
    }
    pos_a = find_range(body, next, call_a);
    pos_b = find_range(body, next, call_b);
    return pos_a && pos_b && pos_a < pos_b;
}

static char *stage(CtArena *arena, CtDiag *diag, const char *jog_root, const char *name) {
    const char *src = NULL;
    char *work = NULL;
    char *mod = NULL;
    src = ct_path_join(arena, jog_root, name);
    work = temp_path(arena, "");
    mod = work ? ct_path_join(arena, work, "mod") : NULL;
    if (!src || !work || !mod || !ensure_dir(work) || !copy_tree(arena, diag, src, mod)) {
        fprintf(stderr, "FAIL %s: cannot stage the module\n", name);
        ct_diag_print(diag, stderr);
        CHECK(0);
        if (work) {
            remove_tree(arena, diag, work);
        }
        return NULL;
    }
    return mod;
}

static void finish_case(CtArena *arena, CtDiag *diag, const char *mod, char *out_text, char *err_text) {
    char *work = NULL;
    free(out_text);
    free(err_text);
    if (!mod) {
        return;
    }
    work = parent_dir(arena, mod);
    if (work) {
        remove_tree(arena, diag, work);
    }
}

static void expect_stdout(CtArena *arena, CtDiag *diag, const char *jog, const char *root, const char *name,
                          const char *cmd, const char *stdout_text) {
    char *mod = stage(arena, diag, root, name);
    char *out_path = NULL;
    char *err_path = NULL;
    char *out_text = NULL;
    char *err_text = NULL;
    int code = -1;
    if (!mod) {
        return;
    }
    out_path = temp_path(arena, ".out");
    err_path = temp_path(arena, ".err");
    if (!out_path || !err_path || !run_jog(jog, mod, cmd, out_path, err_path, &code)) {
        fprintf(stderr, "FAIL %s: cannot run jog %s\n", name, cmd);
        CHECK(0);
        finish_case(arena, diag, mod, NULL, NULL);
        return;
    }
    out_text = read_stripped(out_path);
    err_text = read_stripped(err_path);
    remove(out_path);
    remove(err_path);
    if (!out_text || !err_text) {
        fprintf(stderr, "FAIL %s: cannot read jog output\n", name);
        CHECK(0);
    } else if (code != 0 || strcmp(out_text, stdout_text) != 0) {
        fprintf(stderr, "FAIL %s: jog %s exited %d\nstdout: %s\nstderr: %s\n", name, cmd, code,
                out_text ? out_text : "", err_text ? err_text : "");
        CHECK(0);
    }
    finish_case(arena, diag, mod, out_text, err_text);
}

static void expect_diag(CtArena *arena, CtDiag *diag, const char *jog, const char *root, const char *name,
                        const char *needle) {
    char *mod = stage(arena, diag, root, name);
    char *out_path = NULL;
    char *err_path = NULL;
    char *out_text = NULL;
    char *err_text = NULL;
    int code = -1;
    if (!mod) {
        return;
    }
    out_path = temp_path(arena, ".out");
    err_path = temp_path(arena, ".err");
    if (!out_path || !err_path || !run_jog(jog, mod, "check", out_path, err_path, &code)) {
        fprintf(stderr, "FAIL %s: cannot run jog check\n", name);
        CHECK(0);
        finish_case(arena, diag, mod, NULL, NULL);
        return;
    }
    out_text = read_stripped(out_path);
    err_text = read_stripped(err_path);
    remove(out_path);
    remove(err_path);
    if (!err_text || code == 0 || !strstr(err_text, needle)) {
        fprintf(stderr, "FAIL %s: expected '%s' (exit %d)\n%s\n", name, needle, code, err_text ? err_text : "");
        CHECK(0);
    }
    finish_case(arena, diag, mod, out_text, err_text);
}

static void expect_init_order(CtArena *arena, CtDiag *diag, const char *jog, const char *root) {
    char *mod = stage(arena, diag, root, "initorder");
    char *out_path = NULL;
    char *err_path = NULL;
    char *dump = NULL;
    char *err_text = NULL;
    int code = -1;
    if (!mod) {
        return;
    }
    out_path = temp_path(arena, ".out");
    err_path = temp_path(arena, ".err");
    if (!out_path || !err_path || !run_jog(jog, mod, "emit-ir", out_path, err_path, &code)) {
        fprintf(stderr, "FAIL initorder: cannot run jog emit-ir\n");
        CHECK(0);
        finish_case(arena, diag, mod, NULL, NULL);
        return;
    }
    dump = read_stripped(out_path);
    err_text = read_stripped(err_path);
    remove(out_path);
    remove(err_path);
    if (!dump || code != 0 ||
        !dispatcher_order(dump, "jog_leaf__0pkginit", "jog_initorder__0rootinit", "jog_initorder__0init")) {
        fprintf(stderr, "FAIL initorder: dispatcher did not call leaf before the root\n%s\n%s\n", dump ? dump : "",
                err_text ? err_text : "");
        CHECK(0);
    }
    finish_case(arena, diag, mod, dump, err_text);
}

enum { PARSED_CAP = 8, PARSED_TEXT = 1024 };

typedef struct ParsedDiag {
    char file[PARSED_TEXT];
    char severity[32];
    char message[PARSED_TEXT];
    unsigned line;
    unsigned column;
} ParsedDiag;

static int next_line(const char **pp, char *buf, size_t cap) {
    const char *p;
    size_t n = 0;
    if (!pp || !*pp) {
        return 0;
    }
    p = *pp;
    if (*p == '\0') {
        return 0;
    }
    while (*p && *p != '\n') {
        if (n + 1 >= cap) {
            return -1;
        }
        buf[n++] = *p++;
    }
    buf[n] = '\0';
    if (*p == '\n') {
        p++;
    }
    *pp = p;
    return 1;
}

static int json_string(const char **pp, char *buf, size_t cap) {
    const char *p;
    size_t n = 0;
    if (!pp || !*pp || **pp != '"') {
        return 0;
    }
    p = *pp + 1;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p;
        if (c == '\\') {
            p++;
            if (*p == '"' || *p == '\\' || *p == '/') {
                c = (unsigned char)*p;
            } else if (*p == 'n') {
                c = '\n';
            } else if (*p == 'r') {
                c = '\r';
            } else if (*p == 't') {
                c = '\t';
            } else if (*p == 'b') {
                c = '\b';
            } else if (*p == 'f') {
                c = '\f';
            } else if (*p == 'u') {
                unsigned value = 0;
                int i;
                for (i = 1; i <= 4; i++) {
                    char h = p[i];
                    unsigned digit;
                    if (h >= '0' && h <= '9') {
                        digit = (unsigned)(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        digit = (unsigned)(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                        digit = (unsigned)(h - 'A' + 10);
                    } else {
                        return 0;
                    }
                    value = (value << 4) | digit;
                }
                if (value > 0xFFu) {
                    return 0;
                }
                c = (unsigned char)value;
                p += 4;
            } else {
                return 0;
            }
        }
        if (n + 1 >= cap) {
            return 0;
        }
        buf[n++] = (char)c;
        p++;
    }
    if (*p != '"') {
        return 0;
    }
    buf[n] = '\0';
    *pp = p + 1;
    return 1;
}

static int parse_json_diag(const char *line, ParsedDiag *out) {
    const char *p = line;
    char *end = NULL;
    unsigned long line_no;
    unsigned long column;
    if (!line || !out || strncmp(p, "{\"file\":", 8) != 0) {
        return 0;
    }
    p += 8;
    if (!json_string(&p, out->file, sizeof out->file)) {
        return 0;
    }
    if (strncmp(p, ",\"line\":", 8) != 0) {
        return 0;
    }
    p += 8;
    line_no = strtoul(p, &end, 10);
    if (end == p || line_no > 1000000ul) {
        return 0;
    }
    p = end;
    if (strncmp(p, ",\"column\":", 10) != 0) {
        return 0;
    }
    p += 10;
    column = strtoul(p, &end, 10);
    if (end == p || column > 1000000ul) {
        return 0;
    }
    p = end;
    if (strncmp(p, ",\"severity\":", 12) != 0) {
        return 0;
    }
    p += 12;
    if (!json_string(&p, out->severity, sizeof out->severity)) {
        return 0;
    }
    if (strncmp(p, ",\"message\":", 11) != 0) {
        return 0;
    }
    p += 11;
    if (!json_string(&p, out->message, sizeof out->message)) {
        return 0;
    }
    if (p[0] != '}' || p[1] != '\0') {
        return 0;
    }
    out->line = (unsigned)line_no;
    out->column = (unsigned)column;
    return 1;
}

static int parse_text_diag(const char *line, ParsedDiag *out) {
    const char *names[3];
    const char *best = NULL;
    const char *severity = NULL;
    const char *message;
    const char *end;
    const char *col_at;
    const char *line_at;
    size_t file_len;
    unsigned long line_no;
    unsigned long column;
    int i;
    if (!line || !out) {
        return 0;
    }
    names[0] = "error";
    names[1] = "warning";
    names[2] = "note";
    for (i = 0; i < 3; i++) {
        char pat[16];
        const char *at;
        int wrote = snprintf(pat, sizeof pat, ": %s: ", names[i]);
        if (wrote < 0 || (size_t)wrote >= sizeof pat) {
            return 0;
        }
        at = strstr(line, pat);
        if (at && (!best || at < best)) {
            best = at;
            severity = names[i];
        }
    }
    if (!best || !severity) {
        return 0;
    }
    message = best + 2 + strlen(severity) + 2;
    if (strlen(message) >= sizeof out->message || strlen(severity) >= sizeof out->severity) {
        return 0;
    }
    memcpy(out->message, message, strlen(message) + 1);
    memcpy(out->severity, severity, strlen(severity) + 1);
    end = best;
    col_at = end;
    while (col_at > line && col_at[-1] >= '0' && col_at[-1] <= '9') {
        col_at--;
    }
    if (col_at == end || col_at == line || col_at[-1] != ':') {
        return 0;
    }
    column = strtoul(col_at, NULL, 10);
    line_at = col_at - 1;
    while (line_at > line && line_at[-1] >= '0' && line_at[-1] <= '9') {
        line_at--;
    }
    if (line_at == col_at - 1 || line_at == line || line_at[-1] != ':') {
        return 0;
    }
    line_no = strtoul(line_at, NULL, 10);
    file_len = (size_t)(line_at - 1 - line);
    if (file_len == 0 || file_len >= sizeof out->file || line_no > 1000000ul || column > 1000000ul) {
        return 0;
    }
    memcpy(out->file, line, file_len);
    out->file[file_len] = '\0';
    out->line = (unsigned)line_no;
    out->column = (unsigned)column;
    return 1;
}

static int fill_diags(const char *text, int as_json, ParsedDiag *out, int cap, int *count) {
    const char *p = text ? text : "";
    char line[2048];
    int n = 0;
    int rc;
    if (!out || !count || cap < 1) {
        return 0;
    }
    *count = 0;
    while ((rc = next_line(&p, line, sizeof line)) == 1) {
        if (line[0] == '\0') {
            continue;
        }
        if (n >= cap) {
            return 0;
        }
        if (as_json) {
            if (!parse_json_diag(line, &out[n])) {
                return 0;
            }
        } else if (!parse_text_diag(line, &out[n])) {
            return 0;
        }
        n++;
    }
    if (rc < 0) {
        return 0;
    }
    *count = n;
    return 1;
}

static int same_diag(const ParsedDiag *a, const ParsedDiag *b) {
    return a && b && a->line == b->line && a->column == b->column && strcmp(a->file, b->file) == 0 &&
           strcmp(a->severity, b->severity) == 0 && strcmp(a->message, b->message) == 0;
}

static void expect_test_failure(CtArena *arena, CtDiag *diag, const char *jog, const char *root) {
    char *mod = stage(arena, diag, root, "failtest");
    char *out_path = NULL;
    char *err_path = NULL;
    char *out_text = NULL;
    char *err_text = NULL;
    int code = -1;
    if (!mod) {
        return;
    }
    out_path = temp_path(arena, ".out");
    err_path = temp_path(arena, ".err");
    if (!out_path || !err_path || !run_jog(jog, mod, "test", out_path, err_path, &code)) {
        fprintf(stderr, "FAIL failtest: cannot run jog test\n");
        CHECK(0);
        finish_case(arena, diag, mod, NULL, NULL);
        return;
    }
    out_text = read_stripped(out_path);
    err_text = read_stripped(err_path);
    remove(out_path);
    remove(err_path);
    if (!out_text || !err_text || code != 1 || strcmp(out_text, "still\n") != 0 || !strstr(err_text, "fail") ||
        !strstr(err_text, "test exited with status 1")) {
        fprintf(stderr, "FAIL failtest: jog test exited %d\nstdout: %s\nstderr: %s\n", code, out_text ? out_text : "",
                err_text ? err_text : "");
        CHECK(0);
    }
    finish_case(arena, diag, mod, out_text, err_text);
}

static void expect_panic_failure(CtArena *arena, CtDiag *diag, const char *jog, const char *root) {
    char *mod = stage(arena, diag, root, "panicfail");
    char *out_path = NULL;
    char *err_path = NULL;
    char *out_text = NULL;
    char *err_text = NULL;
    int code = -1;
    if (!mod) {
        return;
    }
    out_path = temp_path(arena, ".out");
    err_path = temp_path(arena, ".err");
    if (!out_path || !err_path || !run_jog(jog, mod, "test", out_path, err_path, &code)) {
        fprintf(stderr, "FAIL panicfail: cannot run jog test\n");
        CHECK(0);
        finish_case(arena, diag, mod, NULL, NULL);
        return;
    }
    out_text = read_stripped(out_path);
    err_text = read_stripped(err_path);
    remove(out_path);
    remove(err_path);
    if (!out_text || !err_text || code != 1 || strcmp(out_text, "still\n") != 0 || !strstr(err_text, "fail") ||
        !strstr(err_text, "test exited with status 1")) {
        fprintf(stderr, "FAIL panicfail: jog test exited %d\nstdout: %s\nstderr: %s\n", code, out_text ? out_text : "",
                err_text ? err_text : "");
        CHECK(0);
    }
    finish_case(arena, diag, mod, out_text, err_text);
}

static void expect_test_build_failure(CtArena *arena, CtDiag *diag, const char *jog, const char *root) {
    char *mod = stage(arena, diag, root, "badtest");
    char *out_path = NULL;
    char *err_path = NULL;
    char *out_text = NULL;
    char *err_text = NULL;
    int code = -1;
    if (!mod) {
        return;
    }
    out_path = temp_path(arena, ".out");
    err_path = temp_path(arena, ".err");
    if (!out_path || !err_path || !run_jog(jog, mod, "test", out_path, err_path, &code)) {
        fprintf(stderr, "FAIL badtest: cannot run jog test\n");
        CHECK(0);
        finish_case(arena, diag, mod, NULL, NULL);
        return;
    }
    out_text = read_stripped(out_path);
    err_text = read_stripped(err_path);
    remove(out_path);
    remove(err_path);
    if (!err_text || code == 0 || !strstr(err_text, "error")) {
        fprintf(stderr, "FAIL badtest: jog test exited %d\n%s\n", code, err_text ? err_text : "");
        CHECK(0);
    }
    finish_case(arena, diag, mod, out_text, err_text);
}

static void expect_check_json(CtArena *arena, CtDiag *diag, const char *jog, const char *root) {
    char *mod = stage(arena, diag, root, "diagjson");
    char *out_path = NULL;
    char *err_path = NULL;
    char *text_out = NULL;
    char *text_err = NULL;
    char *explicit_err = NULL;
    char *json_out = NULL;
    char *json_err = NULL;
    char *bad_err = NULL;
    ParsedDiag text_diags[PARSED_CAP];
    ParsedDiag json_diags[PARSED_CAP];
    int text_count = 0;
    int json_count = 0;
    int code = -1;
    int explicit_code = -1;
    int json_code = -1;
    int bad_code = -1;
    int i;
    int saw_missing = 0;
    if (!mod) {
        return;
    }
    out_path = temp_path(arena, ".out");
    err_path = temp_path(arena, ".err");
    if (!out_path || !err_path || !run_jog(jog, mod, "check", out_path, err_path, &code)) {
        fprintf(stderr, "FAIL diagjson: cannot run jog check\n");
        CHECK(0);
        finish_case(arena, diag, mod, NULL, NULL);
        return;
    }
    text_out = read_stripped(out_path);
    text_err = read_stripped(err_path);
    remove(out_path);
    remove(err_path);
    out_path = temp_path(arena, ".out");
    err_path = temp_path(arena, ".err");
    if (!out_path || !err_path || !run_jog_flag(jog, mod, "check", "--diag=text", out_path, err_path, &explicit_code)) {
        fprintf(stderr, "FAIL diagjson: cannot run jog check --diag=text\n");
        CHECK(0);
        finish_case(arena, diag, mod, text_out, text_err);
        return;
    }
    explicit_err = read_stripped(err_path);
    remove(out_path);
    remove(err_path);
    out_path = temp_path(arena, ".out");
    err_path = temp_path(arena, ".err");
    if (!out_path || !err_path || !run_jog_flag(jog, mod, "check", "--diag=json", out_path, err_path, &json_code)) {
        fprintf(stderr, "FAIL diagjson: cannot run jog check --diag=json\n");
        CHECK(0);
        free(explicit_err);
        finish_case(arena, diag, mod, text_out, text_err);
        return;
    }
    json_out = read_stripped(out_path);
    json_err = read_stripped(err_path);
    remove(out_path);
    remove(err_path);
    out_path = temp_path(arena, ".out");
    err_path = temp_path(arena, ".err");
    if (!out_path || !err_path || !run_jog_flag(jog, mod, "check", "--diag=nope", out_path, err_path, &bad_code)) {
        fprintf(stderr, "FAIL diagjson: cannot run jog check --diag=nope\n");
        CHECK(0);
        free(explicit_err);
        finish_case(arena, diag, mod, json_out, text_err);
        free(text_out);
        free(json_err);
        return;
    }
    bad_err = read_stripped(err_path);
    remove(out_path);
    remove(err_path);
    if (!text_out || !text_err || !explicit_err || !json_out || !json_err || !bad_err || code == 0 ||
        explicit_code != code || json_code == 0 || bad_code != 2 || strcmp(text_out, "") != 0 ||
        strcmp(text_err, explicit_err) != 0 || strcmp(json_err, "") != 0 ||
        !strstr(bad_err, "unknown diagnostic format") || !fill_diags(text_err, 0, text_diags, PARSED_CAP, &text_count) ||
        !fill_diags(json_out, 1, json_diags, PARSED_CAP, &json_count) || text_count < 1 || text_count != json_count) {
        fprintf(stderr, "FAIL diagjson: text exit %d, json exit %d, bad exit %d\ntext stderr:\n%s\njson stdout:\n%s\njson stderr:\n%s\n",
                code, json_code, bad_code, text_err ? text_err : "", json_out ? json_out : "", json_err ? json_err : "");
        CHECK(0);
    } else {
        for (i = 0; i < text_count; i++) {
            if (!same_diag(&text_diags[i], &json_diags[i]) || text_diags[i].line < 1 || text_diags[i].column < 1 ||
                (strcmp(text_diags[i].severity, "error") != 0 && strcmp(text_diags[i].severity, "warning") != 0)) {
                fprintf(stderr, "FAIL diagjson: field mismatch\n text %s:%u:%u %s %s\n json %s:%u:%u %s %s\n",
                        text_diags[i].file, text_diags[i].line, text_diags[i].column, text_diags[i].severity,
                        text_diags[i].message, json_diags[i].file, json_diags[i].line, json_diags[i].column,
                        json_diags[i].severity, json_diags[i].message);
                CHECK(0);
            }
            if (strcmp(text_diags[i].message, "undefined name 'missing'") == 0) {
                saw_missing = 1;
            }
        }
        CHECK(saw_missing);
    }
    free(explicit_err);
    free(json_err);
    free(bad_err);
    finish_case(arena, diag, mod, text_out, json_out);
    free(text_err);
}

static void expect_check_clean(CtArena *arena, CtDiag *diag, const char *jog, const char *root) {
    char *mod = stage(arena, diag, root, "diagclean");
    char *out_path = NULL;
    char *err_path = NULL;
    char *out_text = NULL;
    char *err_text = NULL;
    int code = -1;
    if (!mod) {
        return;
    }
    out_path = temp_path(arena, ".out");
    err_path = temp_path(arena, ".err");
    if (!out_path || !err_path || !run_jog_flag(jog, mod, "check", "--diag=json", out_path, err_path, &code)) {
        fprintf(stderr, "FAIL diagclean: cannot run jog check --diag=json\n");
        CHECK(0);
        finish_case(arena, diag, mod, NULL, NULL);
        return;
    }
    out_text = read_stripped(out_path);
    err_text = read_stripped(err_path);
    remove(out_path);
    remove(err_path);
    if (!out_text || !err_text || code != 0 || strcmp(out_text, "") != 0 || strcmp(err_text, "") != 0) {
        fprintf(stderr, "FAIL diagclean: jog check --diag=json exited %d\nstdout: %s\nstderr: %s\n", code,
                out_text ? out_text : "", err_text ? err_text : "");
        CHECK(0);
    }
    finish_case(arena, diag, mod, out_text, err_text);
}

/* checksub's root does not import sub. Checking the module stays clean;
   checking a file in sub checks sub too and reports its error. */
static void expect_check_subpackage(CtArena *arena, CtDiag *diag, const char *jog, const char *root) {
    char *mod = stage(arena, diag, root, "checksub");
    const char *file = NULL;
    char *out_path = NULL;
    char *err_path = NULL;
    char *root_out = NULL;
    char *sub_out = NULL;
    char *sub_err = NULL;
    ParsedDiag diags[PARSED_CAP];
    int count = 0;
    int root_code = -1;
    int sub_code = -1;
    size_t flen = 0;
    if (!mod) {
        return;
    }
    file = ct_path_join(arena, mod, "sub/x.jog");
    out_path = temp_path(arena, ".out");
    err_path = temp_path(arena, ".err");
    if (!file || !out_path || !err_path ||
        !run_jog_flag(jog, mod, "check", "--diag=json", out_path, err_path, &root_code)) {
        fprintf(stderr, "FAIL checksub: cannot run jog check on the module\n");
        CHECK(0);
        finish_case(arena, diag, mod, NULL, NULL);
        return;
    }
    root_out = read_stripped(out_path);
    remove(out_path);
    remove(err_path);
    if (!run_jog_flag(jog, file, "check", "--diag=json", out_path, err_path, &sub_code)) {
        fprintf(stderr, "FAIL checksub: cannot run jog check on sub/x.jog\n");
        CHECK(0);
        free(root_out);
        finish_case(arena, diag, mod, NULL, NULL);
        return;
    }
    sub_out = read_stripped(out_path);
    sub_err = read_stripped(err_path);
    remove(out_path);
    remove(err_path);
    if (!root_out || root_code != 0 || strcmp(root_out, "") != 0) {
        fprintf(stderr, "FAIL checksub: module check exited %d\n%s\n", root_code, root_out ? root_out : "");
        CHECK(0);
    }
    if (!sub_out || !sub_err || sub_code == 0 || strcmp(sub_err, "") != 0 ||
        !fill_diags(sub_out, 1, diags, PARSED_CAP, &count) || count != 1) {
        fprintf(stderr, "FAIL checksub: sub check exited %d\nstdout: %s\nstderr: %s\n", sub_code,
                sub_out ? sub_out : "", sub_err ? sub_err : "");
        CHECK(0);
    } else {
        flen = strlen(diags[0].file);
        CHECK(flen >= 5 && strcmp(diags[0].file + flen - 5, "x.jog") == 0);
        CHECK(strstr(diags[0].file, "sub") != NULL);
        CHECK(diags[0].line == 3 && diags[0].column == 12);
        CHECK(strcmp(diags[0].severity, "error") == 0);
        CHECK(strcmp(diags[0].message, "type mismatch: expected i32, got str") == 0);
    }
    free(root_out);
    finish_case(arena, diag, mod, sub_out, sub_err);
}

static const char *concat(CtArena *arena, const char *a, const char *b) {
    size_t a_len = strlen(a);
    size_t b_len = strlen(b);
    char *out = ct_arena_alloc(arena, a_len + b_len + 1, 1);
    if (out) {
        memcpy(out, a, a_len);
        memcpy(out + a_len, b, b_len + 1);
    }
    return out;
}

/* Runs jog with argv (NULL-terminated, argv[0] is jog). want_out NULL skips
   stdout; want_err NULL skips stderr; otherwise stderr must contain it. */
static void expect_cli(CtArena *arena, const char *label, const char **argv, int want_code, const char *want_out,
                       const char *want_err) {
    char *out_path = temp_path(arena, ".out");
    char *err_path = temp_path(arena, ".err");
    char *out_text = NULL;
    char *err_text = NULL;
    int code = -1;
    if (!out_path || !err_path || !ct_proc_run(argv, out_path, err_path, &code)) {
        fprintf(stderr, "FAIL %s: cannot run jog\n", label);
        CHECK(0);
        return;
    }
    out_text = read_stripped(out_path);
    err_text = read_stripped(err_path);
    remove(out_path);
    remove(err_path);
    if (!out_text || !err_text || code != want_code || (want_out && strcmp(out_text, want_out) != 0) ||
        (want_err && !strstr(err_text, want_err))) {
        fprintf(stderr, "FAIL %s: exited %d, expected %d\nstdout: %s\nstderr: %s\n", label, code, want_code,
                out_text ? out_text : "", err_text ? err_text : "");
        CHECK(0);
    }
    free(out_text);
    free(err_text);
}

/* tests/jog/pkg/modules holds an app and a library side by side, plus
   modules that break one rule each. See JogDocs design/Packages.md. */
static void expect_modules(CtArena *arena, CtDiag *diag, const char *jog, const char *root) {
    char *mod = stage(arena, diag, root, "modules");
    const char *app = NULL;
    const char *lib = NULL;
    const char *lib_flag = NULL;
    if (!mod) {
        return;
    }
    app = ct_path_join(arena, mod, "app");
    lib = ct_path_join(arena, mod, "lib");
    lib_flag = lib ? concat(arena, "lib=", lib) : NULL;
    CHECK(app && lib && lib_flag);
    if (app && lib && lib_flag) {
        const char *run[] = {jog, "run", app, "--module", lib_flag, NULL};
        const char *test[] = {jog, "test", app, "--module", lib_flag, NULL};
        const char *eq_form = concat(arena, "--module=", lib_flag);
        const char *check_eq[] = {jog, "check", app, eq_form, NULL};
        const char *emit_c[] = {jog, "emit-c", app, "--module", lib_flag, NULL};
        const char *emit_ir[] = {jog, "emit-ir", app, "--module", lib_flag, NULL};
        const char *build[] = {jog, "build", app, "--module", lib_flag, NULL};
        const char *missing[] = {jog, "check", app, NULL};
        const char *bad_flag[] = {jog, "check", app, "--module", "lib", NULL};
        const char *check_lib[] = {jog, "check", lib, NULL};
        const char *build_lib[] = {jog, "build", lib, NULL};
        const char *noexport[] = {jog, "check", ct_path_join(arena, mod, "noexport"), "--module", lib_flag, NULL};
        const char *embed_lib = ct_path_join(arena, mod, "embedapp/vendor/badembed");
        const char *embed[] = {jog,      "check",
                               ct_path_join(arena, mod, "embedapp"), "--module",
                               embed_lib ? concat(arena, "badembed=", embed_lib) : NULL, NULL};
        expect_cli(arena, "modules run", run, 0, "app util\nlib util\nshapes\nembed ok\n", NULL);
        /* lib.test.jog does not type-check; it must not be loaded. */
        expect_cli(arena, "modules test", test, 0, "app util\nlib util\n", NULL);
        expect_cli(arena, "modules --module=", check_eq, 0, "", NULL);
        expect_cli(arena, "modules emit-c", emit_c, 0, NULL, NULL);
        expect_cli(arena, "modules emit-ir", emit_ir, 0, NULL, NULL);
        expect_cli(arena, "modules build", build, 0, "", NULL);
        expect_cli(arena, "modules missing", missing, 1, "",
                   "module 'lib' is required by jog.mod but was not given with --module; build with jog-build, or "
                   "pass --module lib=<dir>");
        expect_cli(arena, "modules bad flag", bad_flag, 2, "", "--module expects <name>=<dir>");
        expect_cli(arena, "modules check lib", check_lib, 0, "", NULL);
        expect_cli(arena, "modules build lib", build_lib, 1, "", "module 'lib' is a library; use jog check or jog test");
        expect_cli(arena, "modules noexport", noexport, 1, "", "package 'lib.internal' is not exported by module 'lib'");
        expect_cli(arena, "modules embed", embed, 1, "", "@embed path escapes the module");
    }
    finish_case(arena, diag, mod, NULL, NULL);
}

int main(void) {
    CtArena *arena = ct_arena_create();
    CtDiag *diag = arena ? ct_diag_create(arena) : NULL;
    const char *jog = arena ? jog_executable(arena) : NULL;
    const char *root = arena ? ct_path_join(arena, CT_SOURCE_DIR, "tests/jog/pkg") : NULL;
    const char *saved_root = NULL;
    int had_root = 0;
    const char *env = getenv("JOG_ROOT");
    if (env && env[0] != '\0') {
        had_root = 1;
        saved_root = dup_cstr(env);
    }
    CHECK(arena && diag && jog && root && (!had_root || saved_root));
    if (!arena || !diag || !jog || !root || (had_root && !saved_root)) {
        fprintf(stderr, "FAIL: package harness could not find jog or tests/jog/pkg\n");
        ct_arena_destroy(arena);
        free((void *)saved_root);
        return 1;
    }
    if (!env_set(CT_SOURCE_DIR)) {
        fprintf(stderr, "FAIL: cannot set JOG_ROOT\n");
        CHECK(0);
        ct_arena_destroy(arena);
        free((void *)saved_root);
        return 1;
    }
    expect_init_order(arena, diag, jog, root);
    expect_stdout(arena, diag, jog, root, "initorder", "run", "leaf-first\n");
    expect_stdout(arena, diag, jog, root, "visible", "run", "visible\n");
    expect_diag(arena, diag, jog, root, "hidden", "type 'Secret' is not visible");
    expect_diag(arena, diag, jog, root, "hidden", "name 'hidden' is not visible");
    expect_diag(arena, diag, jog, root, "hidden", "undefined name 'secret'");
    expect_diag(arena, diag, jog, root, "filepriv", "undefined name 'secret'");
    expect_stdout(arena, diag, jog, root, "testrun", "run", "run\n");
    expect_stdout(arena, diag, jog, root, "testrun", "test", "ok\n");
    expect_stdout(arena, diag, jog, root, "ctx", "run", "run\n");
    expect_stdout(arena, diag, jog, root, "ctx", "test", "tested\n");
    expect_stdout(arena, diag, jog, root, "testing", "run", "run\n");
    expect_stdout(arena, diag, jog, root, "testing", "test", "ok\n");
    expect_test_failure(arena, diag, jog, root);
    expect_panic_failure(arena, diag, jog, root);
    expect_test_build_failure(arena, diag, jog, root);
    expect_check_json(arena, diag, jog, root);
    expect_check_clean(arena, diag, jog, root);
    expect_check_subpackage(arena, diag, jog, root);
    expect_modules(arena, diag, jog, root);
    {
        /* std/io's own tests: binary mode, end of input, an empty buffer. */
        const char *std_root = ct_path_join(arena, CT_SOURCE_DIR, "std");
        CHECK(std_root != NULL);
        if (std_root) {
            expect_stdout(arena, diag, jog, std_root, "io", "test", "");
        }
    }
    if (had_root) {
        env_set(saved_root);
    } else {
        env_set(NULL);
    }
    free((void *)saved_root);
    ct_arena_destroy(arena);
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
