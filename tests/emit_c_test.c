#include "ir.h"
#include "phases.h"

#include "fs.h"
#include "proc.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define EXE_SUFFIX ".exe"
#else
#include <signal.h>
#include <unistd.h>
#define EXE_SUFFIX ""
#endif

static int g_failures = 0;

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

static int verified(Env *env) {
    if (jir_verify(env->module, env->diag)) {
        return 1;
    }
    fprintf(stderr, "verify failed\n");
    ct_diag_print(env->diag, stderr);
    g_failures++;
    return 0;
}

static void expect_dump(const JirModule *module, const char *expected, const char *file, int line) {
    CtSb first;
    CtSb second;
    ct_sb_init(&first);
    ct_sb_init(&second);
    jir_dump(module, &first);
    jir_dump(module, &second);
    const char *got = ct_sb_cstr(&first);
    const char *again = ct_sb_cstr(&second);
    int same = !first.failed && !second.failed && strcmp(got, again) == 0 && strcmp(got, expected) == 0;
    if (!same) {
        fprintf(stderr, "FAIL %s:%d:\n--- got ---\n%s--- expected ---\n%s---\n", file, line, got, expected);
        if (strcmp(got, again) != 0) {
            fprintf(stderr, "dump was not stable\n--- second ---\n%s---\n", again);
        }
        g_failures++;
    }
    ct_sb_free(&first);
    ct_sb_free(&second);
}

#define EXPECT_DUMP(module, expected) expect_dump((module), (expected), __FILE__, __LINE__)

#define C_PREAMBLE                                                                                                 \
    "#include <stdint.h>\n"                                                                                        \
    "#include <stddef.h>\n"                                                                                        \
    "#include <stdbool.h>\n"                                                                                       \
    "#include \"jogrt.h\"\n"

#define C_ENTRIES                                                                                                  \
    "\n"                                                                                                           \
    "void jog_entry_init(void) {\n"                                                                                \
    "}\n"                                                                                                          \
    "\n"                                                                                                           \
    "void jog_entry_main(void) {\n"                                                                                \
    "}\n"

static void expect_emit(Env *env, const char *expected, const char *file, int line) {
    CtSb first;
    CtSb second;
    ct_sb_init(&first);
    ct_sb_init(&second);
    size_t errors = ct_diag_error_count(env->diag);
    bool ok = jog_emit_c(env->module, JOG_BUILD_DEBUG, &first, env->diag);
    bool again = jog_emit_c(env->module, JOG_BUILD_RELEASE_FAST, &second, env->diag);
    const char *got = ct_sb_cstr(&first);
    const char *other = ct_sb_cstr(&second);
    int same = ok && again && !first.failed && !second.failed && ct_diag_error_count(env->diag) == errors &&
               strcmp(got, other) == 0 && strcmp(got, expected) == 0;
    if (!same) {
        fprintf(stderr, "FAIL %s:%d:\n--- got ---\n%s--- expected ---\n%s---\n", file, line, got, expected);
        if (strcmp(got, other) != 0) {
            fprintf(stderr, "release-fast differed\n--- second ---\n%s---\n", other);
        }
        ct_diag_print(env->diag, stderr);
        g_failures++;
    }
    ct_sb_free(&first);
    ct_sb_free(&second);
}

#define EXPECT_EMIT(env, expected) expect_emit((env), (expected), __FILE__, __LINE__)

static void add_global(Env *env, JogType *type) {
    char buf[32];
    const char *name;
    snprintf(buf, sizeof buf, "g%u", (unsigned)env->module->globals.count);
    name = ct_arena_strdup(env->arena, buf);
    CHECK(name != NULL);
    if (name) {
        CHECK(jir_global_add(env->module, name, type, false) >= 0);
    }
}

static JogType *fn_of(Env *env, JogType *const *params, size_t count, JogType *ret, bool is_extern) {
    return jog_type_fn(env->types, params, count, ret, NULL, 0, false, false, is_extern);
}

static void ret_none(JirFunc *func, uint32_t block) {
    JirTerm term = {0};
    term.kind = JIR_TERM_RET;
    jir_terminate(func, block, term);
}

static void ret_local(JirFunc *func, uint32_t block, uint32_t local) {
    JirTerm term = {0};
    term.kind = JIR_TERM_RET;
    term.value = jir_local(func, local);
    jir_terminate(func, block, term);
}

