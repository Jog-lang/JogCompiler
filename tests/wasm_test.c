#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "fs.h"
#include "proc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

/* jog run examples/hello stays the native greeting. --target wasm32
   compiles that program with clang --target=wasm32 and runs it under node. */

static int g_failures = 0;
static unsigned g_seq = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

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
    size_t len;
    size_t slash;
    char *out;
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

#if defined(_WIN32)
#define WASM_PATH_SEP ';'
#else
#define WASM_PATH_SEP ':'
#endif

static int file_named(CtArena *arena, const char *dir, const char *name) {
    char *path = ct_path_join(arena, dir, name);
    if (path && ct_fs_is_file(path)) {
        return 1;
    }
#if defined(_WIN32)
    if (path && !strchr(name, '.')) {
        size_t len = strlen(path);
        char *exe = ct_arena_alloc(arena, len + 5, 1);
        if (!exe) {
            return 0;
        }
        memcpy(exe, path, len);
        memcpy(exe + len, ".exe", 5);
        if (ct_fs_is_file(exe)) {
            return 1;
        }
    }
#endif
    return 0;
}

/* node runs a built wasm module. A machine without it can still compile. */
static int node_on_path(CtArena *arena) {
    const char *path = ct_fs_getenv(arena, "PATH");
    const char *start;
    if (!arena || !path) {
        return 0;
    }
    start = path;
    for (;;) {
        const char *end = start;
        char *dir = NULL;
        size_t len;
        while (*end && *end != WASM_PATH_SEP) {
            end++;
        }
        len = (size_t)(end - start);
        if (len == 0) {
            dir = ct_arena_strdup(arena, ".");
        } else {
            dir = ct_arena_alloc(arena, len + 1, 1);
            if (dir) {
                memcpy(dir, start, len);
                dir[len] = '\0';
            }
        }
        if (!dir) {
            return 0;
        }
        if (file_named(arena, dir, "node")) {
            return 1;
        }
        if (*end == '\0') {
            break;
        }
        start = end + 1;
    }
    return 0;
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
    return ct_arena_strdup(arena, dir);
#endif
}

static char *temp_path(CtArena *arena, const char *suffix) {
    char name[96];
    const char *dir = temp_dir(arena);
    unsigned pid = 0;
    int wrote;
    if (!dir) {
        return NULL;
    }
#if defined(_WIN32)
    pid = GetCurrentProcessId();
#else
    pid = (unsigned)getpid();
#endif
    wrote = snprintf(name, sizeof name, "jog-wasm-%u-%u%s", pid, g_seq++, suffix);
    if (wrote < 0 || (size_t)wrote >= sizeof name) {
        return NULL;
    }
    return ct_path_join(arena, dir, name);
}

