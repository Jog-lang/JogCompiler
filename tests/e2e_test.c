#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "file.h"
#include "fs.h"
#include "proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#define e2e_mkdir(path) _mkdir(path)
#define e2e_rmdir(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#define e2e_mkdir(path) mkdir((path), 0777)
#define e2e_rmdir(path) rmdir(path)
#endif

/* Builds and runs every tests/jog program whose name ends in .jog.
   Names ending in .test.jog belong to jog test. Each program is copied
   into its own temporary module, because a package is a folder. Until
   check and lower exist, a compile that reports a phase as not
   implemented is a skip, not a failure. Expect lines (// expect:) are
   the program's stdout, one line each. */

static int g_failures = 0;
static unsigned g_seq = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

enum { E2E_PASS = 0, E2E_SKIP = 1, E2E_FAIL = 2 };

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
    wrote = snprintf(name, sizeof name, "jog-e2e-%u-%u%s", pid, g_seq++, suffix);
    if (wrote < 0 || (size_t)wrote >= sizeof name) {
        return NULL;
    }
    return ct_path_join(arena, dir, name);
}

static int is_test_source(const char *name) {
    size_t len = strlen(name);
    const char *suffix = ".test.jog";
    size_t slen = strlen(suffix);
    return len >= slen && memcmp(name + len - slen, suffix, slen) == 0;
}