static double from_bits(uint64_t bits) {
    double value = 0;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void test_empty(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    EXPECT_DUMP(NULL, "module <null>\n");
    EXPECT_DUMP(env.module, "main none\n"
                             "init none\n");
    jir_dump(NULL, NULL);
    jir_dump(env.module, NULL);
    env_close(&env);
}

static void test_max(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    JogType *bool_type = jog_type_builtin(env.types, JOG_BUILTIN_BOOL);
    JogType *params[2] = {i32, i32};
    JogType *fn_type = fn_of(&env, params, 2, i32, false);
    const char *names[2] = {"a", "b"};
    CHECK(jir_func_add(env.module, "jog__max", "max", JIR_LINK_INTERNAL, fn_type, names) == 0);
    JirFunc *f = jir_func(env.module, 0);
    uint32_t entry = jir_block_add(f, "entry");
    uint32_t then_block = jir_block_add(f, "then");
    uint32_t else_block = jir_block_add(f, "else");
    uint32_t cond = jir_local_add(f, NULL, bool_type);
    JirInst *cmp = jir_emit(f, entry, JIR_BINARY, (int32_t)cond);
    cmp->aux = JIR_GT;
    cmp->a = jir_local(f, 0);
    cmp->b = jir_local(f, 1);

    JirTerm br = {0};
    br.kind = JIR_TERM_COND_BR;
    br.value = jir_local(f, cond);
    br.target = then_block;
    br.else_target = else_block;
    jir_terminate(f, entry, br);
    ret_local(f, then_block, 0);
    ret_local(f, else_block, 1);
    if (!verified(&env)) {
        env_close(&env);
        return;
    }
    EXPECT_DUMP(env.module, "main none\n"
                             "init none\n"
                             "\n"
                             "fn f0 internal jog__max \"max\": (i32, i32) -> i32\n"
                             "  l0 a: i32 param\n"
                             "  l1 b: i32 param\n"
                             "  l2: bool\n"
                             "b0 entry:\n"
                             "  l2 = gt l0, l1\n"
                             "  cond_br l2, b1 then, b2 else\n"
                             "b1 then:\n"
                             "  ret l0\n"
                             "b2 else:\n"
                             "  ret l1\n");
    env_close(&env);
}

static void test_globals(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    JogType *str = jog_type_builtin(env.types, JOG_BUILTIN_CONST_BYTES);
    JogType *unit = jog_type_builtin(env.types, JOG_BUILTIN_UNIT);
    JogType *unit_fn = fn_of(&env, NULL, 0, unit, false);
    static const uint8_t hello[] = {0x68, 0x65, 0x6c, 0x6c, 0x6f};
    static const uint8_t empty[] = {0};

    CHECK(jir_global_add(env.module, "jog__counter", i32, false) == 0);
    CHECK(jir_global_add(env.module, "jog__msg", str, true) == 1);
    CHECK(jir_global_add(env.module, "jog__empty", str, true) == 2);
    CHECK(jir_global_add(env.module, "bad name", i32, false) == 3);
    jir_global(env.module, 1)->data = hello;
    jir_global(env.module, 1)->data_len = sizeof hello;
    jir_global(env.module, 2)->data = empty;
    jir_global(env.module, 2)->data_len = 0;

    CHECK(jir_func_add(env.module, "jog__init", "init", JIR_LINK_INTERNAL, unit_fn, NULL) == 0);
    CHECK(jir_func_add(env.module, "jog__main", "main", JIR_LINK_PUBLIC, unit_fn, NULL) == 1);
    JirFunc *init = jir_func(env.module, 0);
    JirFunc *main_fn = jir_func(env.module, 1);
    ret_none(init, jir_block_add(init, NULL));
    ret_none(main_fn, jir_block_add(main_fn, NULL));
    env.module->init_func = 0;
    env.module->main_func = 1;
    if (!verified(&env)) {
        env_close(&env);
        return;
    }
    EXPECT_DUMP(env.module, "main f1\n"
                             "init f0\n"
                             "\n"
                             "global g0 var jog__counter: i32 = zero\n"
                             "global g1 const jog__msg: const bytes = hex:68 65 6c 6c 6f\n"
                             "global g2 const jog__empty: const bytes = hex:\n"
                             "global g3 var \"bad name\": i32 = zero\n"
                             "\n"
                             "fn f0 internal jog__init \"init\": () -> ()\n"
                             "b0:\n"
                             "  ret\n"
                             "\n"
                             "fn f1 public jog__main \"main\": () -> ()\n"
                             "b0:\n"
                             "  ret\n");
    env_close(&env);
}

static void test_constants(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    JogType *i64 = jog_type_builtin(env.types, JOG_BUILTIN_I64);
    JogType *u32 = jog_type_builtin(env.types, JOG_BUILTIN_U32);
    JogType *bool_type = jog_type_builtin(env.types, JOG_BUILTIN_BOOL);
    JogType *f64 = jog_type_builtin(env.types, JOG_BUILTIN_F64);
    JogType *str = jog_type_builtin(env.types, JOG_BUILTIN_CONST_BYTES);
    JogType *u8 = jog_type_builtin(env.types, JOG_BUILTIN_U8);
    JogType *cstr = jog_type_ptr(env.types, u8, true);
    JogType *opt = jog_type_option(env.types, i32);
    JogType *unit = jog_type_builtin(env.types, JOG_BUILTIN_UNIT);
    JogType *fn_type = fn_of(&env, NULL, 0, unit, false);
    CHECK(jir_func_add(env.module, "jog__consts", "consts", JIR_LINK_INTERNAL, fn_type, NULL) == 0);
    JirFunc *f = jir_func(env.module, 0);
    uint32_t li32 = jir_local_add(f, NULL, i32);
    uint32_t li64 = jir_local_add(f, NULL, i64);
    uint32_t lu32 = jir_local_add(f, NULL, u32);
    uint32_t lbool = jir_local_add(f, NULL, bool_type);
    uint32_t lf64 = jir_local_add(f, NULL, f64);
    uint32_t lstr = jir_local_add(f, NULL, str);
    uint32_t lcstr = jir_local_add(f, NULL, cstr);
    uint32_t lopt = jir_local_add(f, NULL, opt);
    uint32_t block = jir_block_add(f, NULL);

    const char raw[] = {'a', '"', 'b', '\n', '\x01'};
    const char cs[] = {'h', 'i', '\t'};
    struct {
        uint32_t dst;
        JirOperand value;
    } copies[] = {
        {li32, jir_int(i32, ~(uint64_t)1)},
        {li32, jir_int(i32, 0x80000000u)},
        {li64, jir_int(i64, ~(uint64_t)0)},
        {lu32, jir_int(u32, 0xffffffffu)},
        {lbool, jir_int(bool_type, 1)},
        {lbool, jir_int(bool_type, 0)},
        {lbool, jir_int(bool_type, 2)},
        {lf64, jir_float(f64, 0.0)},
        {lf64, jir_float(f64, from_bits(UINT64_C(0x8000000000000000)))},
        {lf64, jir_float(f64, 1.0)},
        {lf64, jir_float(f64, -2.0)},
        {lf64, jir_float(f64, from_bits(UINT64_C(0xc3e0000000000000)))},
        {lf64, jir_float(f64, 1.5)},
        {lf64, jir_float(f64, 2.5)},
        {lf64, jir_float(f64, -0.5)},
        {lf64, jir_float(f64, from_bits(UINT64_C(0x7ff0000000000000)))},
        {lf64, jir_float(f64, from_bits(UINT64_C(0xfff0000000000000)))},
        {lf64, jir_float(f64, from_bits(UINT64_C(0x7ff8000000000000)))},
        {lf64, jir_float(f64, from_bits(UINT64_C(0xfff8000000000000)))},
        {lf64, jir_float(f64, from_bits(UINT64_C(1)))},
        {lstr, jir_string(str, raw, sizeof raw)},
        {lcstr, jir_cstring(cstr, cs, sizeof cs)},
        {lopt, jir_null(opt)},
    };
    for (size_t i = 0; i < sizeof copies / sizeof copies[0]; i++) {
        JirInst *inst = jir_emit(f, block, JIR_COPY, (int32_t)copies[i].dst);
        CHECK(inst != NULL);
        if (!inst) {
            env_close(&env);
            return;
        }
        inst->a = copies[i].value;
    }
    ret_none(f, block);
    if (!verified(&env)) {
        env_close(&env);
        return;
    }
    EXPECT_DUMP(env.module, "main none\n"
                             "init none\n"
                             "\n"
                             "fn f0 internal jog__consts \"consts\": () -> ()\n"
                             "  l0: i32\n"
                             "  l1: i64\n"
                             "  l2: u32\n"
                             "  l3: bool\n"
                             "  l4: f64\n"
                             "  l5: const bytes\n"
                             "  l6: *const u8\n"
                             "  l7: i32?\n"
                             "b0:\n"
                             "  l0 = copy i32 -2\n"
                             "  l0 = copy i32 -2147483648\n"
                             "  l1 = copy i64 -1\n"
                             "  l2 = copy u32 4294967295\n"
                             "  l3 = copy bool true\n"
                             "  l3 = copy bool false\n"
                             "  l3 = copy bool 2\n"
                             "  l4 = copy f64 0.0\n"
                             "  l4 = copy f64 -0.0\n"
                             "  l4 = copy f64 1.0\n"
                             "  l4 = copy f64 -2.0\n"
                             "  l4 = copy f64 -9223372036854775808.0\n"
                             "  l4 = copy f64 0x1.8p0\n"
                             "  l4 = copy f64 0x1.4p1\n"
                             "  l4 = copy f64 -0x1p-1\n"
                             "  l4 = copy f64 inf\n"
                             "  l4 = copy f64 -inf\n"
                             "  l4 = copy f64 nan\n"
                             "  l4 = copy f64 -nan\n"
                             "  l4 = copy f64 0x0.0000000000001p-1022\n"
                             "  l5 = copy const bytes \"a\\\"b\\n\\x01\"\n"
                             "  l6 = copy *const u8 c\"hi\\t\"\n"
                             "  l7 = copy i32? null\n"
                             "  ret\n");
    env_close(&env);
}

static void test_addr_refs(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    JogType *ptr = jog_type_ptr(env.types, i32, false);
    JogType *fn_type = fn_of(&env, NULL, 0, ptr, false);
    CHECK(jir_global_add(env.module, "jog__counter", i32, false) == 0);
    CHECK(jir_func_add(env.module, "jog__addr", "addr", JIR_LINK_INTERNAL, fn_type, NULL) == 0);
    JirFunc *f = jir_func(env.module, 0);
    uint32_t slot = jir_local_add(f, "slot", i32);
    uint32_t p = jir_local_add(f, "p", ptr);
    uint32_t tmp = jir_local_add(f, NULL, i32);
    uint32_t block = jir_block_add(f, NULL);
    JirInst *addr = jir_emit(f, block, JIR_ADDR, (int32_t)p);
    addr->a = jir_local(f, slot);
    JirInst *global = jir_emit(f, block, JIR_COPY, (int32_t)tmp);
    global->a = jir_global_ref(env.module, 0);
    JirInst *func = jir_emit(f, block, JIR_COPY, (int32_t)tmp);
    func->a = jir_func_ref(env.module, 0);
    ret_local(f, block, p);

    CHECK(!jir_local_at(f, slot)->address_taken);
    EXPECT_DUMP(env.module, "main none\n"
                             "init none\n"
                             "\n"
                             "global g0 var jog__counter: i32 = zero\n"
                             "\n"
                             "fn f0 internal jog__addr \"addr\": () -> *i32\n"
                             "  l0 slot: i32\n"
                             "  l1 p: *i32\n"
                             "  l2: i32\n"
                             "b0:\n"
                             "  l1 = addr l0\n"
                             "  l2 = copy g0\n"
                             "  l2 = copy f0\n"
                             "  ret l1\n");
    if (!verified(&env)) {
        env_close(&env);
        return;
    }
    CHECK(jir_local_at(f, slot)->address_taken);
    EXPECT_DUMP(env.module, "main none\n"
                             "init none\n"
                             "\n"
                             "global g0 var jog__counter: i32 = zero\n"
                             "\n"
                             "fn f0 internal jog__addr \"addr\": () -> *i32\n"
                             "  l0 slot: i32 addr\n"
                             "  l1 p: *i32\n"
                             "  l2: i32\n"
                             "b0:\n"
                             "  l1 = addr l0\n"
                             "  l2 = copy g0\n"
                             "  l2 = copy f0\n"
                             "  ret l1\n");
    env_close(&env);
}

static void emit_binop(JirFunc *func, uint32_t block, int32_t dst, JirBinOp op, bool checked, JirOperand a,
                       JirOperand b) {
    JirInst *inst = jir_emit(func, block, JIR_BINARY, dst);
    inst->aux = op;
    inst->checked = checked;
    inst->a = a;
    inst->b = b;
}

static void test_ops(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    JogType *bool_type = jog_type_builtin(env.types, JOG_BUILTIN_BOOL);
    JogType *unit = jog_type_builtin(env.types, JOG_BUILTIN_UNIT);
    JogType *str = jog_type_builtin(env.types, JOG_BUILTIN_CONST_BYTES);
    JogType *params[1] = {i32};
    JogType *id_type = fn_of(&env, params, 1, i32, false);
    JogType *ops_type = fn_of(&env, params, 1, unit, false);
    const char *name = "n";
    CHECK(jir_func_add(env.module, "jog__id", "id", JIR_LINK_INTERNAL, id_type, &name) == 0);
    CHECK(jir_func_add(env.module, "jog__ops", "ops", JIR_LINK_INTERNAL, ops_type, &name) == 1);
    JirFunc *id = jir_func(env.module, 0);
    ret_local(id, jir_block_add(id, NULL), 0);

    JirFunc *ops = jir_func(env.module, 1);
    uint32_t flag = jir_local_add(ops, "flag", bool_type);
    uint32_t acc = jir_local_add(ops, "acc", i32);
    uint32_t ptr = jir_local_add(ops, "p", jog_type_ptr(env.types, i32, false));
    uint32_t fp = jir_local_add(ops, "fp", id_type);
    uint32_t body = jir_block_add(ops, "body");
    JirOperand n = jir_local(ops, 0);
    JirOperand acc_op = jir_local(ops, acc);
    JirOperand flag_op = jir_local(ops, flag);
    JirOperand ptr_op = jir_local(ops, ptr);

    emit_binop(ops, body, (int32_t)acc, JIR_ADD, true, n, jir_int(i32, 1));
    static const JirBinOp arith[] = {JIR_SUB, JIR_MUL, JIR_DIV, JIR_REM, JIR_AND, JIR_OR, JIR_XOR, JIR_SHL, JIR_SHR};
    for (size_t i = 0; i < sizeof arith / sizeof arith[0]; i++) {
        emit_binop(ops, body, (int32_t)acc, arith[i], false, n, n);
    }
    static const JirBinOp cmps[] = {JIR_EQ, JIR_NE, JIR_LT, JIR_LE, JIR_GT, JIR_GE};
    for (size_t i = 0; i < sizeof cmps / sizeof cmps[0]; i++) {
        emit_binop(ops, body, (int32_t)flag, cmps[i], false, n, n);
    }

    JirInst *neg = jir_emit(ops, body, JIR_UNARY, (int32_t)acc);
    neg->aux = JIR_NEG;
    neg->checked = true;
    neg->a = n;
    JirInst *not = jir_emit(ops, body, JIR_UNARY, (int32_t)flag);
    not->aux = JIR_NOT;
    not->a = flag_op;
    JirInst *bitnot = jir_emit(ops, body, JIR_UNARY, (int32_t)acc);
    bitnot->aux = JIR_BITNOT;
    bitnot->a = n;

    JirInst *conv = jir_emit(ops, body, JIR_CONVERT, (int32_t)acc);
    conv->checked = true;
    conv->a = n;
    JirInst *bitcast = jir_emit(ops, body, JIR_BITCAST, (int32_t)acc);
    bitcast->a = n;
    JirInst *copy = jir_emit(ops, body, JIR_COPY, (int32_t)acc);
    copy->a = n;
    jir_emit(ops, body, JIR_ZERO, (int32_t)acc);
    JirInst *load = jir_emit(ops, body, JIR_LOAD, (int32_t)acc);
    load->a = ptr_op;
    JirInst *store = jir_emit(ops, body, JIR_STORE, -1);
    store->a = ptr_op;
    store->b = acc_op;
    JirInst *addr = jir_emit(ops, body, JIR_ADDR, (int32_t)ptr);
    addr->a = acc_op;

    JirInst *field = jir_emit(ops, body, JIR_FIELD_ADDR, (int32_t)ptr);
    field->a = ptr_op;
    field->aux = 1;
    /* aux2 is the variant index plus one; 0 means this is not an enum payload. */
    JirInst *payload = jir_emit(ops, body, JIR_FIELD_ADDR, (int32_t)ptr);
    payload->a = ptr_op;
    payload->aux = 0;
    payload->aux2 = 3;
    JirInst *elem = jir_emit(ops, body, JIR_ELEM_ADDR, (int32_t)ptr);
    elem->a = ptr_op;
    elem->b = n;
    JirInst *extract = jir_emit(ops, body, JIR_EXTRACT, (int32_t)acc);
    extract->a = n;
    extract->aux = 1;

    JirInst *agg = jir_emit(ops, body, JIR_AGGREGATE, (int32_t)acc);
    JirOperand args[2] = {jir_int(i32, 1), n};
    CHECK(jir_set_args(env.module, agg, args, 2));
    JirInst *tag = jir_emit(ops, body, JIR_ENUM_TAG, (int32_t)acc);
    tag->a = n;

    JirOperand call_arg = n;
    JirInst *call = jir_emit(ops, body, JIR_CALL, (int32_t)acc);
    call->a = jir_func_ref(env.module, 0);
    CHECK(jir_set_args(env.module, call, &call_arg, 1));
    JirInst *call_void = jir_emit(ops, body, JIR_CALL, -1);
    call_void->a = jir_func_ref(env.module, 0);
    CHECK(jir_set_args(env.module, call_void, &call_arg, 1));
    JirInst *indirect = jir_emit(ops, body, JIR_CALL, -1);
    indirect->a = jir_local(ops, fp);

    JirInst *bounds = jir_emit(ops, body, JIR_BOUNDS_CHECK, -1);
    bounds->a = n;
    bounds->b = n;
    JirInst *null_check = jir_emit(ops, body, JIR_NULL_CHECK, -1);
    null_check->a = ptr_op;
    JirInst *panic = jir_emit(ops, body, JIR_PANIC, -1);
    panic->a = jir_string(str, "boom", 4);

    JirTerm end = {0};
    end.kind = JIR_TERM_UNREACHABLE;
    jir_terminate(ops, body, end);
    if (!verified(&env)) {
        env_close(&env);
        return;
    }
    EXPECT_DUMP(env.module, "main none\n"
                             "init none\n"
                             "\n"
                             "fn f0 internal jog__id \"id\": (i32) -> i32\n"
                             "  l0 n: i32 param\n"
                             "b0:\n"
                             "  ret l0\n"
                             "\n"
                             "fn f1 internal jog__ops \"ops\": (i32) -> ()\n"
                             "  l0 n: i32 param\n"
                             "  l1 flag: bool\n"
                             "  l2 acc: i32 addr\n"
                             "  l3 p: *i32\n"
                             "  l4 fp: (i32) -> i32\n"
                             "b0 body:\n"
                             "  l2 = checked add l0, i32 1\n"
                             "  l2 = sub l0, l0\n"
                             "  l2 = mul l0, l0\n"
                             "  l2 = div l0, l0\n"
                             "  l2 = rem l0, l0\n"
                             "  l2 = and l0, l0\n"
                             "  l2 = or l0, l0\n"
                             "  l2 = xor l0, l0\n"
                             "  l2 = shl l0, l0\n"
                             "  l2 = shr l0, l0\n"
                             "  l1 = eq l0, l0\n"
                             "  l1 = ne l0, l0\n"
                             "  l1 = lt l0, l0\n"
                             "  l1 = le l0, l0\n"
                             "  l1 = gt l0, l0\n"
                             "  l1 = ge l0, l0\n"
                             "  l2 = checked neg l0\n"
                             "  l1 = not l1\n"
                             "  l2 = bitnot l0\n"
                             "  l2 = checked convert l0\n"
                             "  l2 = bitcast l0\n"
                             "  l2 = copy l0\n"
                             "  l2 = zero\n"
                             "  l2 = load l3\n"
                             "  store l3, l2\n"
                             "  l3 = addr l2\n"
                             "  l3 = field_addr l3, 1\n"
                             "  l3 = field_addr l3, variant 2, field 0\n"
                             "  l3 = elem_addr l3, l0\n"
                             "  l2 = extract l0, 1\n"
                             "  l2 = aggregate (i32 1, l0)\n"
                             "  l2 = enum_tag l0\n"
                             "  l2 = call f0(l0)\n"
                             "  call f0(l0)\n"
                             "  call_indirect l4()\n"
                             "  bounds_check l0, l0\n"
                             "  null_check l3\n"
                             "  panic const bytes \"boom\"\n"
                             "  unreachable\n");
    env_close(&env);
}

static void test_switch(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    JogType *unit = jog_type_builtin(env.types, JOG_BUILTIN_UNIT);
    JogType *params[1] = {i32};
    JogType *sw_type = fn_of(&env, params, 1, i32, false);
    JogType *unit_fn = fn_of(&env, NULL, 0, unit, false);
    const char *name = "n";
    CHECK(jir_func_add(env.module, "jog__sw", "sw", JIR_LINK_INTERNAL, sw_type, &name) == 0);
    JirFunc *sw = jir_func(env.module, 0);
    uint32_t entry = jir_block_add(sw, "entry");
    uint32_t one = jir_block_add(sw, "one");
    uint32_t two = jir_block_add(sw, "two");
    uint32_t other = jir_block_add(sw, "other");
    JirCase cases[2] = {{1, one}, {0xffffffffu, two}};
    JirTerm term = {0};
    term.kind = JIR_TERM_SWITCH;
    term.value = jir_local(sw, 0);
    term.target = other;
    term.cases = cases;
    term.case_count = 2;
    jir_terminate(sw, entry, term);
    ret_local(sw, one, 0);
    JirTerm ret0 = {0};
    ret0.kind = JIR_TERM_RET;
    ret0.value = jir_int(i32, 0);
    jir_terminate(sw, two, ret0);
    JirTerm dead = {0};
    dead.kind = JIR_TERM_UNREACHABLE;
    jir_terminate(sw, other, dead);

    CHECK(jir_func_add(env.module, "jog__go", "go", JIR_LINK_INTERNAL, unit_fn, NULL) == 1);
    JirFunc *go = jir_func(env.module, 1);
    uint32_t go_block = jir_block_add(go, "go");
    uint32_t stop = jir_block_add(go, "stop");
    JirTerm br = {0};
    br.kind = JIR_TERM_BR;
    br.target = stop;
    jir_terminate(go, go_block, br);
    ret_none(go, stop);
    if (!verified(&env)) {
        env_close(&env);
        return;
    }
    EXPECT_DUMP(env.module, "main none\n"
                             "init none\n"
                             "\n"
                             "fn f0 internal jog__sw \"sw\": (i32) -> i32\n"
                             "  l0 n: i32 param\n"
                             "b0 entry:\n"
                             "  switch l0, b3 other\n"
                             "    1 -> b1 one\n"
                             "    -1 -> b2 two\n"
                             "b1 one:\n"
                             "  ret l0\n"
                             "b2 two:\n"
                             "  ret i32 0\n"
                             "b3 other:\n"
                             "  unreachable\n"
                             "\n"
                             "fn f1 internal jog__go \"go\": () -> ()\n"
                             "b0 go:\n"
                             "  br b1 stop\n"
                             "b1 stop:\n"
                             "  ret\n");
    env_close(&env);
}

static void test_linkage(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    JogType *unit = jog_type_builtin(env.types, JOG_BUILTIN_UNIT);
    JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    JogType *u8 = jog_type_builtin(env.types, JOG_BUILTIN_U8);
    JogType *unit_fn = fn_of(&env, NULL, 0, unit, false);
    JogType *cstr = jog_type_ptr(env.types, u8, true);
    JogType *puts_type = fn_of(&env, &cstr, 1, i32, true);
    const char *param = "s";
    CHECK(jir_func_add(env.module, "jog__vis", "vis", JIR_LINK_PUBLIC, unit_fn, NULL) == 0);
    CHECK(jir_func_add(env.module, "jog_exp", "vis.export", JIR_LINK_EXPORT, unit_fn, NULL) == 1);
    CHECK(jir_func_add(env.module, "puts", "c.puts", JIR_LINK_EXTERN, puts_type, &param) == 2);
    JirFunc *pub = jir_func(env.module, 0);
    JirFunc *exp = jir_func(env.module, 1);
    ret_none(pub, jir_block_add(pub, NULL));
    ret_none(exp, jir_block_add(exp, NULL));
    if (!verified(&env)) {
        env_close(&env);
        return;
    }
    EXPECT_DUMP(env.module, "main none\n"
                             "init none\n"
                             "\n"
                             "fn f0 public jog__vis \"vis\": () -> ()\n"
                             "b0:\n"
                             "  ret\n"
                             "\n"
                             "fn f1 export jog_exp \"vis.export\": () -> ()\n"
                             "b0:\n"
                             "  ret\n"
                             "\n"
                             "fn f2 extern puts \"c.puts\": extern (*const u8) -> i32\n"
                             "  l0 s: *const u8 param\n");
    env_close(&env);
}

static void test_positions(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    JogType *params[1] = {i32};
    JogType *fn_type = fn_of(&env, params, 1, i32, false);
    const char *name = "n";
    CHECK(jir_global_add(env.module, "jog__counter", i32, false) == 0);
    jir_global(env.module, 0)->pos = (JogPos){"g.jog", 2, 1};
    CHECK(jir_func_add(env.module, "jog__f", "pkg.f", JIR_LINK_INTERNAL, fn_type, &name) == 0);
    JirFunc *f = jir_func(env.module, 0);
    f->pos = (JogPos){"g.jog", 4, 1};
    uint32_t block = jir_block_add(f, "entry");
    JirInst *copy = jir_emit(f, block, JIR_COPY, 0);
    copy->a = jir_int(i32, 1);
    copy->pos = (JogPos){"g.jog", 5, 3};
    JirTerm ret = {0};
    ret.kind = JIR_TERM_RET;
    ret.value = jir_local(f, 0);
    ret.pos = (JogPos){"g.jog", 6, 3};
    jir_terminate(f, block, ret);
    if (!verified(&env)) {
        env_close(&env);
        return;
    }
    EXPECT_DUMP(env.module, "main none\n"
                             "init none\n"
                             "\n"
                             "global g0 var jog__counter: i32 = zero  ; g.jog:2:1\n"
                             "\n"
                             "fn f0 internal jog__f \"pkg.f\": (i32) -> i32  ; g.jog:4:1\n"
                             "  l0 n: i32 param\n"
                             "b0 entry:\n"
                             "  l0 = copy i32 1  ; g.jog:5:3\n"
                             "  ret l0  ; g.jog:6:3\n");
    env_close(&env);
}

static void test_unterminated(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    JogType *unit = jog_type_builtin(env.types, JOG_BUILTIN_UNIT);
    JogType *fn_type = fn_of(&env, NULL, 0, unit, false);
    CHECK(jir_func_add(env.module, "jog__u", "u", JIR_LINK_INTERNAL, fn_type, NULL) == 0);
    JirFunc *f = jir_func(env.module, 0);
    jir_block_add(f, "a b");
    CHECK(!jir_verify(env.module, env.diag));
    EXPECT_DUMP(env.module, "main none\n"
                             "init none\n"
                             "\n"
                             "fn f0 internal jog__u \"u\": () -> ()\n"
                             "b0 \"a b\":\n"
                             "  unterminated\n");
    env_close(&env);
}

static void test_emit_empty(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    CtSb sb;
    ct_sb_init(&sb);
    CHECK(!jog_emit_c(NULL, JOG_BUILD_DEBUG, &sb, env.diag));
    CHECK(!jog_emit_c(env.module, JOG_BUILD_RELEASE, NULL, env.diag));
    CHECK(ct_diag_error_count(env.diag) == 2);
    EXPECT_EMIT(&env, C_PREAMBLE C_ENTRIES);
    ct_sb_free(&sb);
    env_close(&env);
}

static void test_emit_order(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    JogType *a = jog_type_new_nominal(env.types, JOG_TY_STRUCT, "A", "p", NULL);
    JogType *b = jog_type_new_nominal(env.types, JOG_TY_STRUCT, "B", "p", NULL);
    JogField b_fields[1] = {{"x", i32, 0, false, NULL}};
    JogField a_fields[1] = {{"b", b, 0, false, NULL}};
    b->as.nominal.fields = b_fields;
    b->as.nominal.field_count = 1;
    a->as.nominal.fields = a_fields;
    a->as.nominal.field_count = 1;
    add_global(&env, a);
    EXPECT_EMIT(&env, C_PREAMBLE "\n"
                                 "typedef struct jog_p__A jog_p__A;\n"
                                 "typedef struct jog_p__B jog_p__B;\n"
                                 "\n"
                                 "struct jog_p__B { /* B */\n"
                                 "    int32_t f_x;\n"
                                 "};\n"
                                 "\n"
                                 "struct jog_p__A { /* A */\n"
                                 "    jog_p__B f_b;\n"
                                 "};\n"
                                 "\n"
                                 "static jog_p__A g0 = {0};\n"
                                 "\n"
                                 "void jog_entry_init(void) {\n"
                                 "    (void)&g0;\n"
                                 "}\n"
                                 "\n"
                                 "void jog_entry_main(void) {\n"
                                 "}\n");
    env_close(&env);
}

static void test_emit_dedup(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    JogType *slice = jog_type_slice(env.types, i32, false);
    add_global(&env, slice);
    add_global(&env, slice);
    EXPECT_EMIT(&env, C_PREAMBLE "\n"
                                 "typedef struct jt20 jt20;\n"
                                 "\n"
                                 "struct jt20 { /* []i32 */\n"
                                 "    int32_t *ptr;\n"
                                 "    size_t len;\n"
                                 "};\n"
                                 "\n"
                                 "static jt20 g0 = {0};\n"
                                 "static jt20 g1 = {0};\n"
                                 "\n"
                                 "void jog_entry_init(void) {\n"
                                 "    (void)&g0;\n"
                                 "    (void)&g1;\n"
                                 "}\n"
                                 "\n"
                                 "void jog_entry_main(void) {\n"
                                 "}\n");
    env_close(&env);
}

static void test_emit_collision(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    JogType *first = jog_type_new_nominal(env.types, JOG_TY_STRUCT, "A", "p", NULL);
    JogType *second = jog_type_new_nominal(env.types, JOG_TY_STRUCT, "A", "p", NULL);
    JogField fields[1] = {{"x", i32, 0, false, NULL}};
    first->as.nominal.fields = fields;
    first->as.nominal.field_count = 1;
    second->as.nominal.fields = fields;
    second->as.nominal.field_count = 1;
    add_global(&env, second);
    add_global(&env, first);
    CHECK(first->id == 20 && second->id == 21);
    EXPECT_EMIT(&env, C_PREAMBLE "\n"
                                 "typedef struct jog_p__A jog_p__A;\n"
                                 "typedef struct jog_p__A__21 jog_p__A__21;\n"
                                 "\n"
                                 "struct jog_p__A { /* A */\n"
                                 "    int32_t f_x;\n"
                                 "};\n"
                                 "\n"
                                 "struct jog_p__A__21 { /* A */\n"
                                 "    int32_t f_x;\n"
                                 "};\n"
                                 "\n"
                                 "static jog_p__A__21 g0 = {0};\n"
                                 "static jog_p__A g1 = {0};\n"
                                 "\n"
                                 "void jog_entry_init(void) {\n"
                                 "    (void)&g0;\n"
                                 "    (void)&g1;\n"
                                 "}\n"
                                 "\n"
                                 "void jog_entry_main(void) {\n"
                                 "}\n");
    env_close(&env);
}

static void test_emit_fn_type_only(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    JogType *fn = fn_of(&env, NULL, 0, i32, false);
    CHECK(jir_func_add(env.module, "jog__keep", "keep", JIR_LINK_INTERNAL, fn, NULL) == 0);
    JirFunc *f = jir_func(env.module, 0);
    uint32_t block = jir_block_add(f, "entry");
    JirTerm ret = {0};
    ret.kind = JIR_TERM_RET;
    ret.value = jir_int(i32, 1);
    jir_terminate(f, block, ret);
    EXPECT_EMIT(&env, C_PREAMBLE "\n"
                                 "typedef struct jt20 jt20;\n"
                                 "\n"
                                 "struct jt20 { /* () -> i32 */\n"
                                 "    int32_t (*code)(void *env);\n"
                                 "    void *env;\n"
                                 "};\n"
                                 "\n"
                                 "static int32_t jog__keep(void);\n"
                                 "\n"
                                 "static int32_t jog__keep(void) {\n"
                                 "    goto b0;\n"
                                 "b0:;\n"
                                 "    return (1);\n"
                                 "}\n" C_ENTRIES);
    CtSb sb;
    ct_sb_init(&sb);
    CHECK(jog_emit_c(env.module, JOG_BUILD_DEBUG, &sb, env.diag));
    CHECK(strstr(ct_sb_cstr(&sb), "static int32_t jog__keep(void)") != NULL);
    CHECK(strstr(ct_sb_cstr(&sb), "jog__keep(void *env)") == NULL);
    CHECK(strstr(ct_sb_cstr(&sb), "void jog_entry_init(void)") != NULL);
    CHECK(strstr(ct_sb_cstr(&sb), "void jog_entry_main(void)") != NULL);
    ct_sb_free(&sb);
    env_close(&env);
}

static void test_emit_kinds(void) {
    Env env = env_open();
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    JogType *i8 = jog_type_builtin(env.types, JOG_BUILTIN_I8);
    JogType *i16 = jog_type_builtin(env.types, JOG_BUILTIN_I16);
    JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    JogType *i64 = jog_type_builtin(env.types, JOG_BUILTIN_I64);
    JogType *u8 = jog_type_builtin(env.types, JOG_BUILTIN_U8);
    JogType *u16 = jog_type_builtin(env.types, JOG_BUILTIN_U16);
    JogType *u32 = jog_type_builtin(env.types, JOG_BUILTIN_U32);
    JogType *u64 = jog_type_builtin(env.types, JOG_BUILTIN_U64);
    JogType *isize = jog_type_builtin(env.types, JOG_BUILTIN_ISIZE);
    JogType *usize = jog_type_builtin(env.types, JOG_BUILTIN_USIZE);
    JogType *f32 = jog_type_builtin(env.types, JOG_BUILTIN_F32);
    JogType *f64 = jog_type_builtin(env.types, JOG_BUILTIN_F64);
    JogType *bool_type = jog_type_builtin(env.types, JOG_BUILTIN_BOOL);
    JogType *unit = jog_type_builtin(env.types, JOG_BUILTIN_UNIT);
    JogType *bytes = jog_type_builtin(env.types, JOG_BUILTIN_BYTES);
    JogType *str = jog_type_builtin(env.types, JOG_BUILTIN_CONST_BYTES);
    JogType *untyped_int = jog_type_builtin(env.types, JOG_BUILTIN_UNTYPED_INT);
    JogType *untyped_float = jog_type_builtin(env.types, JOG_BUILTIN_UNTYPED_FLOAT);
    JogType *null_type = jog_type_builtin(env.types, JOG_BUILTIN_NULL);
    JogType *error = jog_type_builtin(env.types, JOG_BUILTIN_ERROR);
    CHECK(bytes->id == 14 && str->id == 15 && null_type->id == 18 && error->id == 19);

    JogType *box = jog_type_new_nominal(env.types, JOG_TY_STRUCT, "Scalars", "demo", NULL);
    JogType *b4 = jog_type_bytes_n(env.types, 4);
    JogType *b0 = jog_type_bytes_n(env.types, 0);
    JogType *slice = jog_type_slice(env.types, i32, false);
    JogType *cslice = jog_type_slice(env.types, i32, true);
    JogType *ptr = jog_type_ptr(env.types, i32, false);
    JogType *cptr = jog_type_ptr(env.types, i32, true);
    JogType *arr = jog_type_array(env.types, i32, 4);
    JogType *arr0 = jog_type_array(env.types, i32, 0);
    JogType *pair_elems[2] = {i32, str};
    JogType *pair = jog_type_tuple(env.types, pair_elems, 2);
    JogType *empty_tup = jog_type_tuple(env.types, NULL, 0);
    JogType *niche = jog_type_option(env.types, ptr);
    JogType *boxed = jog_type_option(env.types, i32);
    JogType *opt_unit = jog_type_option(env.types, unit);
    JogType *point = jog_type_new_nominal(env.types, JOG_TY_STRUCT, "Point", "net.http", NULL);
    JogType *shape = jog_type_new_nominal(env.types, JOG_TY_ENUM, "Shape", "", NULL);
    JogType *celsius = jog_type_new_nominal(env.types, JOG_TY_DISTINCT, "Celsius", "", NULL);
    JogType *writer = jog_type_new_nominal(env.types, JOG_TY_INTERFACE, "Writer", "std", NULL);
    JogType *tparam = jog_type_new_param(env.types, "T", NULL, writer);
    JogType *cstr = jog_type_ptr(env.types, u8, true);
    JogType *puts = jog_type_fn(env.types, &cstr, 1, i32, NULL, 0, true, false, true);
    JogType *closure_params[1] = {i32};
    JogType *closure_ctx[1] = {str};
    JogType *closure = jog_type_fn(env.types, closure_params, 1, unit, closure_ctx, 1, false, true, false);
    JogType *node = jog_type_new_nominal(env.types, JOG_TY_STRUCT, "Node", "pkg", NULL);
    JogType *next = jog_type_ptr(env.types, node, false);
    JogType *map = jog_type_new_nominal(env.types, JOG_TY_STRUCT, "Map", "std", NULL);
    JogType *id = jog_type_new_nominal(env.types, JOG_TY_DISTINCT, "Id", "std", NULL);
    CHECK(box->id == 20 && id->id == 45 && next->id == 43 && map->id == 44);
    CHECK(jog_type_option_is_niche(niche) && !jog_type_option_is_niche(boxed));

    JogField scalars[] = {
        {"i8", i8, 0, false, NULL},
        {"i16", i16, 0, false, NULL},
        {"i32", i32, 0, false, NULL},
        {"i64", i64, 0, false, NULL},
        {"u8", u8, 0, false, NULL},
        {"u16", u16, 0, false, NULL},
        {"u32", u32, 0, false, NULL},
        {"u64", u64, 0, false, NULL},
        {"isz", isize, 0, false, NULL},
        {"usz", usize, 0, false, NULL},
        {"f32", f32, 0, false, NULL},
        {"f64", f64, 0, false, NULL},
        {"flag", bool_type, 0, false, NULL},
        {"raw", untyped_int, 0, false, NULL},
        {"flt", untyped_float, 0, false, NULL},
        {"nothing", unit, 0, false, NULL},
    };
    box->as.nominal.fields = scalars;
    box->as.nominal.field_count = sizeof scalars / sizeof scalars[0];
    JogField point_fields[] = {{"x", u8, 0, false, NULL}, {"y", i64, 0, false, NULL}};
    point->as.nominal.fields = point_fields;
    point->as.nominal.field_count = 2;
    JogField circle_fields[] = {{"r", f64, 0, false, NULL}};
    JogVariant variants[] = {{"Circle", circle_fields, 1, 0, NULL}, {"Empty", NULL, 0, 1, NULL}};
    shape->as.nominal.variants = variants;
    shape->as.nominal.variant_count = 2;
    celsius->as.nominal.base = f64;
    JogField node_fields[] = {{"next", next, 0, false, NULL}, {"value", i32, 0, false, NULL}};
    node->as.nominal.fields = node_fields;
    node->as.nominal.field_count = 2;
    JogField map_fields[] = {{"len", usize, 0, false, NULL}};
    JogType *map_args[] = {str, i32};
    map->as.nominal.fields = map_fields;
    map->as.nominal.field_count = 1;
    map->as.nominal.type_args = map_args;
    map->as.nominal.type_arg_count = 2;
    id->as.nominal.base = ptr;

    JogType *roots[] = {box,     b4,     b0,     bytes, str,   slice,  cslice, ptr,    cptr,   arr,  arr0, pair,
                        empty_tup, niche, boxed, opt_unit, point, shape, celsius, writer, tparam, puts, closure,
                        node,    next,   map,    id,    null_type, error, cstr};
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        add_global(&env, roots[i]);
    }
    add_global(&env, slice);

    EXPECT_EMIT(&env, C_PREAMBLE "\n"
                                 "typedef struct jt14 jt14;\n"
                                 "typedef struct jt15 jt15;\n"
                                 "typedef struct jt18 jt18; /* null */\n"
                                 "typedef struct jt19 jt19; /* <error> */\n"
                                 "typedef struct jog_demo__Scalars jog_demo__Scalars;\n"
                                 "typedef struct jt21 jt21;\n"
                                 "typedef struct jt22 jt22; /* [0]byte */\n"
                                 "typedef struct jt23 jt23;\n"
                                 "typedef struct jt24 jt24;\n"
                                 "typedef struct jt27 jt27;\n"
                                 "typedef struct jt28 jt28; /* [0]i32 */\n"
                                 "typedef struct jt29 jt29;\n"
                                 "typedef struct jt30 jt30; /* () */\n"
                                 "typedef struct jt32 jt32;\n"
                                 "typedef struct jt33 jt33;\n"
                                 "typedef struct jog_net_http__Point jog_net_http__Point;\n"
                                 "typedef struct jog___Shape jog___Shape;\n"
                                 "typedef struct jog_std__Writer jog_std__Writer;\n"
                                 "typedef struct jt38 jt38; /* T */\n"
                                 "typedef struct jt41 jt41;\n"
                                 "typedef struct jog_pkg__Node jog_pkg__Node;\n"
                                 "typedef struct jog_std__Map__15_2 jog_std__Map__15_2;\n"
                                 "\n"
                                 "struct jt14 { /* bytes */\n"
                                 "    uint8_t *ptr;\n"
                                 "    size_t len;\n"
                                 "};\n"
                                 "\n"
                                 "struct jt15 { /* const bytes */\n"
                                 "    const uint8_t *ptr;\n"
                                 "    size_t len;\n"
                                 "};\n"
                                 "\n"
                                 "struct jog_demo__Scalars { /* Scalars */\n"
                                 "    int8_t f_i8;\n"
                                 "    int16_t f_i16;\n"
                                 "    int32_t f_i32;\n"
                                 "    int64_t f_i64;\n"
                                 "    uint8_t f_u8;\n"
                                 "    uint16_t f_u16;\n"
                                 "    uint32_t f_u32;\n"
                                 "    uint64_t f_u64;\n"
                                 "    intptr_t f_isz;\n"
                                 "    uintptr_t f_usz;\n"
                                 "    float f_f32;\n"
                                 "    double f_f64;\n"
                                 "    _Bool f_flag;\n"
                                 "    int64_t f_raw;\n"
                                 "    double f_flt;\n"
                                 "    /* f_nothing: () */\n"
                                 "};\n"
                                 "\n"
                                 "struct jt21 { /* [4]byte */\n"
                                 "    uint8_t b[4];\n"
                                 "};\n"
                                 "\n"
                                 "struct jt23 { /* []i32 */\n"
                                 "    int32_t *ptr;\n"
                                 "    size_t len;\n"
                                 "};\n"
                                 "\n"
                                 "struct jt24 { /* []const i32 */\n"
                                 "    const int32_t *ptr;\n"
                                 "    size_t len;\n"
                                 "};\n"
                                 "\n"
                                 "typedef int32_t *jt25; /* *i32 */\n"
                                 "\n"
                                 "typedef const int32_t *jt26; /* *const i32 */\n"
                                 "\n"
                                 "struct jt27 { /* [4]i32 */\n"
                                 "    int32_t v[4];\n"
                                 "};\n"
                                 "\n"
                                 "struct jt29 { /* (i32, const bytes) */\n"
                                 "    int32_t f_0;\n"
                                 "    jt15 f_1;\n"
                                 "};\n"
                                 "\n"
                                 "typedef jt25 jt31; /* *i32? */\n"
                                 "\n"
                                 "struct jt32 { /* i32? */\n"
                                 "    _Bool has;\n"
                                 "    int32_t value;\n"
                                 "};\n"
                                 "\n"
                                 "struct jt33 { /* ()? */\n"
                                 "    _Bool has;\n"
                                 "    /* value: () */\n"
                                 "};\n"
                                 "\n"
                                 "struct jog_net_http__Point { /* Point */\n"
                                 "    uint8_t f_x;\n"
                                 "    int64_t f_y;\n"
                                 "};\n"
                                 "\n"
                                 "struct jog___Shape { /* Shape */\n"
                                 "    uint8_t tag;\n"
                                 "    union {\n"
                                 "        struct {\n"
                                 "            double f_r;\n"
                                 "        } f_Circle; /* tag 0 */\n"
                                 "        /* f_Empty tag 1 */\n"
                                 "    } payload;\n"
                                 "};\n"
                                 "\n"
                                 "typedef double jog___Celsius; /* Celsius */\n"
                                 "\n"
                                 "struct jog_std__Writer { /* Writer */\n"
                                 "    void *ptr;\n"
                                 "    void *vtable;\n"
                                 "};\n"
                                 "\n"
                                 "typedef const uint8_t *jt39; /* *const u8 */\n"
                                 "\n"
                                 "typedef int32_t (*jt40)(jt39, ...); /* extern (*const u8, ...) -> i32 */\n"
                                 "\n"
                                 "struct jt41 { /* escaping (i32) -> () with const bytes */\n"
                                 "    void (*code)(int32_t, jt15, void *env);\n"
                                 "    void *env;\n"
                                 "};\n"
                                 "\n"
                                 "typedef jog_pkg__Node *jt43; /* *Node */\n"
                                 "\n"
                                 "struct jog_pkg__Node { /* Node */\n"
                                 "    jt43 f_next;\n"
                                 "    int32_t f_value;\n"
                                 "};\n"
                                 "\n"
                                 "struct jog_std__Map__15_2 { /* Map<const bytes, i32> */\n"
                                 "    uintptr_t f_len;\n"
                                 "};\n"
                                 "\n"
                                 "typedef jt25 jog_std__Id; /* Id */\n"
                                 "\n"
                                 "static jog_demo__Scalars g0 = {0};\n"
                                 "static jt21 g1 = {0};\n"
                                 "static jt14 g3 = {0};\n"
                                 "static jt15 g4 = {0};\n"
                                 "static jt23 g5 = {0};\n"
                                 "static jt24 g6 = {0};\n"
                                 "static jt25 g7 = 0;\n"
                                 "static jt26 g8 = 0;\n"
                                 "static jt27 g9 = {0};\n"
                                 "static jt29 g11 = {0};\n"
                                 "static jt31 g13 = 0;\n"
                                 "static jt32 g14 = {0};\n"
                                 "static jt33 g15 = {0};\n"
                                 "static jog_net_http__Point g16 = {0};\n"
                                 "static jog___Shape g17 = {0};\n"
                                 "static jog___Celsius g18 = 0;\n"
                                 "static jog_std__Writer g19 = {0};\n"
                                 "static jt40 g21 = 0;\n"
                                 "static jt41 g22 = {0};\n"
                                 "static jog_pkg__Node g23 = {0};\n"
                                 "static jt43 g24 = 0;\n"
                                 "static jog_std__Map__15_2 g25 = {0};\n"
                                 "static jog_std__Id g26 = 0;\n"
                                 "static jt39 g29 = 0;\n"
                                 "static jt23 g30 = {0};\n"
                                 "\n"
                                 "void jog_entry_init(void) {\n"
                                 "    (void)&g0;\n"
                                 "    (void)&g1;\n"
                                 "    (void)&g3;\n"
                                 "    (void)&g4;\n"
                                 "    (void)&g5;\n"
                                 "    (void)&g6;\n"
                                 "    (void)&g7;\n"
                                 "    (void)&g8;\n"
                                 "    (void)&g9;\n"
                                 "    (void)&g11;\n"
                                 "    (void)&g13;\n"
                                 "    (void)&g14;\n"
                                 "    (void)&g15;\n"
                                 "    (void)&g16;\n"
                                 "    (void)&g17;\n"
                                 "    (void)&g18;\n"
                                 "    (void)&g19;\n"
                                 "    (void)&g21;\n"
                                 "    (void)&g22;\n"
                                 "    (void)&g23;\n"
                                 "    (void)&g24;\n"
                                 "    (void)&g25;\n"
                                 "    (void)&g26;\n"
                                 "    (void)&g29;\n"
                                 "    (void)&g30;\n"
                                 "}\n"
                                 "\n"
                                 "void jog_entry_main(void) {\n"
                                 "}\n");
    env_close(&env);
}

