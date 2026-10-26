#include "ir.h"
#include "phases.h"

#include "fs.h"
#include "proc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define EXE_SUFFIX ".exe"
#else
#include <unistd.h>
#define EXE_SUFFIX ""
#endif

static int g_failures = 0;
static unsigned g_seq = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

typedef struct Env {
    CtArena *arena;
    CtDiag *diag;
    JogTypes *types;
    JirModule *module;
} Env;

static Env env_open(void) {
    Env env;
    env.arena = ct_arena_create();
    env.diag = env.arena ? ct_diag_create(env.arena) : NULL;
    env.types = env.arena ? jog_types_create(env.arena, 8) : NULL;
    env.module = env.types ? jir_module_create(env.arena, env.types) : NULL;
    return env;
}

static void env_close(Env *env) {
    if (!env->arena) {
        return;
    }
    jir_module_destroy(env->module);
    jog_types_release(env->types);
    ct_arena_destroy(env->arena);
}

static int env_ok(const Env *env) {
    return env->arena && env->diag && env->types && env->module;
}

static JogType *fn_of(Env *env, JogType *ret) {
    return jog_type_fn(env->types, NULL, 0, ret, NULL, 0, false, false, false);
}

static JirFunc *add_fn(Env *env, const char *name, JogType *type, JirLinkage linkage) {
    int32_t id = jir_func_add(env->module, name, name, linkage, type, NULL);
    CHECK(id >= 0);
    if (id < 0) {
        return NULL;
    }
    return jir_func(env->module, (uint32_t)id);
}

static void emit_bin(JirFunc *f, uint32_t block, int32_t dst, JirBinOp op, JirOperand a, JirOperand b, int checked) {
    JirInst *in = jir_emit(f, block, JIR_BINARY, dst);
    CHECK(in != NULL);
    if (!in) {
        return;
    }
    in->aux = (uint32_t)op;
    in->checked = checked ? true : false;
    in->a = a;
    in->b = b;
}

static void term_br(JirFunc *f, uint32_t block, uint32_t target) {
    JirTerm term;
    memset(&term, 0, sizeof term);
    term.kind = JIR_TERM_BR;
    term.target = target;
    jir_terminate(f, block, term);
}

static void term_cond(JirFunc *f, uint32_t block, JirOperand value, uint32_t target, uint32_t else_target) {
    JirTerm term;
    memset(&term, 0, sizeof term);
    term.kind = JIR_TERM_COND_BR;
    term.value = value;
    term.target = target;
    term.else_target = else_target;
    jir_terminate(f, block, term);
}

static void term_ret(JirFunc *f, uint32_t block, uint32_t local) {
    JirTerm term;
    memset(&term, 0, sizeof term);
    term.kind = JIR_TERM_RET;
    term.value = jir_local(f, local);
    jir_terminate(f, block, term);
}

static void copy_int(JirFunc *f, uint32_t block, uint32_t dst, JogType *type, uint64_t bits) {
    JirInst *in = jir_emit(f, block, JIR_COPY, (int32_t)dst);
    CHECK(in != NULL);
    if (in) {
        in->a = jir_int(type, bits);
    }
}

static int verified(Env *env) {
    if (jir_verify(env->module, env->diag)) {
        return 1;
    }
    fprintf(stderr, "verify failed\n");
    ct_diag_print(env->diag, stderr);
    g_failures++;
    return 0;
}