static int write_text(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    size_t len = 0;
    if (!fp) {
        return 0;
    }
    len = strlen(text);
    if (fwrite(text, 1, len, fp) != len) {
        fclose(fp);
        return 0;
    }
    return fclose(fp) == 0;
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

static char *read_stripped(const char *path) {
    FILE *fp = fopen(path, "rb");
    long length = 0;
    char *buf = NULL;
    size_t got = 0;
    size_t written = 0;
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
    for (size_t i = 0; i < got; i++) {
        if (buf[i] != '\r') {
            buf[written++] = buf[i];
        }
    }
    buf[written] = '\0';
    return buf;
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

static int collect_expects(CtArena *arena, const char *data, size_t size, CtDa *out) {
    size_t i = 0;
    while (i < size) {
        size_t start = i;
        size_t len = 0;
        const char *text = NULL;
        size_t text_len = 0;
        while (i < size && data[i] != '\n' && data[i] != '\r') {
            i++;
        }
        len = i - start;
        if (line_is_expect(data + start, len, &text, &text_len)) {
            char *copy = dup_range(arena, text, text_len);
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
    return 1;
}

/* Drop the empty piece that a trailing newline would add. A final
   newline still terminates the last real line. */
static int split_lines(char *text, CtDa *out) {
    size_t n = 0;
    size_t i = 0;
    if (!text) {
        return 0;
    }
    n = strlen(text);
    while (i < n) {
        size_t start = i;
        char *line = NULL;
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
    size_t i = 0;
    fprintf(fp, "%s (%zu)\n", title, lines->count);
    for (i = 0; i < lines->count; i++) {
        const char *line = *(const char **)ct_da_at(lines, i);
        fprintf(fp, "  |%s|\n", line ? line : "");
    }
}

static int outputs_match(CtDa *expect, char *stdout_text) {
    CtDa got;
    size_t i = 0;
    int ready = ct_da_init(&got, sizeof(char *));
    int same = 0;
    if (!ready || !split_lines(stdout_text, &got)) {
        ct_da_free(&got);
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

static const char *skip_reason(char *text) {
    char *found = NULL;
    char *start = NULL;
    char *end = NULL;
    if (!text) {
        return "not implemented";
    }
    found = strstr(text, "not implemented");
    if (!found) {
        return "not implemented";
    }
    start = found;
    while (start > text && start[-1] != '\n') {
        start--;
    }
    end = strchr(start, '\n');
    if (end) {
        *end = '\0';
    }
    return start;
}

static void remove_if(const char *path) {
    if (path) {
        remove(path);
    }
}

static int run_one(CtArena *arena, CtDiag *diag, const char *jog, const char *source, const char *display) {
    CtFile file;
    CtDa expects;
    char *work = NULL;
    char *mod = NULL;
    char *src_copy = NULL;
    char *manifest = NULL;
    char *stdout_path = NULL;
    char *stderr_path = NULL;
    char *out_dir = NULL;
    char *out_c = NULL;
    char *out_exe = NULL;
    char *out_plain = NULL;
    char *stdout_text = NULL;
    char *stderr_text = NULL;
    const char *argv[4];
    int code = -1;
    int result = E2E_FAIL;
    int expects_ready = ct_da_init(&expects, sizeof(char *));
    memset(&file, 0, sizeof file);
    work = temp_path(arena, "");
    stdout_path = temp_path(arena, ".out");
    stderr_path = temp_path(arena, ".err");
    if (!expects_ready || !work || !stdout_path || !stderr_path) {
        fprintf(stderr, "FAIL %s: out of memory\n", display);
        CHECK(0);
        goto cleanup;
    }
    mod = ct_path_join(arena, work, "mod");
    src_copy = mod ? ct_path_join(arena, mod, "main.jog") : NULL;
    manifest = mod ? ct_path_join(arena, mod, "jog.mod") : NULL;
    out_dir = mod ? ct_path_join(arena, mod, "out") : NULL;
    out_c = out_dir ? ct_path_join(arena, out_dir, "e2e.c") : NULL;
    out_exe = out_dir ? ct_path_join(arena, out_dir, "e2e.exe") : NULL;
    out_plain = out_dir ? ct_path_join(arena, out_dir, "e2e") : NULL;
    if (!mod || !src_copy || !manifest || !out_c || !out_exe || !out_plain || e2e_mkdir(work) != 0 ||
        e2e_mkdir(mod) != 0) {
        fprintf(stderr, "FAIL %s: cannot create a temporary module\n", display);
        CHECK(0);
        goto cleanup;
    }
    if (!ct_file_read(arena, diag, source, &file) || !collect_expects(arena, file.data, file.size, &expects) ||
        !write_text(manifest, "module e2e\n") || !copy_file(source, src_copy)) {
        fprintf(stderr, "FAIL %s: cannot stage the program\n", display);
        CHECK(0);
        goto cleanup;
    }
    argv[0] = jog;
    argv[1] = "run";
    argv[2] = mod;
    argv[3] = NULL;
    if (!ct_proc_run(argv, stdout_path, stderr_path, &code)) {
        fprintf(stderr, "FAIL %s: cannot run jog\n", display);
        CHECK(0);
        goto cleanup;
    }
    stdout_text = read_stripped(stdout_path);
    stderr_text = read_stripped(stderr_path);
    if (!stdout_text || !stderr_text) {
        fprintf(stderr, "FAIL %s: cannot read jog output\n", display);
        CHECK(0);
        goto cleanup;
    }
    if (code != 0 && strstr(stderr_text, "not implemented")) {
        printf("SKIP %s: %s\n", display, skip_reason(stderr_text));
        result = E2E_SKIP;
        goto cleanup;
    }
    if (code != 0) {
        fprintf(stderr, "FAIL %s: jog exited %d\n%s\n", display, code, stderr_text);
        CHECK(code == 0);
        goto cleanup;
    }
    if (!outputs_match(&expects, stdout_text)) {
        fprintf(stderr, "FAIL %s: stdout did not match // expect: lines\n", display);
        CHECK(0);
        goto cleanup;
    }
    result = E2E_PASS;

cleanup:
    free(stdout_text);
    free(stderr_text);
    remove_if(stdout_path);
    remove_if(stderr_path);
    remove_if(src_copy);
    remove_if(manifest);
    remove_if(out_c);
    remove_if(out_exe);
    remove_if(out_plain);
    if (out_dir) {
        e2e_rmdir(out_dir);
    }
    if (mod) {
        e2e_rmdir(mod);
    }
    if (work) {
        e2e_rmdir(work);
    }
    ct_da_free(&expects);
    return result;
}

int main(void) {
    CtArena *arena = ct_arena_create();
    CtDiag *diag = arena ? ct_diag_create(arena) : NULL;
    const char *jog = arena ? jog_executable(arena) : NULL;
    const char *sources = arena ? ct_path_join(arena, CT_SOURCE_DIR, "tests/jog") : NULL;
    const char *saved_root = NULL;
    int had_root = 0;
    CtDa names;
    int names_ready = ct_da_init(&names, sizeof(const char *));
    int passed = 0;
    int skipped = 0;
    size_t i = 0;
    const char *env = getenv("JOG_ROOT");
    if (env && env[0] != '\0') {
        had_root = 1;
        saved_root = dup_cstr(env);
    }
    CHECK(arena && diag && jog && sources && names_ready && (!had_root || saved_root));
    if (!arena || !diag || !jog || !sources || !names_ready || (had_root && !saved_root)) {
        fprintf(stderr, "FAIL: e2e harness could not find jog or the tests/jog directory\n");
        ct_da_free(&names);
        ct_arena_destroy(arena);
        free((void *)saved_root);
        return 1;
    }
    if (!env_set(CT_SOURCE_DIR)) {
        fprintf(stderr, "FAIL: cannot set JOG_ROOT\n");
        CHECK(0);
        ct_da_free(&names);
        ct_arena_destroy(arena);
        free((void *)saved_root);
        return 1;
    }
    if (!ct_fs_list(arena, diag, sources, CT_FS_FILES, ".jog", &names)) {
        ct_diag_print(diag, stderr);
        CHECK(0);
    }
    for (i = 0; i < names.count; i++) {
        const char *name = *(const char **)ct_da_at(&names, i);
        const char *source = NULL;
        const char *display = NULL;
        int result = E2E_FAIL;
        if (!name || is_test_source(name)) {
            continue;
        }
        source = ct_path_join(arena, sources, name);
        display = ct_path_join(arena, "tests/jog", name);
        if (!source || !display) {
            fprintf(stderr, "FAIL: out of memory\n");
            CHECK(0);
            break;
        }
        result = run_one(arena, diag, jog, source, display);
        if (result == E2E_PASS) {
            passed++;
        } else if (result == E2E_SKIP) {
            skipped++;
        }
    }
    printf("e2e: %d passed, %d skipped, %d failed\n", passed, skipped, g_failures);
    if (had_root) {
        env_set(saved_root);
    } else {
        env_set(NULL);
    }
    free((void *)saved_root);
    ct_da_free(&names);
    ct_arena_destroy(arena);
    return g_failures ? 1 : 0;
}