static void test_emit_void_ptr(void) {
    Env env = env_open();
    JogType *void_type;
    JogType *pvoid;
    JogType *cpvoid;
    JogType *pkg_void;
    JogType *ppkg;
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    void_type = jog_type_new_nominal(env.types, JOG_TY_STRUCT, "void", NULL, NULL);
    pvoid = jog_type_ptr(env.types, void_type, false);
    cpvoid = jog_type_ptr(env.types, void_type, true);
    pkg_void = jog_type_new_nominal(env.types, JOG_TY_STRUCT, "void", "pkg", NULL);
    CHECK(void_type && pvoid && cpvoid && pkg_void);
    CHECK(jog_type_layout(env.types, pkg_void));
    ppkg = jog_type_ptr(env.types, pkg_void, false);
    CHECK(ppkg != NULL);
    CHECK(void_type->id == 20 && pvoid->id == 21 && cpvoid->id == 22);
    CHECK(pkg_void->id == 23 && ppkg->id == 24);
    CHECK(!void_type->has_layout && pkg_void->has_layout && pkg_void->size == 0);
    add_global(&env, pvoid);
    add_global(&env, cpvoid);
    add_global(&env, void_type);
    add_global(&env, pkg_void);
    add_global(&env, ppkg);
    EXPECT_EMIT(&env, C_PREAMBLE "\n"
                                 "typedef struct jog_pkg__void jog_pkg__void; /* void */\n"
                                 "\n"
                                 "typedef void *jt21; /* *void */\n"
                                 "\n"
                                 "typedef const void *jt22; /* *const void */\n"
                                 "\n"
                                 "typedef jog_pkg__void *jt24; /* *void */\n"
                                 "\n"
                                 "static jt21 g0 = 0;\n"
                                 "static jt22 g1 = 0;\n"
                                 "static jt24 g4 = 0;\n"
                                 "\n"
                                 "void jog_entry_init(void) {\n"
                                 "    (void)&g0;\n"
                                 "    (void)&g1;\n"
                                 "    (void)&g4;\n"
                                 "}\n"
                                 "\n"
                                 "void jog_entry_main(void) {\n"
                                 "}\n");
    CHECK(!void_type->has_layout);
    env_close(&env);
}