static char *diag_text(CtDiag *diag) {
    FILE *fp = tmpfile();
    long n;
    char *buf;
    size_t got;
    if (!fp) {
        return NULL;
    }
    ct_diag_print(diag, fp);
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
    got = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

static void expect_has(const char *text, const char *needle) {
    if (!text || !strstr(text, needle)) {
        fprintf(stderr, "FAIL missing '%s'\n%s\n", needle, text ? text : "(null)");
        g_failures++;
    }
}

static uint64_t le_bytes(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    size_t i;
    for (i = 0; i < n && i < 8 && p; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

static int interp_u64(Env *env, uint32_t fn, size_t n, uint64_t budget, uint64_t *out) {
    const uint8_t *bytes = NULL;
    if (!jir_interp_call(env->module, fn, NULL, 0, budget, &bytes, env->diag) || !bytes) {
        ct_diag_print(env->diag, stderr);
        return 0;
    }
    *out = le_bytes(bytes, n);
    return 1;
}

static unsigned proc_id(void) {
#if defined(_WIN32)
    return (unsigned)GetCurrentProcessId();
#else
    return (unsigned)getpid();
#endif
}

static int is_sep(char c) {
    return c == '/' || c == '\\';
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
    if (!dir) {
        return NULL;
    }
    (void)snprintf(name, sizeof name, "jog-interp-%u-%u%s", proc_id(), g_seq++, suffix);
    return ct_path_join(arena, dir, name);
}

static const char *file_in_dir(CtArena *arena, const char *dir, const char *name) {
    char *path = ct_path_join(arena, dir, name);
    const char *base = name;
    if (!path) {
        return NULL;
    }
    if (ct_fs_is_file(path)) {
        return path;
    }
#if defined(_WIN32)
    for (const char *p = name; *p; p++) {
        if (is_sep(*p)) {
            base = p + 1;
        }
    }
    if (!strchr(base, '.')) {
        size_t len = strlen(path);
        char *exe = ct_arena_alloc(arena, len + 5, 1);
        if (!exe) {
            return NULL;
        }
        memcpy(exe, path, len);
        memcpy(exe + len, ".exe", 5);
        if (ct_fs_is_file(exe)) {
            return exe;
        }
    }
#else
    (void)base;
#endif
    return NULL;
}

static const char *search_path(CtArena *arena, const char *name) {
    const char *path = getenv("PATH");
    const char *start = path;
    char sep = ';';
    if (!path) {
        return NULL;
    }
#if !defined(_WIN32)
    sep = ':';
#endif
    for (;;) {
        const char *end = start;
        char *dir = NULL;
        const char *found = NULL;
        while (*end && *end != sep) {
            end++;
        }
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
            return NULL;
        }
        found = file_in_dir(arena, dir, name);
        if (found) {
            return found;
        }
        if (*end == '\0') {
            break;
        }
        start = end + 1;
    }
    return NULL;
}

static const char *find_compiler(CtArena *arena) {
    const char *override = getenv("JOG_CC");
    if (override && override[0] != '\0') {
        int slash = 0;
        const char *p;
        for (p = override; *p; p++) {
            if (is_sep(*p)) {
                slash = 1;
            }
        }
        return slash ? file_in_dir(arena, "", override) : search_path(arena, override);
    }
    {
        const char *clang = search_path(arena, "clang");
        if (clang) {
            return clang;
        }
        return search_path(arena, "gcc");
    }
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

static int write_parts(const char *path, const char *a, const char *b) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return 0;
    }
    if (a && fputs(a, fp) < 0) {
        fclose(fp);
        return 0;
    }
    if (b && fputs(b, fp) < 0) {
        fclose(fp);
        return 0;
    }
    return fclose(fp) == 0;
}

static const char k_harness[] =
    "\n#include <stdio.h>\n"
    "int main(void) {\n"
    "    printf(\"%d\\n\", (int)jog_add());\n"
    "    printf(\"%d\\n\", (int)jog_sum());\n"
    "    printf(\"%d\\n\", (int)jog_slot());\n"
    "    printf(\"%u\\n\", (unsigned)jog_mix());\n"
    "    printf(\"%d\\n\", (int)jog_twice());\n"
    "    printf(\"%d\\n\", (int)jog_g());\n"
    "    return 0;\n"
    "}\n";

static const char k_ok_out[] =
    "42\n"
    "6\n"
    "7\n"
    "1996959894\n"
    "42\n"
    "9\n";

static int compile_src(CtArena *arena, const char *cc, const char *src, const char *exe) {
    char *err_path = temp_path(arena, ".err");
    const char *argv[] = {cc,
                          "-std=gnu17",
                          "-O0",
                          "-Wall",
                          "-Wextra",
                          "-Werror",
                          "-I",
                          CT_SOURCE_DIR "/runtime",
                          src,
                          CT_SOURCE_DIR "/runtime/jogrt.c",
                          CT_SOURCE_DIR "/runtime/jogrt_sched.c",
                          CT_SOURCE_DIR "/runtime/jogrt_switch.c",
                          "-o",
                          exe,
                          NULL};
    int code = -1;
    char *err = NULL;
    int ok = 0;
    if (!err_path || !ct_proc_run(argv, NULL, err_path, &code) || code != 0) {
        err = err_path ? read_stripped(err_path) : NULL;
        fprintf(stderr, "compile failed with %s (status %d)\n%s\n", cc, code, err ? err : "(no stderr)");
        free(err);
    } else {
        ok = 1;
    }
    if (err_path) {
        remove(err_path);
    }
    return ok;
}

static uint32_t func_index(JirModule *module, JirFunc *func) {
    uint32_t i;
    for (i = 0; i < module->funcs.count; i++) {
        if (jir_func(module, i) == func) {
            return i;
        }
    }
    return UINT32_MAX;
}

static int build_agree(Env *env, uint32_t *add_i, uint32_t *sum_i, uint32_t *slot_i, uint32_t *mix_i, uint32_t *twice_i,
                       uint32_t *g_i) {
    JogType *i32 = jog_type_builtin(env->types, JOG_BUILTIN_I32);
    JogType *u32 = jog_type_builtin(env->types, JOG_BUILTIN_U32);
    JogType *boolean = jog_type_builtin(env->types, JOG_BUILTIN_BOOL);
    JogType *usize = jog_type_builtin(env->types, JOG_BUILTIN_USIZE);
    JogType *arr;
    JogType *arr_ptr;
    JogType *elem_ptr;
    JirFunc *add;
    JirFunc *sum;
    JirFunc *slot;
    JirFunc *mix;
    JirFunc *twice;
    JirFunc *gfn;
    static const uint8_t nine[4] = {9, 0, 0, 0};
    int32_t gindex;
    JirGlobal *global;
    uint32_t block;
    uint32_t dst;
    uint32_t t;
    uint32_t ap;
    uint32_t ep;
    uint32_t elem;
    uint32_t loaded;
    uint32_t c;
    uint32_t k;
    uint32_t cmp;
    uint32_t bit;
    uint32_t tmp;
    uint32_t entry;
    uint32_t head;
    uint32_t body;
    uint32_t odd;
    uint32_t even;
    uint32_t step;
    uint32_t exit_b;
    JirInst *in;
    if (!i32 || !u32 || !boolean || !usize) {
        return 0;
    }
    add = add_fn(env, "jog_add", fn_of(env, i32), JIR_LINK_INTERNAL);
    if (!add) {
        return 0;
    }
    block = jir_block_add(add, "entry");
    dst = jir_local_add(add, NULL, i32);
    emit_bin(add, block, (int32_t)dst, JIR_ADD, jir_int(i32, 20), jir_int(i32, 22), 1);
    term_ret(add, block, dst);
    *add_i = func_index(env->module, add);

    sum = add_fn(env, "jog_sum", fn_of(env, i32), JIR_LINK_INTERNAL);
    if (!sum) {
        return 0;
    }
    {
        uint32_t i_loc = jir_local_add(sum, "i", i32);
        uint32_t sum_l = jir_local_add(sum, "sum", i32);
        uint32_t cmp_l = jir_local_add(sum, "cmp", boolean);
        uint32_t ent = jir_block_add(sum, "entry");
        uint32_t hd = jir_block_add(sum, "head");
        uint32_t bd = jir_block_add(sum, "body");
        uint32_t ex = jir_block_add(sum, "exit");
        copy_int(sum, ent, i_loc, i32, 0);
        copy_int(sum, ent, sum_l, i32, 0);
        term_br(sum, ent, hd);
        emit_bin(sum, hd, (int32_t)cmp_l, JIR_LT, jir_local(sum, i_loc), jir_int(i32, 4), 0);
        term_cond(sum, hd, jir_local(sum, cmp_l), bd, ex);
        emit_bin(sum, bd, (int32_t)sum_l, JIR_ADD, jir_local(sum, sum_l), jir_local(sum, i_loc), 1);
        emit_bin(sum, bd, (int32_t)i_loc, JIR_ADD, jir_local(sum, i_loc), jir_int(i32, 1), 1);
        term_br(sum, bd, hd);
        term_ret(sum, ex, sum_l);
    }
    *sum_i = func_index(env->module, sum);

    arr = jog_type_array(env->types, u32, 4);
    arr_ptr = arr ? jog_type_ptr(env->types, arr, false) : NULL;
    elem_ptr = jog_type_ptr(env->types, u32, false);
    slot = arr && arr_ptr && elem_ptr ? add_fn(env, "jog_slot", fn_of(env, u32), JIR_LINK_INTERNAL) : NULL;
    if (!slot) {
        return 0;
    }
    t = jir_local_add(slot, "t", arr);
    ap = jir_local_add(slot, "ap", arr_ptr);
    ep = jir_local_add(slot, "ep", elem_ptr);
    elem = jir_local_add(slot, "elem", elem_ptr);
    loaded = jir_local_add(slot, "loaded", u32);
    block = jir_block_add(slot, "entry");
    jir_emit(slot, block, JIR_ZERO, (int32_t)t);
    in = jir_emit(slot, block, JIR_ADDR, (int32_t)ap);
    CHECK(in != NULL);
    if (in) {
        in->a = jir_local(slot, t);
    }
    in = jir_emit(slot, block, JIR_BITCAST, (int32_t)ep);
    CHECK(in != NULL);
    if (in) {
        in->a = jir_local(slot, ap);
    }
    in = jir_emit(slot, block, JIR_BOUNDS_CHECK, -1);
    CHECK(in != NULL);
    if (in) {
        in->a = jir_int(usize, 1);
        in->b = jir_int(usize, 4);
    }
    in = jir_emit(slot, block, JIR_ELEM_ADDR, (int32_t)elem);
    CHECK(in != NULL);
    if (in) {
        in->a = jir_local(slot, ep);
        in->b = jir_int(usize, 1);
    }
    in = jir_emit(slot, block, JIR_STORE, -1);
    CHECK(in != NULL);
    if (in) {
        in->a = jir_local(slot, elem);
        in->b = jir_int(u32, 7);
    }
    in = jir_emit(slot, block, JIR_LOAD, (int32_t)loaded);
    CHECK(in != NULL);
    if (in) {
        in->a = jir_local(slot, elem);
    }
    term_ret(slot, block, loaded);
    *slot_i = func_index(env->module, slot);

    mix = add_fn(env, "jog_mix", fn_of(env, u32), JIR_LINK_INTERNAL);
    if (!mix) {
        return 0;
    }
    c = jir_local_add(mix, "c", u32);
    k = jir_local_add(mix, "k", u32);
    cmp = jir_local_add(mix, "cmp", boolean);
    bit = jir_local_add(mix, "bit", u32);
    tmp = jir_local_add(mix, "tmp", u32);
    entry = jir_block_add(mix, "entry");
    head = jir_block_add(mix, "head");
    body = jir_block_add(mix, "body");
    odd = jir_block_add(mix, "odd");
    even = jir_block_add(mix, "even");
    step = jir_block_add(mix, "step");
    exit_b = jir_block_add(mix, "exit");
    copy_int(mix, entry, c, u32, 1);
    copy_int(mix, entry, k, u32, 0);
    term_br(mix, entry, head);
    emit_bin(mix, head, (int32_t)cmp, JIR_LT, jir_local(mix, k), jir_int(u32, 8), 0);
    term_cond(mix, head, jir_local(mix, cmp), body, exit_b);
    emit_bin(mix, body, (int32_t)bit, JIR_AND, jir_local(mix, c), jir_int(u32, 1), 0);
    emit_bin(mix, body, (int32_t)cmp, JIR_NE, jir_local(mix, bit), jir_int(u32, 0), 0);
    term_cond(mix, body, jir_local(mix, cmp), odd, even);
    emit_bin(mix, odd, (int32_t)tmp, JIR_SHR, jir_local(mix, c), jir_int(u32, 1), 1);
    emit_bin(mix, odd, (int32_t)c, JIR_XOR, jir_int(u32, 0xEDB88320u), jir_local(mix, tmp), 0);
    term_br(mix, odd, step);
    emit_bin(mix, even, (int32_t)c, JIR_SHR, jir_local(mix, c), jir_int(u32, 1), 1);
    term_br(mix, even, step);
    emit_bin(mix, step, (int32_t)k, JIR_ADD, jir_local(mix, k), jir_int(u32, 1), 1);
    term_br(mix, step, head);
    term_ret(mix, exit_b, c);
    *mix_i = func_index(env->module, mix);

    twice = add_fn(env, "jog_twice", fn_of(env, i32), JIR_LINK_INTERNAL);
    if (!twice) {
        return 0;
    }
    block = jir_block_add(twice, "entry");
    dst = jir_local_add(twice, NULL, i32);
    in = jir_emit(twice, block, JIR_CALL, (int32_t)dst);
    CHECK(in != NULL);
    if (in) {
        in->a = jir_func_ref(env->module, *add_i);
    }
    term_ret(twice, block, dst);
    *twice_i = func_index(env->module, twice);

    gindex = jir_global_add(env->module, "jog_g_value", i32, true);
    global = gindex >= 0 ? jir_global(env->module, (uint32_t)gindex) : NULL;
    CHECK(global != NULL);
    if (!global) {
        return 0;
    }
    global->data = nine;
    global->data_len = 4;
    gfn = add_fn(env, "jog_g", fn_of(env, i32), JIR_LINK_INTERNAL);
    if (!gfn) {
        return 0;
    }
    block = jir_block_add(gfn, "entry");
    dst = jir_local_add(gfn, NULL, i32);
    in = jir_emit(gfn, block, JIR_LOAD, (int32_t)dst);
    CHECK(in != NULL);
    if (in) {
        in->a = jir_global_ref(env->module, (uint32_t)gindex);
    }
    term_ret(gfn, block, dst);
    *g_i = func_index(env->module, gfn);
    return 1;
}

static void test_agree(void) {
    Env env = env_open();
    uint32_t add_i = 0;
    uint32_t sum_i = 0;
    uint32_t slot_i = 0;
    uint32_t mix_i = 0;
    uint32_t twice_i = 0;
    uint32_t g_i = 0;
    uint64_t got = 0;
    CtSb sb;
    CtArena *scratch;
    const char *cc;
    char *path = NULL;
    char *exe = NULL;
    char *out_path = NULL;
    char *err_path = NULL;
    char *out_text = NULL;
    int code = -1;
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    CHECK(build_agree(&env, &add_i, &sum_i, &slot_i, &mix_i, &twice_i, &g_i));
    if (!verified(&env)) {
        env_close(&env);
        return;
    }
    CHECK(interp_u64(&env, add_i, 4, 10000000ull, &got) && got == 42);
    CHECK(interp_u64(&env, sum_i, 4, 10000000ull, &got) && got == 6);
    CHECK(interp_u64(&env, slot_i, 4, 10000000ull, &got) && got == 7);
    CHECK(interp_u64(&env, mix_i, 4, 10000000ull, &got) && got == 1996959894ull);
    CHECK(interp_u64(&env, twice_i, 4, 10000000ull, &got) && got == 42);
    CHECK(interp_u64(&env, g_i, 4, 10000000ull, &got) && got == 9);
    ct_sb_init(&sb);
    CHECK(jog_emit_c(env.module, JOG_BUILD_DEBUG, &sb, env.diag));
    CHECK(ct_diag_error_count(env.diag) == 0);
    scratch = ct_arena_create();
    cc = scratch ? find_compiler(scratch) : NULL;
    CHECK(cc != NULL);
    if (scratch && cc && !sb.failed) {
        path = temp_path(scratch, ".c");
        exe = temp_path(scratch, EXE_SUFFIX);
        CHECK(path && exe && write_parts(path, ct_sb_cstr(&sb), k_harness));
        if (path && exe && compile_src(scratch, cc, path, exe)) {
            out_path = temp_path(scratch, ".out");
            err_path = temp_path(scratch, ".err");
            code = -1;
            if (out_path && err_path) {
                const char *argv[2];
                argv[0] = exe;
                argv[1] = NULL;
                if (ct_proc_run(argv, out_path, err_path, &code)) {
                    out_text = read_stripped(out_path);
                }
            }
            if (code != 0 || !out_text || strcmp(out_text, k_ok_out) != 0) {
                fprintf(stderr, "C run status %d\n got:\n%s\n want:\n%s\n", code, out_text ? out_text : "(null)",
                        k_ok_out);
                g_failures++;
            }
            free(out_text);
            if (out_path) {
                remove(out_path);
            }
            if (err_path) {
                remove(err_path);
            }
        }
        if (path) {
            remove(path);
        }
        if (exe) {
            remove(exe);
        }
    }
    ct_arena_destroy(scratch);
    ct_sb_free(&sb);
    env_close(&env);
}

static void test_div0(void) {
    Env env = env_open();
    JogType *i32;
    JirFunc *f;
    uint32_t block;
    uint32_t dst;
    const uint8_t *bytes = NULL;
    char *text;
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    f = add_fn(&env, "div0", fn_of(&env, i32), JIR_LINK_INTERNAL);
    if (f && i32) {
        block = jir_block_add(f, "entry");
        dst = jir_local_add(f, NULL, i32);
        emit_bin(f, block, (int32_t)dst, JIR_DIV, jir_int(i32, 1), jir_int(i32, 0), 1);
        term_ret(f, block, dst);
        CHECK(verified(&env));
        CHECK(!jir_interp_call(env.module, func_index(env.module, f), NULL, 0, 10000000ull, &bytes, env.diag));
        text = diag_text(env.diag);
        expect_has(text, "compile-time panic: divide by zero");
        expect_has(text, "at div0");
        free(text);
    }
    env_close(&env);
}

static void test_extern(void) {
    Env env = env_open();
    JogType *i32;
    JogType *fn;
    JirFunc *caller;
    int32_t ext;
    uint32_t block;
    uint32_t dst;
    JirInst *in;
    const uint8_t *bytes = NULL;
    char *text;
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    fn = i32 ? fn_of(&env, i32) : NULL;
    ext = fn ? jir_func_add(env.module, "jog_extern_target", "jog_extern_target", JIR_LINK_EXTERN, fn, NULL) : -1;
    caller = fn ? add_fn(&env, "jog_extern_call", fn, JIR_LINK_INTERNAL) : NULL;
    CHECK(ext >= 0 && caller);
    if (caller && ext >= 0) {
        block = jir_block_add(caller, "entry");
        dst = jir_local_add(caller, NULL, i32);
        in = jir_emit(caller, block, JIR_CALL, (int32_t)dst);
        CHECK(in != NULL);
        if (in) {
            in->a = jir_func_ref(env.module, (uint32_t)ext);
        }
        term_ret(caller, block, dst);
        CHECK(verified(&env));
        CHECK(!jir_interp_call(env.module, func_index(env.module, caller), NULL, 0, 10000000ull, &bytes, env.diag));
        text = diag_text(env.diag);
        expect_has(text, "cannot call an extern function");
        free(text);
    }
    env_close(&env);
}

static void test_budget(void) {
    Env env = env_open();
    JogType *i32;
    JogType *boolean;
    JirFunc *sum;
    const uint8_t *bytes = NULL;
    char *text;
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    boolean = jog_type_builtin(env.types, JOG_BUILTIN_BOOL);
    sum = i32 && boolean ? add_fn(&env, "jog_sum", fn_of(&env, i32), JIR_LINK_INTERNAL) : NULL;
    if (sum) {
        uint32_t i_loc = jir_local_add(sum, "i", i32);
        uint32_t sum_l = jir_local_add(sum, "sum", i32);
        uint32_t cmp_l = jir_local_add(sum, "cmp", boolean);
        uint32_t ent = jir_block_add(sum, "entry");
        uint32_t hd = jir_block_add(sum, "head");
        uint32_t bd = jir_block_add(sum, "body");
        uint32_t ex = jir_block_add(sum, "exit");
        copy_int(sum, ent, i_loc, i32, 0);
        copy_int(sum, ent, sum_l, i32, 0);
        term_br(sum, ent, hd);
        emit_bin(sum, hd, (int32_t)cmp_l, JIR_LT, jir_local(sum, i_loc), jir_int(i32, 4), 0);
        term_cond(sum, hd, jir_local(sum, cmp_l), bd, ex);
        emit_bin(sum, bd, (int32_t)sum_l, JIR_ADD, jir_local(sum, sum_l), jir_local(sum, i_loc), 1);
        emit_bin(sum, bd, (int32_t)i_loc, JIR_ADD, jir_local(sum, i_loc), jir_int(i32, 1), 1);
        term_br(sum, bd, hd);
        term_ret(sum, ex, sum_l);
        CHECK(verified(&env));
        CHECK(!jir_interp_call(env.module, func_index(env.module, sum), NULL, 0, 4, &bytes, env.diag));
        text = diag_text(env.diag);
        expect_has(text, "compile-time step budget exceeded");
        free(text);
    }
    env_close(&env);
}

int main(void) {
    test_agree();
    test_div0();
    test_extern();
    test_budget();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