static char *read_stripped(const char *path) {
    FILE *fp = fopen(path, "rb");
    long length = 0;
    char *buf = NULL;
    size_t got = 0;
    size_t written = 0;
    size_t i;
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

static void show_file(const char *label, const char *path) {
    char *text = read_stripped(path);
    fprintf(stderr, "%s:\n%s\n", label, text ? text : "(unreadable)");
    free(text);
}

static int run_jog(const char **argv, const char *stdout_path, const char *stderr_path, int *code) {
    if (!ct_proc_run((const char *const *)argv, stdout_path, stderr_path, code)) {
        return 0;
    }
    return 1;
}

static int make_dir(const char *path) {
#if defined(_WIN32)
    if (_mkdir(path) == 0) {
        return 1;
    }
#else
    if (mkdir(path, 0777) == 0) {
        return 1;
    }
#endif
    return ct_fs_is_dir(path);
}

static int write_text(const char *path, const char *text) {
    FILE *fp;
    size_t n;
    if (!path || !text) {
        return 0;
    }
    fp = fopen(path, "wb");
    if (!fp) {
        return 0;
    }
    n = strlen(text);
    if (n > 0 && fwrite(text, 1, n, fp) != n) {
        fclose(fp);
        return 0;
    }
    return fclose(fp) == 0;
}

static char *make_module(CtArena *arena, const char *mod_name, const char *source) {
    char *dir = temp_path(arena, "");
    const char *mod_path;
    const char *src_path;
    char mod_text[80];
    int wrote;
    if (!dir || !make_dir(dir)) {
        return NULL;
    }
    wrote = snprintf(mod_text, sizeof mod_text, "module %s\n", mod_name);
    if (wrote < 0 || (size_t)wrote >= sizeof mod_text) {
        return NULL;
    }
    mod_path = ct_path_join(arena, dir, "jog.mod");
    src_path = ct_path_join(arena, dir, "main.jog");
    if (!mod_path || !src_path || !write_text(mod_path, mod_text) || !write_text(src_path, source)) {
        return NULL;
    }
    return dir;
}

static void remove_module(CtArena *arena, const char *dir, const char *mod_name) {
    const char *out;
    const char *path;
    char name[96];
    if (!arena || !dir) {
        return;
    }
    path = ct_path_join(arena, dir, "main.jog");
    if (path) {
        remove(path);
    }
    path = ct_path_join(arena, dir, "jog.mod");
    if (path) {
        remove(path);
    }
    out = ct_path_join(arena, dir, "out");
    if (out && mod_name) {
        snprintf(name, sizeof name, "%s.c", mod_name);
        path = ct_path_join(arena, out, name);
        if (path) {
            remove(path);
        }
        snprintf(name, sizeof name, "%s.wasm", mod_name);
        path = ct_path_join(arena, out, name);
        if (path) {
            remove(path);
        }
        snprintf(name, sizeof name, "%s.exe", mod_name);
        path = ct_path_join(arena, out, name);
        if (path) {
            remove(path);
        }
        path = ct_path_join(arena, out, mod_name);
        if (path) {
            remove(path);
        }
#if defined(_WIN32)
        _rmdir(out);
#else
        rmdir(out);
#endif
    }
#if defined(_WIN32)
    _rmdir(dir);
#else
    rmdir(dir);
#endif
}

static int read_leb_u32(const unsigned char *p, size_t n, size_t *at, uint32_t *out) {
    uint32_t result = 0;
    unsigned shift = 0;
    for (;;) {
        unsigned char b;
        if (*at >= n || shift > 28) {
            return 0;
        }
        b = p[(*at)++];
        result |= (uint32_t)(b & 0x7fu) << shift;
        if ((b & 0x80u) == 0) {
            *out = result;
            return 1;
        }
        shift = shift + 7;
    }
}

static int read_wasm_name(const unsigned char *p, size_t n, size_t *at, char *dst, size_t cap) {
    uint32_t len = 0;
    if (*at > n || !read_leb_u32(p, n, at, &len)) {
        return 0;
    }
    if ((size_t)len + 1 > cap || *at > n || (size_t)len > n - *at) {
        return 0;
    }
    memcpy(dst, p + *at, len);
    dst[len] = '\0';
    *at += len;
    return 1;
}

typedef struct WasmFnImport {
    char module[64];
    char name[64];
} WasmFnImport;

static int wasm_fn_imports(const char *path, WasmFnImport *out, size_t cap, size_t *count) {
    FILE *fp;
    long length;
    unsigned char *buf;
    size_t n;
    size_t at;
    if (!path || !out || !count || cap == 0) {
        return 0;
    }
    *count = 0;
    fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    length = ftell(fp);
    if (length < 8 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }
    buf = malloc((size_t)length);
    if (!buf) {
        fclose(fp);
        return 0;
    }
    n = fread(buf, 1, (size_t)length, fp);
    fclose(fp);
    if (n != (size_t)length || buf[0] != 0 || buf[1] != 'a' || buf[2] != 's' || buf[3] != 'm' || buf[4] != 1) {
        free(buf);
        return 0;
    }
    at = 8;
    while (at < n) {
        unsigned char id;
        uint32_t size = 0;
        size_t payload;
        id = buf[at++];
        if (!read_leb_u32(buf, n, &at, &size) || (size_t)size > n - at) {
            free(buf);
            return 0;
        }
        payload = at;
        if (id == 2) {
            size_t cur = payload;
            size_t end = payload + size;
            uint32_t nimp = 0;
            uint32_t i;
            if (!read_leb_u32(buf, end, &cur, &nimp)) {
                free(buf);
                return 0;
            }
            for (i = 0; i < nimp; i++) {
                WasmFnImport item;
                unsigned char kind;
                uint32_t type_index = 0;
                memset(&item, 0, sizeof item);
                if (!read_wasm_name(buf, end, &cur, item.module, sizeof item.module) ||
                    !read_wasm_name(buf, end, &cur, item.name, sizeof item.name) || cur >= end) {
                    free(buf);
                    return 0;
                }
                kind = buf[cur++];
                if (kind != 0 || !read_leb_u32(buf, end, &cur, &type_index)) {
                    free(buf);
                    return 0;
                }
                (void)type_index;
                if (*count < cap) {
                    out[*count] = item;
                }
                *count += 1;
            }
            if (cur != end) {
                free(buf);
                return 0;
            }
        }
        at = payload + size;
    }
    free(buf);
    return 1;
}

static const char *import_module_named(const WasmFnImport *items, size_t n, const char *name) {
    size_t i;
    const char *found = NULL;
    for (i = 0; i < n; i++) {
        if (strcmp(items[i].name, name) == 0) {
            if (found) {
                return NULL;
            }
            found = items[i].module;
        }
    }
    return found;
}

static int file_has(const char *path, const char *needle) {
    char *text = read_stripped(path);
    int ok = text && needle && strstr(text, needle) != NULL;
    free(text);
    return ok;
}

static const char *k_argc_source =
    "extern {\n"
    "    fn puts(s: *const c_char): c_int\n"
    "    fn jogrt_argc(): c_int\n"
    "}\n"
    "type c_char = i8\n"
    "type c_int = i32\n"
    "fn main() {\n"
    "    val n = jogrt_argc()\n"
    "    if n != 3 {\n"
    "        return\n"
    "    }\n"
    "    if puts(c\"counted\") < 0 {\n"
    "        return\n"
    "    }\n"
    "}\n";

static const char *k_args_source =
    "extern {\n"
    "    fn puts(s: *const c_char): c_int\n"
    "    fn jogrt_argc(): c_int\n"
    "    fn jogrt_arg_ptr(i: c_int): *const c_char\n"
    "}\n"
    "type c_char = i8\n"
    "type c_int = i32\n"
    "fn main() {\n"
    "    val n = jogrt_argc()\n"
    "    for i in 1..n {\n"
    "        val p = jogrt_arg_ptr(i)\n"
    "        if puts(p) < 0 {\n"
    "            return\n"
    "        }\n"
    "    }\n"
    "}\n";

static const char *k_link_source =
    "@link(\"app\")\n"
    "extern {\n"
    "    fn puts(s: *const c_char): c_int\n"
    "    @link(\"side\")\n"
    "    fn jog_host_marker(): c_int\n"
    "}\n"
    "@link(\"env\")\n"
    "extern {\n"
    "    fn jog_host_ok(): c_int\n"
    "}\n"
    "type c_char = i8\n"
    "type c_int = i32\n"
    "fn main() {\n"
    "    if jog_host_ok() != 0 {\n"
    "        return\n"
    "    }\n"
    "    if jog_host_marker() < 0 {\n"
    "        return\n"
    "    }\n"
    "    if puts(c\"linked\") < 0 {\n"
    "        return\n"
    "    }\n"
    "}\n";

static const char *k_steps_source =
    "extern {\n"
    "    fn puts(s: *const c_char): c_int\n"
    "}\n"
    "type c_char = i8\n"
    "type c_int = i32\n"
    "fn spin(): i32 {\n"
    "    var i: i32 = 0\n"
    "    for i < 100 {\n"
    "        i = i + 1\n"
    "    }\n"
    "    return i\n"
    "}\n"
    "fn main() {\n"
    "    val n = @comptime(spin())\n"
    "    if n != 100 {\n"
    "        return\n"
    "    }\n"
    "    if puts(c\"spun\") < 0 {\n"
    "        return\n"
    "    }\n"
    "}\n";

static void test_program_args(CtArena *arena, const char *jog, const char *hello, int have_node) {
    char *native_dir = make_module(arena, "argc", k_argc_source);
    char *dir = make_module(arena, "args", k_args_source);
    char *out_path = temp_path(arena, ".args");
    char *err_path = temp_path(arena, ".args.err");
    const char *native_argv[8];
    const char *wasm_argv[10];
    int code = 1;
    char *text = NULL;
    CHECK(native_dir != NULL && dir != NULL && out_path && err_path);
    if (!native_dir || !dir || !out_path || !err_path) {
        return;
    }
    native_argv[0] = jog;
    native_argv[1] = "run";
    native_argv[2] = native_dir;
    native_argv[3] = "--";
    native_argv[4] = "hello there";
    native_argv[5] = "x";
    native_argv[6] = NULL;
    if (!run_jog(native_argv, out_path, err_path, &code) || code != 0) {
        show_file("args native stderr", err_path);
    }
    CHECK(code == 0);
    text = read_stripped(out_path);
    CHECK(text && strcmp(text, "counted\n") == 0);
    free(text);
    if (!have_node) {
        remove(out_path);
        remove(err_path);
        remove_module(arena, native_dir, "argc");
        remove_module(arena, dir, "args");
        return;
    }
    wasm_argv[0] = jog;
    wasm_argv[1] = "run";
    wasm_argv[2] = "--target";
    wasm_argv[3] = "wasm32";
    wasm_argv[4] = dir;
    wasm_argv[5] = "--";
    wasm_argv[6] = "hello there";
    wasm_argv[7] = "x";
    wasm_argv[8] = NULL;
    code = 1;
    if (!run_jog(wasm_argv, out_path, err_path, &code) || code != 0) {
        show_file("args wasm stderr", err_path);
        show_file("args wasm stdout", out_path);
    }
    CHECK(code == 0);
    text = read_stripped(out_path);
    if (!text || strcmp(text, "hello there\nx\n") != 0) {
        fprintf(stderr, "wasm args stdout: [%s]\n", text ? text : "(null)");
        show_file("args wasm stderr", err_path);
    }
    CHECK(text && strcmp(text, "hello there\nx\n") == 0);
    free(text);
    wasm_argv[4] = hello;
    wasm_argv[5] = "--";
    wasm_argv[6] = NULL;
    code = 1;
    if (!run_jog(wasm_argv, out_path, err_path, &code) || code != 0) {
        show_file("hello dash stderr", err_path);
        show_file("hello dash stdout", out_path);
    }
    CHECK(code == 0);
    text = read_stripped(out_path);
    CHECK(text && strcmp(text, "Hello from Jog!\n") == 0);
    free(text);
    remove(out_path);
    remove(err_path);
    remove_module(arena, native_dir, "argc");
    remove_module(arena, dir, "args");
}

static void test_link_module(CtArena *arena, const char *jog, int have_node) {
    char *dir = make_module(arena, "linkhost", k_link_source);
    char *out_path = temp_path(arena, ".link");
    char *err_path = temp_path(arena, ".link.err");
    const char *build_argv[8];
    const char *run_argv[8];
    const char *c_path;
    const char *wasm_path;
    const char *out_dir;
    WasmFnImport imports[32];
    size_t nimp = 0;
    size_t stored;
    const char *mod;
    int code = 1;
    char *text = NULL;
    CHECK(dir != NULL);
    if (!dir || !out_path || !err_path) {
        return;
    }
    build_argv[0] = jog;
    build_argv[1] = "build";
    build_argv[2] = "--target";
    build_argv[3] = "wasm32";
    build_argv[4] = "--keep-c";
    build_argv[5] = dir;
    build_argv[6] = NULL;
    if (!run_jog(build_argv, out_path, err_path, &code) || code != 0) {
        show_file("link build stderr", err_path);
    }
    CHECK(code == 0);
    out_dir = ct_path_join(arena, dir, "out");
    c_path = out_dir ? ct_path_join(arena, out_dir, "linkhost.c") : NULL;
    wasm_path = out_dir ? ct_path_join(arena, out_dir, "linkhost.wasm") : NULL;
    CHECK(c_path && file_has(c_path, "__attribute__((import_module(\"app\")))"));
    CHECK(c_path && file_has(c_path, "__attribute__((import_module(\"side\")))"));
    CHECK(c_path && file_has(c_path, "__attribute__((import_module(\"env\")))"));
    CHECK(c_path && file_has(c_path, "__attribute__((import_name(\"puts\")))"));
    CHECK(wasm_path && wasm_fn_imports(wasm_path, imports, 32, &nimp));
    stored = nimp < 32 ? nimp : 32;
    mod = import_module_named(imports, stored, "puts");
    CHECK(mod && strcmp(mod, "app") == 0);
    mod = import_module_named(imports, stored, "jog_host_marker");
    CHECK(mod && strcmp(mod, "side") == 0);
    mod = import_module_named(imports, stored, "jog_host_ok");
    CHECK(mod && strcmp(mod, "env") == 0);
    if (!have_node) {
        remove(out_path);
        remove(err_path);
        remove_module(arena, dir, "linkhost");
        return;
    }
    run_argv[0] = jog;
    run_argv[1] = "run";
    run_argv[2] = "--target";
    run_argv[3] = "wasm32";
    run_argv[4] = dir;
    run_argv[5] = NULL;
    code = 1;
    if (!run_jog(run_argv, out_path, err_path, &code) || code != 0) {
        show_file("link run stderr", err_path);
        show_file("link run stdout", out_path);
    }
    CHECK(code == 0);
    text = read_stripped(out_path);
    if (!text || strcmp(text, "marker\nlinked\n") != 0) {
        fprintf(stderr, "link stdout: [%s]\n", text ? text : "(null)");
        show_file("link run stderr", err_path);
    }
    CHECK(text && strcmp(text, "marker\nlinked\n") == 0);
    free(text);
    remove(out_path);
    remove(err_path);
    remove_module(arena, dir, "linkhost");
}

static void test_comptime_steps(CtArena *arena, const char *jog, const char *hello) {
    char *dir = make_module(arena, "steps", k_steps_source);
    char *out_path = temp_path(arena, ".steps");
    char *err_path = temp_path(arena, ".steps.err");
    const char *argv[8];
    int code = 0;
    char *text = NULL;
    char *err = NULL;
    CHECK(dir != NULL && out_path && err_path);
    if (!dir || !out_path || !err_path) {
        return;
    }
    argv[0] = jog;
    argv[1] = "run";
    argv[2] = "--comptime-steps";
    argv[3] = NULL;
    CHECK(run_jog(argv, out_path, err_path, &code));
    CHECK(code == 2);
    argv[2] = "--comptime-steps";
    argv[3] = "0";
    argv[4] = hello;
    argv[5] = NULL;
    code = 0;
    CHECK(run_jog(argv, out_path, err_path, &code));
    CHECK(code == 2);
    argv[2] = "--comptime-steps=no";
    argv[3] = hello;
    argv[4] = NULL;
    code = 0;
    CHECK(run_jog(argv, out_path, err_path, &code));
    CHECK(code == 2);
    argv[0] = jog;
    argv[1] = "run";
    argv[2] = "--comptime-steps";
    argv[3] = "1";
    argv[4] = hello;
    argv[5] = NULL;
    code = 1;
    if (!run_jog(argv, out_path, err_path, &code) || code != 0) {
        show_file("hello steps stderr", err_path);
    }
    CHECK(code == 0);
    text = read_stripped(out_path);
    CHECK(text && strcmp(text, "Hello from Jog!\n") == 0);
    free(text);
    argv[2] = "--comptime-steps";
    argv[3] = "40";
    argv[4] = dir;
    argv[5] = NULL;
    code = 0;
    CHECK(run_jog(argv, out_path, err_path, &code));
    CHECK(code != 0);
    err = read_stripped(err_path);
    CHECK(err && strstr(err, "compile-time step budget exceeded") != NULL);
    free(err);
    argv[2] = "--comptime-steps=10000000";
    argv[3] = dir;
    argv[4] = NULL;
    code = 1;
    if (!run_jog(argv, out_path, err_path, &code) || code != 0) {
        show_file("steps large stderr", err_path);
    }
    CHECK(code == 0);
    text = read_stripped(out_path);
    CHECK(text && strcmp(text, "spun\n") == 0);
    free(text);
    argv[0] = jog;
    argv[1] = "run";
    argv[2] = dir;
    argv[3] = NULL;
    code = 1;
    if (!run_jog(argv, out_path, err_path, &code) || code != 0) {
        show_file("steps default stderr", err_path);
    }
    CHECK(code == 0);
    text = read_stripped(out_path);
    CHECK(text && strcmp(text, "spun\n") == 0);
    free(text);
    remove(out_path);
    remove(err_path);
    remove_module(arena, dir, "steps");
}

int main(void) {
    CtArena *arena = ct_arena_create();
    const char *jog = arena ? jog_executable(arena) : NULL;
    const char *hello = CT_SOURCE_DIR "/examples/hello";
    const char *wasm_out = CT_SOURCE_DIR "/examples/hello/out/hello.wasm";
    char *native_out = arena ? temp_path(arena, ".native") : NULL;
    char *native_err = arena ? temp_path(arena, ".native.err") : NULL;
    char *wasm_stdout = arena ? temp_path(arena, ".wasm") : NULL;
    char *wasm_err = arena ? temp_path(arena, ".wasm.err") : NULL;
    char *bad_err = arena ? temp_path(arena, ".bad.err") : NULL;
    int code = 0;
    char *native_text = NULL;
    char *wasm_text = NULL;
    const char *bad_argv[6];
    const char *native_argv[4];
    const char *wasm_argv[6];

    CHECK(arena != NULL);
    CHECK(jog != NULL);
    CHECK(native_out && native_err && wasm_stdout && wasm_err && bad_err);
    if (!arena || !jog || !native_out || !native_err || !wasm_stdout || !wasm_err || !bad_err) {
        fprintf(stderr, "wasm_test setup failed\n");
        ct_arena_destroy(arena);
        return 1;
    }

    bad_argv[0] = jog;
    bad_argv[1] = "run";
    bad_argv[2] = "--target";
    bad_argv[3] = "wasm64";
    bad_argv[4] = hello;
    bad_argv[5] = NULL;
    CHECK(run_jog(bad_argv, NULL, bad_err, &code));
    CHECK(code == 2);

    native_argv[0] = jog;
    native_argv[1] = "run";
    native_argv[2] = hello;
    native_argv[3] = NULL;
    code = 1;
    if (!run_jog(native_argv, native_out, native_err, &code) || code != 0) {
        show_file("native stderr", native_err);
    }
    CHECK(code == 0);
    native_text = read_stripped(native_out);
    if (!native_text || strcmp(native_text, "Hello from Jog!\n") != 0) {
        fprintf(stderr, "native stdout: [%s]\n", native_text ? native_text : "(null)");
        show_file("native stderr", native_err);
    }
    CHECK(native_text && strcmp(native_text, "Hello from Jog!\n") == 0);

    {
        int have_node = node_on_path(arena);
        if (!have_node) {
            fprintf(stderr, "wasm_test: node is not on PATH; skipping wasm runs\n");
            wasm_argv[0] = jog;
            wasm_argv[1] = "build";
            wasm_argv[2] = "--target";
            wasm_argv[3] = "wasm32";
            wasm_argv[4] = hello;
            wasm_argv[5] = NULL;
            code = 1;
            if (!run_jog(wasm_argv, wasm_stdout, wasm_err, &code) || code != 0) {
                show_file("wasm build stderr", wasm_err);
            }
            CHECK(code == 0);
        } else {
            wasm_argv[0] = jog;
            wasm_argv[1] = "run";
            wasm_argv[2] = "--target";
            wasm_argv[3] = "wasm32";
            wasm_argv[4] = hello;
            wasm_argv[5] = NULL;
            code = 1;
            if (!run_jog(wasm_argv, wasm_stdout, wasm_err, &code) || code != 0) {
                show_file("wasm stderr", wasm_err);
                show_file("wasm stdout", wasm_stdout);
            }
            CHECK(code == 0);
        }
        CHECK(ct_fs_is_file(wasm_out));
        if (have_node) {
            wasm_text = read_stripped(wasm_stdout);
            if (!wasm_text || strcmp(wasm_text, "Hello from Jog!\n") != 0) {
                fprintf(stderr, "wasm stdout: [%s]\n", wasm_text ? wasm_text : "(null)");
                show_file("wasm stderr", wasm_err);
            }
            CHECK(wasm_text && strcmp(wasm_text, "Hello from Jog!\n") == 0);
        }
        {
            WasmFnImport imports[32];
            size_t nimp = 0;
            size_t stored;
            const char *mod;
            CHECK(wasm_fn_imports(wasm_out, imports, 32, &nimp));
            stored = nimp < 32 ? nimp : 32;
            mod = import_module_named(imports, stored, "puts");
            CHECK(mod && strcmp(mod, "env") == 0);
        }
        test_program_args(arena, jog, hello, have_node);
        test_link_module(arena, jog, have_node);
        test_comptime_steps(arena, jog, hello);
    }

    free(native_text);
    free(wasm_text);
    remove(native_out);
    remove(native_err);
    remove(wasm_stdout);
    remove(wasm_err);
    remove(bad_err);
    ct_arena_destroy(arena);
    if (g_failures != 0) {
        fprintf(stderr, "wasm_test: %d failure%s\n", g_failures, g_failures == 1 ? "" : "s");
        return 1;
    }
    return 0;
}