static unsigned g_seq = 0;

static unsigned proc_id(void) {
#if defined(_WIN32)
    return (unsigned)GetCurrentProcessId();
#else
    return (unsigned)getpid();
#endif
}

static int is_abort_status(int code) {
#if defined(_WIN32)
    return (unsigned)code == 0xC0000409u;
#else
    return code == 128 + SIGABRT;
#endif
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
    snprintf(name, sizeof name, "jog-emit-%u-%u%s", proc_id(), g_seq++, suffix);
    return ct_path_join(arena, dir, name);
}

static int is_sep(char c) {
    return c == '/' || c == '\\';
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

static int find_compilers(CtArena *arena, const char **out, int cap) {
    const char *override = getenv("JOG_CC");
    int count = 0;
    if (override && override[0] != '\0') {
        const char *found = NULL;
        int slash = 0;
        for (const char *p = override; *p; p++) {
            if (is_sep(*p)) {
                slash = 1;
            }
        }
        found = slash ? file_in_dir(arena, "", override) : search_path(arena, override);
        if (found && count < cap) {
            out[count++] = found;
        }
        return count;
    }
    {
        const char *clang = search_path(arena, "clang");
        const char *gcc = search_path(arena, "gcc");
        if (clang && count < cap) {
            out[count++] = clang;
        }
        if (gcc && count < cap) {
            out[count++] = gcc;
        }
    }
    return count;
}

static const char k_harness[] =
    "\n#include <stdio.h>\n"
    "#include <string.h>\n"
    "int32_t jog_extern_va(int jog_va, ...) {\n"
    "    (void)jog_va;\n"
    "    return 4;\n"
    "}\n"
    "int main(int argc, char **argv) {\n"
    "    const char *mode = argc > 1 && argv[1] ? argv[1] : \"ok\";\n"
    "    if (strcmp(mode, \"ok\") == 0) {\n"
    "        printf(\"copy %d\\n\", (int)jog__copy(41));\n"
    "        printf(\"zero %d\\n\", (int)jog__zero());\n"
    "        printf(\"add %d\\n\", (int)jog__add());\n"
    "        printf(\"sub %d\\n\", (int)jog__sub());\n"
    "        printf(\"mul %d\\n\", (int)jog__mul());\n"
    "        printf(\"div %d\\n\", (int)jog__div());\n"
    "        printf(\"rem %d\\n\", (int)jog__rem());\n"
    "        printf(\"and %d\\n\", (int)jog__and());\n"
    "        printf(\"or %d\\n\", (int)jog__or());\n"
    "        printf(\"xor %d\\n\", (int)jog__xor());\n"
    "        printf(\"shl %d\\n\", (int)jog__shl());\n"
    "        printf(\"shr %d\\n\", (int)jog__shr());\n"
    "        printf(\"cmp %d\\n\", (int)jog__cmp());\n"
    "        printf(\"neg %d\\n\", (int)jog__neg());\n"
    "        printf(\"not %d\\n\", (int)jog__not());\n"
    "        printf(\"bitnot %d\\n\", (int)jog__bitnot());\n"
    "        printf(\"conv %d\\n\", (int)jog__conv());\n"
    "        printf(\"fadd %d\\n\", (int)jog__fadd());\n"
    "        printf(\"bitcast %d\\n\", (int)jog__bitcast());\n"
    "        printf(\"load %d\\n\", (int)jog__load());\n"
    "        printf(\"store %d\\n\", (int)jog__store());\n"
    "        printf(\"field %d\\n\", (int)jog__field());\n"
    "        printf(\"elem %d\\n\", (int)jog__elem());\n"
    "        printf(\"agg %d\\n\", (int)jog__agg());\n"
    "        printf(\"tag %d\\n\", (int)jog__tag());\n"
    "        printf(\"call %d\\n\", (int)jog__call());\n"
    "        printf(\"indirect %d\\n\", (int)jog__indirect());\n"
    "        printf(\"bounds %d\\n\", (int)jog__bounds());\n"
    "        printf(\"nullck %d\\n\", (int)jog__nullck());\n"
    "        printf(\"max %d\\n\", (int)jog__max(9, 4));\n"
    "        printf(\"max2 %d\\n\", (int)jog__max(1, 4));\n"
    "        printf(\"sw %d\\n\", (int)jog__sw(2));\n"
    "        printf(\"swd %d\\n\", (int)jog__sw(7));\n"
    "        printf(\"va %d\\n\", (int)jog__va(7, 1));\n"
    "        printf(\"va0 %d\\n\", (int)jog__va0());\n"
    "        return 0;\n"
    "    }\n"
    "    if (strcmp(mode, \"overflow\") == 0) { (void)jog__overflow(); return 2; }\n"
    "    if (strcmp(mode, \"shldrop\") == 0) { (void)jog__shldrop(); return 2; }\n"
    "    if (strcmp(mode, \"div0\") == 0) { (void)jog__div0(); return 2; }\n"
    "    if (strcmp(mode, \"bounds\") == 0) { printf(\"%d\\n\", (int)jog__bounds_bad()); return 0; }\n"
    "    if (strcmp(mode, \"null\") == 0) { (void)jog__null_bad(); return 2; }\n"
    "    if (strcmp(mode, \"convert\") == 0) { (void)jog__conv_bad(); return 2; }\n"
    "    if (strcmp(mode, \"panic\") == 0) { (void)jog__panic(); return 2; }\n"
    "    if (strcmp(mode, \"unreach\") == 0) { (void)jog__unreach(); return 2; }\n"
    "    return 2;\n"
    "}\n";

static const char k_ok_out[] =
    "copy 41\n"
    "zero 0\n"
    "add 42\n"
    "sub 5\n"
    "mul 42\n"
    "div 2\n"
    "rem 1\n"
    "and 2\n"
    "or 7\n"
    "xor 5\n"
    "shl 8\n"
    "shr -4\n"
    "cmp 4\n"
    "neg -3\n"
    "not 0\n"
    "bitnot -1\n"
    "conv 42\n"
    "fadd 4\n"
    "bitcast -1\n"
    "load 9\n"
    "store 8\n"
    "field 5\n"
    "elem 7\n"
    "agg 6\n"
    "tag 1\n"
    "call 21\n"
    "indirect 6\n"
    "bounds 1\n"
    "nullck 1\n"
    "max 9\n"
    "max2 4\n"
    "sw 20\n"
    "swd 30\n"
    "va 7\n"
    "va0 4\n";

static void expect_has(const char *text, const char *needle) {
    if (!text || !strstr(text, needle)) {
        fprintf(stderr, "FAIL missing '%s'\n", needle);
        g_failures++;
    }
}

static void expect_lacks(const char *text, const char *needle) {
    if (text && needle && strstr(text, needle)) {
        fprintf(stderr, "FAIL unexpected '%s'\n", needle);
        g_failures++;
    }
}

static JogPos pos_at(const char *file, uint32_t line) {
    JogPos pos;
    pos.file = file;
    pos.line = line;
    pos.column = 1;
    return pos;
}

static JirFunc *add_fn(Env *env, const char *name, JogType *type, JirLinkage linkage, const char *const *param_names) {
    int32_t id = jir_func_add(env->module, name, name, linkage, type, param_names);
    CHECK(id >= 0);
    if (id < 0) {
        return NULL;
    }
    return jir_func(env->module, (uint32_t)id);
}

static void bin_fn(Env *env, const char *name, JogType *type, JirBinOp op, int64_t a, int64_t b, bool checked) {
    JogType *fn = fn_of(env, NULL, 0, type, false);
    JirFunc *f = add_fn(env, name, fn, JIR_LINK_INTERNAL, NULL);
    uint32_t block = 0;
    uint32_t dst = 0;
    JirInst *in = NULL;
    if (!f) {
        return;
    }
    block = jir_block_add(f, NULL);
    dst = jir_local_add(f, NULL, type);
    in = jir_emit(f, block, JIR_BINARY, (int32_t)dst);
    in->aux = op;
    in->checked = checked;
    in->a = jir_int(type, (uint64_t)a);
    in->b = jir_int(type, (uint64_t)b);
    ret_local(f, block, dst);
}

static int compile_src(CtArena *arena, const char *cc, const char *src, const char *exe, JogBuildMode mode) {
    const char *opt = mode == JOG_BUILD_DEBUG ? "-O0" : "-O2";
    char *err_path = temp_path(arena, ".err");
    const char *argv[] = {cc,
                          "-std=gnu17",
                          opt,
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

static int run_exe(const char *exe, const char *mode, int *code, char **out_text, char **err_text) {
    char *out_path = NULL;
    char *err_path = NULL;
    const char *argv[3];
    CtArena *arena = ct_arena_create();
    int ok = 0;
    *code = -1;
    *out_text = NULL;
    *err_text = NULL;
    if (!arena) {
        return 0;
    }
    out_path = temp_path(arena, ".out");
    err_path = temp_path(arena, ".err");
    argv[0] = exe;
    argv[1] = mode;
    argv[2] = NULL;
    if (out_path && err_path && ct_proc_run(argv, out_path, err_path, code)) {
        *out_text = read_stripped(out_path);
        *err_text = read_stripped(err_path);
        ok = *out_text && *err_text;
    }
    if (out_path) {
        remove(out_path);
    }
    if (err_path) {
        remove(err_path);
    }
    ct_arena_destroy(arena);
    return ok;
}

static void expect_run(const char *exe, const char *mode, int want_abort, const char *want_out, const char *want_err) {
    int code = -1;
    char *out_text = NULL;
    char *err_text = NULL;
    int ran = run_exe(exe, mode, &code, &out_text, &err_text);
    CHECK(ran);
    if (!ran) {
        fprintf(stderr, "  %s did not start\n", mode);
        free(out_text);
        free(err_text);
        return;
    }
    if (want_abort) {
        if (!is_abort_status(code)) {
            fprintf(stderr, "  %s status %d (0x%x)\n", mode, code, (unsigned)code);
        }
        CHECK(is_abort_status(code));
    } else if (code != 0) {
        fprintf(stderr, "  %s status %d\nstderr:\n%s\n", mode, code, err_text ? err_text : "");
        g_failures++;
    }
    if (want_out && (!out_text || strcmp(out_text, want_out) != 0)) {
        fprintf(stderr, "  %s stdout\n got:\n%s\n want:\n%s\n", mode, out_text ? out_text : "(null)", want_out);
        g_failures++;
    }
    if (want_err && (!err_text || strcmp(err_text, want_err) != 0)) {
        fprintf(stderr, "  %s stderr\n got:\n%s\n want:\n%s\n", mode, err_text ? err_text : "(null)", want_err);
        g_failures++;
    }
    free(out_text);
    free(err_text);
}

static void test_execute(void) {
    Env env = env_open();
    JogType *i32 = NULL;
    JogType *i64 = NULL;
    JogType *i8 = NULL;
    JogType *u32 = NULL;
    JogType *u8 = NULL;
    JogType *f64 = NULL;
    JogType *boolean = NULL;
    JogType *usize = NULL;
    JogType *str = NULL;
    JogType *ptr = NULL;
    JogType *array = NULL;
    JogType *point = NULL;
    JogType *shape = NULL;
    JogField point_fields[2];
    JogVariant variants[2];
    JirCase sw_cases[2];
    CtSb debug_sb;
    CtSb fast_sb;
    CtArena *scratch = NULL;
    const char *compilers[2];
    int compiler_count = 0;
    char *debug_path = NULL;
    char *fast_path = NULL;
    char *debug_exe = NULL;
    char *fast_exe = NULL;
    const char *debug_c = NULL;
    const char *fast_c = NULL;
    int i = 0;
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    i64 = jog_type_builtin(env.types, JOG_BUILTIN_I64);
    i8 = jog_type_builtin(env.types, JOG_BUILTIN_I8);
    u32 = jog_type_builtin(env.types, JOG_BUILTIN_U32);
    u8 = jog_type_builtin(env.types, JOG_BUILTIN_U8);
    f64 = jog_type_builtin(env.types, JOG_BUILTIN_F64);
    boolean = jog_type_builtin(env.types, JOG_BUILTIN_BOOL);
    usize = jog_type_builtin(env.types, JOG_BUILTIN_USIZE);
    str = jog_type_builtin(env.types, JOG_BUILTIN_CONST_BYTES);

    {
        JogType *fn = fn_of(&env, &i32, 1, i32, false);
        JirFunc *f = add_fn(&env, "jog__copy", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *in = jir_emit(f, block, JIR_COPY, (int32_t)dst);
        in->a = jir_local(f, 0);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__zero", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t dst = jir_local_add(f, NULL, i32);
        jir_emit(f, block, JIR_ZERO, (int32_t)dst);
        ret_local(f, block, dst);
    }
    bin_fn(&env, "jog__add", i32, JIR_ADD, 20, 22, true);
    bin_fn(&env, "jog__sub", i32, JIR_SUB, 8, 3, false);
    bin_fn(&env, "jog__mul", i32, JIR_MUL, 6, 7, true);
    bin_fn(&env, "jog__div", i32, JIR_DIV, 9, 4, true);
    bin_fn(&env, "jog__rem", i32, JIR_REM, 9, 4, true);
    bin_fn(&env, "jog__and", i32, JIR_AND, 6, 3, false);
    bin_fn(&env, "jog__or", i32, JIR_OR, 5, 2, false);
    bin_fn(&env, "jog__xor", i32, JIR_XOR, 6, 3, false);
    bin_fn(&env, "jog__shl", i32, JIR_SHL, 1, 3, true);
    bin_fn(&env, "jog__shr", i32, JIR_SHR, -8, 1, false);
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__cmp", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t acc = jir_local_add(f, NULL, i32);
        JirBinOp ops[6] = {JIR_LT, JIR_GT, JIR_EQ, JIR_NE, JIR_LE, JIR_GE};
        int64_t rhs[6] = {4, 4, 3, 4, 3, 4};
        JirInst *zero = jir_emit(f, block, JIR_COPY, (int32_t)acc);
        zero->a = jir_int(i32, 0);
        for (i = 0; i < 6; i++) {
            uint32_t flag = jir_local_add(f, NULL, boolean);
            JirInst *cmp = jir_emit(f, block, JIR_BINARY, (int32_t)flag);
            JirInst *add = NULL;
            cmp->aux = ops[i];
            cmp->a = jir_int(i32, 3);
            cmp->b = jir_int(i32, (uint64_t)rhs[i]);
            add = jir_emit(f, block, JIR_BINARY, (int32_t)acc);
            add->aux = JIR_ADD;
            add->a = jir_local(f, acc);
            add->b = jir_local(f, flag);
        }
        ret_local(f, block, acc);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__neg", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *in = jir_emit(f, block, JIR_UNARY, (int32_t)dst);
        in->aux = JIR_NEG;
        in->checked = true;
        in->a = jir_int(i32, 3);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__not", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t flag = jir_local_add(f, NULL, boolean);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *copy = jir_emit(f, block, JIR_COPY, (int32_t)flag);
        JirInst *in = NULL;
        copy->a = jir_int(boolean, 1);
        in = jir_emit(f, block, JIR_UNARY, (int32_t)dst);
        in->aux = JIR_NOT;
        in->a = jir_local(f, flag);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__bitnot", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *in = jir_emit(f, block, JIR_UNARY, (int32_t)dst);
        in->aux = JIR_BITNOT;
        in->a = jir_int(i32, 0);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__conv", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *in = jir_emit(f, block, JIR_CONVERT, (int32_t)dst);
        in->checked = true;
        in->a = jir_int(i64, 42);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__fadd", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t left = jir_local_add(f, NULL, f64);
        uint32_t right = jir_local_add(f, NULL, f64);
        uint32_t sum = jir_local_add(f, NULL, f64);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *a = jir_emit(f, block, JIR_COPY, (int32_t)left);
        JirInst *b = NULL;
        JirInst *add = NULL;
        JirInst *conv = NULL;
        a->a = jir_float(f64, 1.5);
        b = jir_emit(f, block, JIR_COPY, (int32_t)right);
        b->a = jir_float(f64, 2.5);
        add = jir_emit(f, block, JIR_BINARY, (int32_t)sum);
        add->aux = JIR_ADD;
        add->a = jir_local(f, left);
        add->b = jir_local(f, right);
        conv = jir_emit(f, block, JIR_CONVERT, (int32_t)dst);
        conv->a = jir_local(f, sum);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__bitcast", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t src = jir_local_add(f, NULL, u32);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *copy = jir_emit(f, block, JIR_COPY, (int32_t)src);
        JirInst *cast = NULL;
        copy->a = jir_int(u32, 0xffffffffu);
        cast = jir_emit(f, block, JIR_BITCAST, (int32_t)dst);
        cast->a = jir_local(f, src);
        ret_local(f, block, dst);
    }
    ptr = jog_type_ptr(env.types, i32, false);
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__load", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t slot = jir_local_add(f, NULL, i32);
        uint32_t addr = jir_local_add(f, NULL, ptr);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *copy = jir_emit(f, block, JIR_COPY, (int32_t)slot);
        JirInst *adr = NULL;
        JirInst *load = NULL;
        copy->a = jir_int(i32, 9);
        adr = jir_emit(f, block, JIR_ADDR, (int32_t)addr);
        adr->a = jir_local(f, slot);
        load = jir_emit(f, block, JIR_LOAD, (int32_t)dst);
        load->a = jir_local(f, addr);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__store", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t slot = jir_local_add(f, NULL, i32);
        uint32_t addr = jir_local_add(f, NULL, ptr);
        JirInst *zero = jir_emit(f, block, JIR_ZERO, (int32_t)slot);
        JirInst *adr = NULL;
        JirInst *store = NULL;
        (void)zero;
        adr = jir_emit(f, block, JIR_ADDR, (int32_t)addr);
        adr->a = jir_local(f, slot);
        store = jir_emit(f, block, JIR_STORE, -1);
        store->a = jir_local(f, addr);
        store->b = jir_int(i32, 8);
        ret_local(f, block, slot);
    }
    point = jog_type_new_nominal(env.types, JOG_TY_STRUCT, "Point", "t", NULL);
    point_fields[0].name = "x";
    point_fields[0].type = i32;
    point_fields[0].offset = 0;
    point_fields[0].embedded = false;
    point_fields[0].decl = NULL;
    point_fields[1].name = "y";
    point_fields[1].type = i32;
    point_fields[1].offset = 0;
    point_fields[1].embedded = false;
    point_fields[1].decl = NULL;
    point->as.nominal.fields = point_fields;
    point->as.nominal.field_count = 2;
    {
        JogType *pp = jog_type_ptr(env.types, point, false);
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__field", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t slot = jir_local_add(f, NULL, point);
        uint32_t addr = jir_local_add(f, NULL, pp);
        uint32_t field = jir_local_add(f, NULL, ptr);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *zero = jir_emit(f, block, JIR_ZERO, (int32_t)slot);
        JirInst *adr = NULL;
        JirInst *fld = NULL;
        JirInst *store = NULL;
        JirInst *ex = NULL;
        (void)zero;
        adr = jir_emit(f, block, JIR_ADDR, (int32_t)addr);
        adr->a = jir_local(f, slot);
        fld = jir_emit(f, block, JIR_FIELD_ADDR, (int32_t)field);
        fld->a = jir_local(f, addr);
        fld->aux = 0;
        store = jir_emit(f, block, JIR_STORE, -1);
        store->a = jir_local(f, field);
        store->b = jir_int(i32, 5);
        ex = jir_emit(f, block, JIR_EXTRACT, (int32_t)dst);
        ex->a = jir_local(f, slot);
        ex->aux = 0;
        ret_local(f, block, dst);
    }
    array = jog_type_array(env.types, i32, 4);
    {
        JogType *pa = jog_type_ptr(env.types, array, false);
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__elem", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t slot = jir_local_add(f, NULL, array);
        uint32_t arr_ptr = jir_local_add(f, NULL, pa);
        uint32_t ip = jir_local_add(f, NULL, ptr);
        uint32_t ep = jir_local_add(f, NULL, ptr);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *zero = jir_emit(f, block, JIR_ZERO, (int32_t)slot);
        JirInst *adr = NULL;
        JirInst *cast = NULL;
        JirInst *elem = NULL;
        JirInst *store = NULL;
        JirInst *load = NULL;
        (void)zero;
        adr = jir_emit(f, block, JIR_ADDR, (int32_t)arr_ptr);
        adr->a = jir_local(f, slot);
        cast = jir_emit(f, block, JIR_BITCAST, (int32_t)ip);
        cast->a = jir_local(f, arr_ptr);
        elem = jir_emit(f, block, JIR_ELEM_ADDR, (int32_t)ep);
        elem->a = jir_local(f, ip);
        elem->b = jir_int(usize, 2);
        store = jir_emit(f, block, JIR_STORE, -1);
        store->a = jir_local(f, ep);
        store->b = jir_int(i32, 7);
        load = jir_emit(f, block, JIR_LOAD, (int32_t)dst);
        load->a = jir_local(f, ep);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__agg", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t slot = jir_local_add(f, NULL, point);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirOperand args[2];
        JirInst *agg = jir_emit(f, block, JIR_AGGREGATE, (int32_t)slot);
        JirInst *ex = NULL;
        args[0] = jir_int(i32, 5);
        args[1] = jir_int(i32, 6);
        CHECK(jir_set_args(env.module, agg, args, 2));
        ex = jir_emit(f, block, JIR_EXTRACT, (int32_t)dst);
        ex->a = jir_local(f, slot);
        ex->aux = 1;
        ret_local(f, block, dst);
    }
    shape = jog_type_new_nominal(env.types, JOG_TY_ENUM, "Shape", "t", NULL);
    variants[0].name = "Empty";
    variants[0].fields = NULL;
    variants[0].field_count = 0;
    variants[0].tag = 0;
    variants[0].decl = NULL;
    variants[1].name = "Other";
    variants[1].fields = NULL;
    variants[1].field_count = 0;
    variants[1].tag = 1;
    variants[1].decl = NULL;
    shape->as.nominal.base = u8;
    shape->as.nominal.variants = variants;
    shape->as.nominal.variant_count = 2;
    {
        JogType *ps = jog_type_ptr(env.types, shape, false);
        JogType *pu = jog_type_ptr(env.types, u8, false);
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__tag", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t slot = jir_local_add(f, NULL, shape);
        uint32_t addr = jir_local_add(f, NULL, ps);
        uint32_t raw = jir_local_add(f, NULL, pu);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *zero = jir_emit(f, block, JIR_ZERO, (int32_t)slot);
        JirInst *adr = NULL;
        JirInst *cast = NULL;
        JirInst *store = NULL;
        JirInst *tag = NULL;
        (void)zero;
        adr = jir_emit(f, block, JIR_ADDR, (int32_t)addr);
        adr->a = jir_local(f, slot);
        cast = jir_emit(f, block, JIR_BITCAST, (int32_t)raw);
        cast->a = jir_local(f, addr);
        store = jir_emit(f, block, JIR_STORE, -1);
        store->a = jir_local(f, raw);
        store->b = jir_int(u8, 1);
        tag = jir_emit(f, block, JIR_ENUM_TAG, (int32_t)dst);
        tag->a = jir_local(f, slot);
        ret_local(f, block, dst);
    }
    {
        JogType *id_type = jog_type_fn(env.types, &i32, 1, i32, NULL, 0, false, false, true);
        JogType *call_type = fn_of(&env, NULL, 0, i32, false);
        int32_t id_index = jir_func_add(env.module, "jog__id", "jog__id", JIR_LINK_INTERNAL, id_type, NULL);
        JirFunc *id = jir_func(env.module, (uint32_t)id_index);
        JirFunc *caller = NULL;
        JirFunc *indirect = NULL;
        uint32_t block = 0;
        CHECK(id_index >= 0);
        block = jir_block_add(id, NULL);
        ret_local(id, block, 0);
        caller = add_fn(&env, "jog__call", call_type, JIR_LINK_INTERNAL, NULL);
        block = jir_block_add(caller, NULL);
        {
            uint32_t dst = jir_local_add(caller, NULL, i32);
            JirOperand arg = jir_int(i32, 21);
            JirInst *call = jir_emit(caller, block, JIR_CALL, (int32_t)dst);
            call->a = jir_func_ref(env.module, (uint32_t)id_index);
            CHECK(jir_set_args(env.module, call, &arg, 1));
            ret_local(caller, block, dst);
        }
        indirect = add_fn(&env, "jog__indirect", call_type, JIR_LINK_INTERNAL, NULL);
        block = jir_block_add(indirect, NULL);
        {
            uint32_t fn_local = jir_local_add(indirect, NULL, id_type);
            uint32_t dst = jir_local_add(indirect, NULL, i32);
            JirOperand arg = jir_int(i32, 6);
            JirInst *copy = jir_emit(indirect, block, JIR_COPY, (int32_t)fn_local);
            JirInst *call = NULL;
            copy->a = jir_func_ref(env.module, (uint32_t)id_index);
            call = jir_emit(indirect, block, JIR_CALL, (int32_t)dst);
            call->a = jir_local(indirect, fn_local);
            CHECK(jir_set_args(env.module, call, &arg, 1));
            ret_local(indirect, block, dst);
        }
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__bounds", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *check = jir_emit(f, block, JIR_BOUNDS_CHECK, -1);
        JirInst *copy = NULL;
        check->a = jir_int(usize, 1);
        check->b = jir_int(usize, 4);
        check->pos = pos_at("b.jog", 2);
        copy = jir_emit(f, block, JIR_COPY, (int32_t)dst);
        copy->a = jir_int(i32, 1);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__nullck", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t slot = jir_local_add(f, NULL, i32);
        uint32_t addr = jir_local_add(f, NULL, ptr);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *copy = jir_emit(f, block, JIR_COPY, (int32_t)slot);
        JirInst *adr = NULL;
        JirInst *check = NULL;
        JirInst *one = NULL;
        copy->a = jir_int(i32, 1);
        adr = jir_emit(f, block, JIR_ADDR, (int32_t)addr);
        adr->a = jir_local(f, slot);
        check = jir_emit(f, block, JIR_NULL_CHECK, -1);
        check->a = jir_local(f, addr);
        check->pos = pos_at("n.jog", 3);
        one = jir_emit(f, block, JIR_COPY, (int32_t)dst);
        one->a = jir_int(i32, 1);
        ret_local(f, block, dst);
    }
    {
        JogType *params[2] = {i32, i32};
        JogType *fn = fn_of(&env, params, 2, i32, false);
        JirFunc *f = add_fn(&env, "jog__max", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t entry = jir_block_add(f, NULL);
        uint32_t then_b = jir_block_add(f, NULL);
        uint32_t else_b = jir_block_add(f, NULL);
        uint32_t done = jir_block_add(f, NULL);
        uint32_t cond = jir_local_add(f, NULL, boolean);
        uint32_t result = jir_local_add(f, NULL, i32);
        JirInst *cmp = jir_emit(f, entry, JIR_BINARY, (int32_t)cond);
        JirTerm br;
        JirInst *take = NULL;
        memset(&br, 0, sizeof br);
        cmp->aux = JIR_GT;
        cmp->a = jir_local(f, 0);
        cmp->b = jir_local(f, 1);
        br.kind = JIR_TERM_COND_BR;
        br.value = jir_local(f, cond);
        br.target = then_b;
        br.else_target = else_b;
        jir_terminate(f, entry, br);
        take = jir_emit(f, then_b, JIR_COPY, (int32_t)result);
        take->a = jir_local(f, 0);
        memset(&br, 0, sizeof br);
        br.kind = JIR_TERM_BR;
        br.target = done;
        jir_terminate(f, then_b, br);
        take = jir_emit(f, else_b, JIR_COPY, (int32_t)result);
        take->a = jir_local(f, 1);
        memset(&br, 0, sizeof br);
        br.kind = JIR_TERM_BR;
        br.target = done;
        jir_terminate(f, else_b, br);
        ret_local(f, done, result);
    }
    {
        JogType *fn = fn_of(&env, &i32, 1, i32, false);
        JirFunc *f = add_fn(&env, "jog__sw", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t entry = jir_block_add(f, NULL);
        uint32_t one = jir_block_add(f, NULL);
        uint32_t two = jir_block_add(f, NULL);
        uint32_t other = jir_block_add(f, NULL);
        JirTerm sw;
        JirTerm ret;
        memset(&sw, 0, sizeof sw);
        memset(&ret, 0, sizeof ret);
        sw_cases[0].value = 1;
        sw_cases[0].target = one;
        sw_cases[1].value = 2;
        sw_cases[1].target = two;
        sw.kind = JIR_TERM_SWITCH;
        sw.value = jir_local(f, 0);
        sw.target = other;
        sw.cases = sw_cases;
        sw.case_count = 2;
        jir_terminate(f, entry, sw);
        ret.kind = JIR_TERM_RET;
        ret.value = jir_int(i32, 10);
        jir_terminate(f, one, ret);
        ret.value = jir_int(i32, 20);
        jir_terminate(f, two, ret);
        ret.value = jir_int(i32, 30);
        jir_terminate(f, other, ret);
    }
    {
        JogType *va = jog_type_fn(env.types, &i32, 1, i32, NULL, 0, true, false, true);
        JirFunc *f = add_fn(&env, "jog__va", va, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        ret_local(f, block, 0);
    }
    {
        JogType *ext = jog_type_fn(env.types, NULL, 0, i32, NULL, 0, true, false, true);
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        int32_t ext_index = jir_func_add(env.module, "jog_extern_va", "jog_extern_va", JIR_LINK_EXTERN, ext, NULL);
        JirFunc *f = add_fn(&env, "jog__va0", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = 0;
        uint32_t dst = 0;
        JirInst *call = NULL;
        CHECK(ext_index >= 0);
        block = jir_block_add(f, NULL);
        dst = jir_local_add(f, NULL, i32);
        call = jir_emit(f, block, JIR_CALL, (int32_t)dst);
        call->a = jir_func_ref(env.module, (uint32_t)ext_index);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__overflow", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *in = jir_emit(f, block, JIR_BINARY, (int32_t)dst);
        in->aux = JIR_ADD;
        in->checked = true;
        in->a = jir_int(i32, 2147483647);
        in->b = jir_int(i32, 1);
        in->pos = pos_at("ov.jog", 8);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, u32, false);
        JirFunc *f = add_fn(&env, "jog__shldrop", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t dst = jir_local_add(f, NULL, u32);
        JirInst *in = jir_emit(f, block, JIR_BINARY, (int32_t)dst);
        in->aux = JIR_SHL;
        in->checked = true;
        in->a = jir_int(u32, 0xffffffffu);
        in->b = jir_int(u32, 1);
        in->pos = pos_at("sh.jog", 4);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__div0", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t left = jir_local_add(f, NULL, i32);
        uint32_t right = jir_local_add(f, NULL, i32);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *a = jir_emit(f, block, JIR_COPY, (int32_t)left);
        JirInst *b = NULL;
        JirInst *div = NULL;
        a->a = jir_int(i32, 1);
        b = jir_emit(f, block, JIR_COPY, (int32_t)right);
        b->a = jir_int(i32, 0);
        div = jir_emit(f, block, JIR_BINARY, (int32_t)dst);
        div->aux = JIR_DIV;
        div->checked = true;
        div->a = jir_local(f, left);
        div->b = jir_local(f, right);
        div->pos = pos_at("div.jog", 4);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__bounds_bad", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *check = jir_emit(f, block, JIR_BOUNDS_CHECK, -1);
        JirInst *copy = NULL;
        check->a = jir_int(usize, 3);
        check->b = jir_int(usize, 3);
        check->pos = pos_at("b.jog", 5);
        copy = jir_emit(f, block, JIR_COPY, (int32_t)dst);
        copy->a = jir_int(i32, 7);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__null_bad", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t slot = jir_local_add(f, NULL, ptr);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *copy = jir_emit(f, block, JIR_COPY, (int32_t)slot);
        JirInst *check = NULL;
        JirInst *done = NULL;
        copy->a = jir_null(ptr);
        check = jir_emit(f, block, JIR_NULL_CHECK, -1);
        check->a = jir_local(f, slot);
        check->pos = pos_at("n.jog", 6);
        done = jir_emit(f, block, JIR_COPY, (int32_t)dst);
        done->a = jir_int(i32, 0);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i8, false);
        JirFunc *f = add_fn(&env, "jog__conv_bad", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t src = jir_local_add(f, NULL, i32);
        uint32_t dst = jir_local_add(f, NULL, i8);
        JirInst *copy = jir_emit(f, block, JIR_COPY, (int32_t)src);
        JirInst *conv = NULL;
        copy->a = jir_int(i32, 256);
        conv = jir_emit(f, block, JIR_CONVERT, (int32_t)dst);
        conv->checked = true;
        conv->a = jir_local(f, src);
        conv->pos = pos_at("c.jog", 7);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__panic", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        JirInst *in = jir_emit(f, block, JIR_PANIC, -1);
        JirTerm term;
        memset(&term, 0, sizeof term);
        in->a = jir_string(str, "boom", 4);
        in->pos = pos_at("p.jog", 9);
        term.kind = JIR_TERM_RET;
        term.value = jir_int(i32, 0);
        jir_terminate(f, block, term);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__unreach", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        JirTerm term;
        memset(&term, 0, sizeof term);
        term.kind = JIR_TERM_UNREACHABLE;
        term.pos = pos_at("u.jog", 2);
        jir_terminate(f, block, term);
    }

    if (!verified(&env)) {
        env_close(&env);
        return;
    }
    ct_sb_init(&debug_sb);
    ct_sb_init(&fast_sb);
    CHECK(jog_emit_c(env.module, JOG_BUILD_DEBUG, &debug_sb, env.diag));
    CHECK(jog_emit_c(env.module, JOG_BUILD_RELEASE_FAST, &fast_sb, env.diag));
    debug_c = ct_sb_cstr(&debug_sb);
    fast_c = ct_sb_cstr(&fast_sb);
    expect_has(debug_c, "__builtin_add_overflow");
    expect_has(debug_c, "__builtin_sub_overflow");
    expect_has(debug_c, "__builtin_mul_overflow");
    expect_has(debug_c, "jogrt_panic_divide_by_zero");
    expect_has(debug_c, "jogrt_panic_bounds");
    expect_has(debug_c, "jogrt_panic_null");
    expect_has(debug_c, "#line 8 \"ov.jog\"");
    expect_has(debug_c, "jog_extern_va(int jog_va, ...)");
    expect_lacks(debug_c, "jog_extern_va(...)");
    expect_lacks(debug_c, "jog__copy(int32_t l0, void *env)");
    expect_has(debug_c, "static int32_t jog__copy(int32_t l0)");
    expect_has(fast_c, "__builtin_add_overflow");
    expect_has(fast_c, "jogrt_panic_null");
    expect_lacks(fast_c, "jogrt_panic_bounds");
    expect_has(debug_c, "static const uint8_t jog_s0[]");

    scratch = ct_arena_create();
    compiler_count = scratch ? find_compilers(scratch, compilers, 2) : 0;
    CHECK(compiler_count > 0);
    if (!scratch || compiler_count == 0) {
        fprintf(stderr, "no C compiler on PATH\n");
        ct_sb_free(&debug_sb);
        ct_sb_free(&fast_sb);
        ct_arena_destroy(scratch);
        env_close(&env);
        return;
    }
    debug_path = temp_path(scratch, ".c");
    fast_path = temp_path(scratch, "-fast.c");
    debug_exe = temp_path(scratch, EXE_SUFFIX);
    fast_exe = temp_path(scratch, "-fast" EXE_SUFFIX);
    CHECK(debug_path && fast_path && debug_exe && fast_exe);
    if (!debug_path || !fast_path || !debug_exe || !fast_exe || !write_parts(debug_path, debug_c, k_harness) ||
        !write_parts(fast_path, fast_c, k_harness)) {
        fprintf(stderr, "could not write generated C\n");
        g_failures++;
    } else {
        int debug_built = 0;
        int fast_built = 0;
        for (i = 0; i < compiler_count; i++) {
            if (compile_src(scratch, compilers[i], debug_path, debug_exe, JOG_BUILD_DEBUG)) {
                debug_built = 1;
            } else {
                debug_built = 0;
                break;
            }
            if (i == 0) {
                expect_run(debug_exe, "ok", 0, k_ok_out, "");
                expect_run(debug_exe, "overflow", 1, "", "panic: integer overflow\n  at ov.jog:8\n");
                expect_run(debug_exe, "shldrop", 1, "", "panic: integer overflow\n  at sh.jog:4\n");
                expect_run(debug_exe, "div0", 1, "", "panic: division by zero\n  at div.jog:4\n");
                expect_run(debug_exe, "bounds", 1, "", "panic: index 3 out of bounds for length 3\n  at b.jog:5\n");
                expect_run(debug_exe, "null", 1, "", "panic: null pointer\n  at n.jog:6\n");
                expect_run(debug_exe, "convert", 1, "", "panic: value out of range\n  at c.jog:7\n");
                expect_run(debug_exe, "panic", 1, "", "panic: boom\n  at p.jog:9\n");
                expect_run(debug_exe, "unreach", 1, "", "panic: unreachable\n  at u.jog:2\n");
            }
        }
        for (i = 0; i < compiler_count; i++) {
            if (!compile_src(scratch, compilers[i], fast_path, fast_exe, JOG_BUILD_RELEASE_FAST)) {
                fast_built = 0;
                break;
            }
            fast_built = 1;
            if (i == 0) {
                expect_run(fast_exe, "ok", 0, k_ok_out, "");
                expect_run(fast_exe, "bounds", 0, "7\n", "");
                expect_run(fast_exe, "overflow", 1, "", "panic: integer overflow\n  at ov.jog:8\n");
                expect_run(fast_exe, "shldrop", 1, "", "panic: integer overflow\n  at sh.jog:4\n");
            }
        }
        CHECK(debug_built);
        CHECK(fast_built);
    }
    if (debug_path) {
        remove(debug_path);
    }
    if (fast_path) {
        remove(fast_path);
    }
    if (debug_exe) {
        remove(debug_exe);
    }
    if (fast_exe) {
        remove(fast_exe);
    }
    ct_sb_free(&debug_sb);
    ct_sb_free(&fast_sb);
    ct_arena_destroy(scratch);
    env_close(&env);
}

static void expect_reject(Env *env) {
    CtSb sb;
    ct_sb_init(&sb);
    size_t before = ct_diag_error_count(env->diag);
    bool ok = jog_emit_c(env->module, JOG_BUILD_DEBUG, &sb, env->diag);
    if (ok || ct_diag_error_count(env->diag) == before) {
        fprintf(stderr, "FAIL expected emit to reject\n%s\n", ct_sb_cstr(&sb));
        ct_diag_print(env->diag, stderr);
        g_failures++;
    }
    ct_sb_free(&sb);
    env_close(env);
}

static void test_emit_global_data(void) {
    Env env = env_open();
    JogType *i32;
    JogType *boolean;
    JogType *str;
    JogType *pair;
    JogType *raw;
    JogField fields[2];
    uint8_t n_bytes[4] = {0xfe, 0xff, 0xff, 0xff};
    uint8_t one[4] = {1, 0, 0, 0};
    uint8_t flag[1] = {1};
    uint8_t zeros[4] = {0, 0, 0, 0};
    uint8_t pair_bytes[8] = {3, 0, 0, 0, 4, 0, 0, 0};
    uint8_t raw_bytes[4] = {104, 101, 108, 108};
    uint8_t view[16] = {0};
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    boolean = jog_type_builtin(env.types, JOG_BUILTIN_BOOL);
    str = jog_type_builtin(env.types, JOG_BUILTIN_CONST_BYTES);
    pair = jog_type_new_nominal(env.types, JOG_TY_STRUCT, "Pair", "p", NULL);
    raw = jog_type_bytes_n(env.types, 4);
    fields[0] = (JogField){"x", i32, 0, false, NULL};
    fields[1] = (JogField){"y", i32, 0, false, NULL};
    pair->as.nominal.fields = fields;
    pair->as.nominal.field_count = 2;
    CHECK(jog_type_layout(env.types, pair));
    CHECK(str->id == 15 && pair->id == 20 && raw->id == 21);
    CHECK(pair->size == 8 && fields[1].offset == 4 && str->size == 16 && boolean->size == 1);
    view[8] = 5;

    CHECK(jir_global_add(env.module, "jog__n", i32, true) == 0);
    CHECK(jir_global_add(env.module, "jog__counter", i32, false) == 1);
    CHECK(jir_global_add(env.module, "jog__flag", boolean, true) == 2);
    CHECK(jir_global_add(env.module, "jog__zilch", i32, false) == 3);
    CHECK(jir_global_add(env.module, "jog__pair", pair, true) == 4);
    CHECK(jir_global_add(env.module, "jog__raw", raw, true) == 5);
    CHECK(jir_global_add(env.module, "jog__view", str, true) == 6);
    jir_global(env.module, 0)->data = n_bytes;
    jir_global(env.module, 0)->data_len = sizeof n_bytes;
    jir_global(env.module, 0)->pos = (JogPos){"g.jog", 3, 1};
    jir_global(env.module, 1)->data = one;
    jir_global(env.module, 1)->data_len = sizeof one;
    jir_global(env.module, 2)->data = flag;
    jir_global(env.module, 2)->data_len = sizeof flag;
    jir_global(env.module, 3)->data = zeros;
    jir_global(env.module, 3)->data_len = sizeof zeros;
    jir_global(env.module, 4)->data = pair_bytes;
    jir_global(env.module, 4)->data_len = sizeof pair_bytes;
    jir_global(env.module, 5)->data = raw_bytes;
    jir_global(env.module, 5)->data_len = sizeof raw_bytes;
    jir_global(env.module, 6)->data = view;
    jir_global(env.module, 6)->data_len = sizeof view;

    EXPECT_EMIT(&env, C_PREAMBLE "\n"
                                 "typedef struct jt15 jt15;\n"
                                 "typedef struct jog_p__Pair jog_p__Pair;\n"
                                 "typedef struct jt21 jt21;\n"
                                 "\n"
                                 "struct jt15 { /* const bytes */\n"
                                 "    const uint8_t *ptr;\n"
                                 "    size_t len;\n"
                                 "};\n"
                                 "\n"
                                 "struct jog_p__Pair { /* Pair */\n"
                                 "    int32_t f_x;\n"
                                 "    int32_t f_y;\n"
                                 "};\n"
                                 "\n"
                                 "struct jt21 { /* [4]byte */\n"
                                 "    uint8_t b[4];\n"
                                 "};\n"
                                 "\n"
                                 "#line 3 \"g.jog\"\n"
                                 "static const int32_t jog__n = -2;\n"
                                 "#line 1 \"jog_emit.c\"\n"
                                 "static int32_t jog__counter = 1;\n"
                                 "static const _Bool jog__flag = 1;\n"
                                 "static int32_t jog__zilch = 0;\n"
                                 "static const jog_p__Pair jog__pair = {.f_x = 3, .f_y = 4};\n"
                                 "static const jt21 jog__raw = {.b = {104, 101, 108, 108}};\n"
                                 "static const jt15 jog__view = {.ptr = 0, .len = 5u};\n"
                                 "\n"
                                 "void jog_entry_init(void) {\n"
                                 "    (void)&jog__n;\n"
                                 "    (void)&jog__counter;\n"
                                 "    (void)&jog__flag;\n"
                                 "    (void)&jog__zilch;\n"
                                 "    (void)&jog__pair;\n"
                                 "    (void)&jog__raw;\n"
                                 "    (void)&jog__view;\n"
                                 "}\n"
                                 "\n"
                                 "void jog_entry_main(void) {\n"
                                 "}\n");
    env_close(&env);
}

static void test_emit_global_reject(void) {
    {
        Env env = env_open();
        CHECK(env_ok(&env));
        CHECK(jir_global_add(env.module, "bad name", jog_type_builtin(env.types, JOG_BUILTIN_I32), false) == 0);
        expect_reject(&env);
    }
    {
        Env env = env_open();
        JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
        uint8_t short_bytes[2] = {1, 0};
        CHECK(env_ok(&env));
        CHECK(jir_global_add(env.module, "jog__n", i32, false) == 0);
        jir_global(env.module, 0)->data = short_bytes;
        jir_global(env.module, 0)->data_len = sizeof short_bytes;
        expect_reject(&env);
    }
    {
        Env env = env_open();
        CHECK(env_ok(&env));
        CHECK(jir_global_add(env.module, "jog__n", jog_type_builtin(env.types, JOG_BUILTIN_I32), false) == 0);
        jir_global(env.module, 0)->data = NULL;
        jir_global(env.module, 0)->data_len = 4;
        expect_reject(&env);
    }
    {
        Env env = env_open();
        JogType *str = jog_type_builtin(env.types, JOG_BUILTIN_CONST_BYTES);
        uint8_t bytes[16] = {1};
        CHECK(env_ok(&env));
        CHECK(str->size == 16);
        CHECK(jir_global_add(env.module, "jog__view", str, true) == 0);
        jir_global(env.module, 0)->data = bytes;
        jir_global(env.module, 0)->data_len = sizeof bytes;
        expect_reject(&env);
    }
    {
        Env env = env_open();
        JogType *u8 = jog_type_builtin(env.types, JOG_BUILTIN_U8);
        JogType *i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
        JogType *pad = jog_type_new_nominal(env.types, JOG_TY_STRUCT, "Pad", "p", NULL);
        JogField fields[2];
        uint8_t bytes[8] = {1, 2, 0, 0, 0, 0, 0, 0};
        CHECK(env_ok(&env));
        fields[0] = (JogField){"a", u8, 0, false, NULL};
        fields[1] = (JogField){"b", i32, 0, false, NULL};
        pad->as.nominal.fields = fields;
        pad->as.nominal.field_count = 2;
        CHECK(jog_type_layout(env.types, pad));
        CHECK(pad->size == 8 && fields[1].offset == 4);
        CHECK(jir_global_add(env.module, "jog__pad", pad, true) == 0);
        jir_global(env.module, 0)->data = bytes;
        jir_global(env.module, 0)->data_len = sizeof bytes;
        expect_reject(&env);
    }
    {
        Env env = env_open();
        uint8_t byte = 1;
        CHECK(env_ok(&env));
        CHECK(jir_global_add(env.module, "jog__unit", jog_type_builtin(env.types, JOG_BUILTIN_UNIT), false) == 0);
        jir_global(env.module, 0)->data = &byte;
        jir_global(env.module, 0)->data_len = 1;
        expect_reject(&env);
    }
}

static const char k_data_harness[] =
    "\n#include <stdio.h>\n"
    "int main(void) {\n"
    "    printf(\"%d\\n\", (int)jog__read_n());\n"
    "    printf(\"%d\\n\", (int)jog__bump());\n"
    "    printf(\"%d\\n\", (int)jog__read_pair());\n"
    "    printf(\"%d\\n\", (int)jog__read_byte());\n"
    "    printf(\"%d\\n\", (int)jog__read_opt());\n"
    "    printf(\"%d\\n\", (int)jog__read_enum());\n"
    "    printf(\"%d\\n\", (int)jog__read_flt());\n"
    "    printf(\"%d\\n\", (int)jog__read_str());\n"
    "    printf(\"%d\\n\", (int)jog__read_cstr());\n"
    "    printf(\"%d\\n\", (int)jog__read_zero());\n"
    "    return 0;\n"
    "}\n";

static const char k_data_out[] = "42\n"
                                 "11\n"
                                 "7\n"
                                 "104\n"
                                 "6\n"
                                 "9\n"
                                 "1\n"
                                 "5\n"
                                 "65\n"
                                 "0\n";

static void test_execute_data(void) {
    Env env = env_open();
    JogType *i32;
    JogType *u8;
    JogType *boolean;
    JogType *f64;
    JogType *usize;
    JogType *str;
    JogType *cu8;
    JogType *ci32;
    JogType *pair;
    JogType *raw;
    JogType *opt;
    JogType *shape;
    JogField pair_fields[2];
    JogField circle_fields[1];
    JogVariant variants[2];
    uint8_t n_bytes[4] = {42, 0, 0, 0};
    uint8_t ten[4] = {10, 0, 0, 0};
    uint8_t pair_bytes[8] = {3, 0, 0, 0, 4, 0, 0, 0};
    uint8_t raw_bytes[4] = {'h', 'e', 'l', 'l'};
    uint8_t opt_bytes[8] = {1, 0, 0, 0, 6, 0, 0, 0};
    uint8_t shape_bytes[8] = {0, 0, 0, 0, 9, 0, 0, 0};
    uint8_t flt_bytes[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x3f};
    int32_t n_id;
    int32_t counter_id;
    int32_t pair_id;
    int32_t raw_id;
    int32_t opt_id;
    int32_t shape_id;
    int32_t flt_id;
    int32_t zero_id;
    CtSb debug_sb;
    CtSb fast_sb;
    CtArena *scratch = NULL;
    const char *compilers[2];
    const char *debug_c;
    char *path = NULL;
    char *exe = NULL;
    int compiler_count;
    int i;
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    u8 = jog_type_builtin(env.types, JOG_BUILTIN_U8);
    boolean = jog_type_builtin(env.types, JOG_BUILTIN_BOOL);
    f64 = jog_type_builtin(env.types, JOG_BUILTIN_F64);
    usize = jog_type_builtin(env.types, JOG_BUILTIN_USIZE);
    str = jog_type_builtin(env.types, JOG_BUILTIN_CONST_BYTES);
    cu8 = jog_type_ptr(env.types, u8, true);
    ci32 = jog_type_ptr(env.types, i32, true);
    pair = jog_type_new_nominal(env.types, JOG_TY_STRUCT, "Pair", "p", NULL);
    raw = jog_type_bytes_n(env.types, 4);
    opt = jog_type_option(env.types, i32);
    shape = jog_type_new_nominal(env.types, JOG_TY_ENUM, "Shape", "p", NULL);
    pair_fields[0] = (JogField){"x", i32, 0, false, NULL};
    pair_fields[1] = (JogField){"y", i32, 0, false, NULL};
    pair->as.nominal.fields = pair_fields;
    pair->as.nominal.field_count = 2;
    circle_fields[0] = (JogField){"r", i32, 0, false, NULL};
    variants[0] = (JogVariant){"Circle", circle_fields, 1, 0, NULL};
    variants[1] = (JogVariant){"Empty", NULL, 0, 1, NULL};
    shape->as.nominal.variants = variants;
    shape->as.nominal.variant_count = 2;
    CHECK(jog_type_layout(env.types, pair));
    CHECK(jog_type_layout(env.types, shape));
    CHECK(pair->size == 8 && pair_fields[1].offset == 4);
    CHECK(shape->size == 8 && shape->as.nominal.payload_offset == 4);
    CHECK(opt->size == 8 && raw->size == 4 && f64->size == 8);

    n_id = jir_global_add(env.module, "jog__n", i32, true);
    counter_id = jir_global_add(env.module, "jog__counter", i32, false);
    pair_id = jir_global_add(env.module, "jog__pair", pair, true);
    raw_id = jir_global_add(env.module, "jog__raw", raw, true);
    opt_id = jir_global_add(env.module, "jog__opt", opt, true);
    shape_id = jir_global_add(env.module, "jog__shape", shape, true);
    flt_id = jir_global_add(env.module, "jog__flt", f64, true);
    zero_id = jir_global_add(env.module, "jog__zero", i32, false);
    CHECK(n_id == 0 && zero_id == 7);
    jir_global(env.module, (uint32_t)n_id)->data = n_bytes;
    jir_global(env.module, (uint32_t)n_id)->data_len = sizeof n_bytes;
    jir_global(env.module, (uint32_t)counter_id)->data = ten;
    jir_global(env.module, (uint32_t)counter_id)->data_len = sizeof ten;
    jir_global(env.module, (uint32_t)pair_id)->data = pair_bytes;
    jir_global(env.module, (uint32_t)pair_id)->data_len = sizeof pair_bytes;
    jir_global(env.module, (uint32_t)raw_id)->data = raw_bytes;
    jir_global(env.module, (uint32_t)raw_id)->data_len = sizeof raw_bytes;
    jir_global(env.module, (uint32_t)opt_id)->data = opt_bytes;
    jir_global(env.module, (uint32_t)opt_id)->data_len = sizeof opt_bytes;
    jir_global(env.module, (uint32_t)shape_id)->data = shape_bytes;
    jir_global(env.module, (uint32_t)shape_id)->data_len = sizeof shape_bytes;
    jir_global(env.module, (uint32_t)flt_id)->data = flt_bytes;
    jir_global(env.module, (uint32_t)flt_id)->data_len = sizeof flt_bytes;

    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__read_n", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *load = jir_emit(f, block, JIR_LOAD, (int32_t)dst);
        load->a = jir_global_ref(env.module, (uint32_t)n_id);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__bump", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t cur = jir_local_add(f, NULL, i32);
        uint32_t next = jir_local_add(f, NULL, i32);
        JirInst *load = jir_emit(f, block, JIR_LOAD, (int32_t)cur);
        JirInst *add;
        JirInst *store;
        load->a = jir_global_ref(env.module, (uint32_t)counter_id);
        add = jir_emit(f, block, JIR_BINARY, (int32_t)next);
        add->aux = JIR_ADD;
        add->a = jir_local(f, cur);
        add->b = jir_int(i32, 1);
        store = jir_emit(f, block, JIR_STORE, -1);
        store->a = jir_global_ref(env.module, (uint32_t)counter_id);
        store->b = jir_local(f, next);
        ret_local(f, block, next);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__read_pair", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t value = jir_local_add(f, NULL, pair);
        uint32_t x = jir_local_add(f, NULL, i32);
        uint32_t y = jir_local_add(f, NULL, i32);
        uint32_t sum = jir_local_add(f, NULL, i32);
        JirInst *load = jir_emit(f, block, JIR_LOAD, (int32_t)value);
        JirInst *ex;
        JirInst *ey;
        JirInst *add;
        load->a = jir_global_ref(env.module, (uint32_t)pair_id);
        ex = jir_emit(f, block, JIR_EXTRACT, (int32_t)x);
        ex->aux = 0;
        ex->a = jir_local(f, value);
        ey = jir_emit(f, block, JIR_EXTRACT, (int32_t)y);
        ey->aux = 1;
        ey->a = jir_local(f, value);
        add = jir_emit(f, block, JIR_BINARY, (int32_t)sum);
        add->aux = JIR_ADD;
        add->a = jir_local(f, x);
        add->b = jir_local(f, y);
        ret_local(f, block, sum);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__read_byte", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t ptr = jir_local_add(f, NULL, cu8);
        uint32_t byte = jir_local_add(f, NULL, u8);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *cast = jir_emit(f, block, JIR_BITCAST, (int32_t)ptr);
        JirInst *load;
        JirInst *conv;
        cast->a = jir_global_ref(env.module, (uint32_t)raw_id);
        load = jir_emit(f, block, JIR_LOAD, (int32_t)byte);
        load->a = jir_local(f, ptr);
        conv = jir_emit(f, block, JIR_CONVERT, (int32_t)dst);
        conv->a = jir_local(f, byte);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__read_opt", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t value = jir_local_add(f, NULL, opt);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *load = jir_emit(f, block, JIR_LOAD, (int32_t)value);
        JirInst *ex;
        load->a = jir_global_ref(env.module, (uint32_t)opt_id);
        ex = jir_emit(f, block, JIR_EXTRACT, (int32_t)dst);
        ex->aux = 1;
        ex->a = jir_local(f, value);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__read_enum", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t addr = jir_local_add(f, NULL, ci32);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *field = jir_emit(f, block, JIR_FIELD_ADDR, (int32_t)addr);
        JirInst *load;
        field->a = jir_global_ref(env.module, (uint32_t)shape_id);
        field->aux = 0;
        field->aux2 = 1;
        load = jir_emit(f, block, JIR_LOAD, (int32_t)dst);
        load->a = jir_local(f, addr);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__read_flt", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t value = jir_local_add(f, NULL, f64);
        uint32_t flag = jir_local_add(f, NULL, boolean);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *load = jir_emit(f, block, JIR_LOAD, (int32_t)value);
        JirInst *cmp;
        JirInst *conv;
        load->a = jir_global_ref(env.module, (uint32_t)flt_id);
        cmp = jir_emit(f, block, JIR_BINARY, (int32_t)flag);
        cmp->aux = JIR_EQ;
        cmp->a = jir_local(f, value);
        cmp->b = jir_float(f64, 1.5);
        conv = jir_emit(f, block, JIR_CONVERT, (int32_t)dst);
        conv->a = jir_local(f, flag);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__read_str", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t value = jir_local_add(f, NULL, str);
        uint32_t len = jir_local_add(f, NULL, usize);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *copy = jir_emit(f, block, JIR_COPY, (int32_t)value);
        JirInst *ex;
        JirInst *conv;
        copy->a = jir_string(str, "hello", 5);
        ex = jir_emit(f, block, JIR_EXTRACT, (int32_t)len);
        ex->aux = 1;
        ex->a = jir_local(f, value);
        conv = jir_emit(f, block, JIR_CONVERT, (int32_t)dst);
        conv->a = jir_local(f, len);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__read_cstr", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t ptr = jir_local_add(f, NULL, cu8);
        uint32_t byte = jir_local_add(f, NULL, u8);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *copy = jir_emit(f, block, JIR_COPY, (int32_t)ptr);
        JirInst *load;
        JirInst *conv;
        copy->a = jir_cstring(cu8, "A", 1);
        load = jir_emit(f, block, JIR_LOAD, (int32_t)byte);
        load->a = jir_local(f, ptr);
        conv = jir_emit(f, block, JIR_CONVERT, (int32_t)dst);
        conv->a = jir_local(f, byte);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__read_zero", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block = jir_block_add(f, NULL);
        uint32_t dst = jir_local_add(f, NULL, i32);
        JirInst *load = jir_emit(f, block, JIR_LOAD, (int32_t)dst);
        load->a = jir_global_ref(env.module, (uint32_t)zero_id);
        ret_local(f, block, dst);
    }

    if (!verified(&env)) {
        env_close(&env);
        return;
    }
    ct_sb_init(&debug_sb);
    ct_sb_init(&fast_sb);
    if (!jog_emit_c(env.module, JOG_BUILD_DEBUG, &debug_sb, env.diag) ||
        !jog_emit_c(env.module, JOG_BUILD_RELEASE_FAST, &fast_sb, env.diag)) {
        ct_diag_print(env.diag, stderr);
        g_failures++;
        ct_sb_free(&debug_sb);
        ct_sb_free(&fast_sb);
        env_close(&env);
        return;
    }
    debug_c = ct_sb_cstr(&debug_sb);
    CHECK(strcmp(debug_c, ct_sb_cstr(&fast_sb)) == 0);
    expect_has(debug_c, "static const int32_t jog__n = 42;");
    expect_has(debug_c, "static int32_t jog__counter = 10;");
    expect_has(debug_c, "static int32_t jog__zero = 0;");
    expect_has(debug_c, ".f_x = 3, .f_y = 4");
    expect_has(debug_c, ".b = {104, 101, 108, 108}");
    expect_has(debug_c, ".has = 1, .value = 6");
    expect_has(debug_c, ".f_r = 9");
    expect_has(debug_c, "static const uint8_t jog_s0[] = {104, 101, 108, 108, 111};");
    expect_has(debug_c, "static const uint8_t jog_c1[] = {65, 0};");
    expect_lacks(debug_c, "jog_s1");
    expect_lacks(debug_c, "jog_c0");
    expect_has(debug_c, "(void)&jog__n;");

    scratch = ct_arena_create();
    compiler_count = scratch ? find_compilers(scratch, compilers, 2) : 0;
    CHECK(compiler_count > 0);
    path = scratch ? temp_path(scratch, "-data.c") : NULL;
    exe = scratch ? temp_path(scratch, "-data" EXE_SUFFIX) : NULL;
    if (!scratch || compiler_count == 0 || !path || !exe || !write_parts(path, debug_c, k_data_harness)) {
        fprintf(stderr, "could not write global data C\n");
        g_failures++;
    } else {
        for (i = 0; i < compiler_count; i++) {
            if (!compile_src(scratch, compilers[i], path, exe, JOG_BUILD_DEBUG)) {
                g_failures++;
                break;
            }
            expect_run(exe, "data", 0, k_data_out, "");
        }
    }
    if (path) {
        remove(path);
    }
    if (exe) {
        remove(exe);
    }
    ct_arena_destroy(scratch);
    ct_sb_free(&debug_sb);
    ct_sb_free(&fast_sb);
    env_close(&env);
}

static const char k_init_harness[] =
    "\n#include <stdio.h>\n"
    "int main(void) {\n"
    "    jog_entry_init();\n"
    "    printf(\"%d\\n\", (int)jog__read_n());\n"
    "    printf(\"%d\\n\", (int)jog__read_k());\n"
    "    return 0;\n"
    "}\n";

static const char k_init_out[] = "42\n"
                                 "7\n";

/* A top-level val whose initializer is not a folded integer or bool is
   is_const, and init stores it. That object must be writable. A folded
   integer or bool with no store stays const. */
static void test_execute_init_store(void) {
    Env env = env_open();
    JogType *i32;
    JogType *boolean;
    JogType *unit;
    JogType *unit_fn;
    uint8_t seven[4] = {7, 0, 0, 0};
    uint8_t flag[1] = {1};
    int32_t n_id;
    int32_t k_id;
    int32_t flag_id;
    CtSb debug_sb;
    CtSb fast_sb;
    CtArena *scratch = NULL;
    const char *compilers[2];
    const char *debug_c;
    char *path = NULL;
    char *exe = NULL;
    int compiler_count;
    int i;
    CHECK(env_ok(&env));
    if (!env_ok(&env)) {
        env_close(&env);
        return;
    }
    i32 = jog_type_builtin(env.types, JOG_BUILTIN_I32);
    boolean = jog_type_builtin(env.types, JOG_BUILTIN_BOOL);
    unit = jog_type_builtin(env.types, JOG_BUILTIN_UNIT);
    unit_fn = fn_of(&env, NULL, 0, unit, false);
    n_id = jir_global_add(env.module, "jog__n", i32, true);
    k_id = jir_global_add(env.module, "jog__k", i32, true);
    flag_id = jir_global_add(env.module, "jog__flag", boolean, true);
    CHECK(n_id == 0 && k_id == 1 && flag_id == 2);
    jir_global(env.module, (uint32_t)k_id)->data = seven;
    jir_global(env.module, (uint32_t)k_id)->data_len = sizeof seven;
    jir_global(env.module, (uint32_t)flag_id)->data = flag;
    jir_global(env.module, (uint32_t)flag_id)->data_len = sizeof flag;

    {
        JirFunc *f = add_fn(&env, "jog_app__0init", unit_fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block;
        uint32_t sum;
        JirInst *add;
        JirInst *store;
        if (!f) {
            env_close(&env);
            return;
        }
        block = jir_block_add(f, NULL);
        sum = jir_local_add(f, NULL, i32);
        add = jir_emit(f, block, JIR_BINARY, (int32_t)sum);
        add->aux = JIR_ADD;
        add->a = jir_int(i32, 20);
        add->b = jir_int(i32, 22);
        store = jir_emit(f, block, JIR_STORE, -1);
        store->a = jir_global_ref(env.module, (uint32_t)n_id);
        store->b = jir_local(f, sum);
        ret_none(f, block);
        env.module->init_func = 0;
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__read_n", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block;
        uint32_t dst;
        JirInst *load;
        if (!f) {
            env_close(&env);
            return;
        }
        block = jir_block_add(f, NULL);
        dst = jir_local_add(f, NULL, i32);
        load = jir_emit(f, block, JIR_LOAD, (int32_t)dst);
        load->a = jir_global_ref(env.module, (uint32_t)n_id);
        ret_local(f, block, dst);
    }
    {
        JogType *fn = fn_of(&env, NULL, 0, i32, false);
        JirFunc *f = add_fn(&env, "jog__read_k", fn, JIR_LINK_INTERNAL, NULL);
        uint32_t block;
        uint32_t dst;
        JirInst *load;
        if (!f) {
            env_close(&env);
            return;
        }
        block = jir_block_add(f, NULL);
        dst = jir_local_add(f, NULL, i32);
        load = jir_emit(f, block, JIR_LOAD, (int32_t)dst);
        load->a = jir_global_ref(env.module, (uint32_t)k_id);
        ret_local(f, block, dst);
    }
    if (!verified(&env)) {
        env_close(&env);
        return;
    }

    ct_sb_init(&debug_sb);
    ct_sb_init(&fast_sb);
    if (!jog_emit_c(env.module, JOG_BUILD_DEBUG, &debug_sb, env.diag) ||
        !jog_emit_c(env.module, JOG_BUILD_RELEASE_FAST, &fast_sb, env.diag)) {
        ct_diag_print(env.diag, stderr);
        g_failures++;
        ct_sb_free(&debug_sb);
        ct_sb_free(&fast_sb);
        env_close(&env);
        return;
    }
    debug_c = ct_sb_cstr(&debug_sb);
    CHECK(strcmp(debug_c, ct_sb_cstr(&fast_sb)) == 0);
    expect_has(debug_c, "static int32_t jog__n = 0;");
    expect_lacks(debug_c, "static const int32_t jog__n");
    expect_has(debug_c, "static const int32_t jog__k = 7;");
    expect_has(debug_c, "static const _Bool jog__flag = 1;");
    expect_has(debug_c, "jog_app__0init();");

    scratch = ct_arena_create();
    compiler_count = scratch ? find_compilers(scratch, compilers, 2) : 0;
    CHECK(compiler_count > 0);
    path = scratch ? temp_path(scratch, "-init.c") : NULL;
    exe = scratch ? temp_path(scratch, "-init" EXE_SUFFIX) : NULL;
    if (!scratch || compiler_count == 0 || !path || !exe || !write_parts(path, debug_c, k_init_harness)) {
        fprintf(stderr, "could not write init-store C\n");
        g_failures++;
    } else {
        for (i = 0; i < compiler_count; i++) {
            if (!compile_src(scratch, compilers[i], path, exe, JOG_BUILD_DEBUG)) {
                g_failures++;
                break;
            }
            expect_run(exe, "init", 0, k_init_out, "");
        }
    }
    if (path) {
        remove(path);
    }
    if (exe) {
        remove(exe);
    }
    ct_arena_destroy(scratch);
    ct_sb_free(&debug_sb);
    ct_sb_free(&fast_sb);
    env_close(&env);
}

int main(void) {
    test_empty();
    test_max();
    test_globals();
    test_constants();
    test_addr_refs();
    test_ops();
    test_switch();
    test_linkage();
    test_positions();
    test_unterminated();
    test_emit_empty();
    test_emit_order();
    test_emit_dedup();
    test_emit_collision();
    test_emit_fn_type_only();
    test_emit_kinds();
    test_emit_void_ptr();
    test_emit_global_data();
    test_emit_global_reject();
    test_execute();
    test_execute_data();
    test_execute_init_store();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
