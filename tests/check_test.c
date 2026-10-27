#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "const_eval.h"
#include "lexer.h"
#include "phases.h"
#include "resolve_package.h"
#include "resolve_universe.h"
#include "symbols.h"

#include "da.h"
#include "file.h"
#include "fs.h"
#include "str.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

static int g_failures = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

static const char *const k_kind_names[JOG_SYM_COUNT] = {
    "package", "type", "function", "variable", "constant",
    "field", "method", "variant", "parameter", "context"
};

static void test_symbols(void) {
    CtArena *arena = ct_arena_create();
    JogTypes *types = jog_types_create(arena, 8);
    JogPackage package = {0};
    JogPos pos = {"a.jog", 1, 1};
    JogNode *fn_decl;
    JogNode *var_decl;
    JogType *i64;
    JogSymbol *fn_symbol;
    JogSymbol *var_symbol;
    JogSymbol *shadow;
    JogSymbol *local;
    JogPackageScope *package_scope;
    JogBlockScope *outer;
    JogBlockScope *inner;
    JogSymbolKind kind;
    int i;

    CHECK(arena != NULL && types != NULL);
    if (!arena || !types) {
        ct_arena_destroy(arena);
        return;
    }

    package.path = "app";
    package.name = "app";
    i64 = jog_type_builtin(types, JOG_BUILTIN_I64);
    fn_decl = jog_node_new(arena, JOG_NODE_FN, pos);
    var_decl = jog_node_new(arena, JOG_NODE_VAR, pos);
    CHECK(fn_decl != NULL && var_decl != NULL);

    CHECK(jog_symbol_new(NULL, JOG_SYM_FUNCTION, "f", fn_decl, i64, &package) == NULL);
    CHECK(jog_package_scope_new(NULL, &package) == NULL);
    CHECK(jog_block_scope_new(NULL, NULL, NULL) == NULL);
    CHECK(strcmp(jog_symbol_kind_name(JOG_SYM_COUNT), "?") == 0);
    CHECK(strcmp(jog_symbol_kind_name((JogSymbolKind)100), "?") == 0);

    for (i = 0; i < (int)JOG_SYM_COUNT; i++) {
        kind = (JogSymbolKind)i;
        CHECK(strcmp(jog_symbol_kind_name(kind), k_kind_names[i]) == 0);
    }

    fn_symbol = jog_symbol_new(arena, JOG_SYM_FUNCTION, "f", fn_decl, i64, &package);
    var_symbol = jog_symbol_new(arena, JOG_SYM_VARIABLE, "n", var_decl, i64, &package);
    shadow = jog_symbol_new(arena, JOG_SYM_VARIABLE, "n", var_decl, i64, &package);
    local = jog_symbol_new(arena, JOG_SYM_CONSTANT, "local", NULL, i64, &package);
    CHECK(fn_symbol && fn_symbol->kind == JOG_SYM_FUNCTION);
    CHECK(fn_symbol->decl == fn_decl && fn_symbol->type == i64 && fn_symbol->package == &package);
    CHECK(fn_symbol->name && strcmp(fn_symbol->name, "f") == 0);

    package_scope = jog_package_scope_new(arena, &package);
    CHECK(package_scope && package_scope->package == &package);
    CHECK(package_scope->bindings != NULL);
    package.scope = package_scope;

    CHECK(!jog_package_scope_bind(NULL, fn_symbol));
    CHECK(!jog_package_scope_bind(package_scope, NULL));
    CHECK(!jog_package_scope_lookup(NULL, "f"));
    CHECK(!jog_package_scope_lookup(package_scope, NULL));
    CHECK(jog_package_scope_bind(package_scope, fn_symbol));
    CHECK(jog_package_scope_bind(package_scope, var_symbol));
    CHECK(!jog_package_scope_bind(package_scope, var_symbol));
    CHECK(jog_package_scope_lookup(package_scope, "f") == fn_symbol);
    CHECK(jog_package_scope_lookup(package_scope, "n") == var_symbol);
    CHECK(jog_package_scope_lookup(package_scope, "missing") == NULL);
    CHECK(ct_map_count(package_scope->bindings) == 2);

    outer = jog_block_scope_new(arena, NULL, package_scope);
    inner = jog_block_scope_new(arena, outer, NULL);
    CHECK(outer && outer->parent == NULL && outer->package == package_scope);
    CHECK(inner && inner->parent == outer && inner->package == package_scope);
    CHECK(jog_block_scope_lookup(outer, "f") == fn_symbol);
    CHECK(jog_block_scope_bind(outer, shadow));
    CHECK(!jog_block_scope_bind(outer, shadow));
    CHECK(jog_block_scope_lookup(outer, "n") == shadow);
    CHECK(jog_package_scope_lookup(package_scope, "n") == var_symbol);
    CHECK(jog_block_scope_bind(inner, local));
    CHECK(jog_block_scope_lookup(inner, "local") == local);
    CHECK(jog_block_scope_lookup(inner, "n") == shadow);
    CHECK(jog_block_scope_lookup(inner, "f") == fn_symbol);
    CHECK(jog_block_scope_lookup(outer, "local") == NULL);
    CHECK(!jog_block_scope_bind(NULL, local));
    CHECK(!jog_block_scope_lookup(NULL, "f"));
    CHECK(!jog_block_scope_lookup(outer, NULL));

    jog_types_release(types);
    ct_arena_destroy(arena);
}

static const struct {
    const char *name;
    JogSymbolKind kind;
    JogBuiltin builtin;
} k_universe[] = {
    {"i8", JOG_SYM_TYPE, JOG_BUILTIN_I8},
    {"i16", JOG_SYM_TYPE, JOG_BUILTIN_I16},
    {"i32", JOG_SYM_TYPE, JOG_BUILTIN_I32},
    {"i64", JOG_SYM_TYPE, JOG_BUILTIN_I64},
    {"u8", JOG_SYM_TYPE, JOG_BUILTIN_U8},
    {"u16", JOG_SYM_TYPE, JOG_BUILTIN_U16},
    {"u32", JOG_SYM_TYPE, JOG_BUILTIN_U32},
    {"u64", JOG_SYM_TYPE, JOG_BUILTIN_U64},
    {"isize", JOG_SYM_TYPE, JOG_BUILTIN_ISIZE},
    {"usize", JOG_SYM_TYPE, JOG_BUILTIN_USIZE},
    {"f32", JOG_SYM_TYPE, JOG_BUILTIN_F32},
    {"f64", JOG_SYM_TYPE, JOG_BUILTIN_F64},
    {"bool", JOG_SYM_TYPE, JOG_BUILTIN_BOOL},
    {"bytes", JOG_SYM_TYPE, JOG_BUILTIN_BYTES},
    {"str", JOG_SYM_TYPE, JOG_BUILTIN_CONST_BYTES},
    {"true", JOG_SYM_CONSTANT, JOG_BUILTIN_BOOL},
    {"false", JOG_SYM_CONSTANT, JOG_BUILTIN_BOOL},
    {"null", JOG_SYM_CONSTANT, JOG_BUILTIN_NULL},
};

static void test_universe(void) {
    CtArena *arena = ct_arena_create();
    CtIntern *intern;
    JogTypes *types;
    JogPackageScope *scope;
    size_t count;
    size_t i;

    CHECK(arena != NULL);
    if (!arena) {
        return;
    }
    intern = ct_intern_create(arena);
    types = jog_types_create(arena, 8);
    CHECK(intern != NULL && types != NULL);
    if (!intern || !types) {
        jog_types_release(types);
        ct_arena_destroy(arena);
        return;
    }

    CHECK(jog_universe_scope(NULL, intern, types) == NULL);
    CHECK(jog_universe_scope(arena, NULL, types) == NULL);
    CHECK(jog_universe_scope(arena, intern, NULL) == NULL);

    scope = jog_universe_scope(arena, intern, types);
    CHECK(scope != NULL && scope->package == NULL);
    if (!scope) {
        jog_types_release(types);
        ct_arena_destroy(arena);
        return;
    }

    count = sizeof(k_universe) / sizeof(k_universe[0]);
    CHECK(ct_map_count(scope->bindings) == count + 7);
    for (i = 0; i < count; i++) {
        const char *name = ct_intern_cstr(intern, k_universe[i].name);
        JogSymbol *symbol = jog_package_scope_lookup(scope, name);
        JogType *type = jog_type_builtin(types, k_universe[i].builtin);
        CHECK(name != NULL && symbol != NULL && type != NULL);
        if (!name || !symbol || !type) {
            continue;
        }
        CHECK(symbol->kind == k_universe[i].kind);
        CHECK(symbol->decl == NULL);
        CHECK(symbol->package == NULL);
        CHECK(symbol->name == name);
        CHECK(symbol->type == type);
    }
    {
        JogSymbol *void_symbol = jog_package_scope_lookup(scope, "void");
        JogType *unit = jog_type_builtin(types, JOG_BUILTIN_UNIT);
        JogType *ptr;
        JogType *cptr;
        CtSb sb;
        CHECK(void_symbol != NULL && unit != NULL);
        if (void_symbol && unit) {
            CHECK(void_symbol->kind == JOG_SYM_TYPE);
            CHECK(void_symbol->decl == NULL && void_symbol->package == NULL);
            CHECK(void_symbol->name == ct_intern_cstr(intern, "void"));
            CHECK(void_symbol->type != NULL && void_symbol->type != unit);
            CHECK(void_symbol->type->kind == JOG_TY_STRUCT);
            CHECK(void_symbol->type->as.nominal.field_count == 0);
            CHECK(!void_symbol->type->has_layout);
            ptr = jog_type_ptr(types, void_symbol->type, false);
            cptr = jog_type_ptr(types, void_symbol->type, true);
            CHECK(ptr != NULL && cptr != NULL && ptr != cptr);
            CHECK(ptr && !ptr->is_const && ptr->as.elem.elem == void_symbol->type);
            CHECK(cptr && cptr->is_const && cptr->as.elem.elem == void_symbol->type);
            ct_sb_init(&sb);
            jog_type_print(void_symbol->type, &sb);
            CHECK(strcmp(ct_sb_cstr(&sb), "void") == 0);
            ct_sb_clear(&sb);
            jog_type_print(ptr, &sb);
            CHECK(strcmp(ct_sb_cstr(&sb), "*void") == 0);
            ct_sb_clear(&sb);
            jog_type_print(cptr, &sb);
            CHECK(strcmp(ct_sb_cstr(&sb), "*const void") == 0);
            ct_sb_free(&sb);
        }
    }
    {
        JogSymbol *error_symbol = jog_package_scope_lookup(scope, "error");
        CHECK(error_symbol != NULL);
        if (error_symbol) {
            CHECK(error_symbol->kind == JOG_SYM_TYPE);
            CHECK(error_symbol->decl == NULL && error_symbol->package == NULL);
            CHECK(error_symbol->name == ct_intern_cstr(intern, "error"));
            CHECK(error_symbol->type != NULL);
            CHECK(error_symbol->type->kind == JOG_TY_INTERFACE);
            CHECK(error_symbol->type->has_layout);
            CHECK(error_symbol->type->as.nominal.name == error_symbol->name);
        }
    }
    CHECK(jog_package_scope_lookup(scope, "unit") == NULL);

    jog_types_release(types);
    ct_arena_destroy(arena);
}

static JogNode *mk(CtArena *arena, JogNodeKind kind, JogPos pos) {
    JogNode *node = jog_node_new(arena, kind, pos);
    CHECK(node != NULL);
    return node;
}

static JogNode *lit(CtArena *arena, JogPos pos, const char *text) {
    JogNode *node = mk(arena, JOG_NODE_INT_LIT, pos);
    if (!node) {
        return NULL;
    }
    node->as.literal.text = text;
    node->as.literal.len = strlen(text);
    return node;
}

static JogNode *boolean(CtArena *arena, JogPos pos, bool value) {
    JogNode *node = mk(arena, JOG_NODE_BOOL_LIT, pos);
    if (!node) {
        return NULL;
    }
    node->as.literal.value = value;
    return node;
}

static JogNode *una(CtArena *arena, JogPos pos, JogTokenKind op, JogNode *operand) {
    JogNode *node = mk(arena, JOG_NODE_UNARY, pos);
    if (!node) {
        return NULL;
    }
    node->as.unary.op = op;
    node->as.unary.operand = operand;
    return node;
}

static JogNode *bin(CtArena *arena, JogPos pos, JogTokenKind op, JogNode *lhs, JogNode *rhs) {
    JogNode *node = mk(arena, JOG_NODE_BINARY, pos);
    if (!node) {
        return NULL;
    }
    node->as.binary.op = op;
    node->as.binary.lhs = lhs;
    node->as.binary.rhs = rhs;
    return node;
}

static void diag_text(CtDiag *diag, char *buf, size_t cap) {
    FILE *file = tmpfile();
    size_t n;
    buf[0] = '\0';
    if (!file || cap == 0) {
        if (file) {
            fclose(file);
        }
        return;
    }
    ct_diag_print(diag, file);
    rewind(file);
    n = fread(buf, 1, cap - 1, file);
    buf[n] = '\0';
    fclose(file);
}

static void expect_int(CtArena *arena, CtDiag *diag, JogTypes *types, JogNode *node, JogType *target, JogType *want,
                       uint64_t bits, size_t *errors) {
    JogConst value;
    if (!node || !want) {
        return;
    }
    memset(&value, 0, sizeof(value));
    CHECK(jog_const_eval(arena, diag, types, node, target, &value));
    CHECK(value.kind == JOG_CONST_INT);
    CHECK(value.type == want);
    CHECK(node->type == want);
    CHECK(value.bits == bits);
    CHECK(ct_diag_error_count(diag) == *errors);
}

static void expect_bool(CtArena *arena, CtDiag *diag, JogTypes *types, JogNode *node, bool bit, size_t *errors) {
    JogConst value;
    JogType *boolean;
    if (!node) {
        return;
    }
    boolean = jog_type_builtin(types, JOG_BUILTIN_BOOL);
    memset(&value, 0, sizeof(value));
    CHECK(jog_const_eval(arena, diag, types, node, NULL, &value));
    CHECK(value.kind == JOG_CONST_BOOL);
    CHECK(value.type == boolean);
    CHECK(node->type == boolean);
    CHECK(value.bits == (bit ? 1u : 0u));
    CHECK(ct_diag_error_count(diag) == *errors);
}

static const char *last_line(char *text) {
    char *last = text;
    char *p;
    for (p = text; *p != '\0'; p++) {
        if (*p == '\n' && p[1] != '\0') {
            last = p + 1;
        }
    }
    return last;
}

static void expect_err(CtArena *arena, CtDiag *diag, JogTypes *types, JogNode *node, JogType *target, const char *msg,
                       size_t *errors) {
    JogConst value;
    char text[8192];
    size_t before;
    if (!node) {
        return;
    }
    before = ct_diag_error_count(diag);
    memset(&value, 0, sizeof(value));
    CHECK(!jog_const_eval(arena, diag, types, node, target, &value));
    CHECK(ct_diag_error_count(diag) == before + 1);
    CHECK(node->type == NULL);
    diag_text(diag, text, sizeof(text));
    CHECK(strstr(last_line(text), msg) != NULL);
    *errors = ct_diag_error_count(diag);
}

static void test_const_eval(void) {
    CtArena *arena = ct_arena_create();
    CtDiag *diag;
    JogTypes *types;
    JogPos pos = {"a.jog", 4, 7};
    JogType *i8;
    JogType *i32;
    JogType *i64;
    JogType *u8;
    JogType *u16;
    JogType *u32;
    JogType *u64;
    JogType *untyped;
    size_t errors = 0;
    JogNode *wide_l;
    JogNode *wide_r;
    JogNode *sum;
    JogNode *bytes_lit;
    JogConst folded;
    const unsigned char deadbeef[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    const unsigned char ff00[2] = {0xFF, 0x00};

    CHECK(arena != NULL);
    if (!arena) {
        return;
    }
    diag = ct_diag_create(arena);
    types = jog_types_create(arena, 8);
    CHECK(diag != NULL && types != NULL);
    if (!diag || !types) {
        jog_types_release(types);
        ct_arena_destroy(arena);
        return;
    }

    i8 = jog_type_builtin(types, JOG_BUILTIN_I8);
    i32 = jog_type_builtin(types, JOG_BUILTIN_I32);
    i64 = jog_type_builtin(types, JOG_BUILTIN_I64);
    u8 = jog_type_builtin(types, JOG_BUILTIN_U8);
    u16 = jog_type_builtin(types, JOG_BUILTIN_U16);
    u32 = jog_type_builtin(types, JOG_BUILTIN_U32);
    u64 = jog_type_builtin(types, JOG_BUILTIN_U64);
    untyped = jog_type_builtin(types, JOG_BUILTIN_UNTYPED_INT);

    CHECK(!jog_const_eval(NULL, diag, types, lit(arena, pos, "1"), NULL, &folded));
    CHECK(!jog_const_eval(arena, NULL, types, lit(arena, pos, "1"), NULL, &folded));
    CHECK(!jog_const_eval(arena, diag, NULL, lit(arena, pos, "1"), NULL, &folded));
    CHECK(!jog_const_eval(arena, diag, types, NULL, NULL, &folded));
    CHECK(!jog_const_eval(arena, diag, types, lit(arena, pos, "1"), NULL, NULL));
    CHECK(ct_diag_error_count(diag) == 0);

    expect_int(arena, diag, types, lit(arena, pos, "42"), NULL, i64, 42, &errors);
    expect_int(arena, diag, types, lit(arena, pos, "1_000_000"), NULL, i64, 1000000, &errors);
    expect_int(arena, diag, types, lit(arena, pos, "0b1010"), NULL, i64, 10, &errors);
    expect_int(arena, diag, types, lit(arena, pos, "0b1111_0000"), NULL, i64, 240, &errors);
    expect_int(arena, diag, types, lit(arena, pos, "0o755"), NULL, i64, 493, &errors);
    expect_int(arena, diag, types, lit(arena, pos, "0xBEEF"), NULL, i64, 0xBEEF, &errors);
    expect_int(arena, diag, types, lit(arena, pos, "0Xff"), NULL, i64, 255, &errors);
    expect_int(arena, diag, types, lit(arena, pos, "0xFF_00"), u16, u16, 0xFF00, &errors);
    expect_int(arena, diag, types, lit(arena, pos, "0xDEADBEEF"), u32, u32, 0xDEADBEEFu, &errors);
    expect_int(arena, diag, types, lit(arena, pos, "0xFFFFFFFF"), u32, u32, 0xFFFFFFFFu, &errors);
    expect_int(arena, diag, types, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "1")), i32, i32, ~(uint64_t)0, &errors);
    expect_int(arena, diag, types, lit(arena, pos, "18446744073709551615"), u64, u64, ~(uint64_t)0, &errors);
    expect_int(arena, diag, types, lit(arena, pos, "0x8000000000000000"), u64, u64, UINT64_C(1) << 63, &errors);
    expect_int(arena, diag, types, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "9223372036854775808")), NULL, i64,
               (uint64_t)INT64_MIN, &errors);
    expect_int(arena, diag, types, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "128")), i8, i8, (uint64_t)(int64_t)-128,
               &errors);
    expect_int(arena, diag, types, una(arena, pos, JOG_TOK_MINUS, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "5"))), NULL,
               i64, 5, &errors);

    sum = bin(arena, pos, JOG_TOK_PLUS, lit(arena, pos, "1"), lit(arena, pos, "2"));
    expect_int(arena, diag, types, sum, NULL, i64, 3, &errors);
    CHECK(sum && sum->as.binary.lhs->type == untyped && sum->as.binary.rhs->type == untyped);

    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_PLUS,
                   bin(arena, pos, JOG_TOK_STAR, bin(arena, pos, JOG_TOK_PLUS, lit(arena, pos, "1"), lit(arena, pos, "2")),
                       lit(arena, pos, "3")),
                   lit(arena, pos, "4")),
               NULL, i64, 13, &errors);
    expect_int(arena, diag, types, bin(arena, pos, JOG_TOK_PLUS, lit(arena, pos, "200"), lit(arena, pos, "50")), u8, u8, 250,
               &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_MINUS, bin(arena, pos, JOG_TOK_PLUS, lit(arena, pos, "200"), lit(arena, pos, "100")),
                   lit(arena, pos, "50")),
               u8, u8, 250, &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_MINUS, bin(arena, pos, JOG_TOK_PLUS, lit(arena, pos, "100"), lit(arena, pos, "100")),
                   lit(arena, pos, "150")),
               i8, i8, 50, &errors);
    expect_int(arena, diag, types, bin(arena, pos, JOG_TOK_STAR, lit(arena, pos, "100000"), lit(arena, pos, "100000")), NULL,
               i64, 10000000000ull, &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_STAR, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "5")),
                   una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "3"))),
               NULL, i64, 15, &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_STAR, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "5")), lit(arena, pos, "3")), NULL,
               i64, (uint64_t)(int64_t)-15, &errors);

    expect_int(arena, diag, types, bin(arena, pos, JOG_TOK_SLASH, lit(arena, pos, "7"), lit(arena, pos, "3")), NULL, i64, 2,
               &errors);
    expect_int(arena, diag, types, bin(arena, pos, JOG_TOK_PERCENT, lit(arena, pos, "7"), lit(arena, pos, "3")), NULL, i64, 1,
               &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_SLASH, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "8")), lit(arena, pos, "3")), NULL,
               i64, (uint64_t)(int64_t)-2, &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_PERCENT, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "8")), lit(arena, pos, "3")),
               NULL, i64, (uint64_t)(int64_t)-2, &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_SLASH, lit(arena, pos, "8"), una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "3"))), NULL,
               i64, (uint64_t)(int64_t)-2, &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_PERCENT, lit(arena, pos, "8"), una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "3"))),
               NULL, i64, 2, &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_SLASH, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "8")),
                   una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "3"))),
               NULL, i64, 2, &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_PERCENT, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "8")),
                   una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "3"))),
               NULL, i64, (uint64_t)(int64_t)-2, &errors);

    expect_int(arena, diag, types, bin(arena, pos, JOG_TOK_SHL, lit(arena, pos, "1"), lit(arena, pos, "4")), NULL, i64, 16,
               &errors);
    expect_int(arena, diag, types, bin(arena, pos, JOG_TOK_SHL, lit(arena, pos, "1"), lit(arena, pos, "63")), u64, u64,
               UINT64_C(1) << 63, &errors);
    expect_int(arena, diag, types, bin(arena, pos, JOG_TOK_SHL, lit(arena, pos, "0"), lit(arena, pos, "8")), u8, u8, 0,
               &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_SHR, bin(arena, pos, JOG_TOK_SHL, lit(arena, pos, "1"), lit(arena, pos, "100")),
                   lit(arena, pos, "100")),
               NULL, i64, 1, &errors);
    expect_int(arena, diag, types, bin(arena, pos, JOG_TOK_SHR, lit(arena, pos, "5"), lit(arena, pos, "1")), NULL, i64, 2,
               &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_SHR, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "5")), lit(arena, pos, "1")), NULL,
               i64, (uint64_t)(int64_t)-3, &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_SHR, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "1")), lit(arena, pos, "100")), NULL,
               i64, ~(uint64_t)0, &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_SHL, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "3")), lit(arena, pos, "2")), NULL,
               i64, (uint64_t)(int64_t)-12, &errors);

    expect_int(arena, diag, types, bin(arena, pos, JOG_TOK_AMP, lit(arena, pos, "5"), lit(arena, pos, "3")), NULL, i64, 1,
               &errors);
    expect_int(arena, diag, types, bin(arena, pos, JOG_TOK_PIPE, lit(arena, pos, "5"), lit(arena, pos, "2")), NULL, i64, 7,
               &errors);
    expect_int(arena, diag, types, bin(arena, pos, JOG_TOK_CARET, lit(arena, pos, "5"), lit(arena, pos, "1")), NULL, i64, 4,
               &errors);
    expect_int(arena, diag, types, una(arena, pos, JOG_TOK_TILDE, lit(arena, pos, "0")), NULL, i64, ~(uint64_t)0, &errors);
    expect_int(arena, diag, types, una(arena, pos, JOG_TOK_TILDE, lit(arena, pos, "5")), NULL, i64, (uint64_t)(int64_t)-6,
               &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_AMP, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "1")), lit(arena, pos, "5")), NULL,
               i64, 5, &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_AMP, lit(arena, pos, "5"), una(arena, pos, JOG_TOK_TILDE, lit(arena, pos, "1"))), NULL,
               i64, 4, &errors);
    expect_int(arena, diag, types,
               bin(arena, pos, JOG_TOK_AMP, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "2")), lit(arena, pos, "5")), NULL,
               i64, 4, &errors);

    wide_l = lit(arena, pos, "1");
    wide_r = lit(arena, pos, "2");
    wide_l->type = u8;
    wide_r->type = u8;
    expect_int(arena, diag, types, bin(arena, pos, JOG_TOK_PLUS, wide_l, wide_r), NULL, u8, 3, &errors);
    wide_l = lit(arena, pos, "1");
    wide_l->type = u8;
    expect_int(arena, diag, types, bin(arena, pos, JOG_TOK_PLUS, wide_l, lit(arena, pos, "2")), NULL, u8, 3, &errors);

    expect_bool(arena, diag, types, boolean(arena, pos, true), true, &errors);
    expect_bool(arena, diag, types, boolean(arena, pos, false), false, &errors);
    expect_bool(arena, diag, types, una(arena, pos, JOG_TOK_BANG, boolean(arena, pos, true)), false, &errors);
    expect_bool(arena, diag, types, una(arena, pos, JOG_TOK_BANG, boolean(arena, pos, false)), true, &errors);
    expect_bool(arena, diag, types, bin(arena, pos, JOG_TOK_AMPAMP, boolean(arena, pos, true), boolean(arena, pos, false)),
                false, &errors);
    expect_bool(arena, diag, types, bin(arena, pos, JOG_TOK_PIPEPIPE, boolean(arena, pos, false), boolean(arena, pos, true)),
                true, &errors);
    expect_bool(arena, diag, types, bin(arena, pos, JOG_TOK_LT, lit(arena, pos, "1"), lit(arena, pos, "2")), true, &errors);
    expect_bool(arena, diag, types,
                bin(arena, pos, JOG_TOK_LT, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "1")), lit(arena, pos, "1")), true,
                &errors);
    expect_bool(arena, diag, types, bin(arena, pos, JOG_TOK_EQ, lit(arena, pos, "5"), lit(arena, pos, "5")), true, &errors);
    expect_bool(arena, diag, types, bin(arena, pos, JOG_TOK_NEQ, lit(arena, pos, "5"), lit(arena, pos, "4")), true, &errors);
    expect_bool(arena, diag, types, bin(arena, pos, JOG_TOK_GE, lit(arena, pos, "3"), lit(arena, pos, "4")), false, &errors);
    expect_bool(arena, diag, types,
                bin(arena, pos, JOG_TOK_LT, lit(arena, pos, "9223372036854775808"), lit(arena, pos, "9223372036854775809")),
                true, &errors);
    expect_bool(arena, diag, types, bin(arena, pos, JOG_TOK_EQ, boolean(arena, pos, true), boolean(arena, pos, false)), false,
                &errors);
    expect_bool(arena, diag, types,
                bin(arena, pos, JOG_TOK_AMPAMP, boolean(arena, pos, false),
                    bin(arena, pos, JOG_TOK_SLASH, lit(arena, pos, "1"), lit(arena, pos, "0"))),
                false, &errors);
    expect_bool(arena, diag, types,
                bin(arena, pos, JOG_TOK_PIPEPIPE, boolean(arena, pos, true),
                    bin(arena, pos, JOG_TOK_SLASH, lit(arena, pos, "1"), lit(arena, pos, "0"))),
                true, &errors);
    CHECK(ct_diag_error_count(diag) == errors);

    bytes_lit = lit(arena, pos, "0xDEADBEEF");
    memset(&folded, 0, sizeof(folded));
    CHECK(jog_const_eval(arena, diag, types, bytes_lit, jog_type_bytes_n(types, 4), &folded));
    CHECK(folded.kind == JOG_CONST_BYTES);
    CHECK(folded.type == jog_type_bytes_n(types, 4));
    CHECK(folded.len == 4);
    CHECK(folded.bytes && memcmp(folded.bytes, deadbeef, 4) == 0);
    CHECK(bytes_lit->type == folded.type);
    bytes_lit = lit(arena, pos, "0xFF_00");
    memset(&folded, 0, sizeof(folded));
    CHECK(jog_const_eval(arena, diag, types, bytes_lit, jog_type_bytes_n(types, 2), &folded));
    CHECK(folded.len == 2 && folded.bytes && memcmp(folded.bytes, ff00, 2) == 0);
    bytes_lit = lit(arena, pos, "0xdeadbeef");
    memset(&folded, 0, sizeof(folded));
    CHECK(jog_const_eval(arena, diag, types, bytes_lit, jog_type_bytes_n(types, 4), &folded));
    CHECK(folded.bytes && memcmp(folded.bytes, deadbeef, 4) == 0);
    CHECK(ct_diag_error_count(diag) == errors);

    expect_err(arena, diag, types, lit(arena, pos, "0xDEADBEEF"), i32, "integer constant does not fit", &errors);
    expect_err(arena, diag, types, lit(arena, pos, "9223372036854775808"), NULL, "integer constant does not fit", &errors);
    expect_err(arena, diag, types, lit(arena, pos, "128"), i8, "integer constant does not fit", &errors);
    expect_err(arena, diag, types, lit(arena, pos, "256"), u8, "integer constant does not fit", &errors);
    expect_err(arena, diag, types, lit(arena, pos, "0x8000000000000000"), NULL, "integer constant does not fit", &errors);
    expect_err(arena, diag, types, bin(arena, pos, JOG_TOK_PLUS, lit(arena, pos, "200"), lit(arena, pos, "100")), u8,
               "integer overflow", &errors);
    expect_err(arena, diag, types, bin(arena, pos, JOG_TOK_PLUS, lit(arena, pos, "9223372036854775807"), lit(arena, pos, "1")),
               NULL, "integer overflow", &errors);
    expect_err(arena, diag, types,
               bin(arena, pos, JOG_TOK_PLUS, lit(arena, pos, "18446744073709551615"), lit(arena, pos, "1")), u64,
               "integer overflow", &errors);
    expect_err(arena, diag, types, bin(arena, pos, JOG_TOK_SHL, lit(arena, pos, "1"), lit(arena, pos, "63")), NULL,
               "integer overflow", &errors);
    expect_err(arena, diag, types, bin(arena, pos, JOG_TOK_SHL, lit(arena, pos, "1"), lit(arena, pos, "8")), u8,
               "integer overflow", &errors);
    expect_err(arena, diag, types, una(arena, pos, JOG_TOK_TILDE, lit(arena, pos, "0")), u8, "integer overflow", &errors);
    expect_err(arena, diag, types,
               bin(arena, pos, JOG_TOK_SLASH, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "9223372036854775808")),
                   una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "1"))),
               NULL, "integer overflow", &errors);
    expect_err(arena, diag, types,
               bin(arena, pos, JOG_TOK_SLASH, una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "128")),
                   una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "1"))),
               i8, "integer overflow", &errors);
    expect_err(arena, diag, types, bin(arena, pos, JOG_TOK_SLASH, lit(arena, pos, "1"), lit(arena, pos, "0")), NULL,
               "division by zero", &errors);
    expect_err(arena, diag, types, bin(arena, pos, JOG_TOK_PERCENT, lit(arena, pos, "1"), lit(arena, pos, "0")), NULL,
               "division by zero", &errors);
    expect_err(arena, diag, types,
               bin(arena, pos, JOG_TOK_PLUS, bin(arena, pos, JOG_TOK_SLASH, lit(arena, pos, "1"), lit(arena, pos, "0")),
                   lit(arena, pos, "1")),
               NULL, "division by zero", &errors);
    CHECK(errors == ct_diag_error_count(diag));
    expect_err(arena, diag, types,
               bin(arena, pos, JOG_TOK_SHL, lit(arena, pos, "1"), una(arena, pos, JOG_TOK_MINUS, lit(arena, pos, "1"))), NULL,
               "shift count is negative", &errors);
    expect_err(arena, diag, types, lit(arena, pos, "0b2"), NULL, "invalid integer literal", &errors);
    expect_err(arena, diag, types, lit(arena, pos, "0x"), NULL, "invalid integer literal", &errors);
    expect_err(arena, diag, types, lit(arena, pos, "12a"), NULL, "invalid integer literal", &errors);
    expect_err(arena, diag, types, una(arena, pos, JOG_TOK_AMP, lit(arena, pos, "1")), NULL, "not a constant expression",
               &errors);
    expect_err(arena, diag, types, una(arena, pos, JOG_TOK_STAR, lit(arena, pos, "1")), NULL, "not a constant expression",
               &errors);
    expect_err(arena, diag, types, bin(arena, pos, JOG_TOK_CMP3, lit(arena, pos, "1"), lit(arena, pos, "2")), NULL,
               "not a constant expression", &errors);
    expect_err(arena, diag, types, mk(arena, JOG_NODE_FLOAT_LIT, pos), NULL, "not a constant expression", &errors);
    expect_err(arena, diag, types, boolean(arena, pos, true), i64, "constant does not fit", &errors);
    expect_err(arena, diag, types, lit(arena, pos, "1"), jog_type_builtin(types, JOG_BUILTIN_BOOL), "constant does not fit",
               &errors);
    expect_err(arena, diag, types, lit(arena, pos, "255"), jog_type_bytes_n(types, 1), "hex literal required for bytes[N]",
               &errors);
    expect_err(arena, diag, types, lit(arena, pos, "0xBEEF"), jog_type_bytes_n(types, 4),
               "hex literal has 4 digits, bytes[4] needs 8", &errors);
    expect_err(arena, diag, types, bin(arena, pos, JOG_TOK_PLUS, boolean(arena, pos, true), lit(arena, pos, "1")), NULL,
               "operator '+' expects an integer", &errors);
    expect_err(arena, diag, types, una(arena, pos, JOG_TOK_BANG, lit(arena, pos, "1")), NULL, "operator '!' expects a bool",
               &errors);
    expect_err(arena, diag, types,
               bin(arena, pos, JOG_TOK_AMPAMP, boolean(arena, pos, true),
                   bin(arena, pos, JOG_TOK_SLASH, lit(arena, pos, "1"), lit(arena, pos, "0"))),
               NULL, "operator '&&' expects a bool", &errors);
    wide_l = lit(arena, pos, "1");
    wide_r = lit(arena, pos, "2");
    wide_l->type = u8;
    wide_r->type = i32;
    expect_err(arena, diag, types, bin(arena, pos, JOG_TOK_PLUS, wide_l, wide_r), NULL, "integer types do not match", &errors);
    wide_l = lit(arena, pos, "1");
    wide_r = lit(arena, pos, "2");
    wide_l->type = u8;
    wide_r->type = u8;
    expect_err(arena, diag, types, bin(arena, pos, JOG_TOK_PLUS, wide_l, wide_r), i64, "integer types do not match", &errors);

    {
        CtArena *narrow = ct_arena_create();
        JogTypes *types32;
        CtDiag *diag32;
        JogPos at = {"w.jog", 2, 3};
        char text[256];
        CHECK(narrow != NULL);
        if (narrow) {
            types32 = jog_types_create(narrow, 4);
            diag32 = ct_diag_create(narrow);
            CHECK(types32 != NULL && diag32 != NULL);
            if (types32 && diag32) {
                JogNode *too_wide = lit(narrow, at, "0x100000000");
                JogConst value;
                memset(&value, 0, sizeof(value));
                CHECK(!jog_const_eval(narrow, diag32, types32, too_wide, jog_type_builtin(types32, JOG_BUILTIN_ISIZE),
                                      &value));
                CHECK(jog_const_eval(narrow, diag32, types32, lit(narrow, at, "0x7FFFFFFF"),
                                     jog_type_builtin(types32, JOG_BUILTIN_ISIZE), &value));
                CHECK(value.bits == 0x7FFFFFFF);
                CHECK(value.type == jog_type_builtin(types32, JOG_BUILTIN_ISIZE));
                memset(&value, 0, sizeof(value));
                CHECK(jog_const_eval(narrow, diag32, types32, lit(narrow, at, "0xFFFFFFFF"),
                                     jog_type_builtin(types32, JOG_BUILTIN_USIZE), &value));
                CHECK(value.bits == 0xFFFFFFFFu);
                diag_text(diag32, text, sizeof(text));
                CHECK(strstr(text, "w.jog:2:3: error: integer constant does not fit") != NULL);
            }
            jog_types_release(types32);
            ct_arena_destroy(narrow);
        }
    }

    jog_types_release(types);
    ct_arena_destroy(arena);
}

static JogList nodes_of(CtArena *arena, JogNode **nodes, size_t count) {
    CtDa da;
    JogList list;
    size_t i;
    bool oom = false;
    memset(&list, 0, sizeof(list));
    if (count == 0) {
        return list;
    }
    if (!ct_da_init(&da, sizeof(JogNode *))) {
        CHECK(0);
        return list;
    }
    for (i = 0; i < count; i++) {
        if (!ct_da_push(&da, &nodes[i])) {
            oom = true;
            break;
        }
    }
    if (!oom) {
        list = jog_list_from_da(arena, &da, &oom);
    }
    ct_da_free(&da);
    CHECK(!oom);
    return list;
}

static JogNames names_of(CtArena *arena, const char **items, size_t count) {
    CtDa da;
    JogNames names;
    size_t i;
    bool oom = false;
    memset(&names, 0, sizeof(names));
    if (count == 0) {
        return names;
    }
    if (!ct_da_init(&da, sizeof(const char *))) {
        CHECK(0);
        return names;
    }
    for (i = 0; i < count; i++) {
        if (!ct_da_push(&da, &items[i])) {
            oom = true;
            break;
        }
    }
    if (!oom) {
        names = jog_names_from_da(arena, &da, &oom);
    }
    ct_da_free(&da);
    CHECK(!oom);
    return names;
}

static const char *iname(CtIntern *intern, const char *text) {
    const char *name = ct_intern_cstr(intern, text);
    CHECK(name != NULL);
    return name;
}

static JogPos at_file(const char *file, uint32_t line) {
    JogPos pos;
    pos.file = file;
    pos.line = line;
    pos.column = 1;
    return pos;
}

static JogNode *import_of(CtArena *arena, CtIntern *intern, JogPos pos, const char *a, const char *b, const char *alias) {
    const char *segs[2];
    size_t count = 1;
    JogNode *node = mk(arena, JOG_NODE_IMPORT, pos);
    if (!node) {
        return NULL;
    }
    segs[0] = iname(intern, a);
    if (b) {
        segs[1] = iname(intern, b);
        count = 2;
    }
    node->as.import.path = names_of(arena, segs, count);
    node->as.import.alias = alias ? iname(intern, alias) : NULL;
    return node;
}

static JogNode *fn_of(CtArena *arena, CtIntern *intern, JogPos pos, const char *name, JogVisibility vis, bool method) {
    JogNode *node = mk(arena, JOG_NODE_FN, pos);
    if (!node) {
        return NULL;
    }
    node->visibility = vis;
    node->as.fn.name = iname(intern, name);
    if (method) {
        node->as.fn.receiver = mk(arena, JOG_NODE_TYPE_NAME, pos);
    }
    return node;
}

static JogNode *bind_of(CtArena *arena, CtIntern *intern, JogNodeKind kind, JogPos pos, JogVisibility vis,
                        const char *first, const char *second) {
    const char *items[2];
    size_t count = 1;
    JogNode *node = mk(arena, kind, pos);
    if (!node) {
        return NULL;
    }
    node->visibility = vis;
    items[0] = iname(intern, first);
    if (second) {
        items[1] = iname(intern, second);
        count = 2;
        node->as.binding.destructure = true;
    }
    node->as.binding.names = names_of(arena, items, count);
    return node;
}

static JogNode *type_of(CtArena *arena, CtIntern *intern, JogNodeKind kind, JogPos pos, const char *name, JogVisibility vis) {
    JogNode *node = mk(arena, kind, pos);
    if (!node) {
        return NULL;
    }
    node->visibility = vis;
    if (kind == JOG_NODE_TYPE_DECL) {
        node->as.type_decl.name = iname(intern, name);
    } else {
        node->as.aggregate.name = iname(intern, name);
    }
    return node;
}

static JogNode *file_of(CtArena *arena, JogPos pos, JogNode **imports, size_t import_count, JogNode **decls,
                        size_t decl_count) {
    JogNode *file = mk(arena, JOG_NODE_FILE, pos);
    if (!file) {
        return NULL;
    }
    file->as.file.imports = nodes_of(arena, imports, import_count);
    file->as.file.decls = nodes_of(arena, decls, decl_count);
    return file;
}

static JogNode *ident_of(CtArena *arena, CtIntern *intern, JogPos pos, const char *name) {
    JogNode *node = mk(arena, JOG_NODE_IDENT, pos);
    if (!node) {
        return NULL;
    }
    node->as.ident.name = iname(intern, name);
    return node;
}

static JogNode *tname(CtArena *arena, CtIntern *intern, JogPos pos, const char *a, const char *b) {
    const char *segs[2];
    size_t count = 1;
    JogNode *node = mk(arena, JOG_NODE_TYPE_NAME, pos);
    if (!node) {
        return NULL;
    }
    segs[0] = iname(intern, a);
    if (b) {
        segs[1] = iname(intern, b);
        count = 2;
    }
    node->as.type_name.path = names_of(arena, segs, count);
    return node;
}

static JogNode *tptr(CtArena *arena, JogPos pos, JogNode *elem, bool is_const) {
    JogNode *node = mk(arena, JOG_NODE_TYPE_PTR, pos);
    if (!node) {
        return NULL;
    }
    node->as.type_elem.elem = elem;
    node->as.type_elem.is_const = is_const;
    return node;
}

static JogNode *param_of(CtArena *arena, CtIntern *intern, JogPos pos, const char *name, JogNode *type) {
    JogNode *node = mk(arena, JOG_NODE_PARAM, pos);
    if (!node) {
        return NULL;
    }
    node->as.param.name = name ? iname(intern, name) : NULL;
    node->as.param.type = type;
    return node;
}

typedef struct Fix {
    CtArena *arena;
    CtDiag *diag;
    CtIntern *intern;
    JogTypes *types;
    JogProgram program;
} Fix;

static int fix_begin(Fix *f) {
    memset(f, 0, sizeof(*f));
    f->arena = ct_arena_create();
    if (!f->arena) {
        return 0;
    }
    f->diag = ct_diag_create(f->arena);
    f->intern = ct_intern_create(f->arena);
    f->types = jog_types_create(f->arena, 8);
    if (!f->diag || !f->intern || !f->types) {
        return 0;
    }
    f->program.arena = f->arena;
    f->program.diag = f->diag;
    f->program.intern = f->intern;
    f->program.types = f->types;
    return 1;
}

static void fix_end(Fix *f) {
    if (!f) {
        return;
    }
    jog_types_release(f->types);
    ct_arena_destroy(f->arena);
}

static void prep_file(JogSourceFile *file, JogNode *ast) {
    memset(file, 0, sizeof(*file));
    file->ast = ast;
}

static void prep_pkg(JogPackage *pkg, const char *path, JogSourceFile **files, size_t count) {
    memset(pkg, 0, sizeof(*pkg));
    pkg->path = path;
    pkg->name = path;
    pkg->files = files;
    pkg->file_count = count;
}

static int run_pkgs(Fix *f, JogPackage **pkgs, size_t count) {
    f->program.packages = pkgs;
    f->program.package_count = count;
    f->program.root = count > 0 ? pkgs[0] : NULL;
    return jog_check(&f->program) ? 1 : 0;
}

static int run_ok(Fix *f, JogPackage **pkgs, size_t count) {
    int ok = run_pkgs(f, pkgs, count);
    char text[8192];
    if (!ok) {
        diag_text(f->diag, text, sizeof(text));
        fprintf(stderr, "%s\n", text);
    }
    return ok;
}

static void expect_diag(Fix *f, const char *fragment) {
    char text[8192];
    diag_text(f->diag, text, sizeof(text));
    if (!fragment || !strstr(text, fragment)) {
        fprintf(stderr, "diagnostic missing '%s'\n%s\n", fragment ? fragment : "(null)", text);
        CHECK(0);
    }
}

static void test_package_scopes(void) {
    CtArena *arena = ct_arena_create();
    CtDiag *diag;
    CtIntern *intern;
    JogTypes *types;
    JogPackageScope *universe;
    JogProgram program;
    JogPackage io;
    JogPackage store;
    JogPackage app;
    JogPackage *pkgs[3];
    JogSourceFile io_file;
    JogSourceFile store_file;
    JogSourceFile app_a;
    JogSourceFile app_b;
    JogSourceFile *io_files[1];
    JogSourceFile *store_files[1];
    JogSourceFile *app_files[2];
    JogPos io_pos;
    JogPos store_pos;
    JogPos a_pos;
    JogPos b_pos;
    JogNode *print_fn;
    JogNode *io_secret;
    JogNode *open_fn;
    JogNode *hidden_fn;
    JogNode *conn;
    JogNode *import_io;
    JogNode *import_store;
    JogNode *main_fn;
    JogNode *helper;
    JogNode *local_val;
    JogNode *scratch;
    JogNode *method;
    JogNode *malloc_fn;
    JogNode *secret_fn;
    JogNode *extern_block;
    JogNode *first;
    JogNode *other;
    JogNode *local_fn;
    JogNode *shadow;
    JogNode *io_decls[2];
    JogNode *store_decls[3];
    JogNode *a_imports[2];
    JogNode *a_decls[8];
    JogNode *b_decls[3];
    JogNode *extern_fns[1];
    JogPackageScope *app_scope;
    JogPackageScope *store_scope;
    JogSymbol *io_sym;
    JogSymbol *db_sym;
    JogSymbol *main_sym;
    JogSymbol *helper_sym;
    JogSymbol *local_a;
    JogSymbol *local_b;
    JogSymbol *scratch_sym;
    JogSymbol *open_sym;
    JogSymbol *hidden_sym;
    JogSymbol *conn_sym;
    JogSymbol *malloc_sym;
    JogSymbol *secret_sym;
    JogSymbol *first_sym;
    JogSymbol *other_sym;
    JogSymbol *shadow_sym;
    JogSymbol *void_sym;
    JogSymbol *i64_sym;

    CHECK(arena != NULL);
    if (!arena) {
        return;
    }
    diag = ct_diag_create(arena);
    intern = ct_intern_create(arena);
    types = jog_types_create(arena, 8);
    CHECK(diag != NULL && intern != NULL && types != NULL);
    if (!diag || !intern || !types) {
        jog_types_release(types);
        ct_arena_destroy(arena);
        return;
    }
    universe = jog_universe_scope(arena, intern, types);
    CHECK(universe != NULL);
    if (!universe) {
        jog_types_release(types);
        ct_arena_destroy(arena);
        return;
    }

    memset(&program, 0, sizeof(program));
    program.arena = arena;
    program.diag = diag;
    program.intern = intern;
    program.types = types;
    CHECK(!jog_resolve_packages(NULL, universe));
    CHECK(jog_resolve_packages(&program, universe));
    CHECK(ct_diag_error_count(diag) == 0);
    CHECK(jog_file_lookup(NULL, 0, "void") == NULL);
    CHECK(jog_file_lookup(universe, 0, "void") == NULL);
    CHECK(!jog_symbol_visible_from(NULL, NULL, NULL));
    CHECK(jog_symbol_visible_from(jog_package_scope_lookup(universe, "void"), NULL, NULL));

    io_pos = at_file("print.jog", 1);
    store_pos = at_file("db.jog", 1);
    a_pos = at_file("a.jog", 1);
    b_pos = at_file("b.jog", 1);

    print_fn = fn_of(arena, intern, io_pos, "print", JOG_VIS_PUBLIC, false);
    io_secret = fn_of(arena, intern, at_file("print.jog", 2), "scratch", JOG_VIS_PACKAGE, false);
    open_fn = fn_of(arena, intern, store_pos, "open", JOG_VIS_PUBLIC, false);
    hidden_fn = fn_of(arena, intern, at_file("db.jog", 2), "hidden", JOG_VIS_PACKAGE, false);
    conn = type_of(arena, intern, JOG_NODE_STRUCT, at_file("db.jog", 3), "Conn", JOG_VIS_PUBLIC);
    import_io = import_of(arena, intern, at_file("a.jog", 1), "std", "io", NULL);
    import_store = import_of(arena, intern, at_file("a.jog", 2), "store", NULL, "db");
    main_fn = fn_of(arena, intern, at_file("a.jog", 4), "main", JOG_VIS_PUBLIC, false);
    helper = fn_of(arena, intern, at_file("a.jog", 5), "helper", JOG_VIS_PACKAGE, false);
    local_val = bind_of(arena, intern, JOG_NODE_VAL, at_file("a.jog", 6), JOG_VIS_FILE, "local", NULL);
    scratch = bind_of(arena, intern, JOG_NODE_VAR, at_file("a.jog", 7), JOG_VIS_SCOPE, "scratch", NULL);
    method = fn_of(arena, intern, at_file("a.jog", 8), "len", JOG_VIS_PUBLIC, true);
    malloc_fn = fn_of(arena, intern, at_file("a.jog", 10), "malloc", JOG_VIS_PUBLIC, false);
    secret_fn = fn_of(arena, intern, at_file("a.jog", 11), "secret", JOG_VIS_PUBLIC, false);
    extern_block = mk(arena, JOG_NODE_EXTERN_BLOCK, at_file("a.jog", 9));
    first = bind_of(arena, intern, JOG_NODE_VAL, at_file("a.jog", 12), JOG_VIS_PUBLIC, "first", "_");
    other = fn_of(arena, intern, at_file("b.jog", 1), "other", JOG_VIS_PUBLIC, false);
    local_fn = fn_of(arena, intern, at_file("b.jog", 2), "local", JOG_VIS_FILE, false);
    shadow = fn_of(arena, intern, at_file("b.jog", 3), "i32", JOG_VIS_PUBLIC, false);
    if (!print_fn || !io_secret || !open_fn || !hidden_fn || !conn || !import_io || !import_store || !main_fn ||
        !helper || !local_val || !scratch || !method || !malloc_fn || !secret_fn || !extern_block || !first || !other ||
        !local_fn || !shadow) {
        jog_types_release(types);
        ct_arena_destroy(arena);
        return;
    }
    extern_fns[0] = malloc_fn;
    extern_block->visibility = JOG_VIS_PUBLIC;
    extern_block->as.extern_block.fns = nodes_of(arena, extern_fns, 1);
    secret_fn->pos = at_file("a.jog", 14);
    {
        JogNode *private_block = mk(arena, JOG_NODE_EXTERN_BLOCK, at_file("a.jog", 13));
        JogNode *only[1];
        if (!private_block) {
            jog_types_release(types);
            ct_arena_destroy(arena);
            return;
        }
        private_block->visibility = JOG_VIS_PACKAGE;
        only[0] = secret_fn;
        private_block->as.extern_block.fns = nodes_of(arena, only, 1);
        secret_fn = private_block;
    }

    io_decls[0] = print_fn;
    io_decls[1] = io_secret;
    store_decls[0] = open_fn;
    store_decls[1] = hidden_fn;
    store_decls[2] = conn;
    a_imports[0] = import_io;
    a_imports[1] = import_store;
    a_decls[0] = main_fn;
    a_decls[1] = helper;
    a_decls[2] = local_val;
    a_decls[3] = scratch;
    a_decls[4] = method;
    a_decls[5] = extern_block;
    a_decls[6] = secret_fn;
    a_decls[7] = first;
    b_decls[0] = other;
    b_decls[1] = local_fn;
    b_decls[2] = shadow;

    memset(&io_file, 0, sizeof(io_file));
    memset(&store_file, 0, sizeof(store_file));
    memset(&app_a, 0, sizeof(app_a));
    memset(&app_b, 0, sizeof(app_b));
    io_file.ast = file_of(arena, io_pos, NULL, 0, io_decls, 2);
    store_file.ast = file_of(arena, store_pos, NULL, 0, store_decls, 3);
    app_a.ast = file_of(arena, a_pos, a_imports, 2, a_decls, 8);
    app_b.ast = file_of(arena, b_pos, NULL, 0, b_decls, 3);
    io_files[0] = &io_file;
    store_files[0] = &store_file;
    app_files[0] = &app_a;
    app_files[1] = &app_b;

    memset(&io, 0, sizeof(io));
    memset(&store, 0, sizeof(store));
    memset(&app, 0, sizeof(app));
    io.path = "std.io";
    io.name = "io";
    io.files = io_files;
    io.file_count = 1;
    store.path = "store";
    store.name = "store";
    store.files = store_files;
    store.file_count = 1;
    app.path = "app";
    app.name = "app";
    app.files = app_files;
    app.file_count = 2;
    pkgs[0] = &io;
    pkgs[1] = &store;
    pkgs[2] = &app;
    program.packages = pkgs;
    program.package_count = 3;

    CHECK(jog_resolve_packages(&program, universe));
    CHECK(ct_diag_error_count(diag) == 0);
    app_scope = app.scope;
    store_scope = store.scope;
    CHECK(app_scope != NULL && store_scope != NULL && io.scope != NULL);
    CHECK(app_scope && app_scope->package == &app && app_scope->universe == universe);
    CHECK(app_scope && app_scope->file_scope_count == 2 && app_scope->file_scopes != NULL);
    if (!app_scope || !store_scope || !io.scope) {
        jog_types_release(types);
        ct_arena_destroy(arena);
        return;
    }

    io_sym = jog_file_lookup(app_scope, 0, "io");
    db_sym = jog_file_lookup(app_scope, 0, "db");
    main_sym = jog_file_lookup(app_scope, 0, "main");
    helper_sym = jog_file_lookup(app_scope, 0, "helper");
    local_a = jog_file_lookup(app_scope, 0, "local");
    scratch_sym = jog_file_lookup(app_scope, 0, "scratch");
    first_sym = jog_file_lookup(app_scope, 0, "first");
    other_sym = jog_file_lookup(app_scope, 0, "other");
    malloc_sym = jog_file_lookup(app_scope, 0, "malloc");
    secret_sym = jog_file_lookup(app_scope, 0, "secret");
    local_b = jog_file_lookup(app_scope, 1, "local");
    shadow_sym = jog_file_lookup(app_scope, 0, "i32");
    void_sym = jog_file_lookup(app_scope, 0, "void");
    i64_sym = jog_file_lookup(app_scope, 1, "i64");
    open_sym = jog_package_scope_lookup(store_scope, "open");
    hidden_sym = jog_package_scope_lookup(store_scope, "hidden");
    conn_sym = jog_package_scope_lookup(store_scope, "Conn");

    CHECK(io_sym && io_sym->kind == JOG_SYM_PACKAGE && io_sym->package == &io && io_sym->decl == import_io);
    CHECK(db_sym && db_sym->kind == JOG_SYM_PACKAGE && db_sym->package == &store && db_sym->decl == import_store);
    CHECK(jog_file_lookup(app_scope, 1, "io") == NULL);
    CHECK(jog_file_lookup(app_scope, 1, "db") == NULL);
    CHECK(jog_package_scope_lookup(app_scope, "io") == NULL);
    CHECK(jog_package_scope_lookup(app_scope, "db") == NULL);
    CHECK(jog_symbol_visible_from(io_sym, &app, "a.jog"));
    CHECK(!jog_symbol_visible_from(io_sym, &app, "b.jog"));
    CHECK(!jog_symbol_visible_from(db_sym, &store, "db.jog"));

    CHECK(main_sym && main_sym->kind == JOG_SYM_FUNCTION && main_sym->decl == main_fn && main_sym->package == &app);
    CHECK(main_sym && main_sym->type == NULL && main_fn->symbol == main_sym);
    CHECK(jog_package_scope_lookup(app_scope, "main") == main_sym);
    CHECK(jog_symbol_visible_from(main_sym, &store, "db.jog"));
    CHECK(helper_sym && helper_sym->kind == JOG_SYM_FUNCTION && helper_sym->decl == helper);
    CHECK(jog_package_scope_lookup(app_scope, "helper") == helper_sym);
    CHECK(jog_file_lookup(app_scope, 1, "helper") == helper_sym);
    CHECK(jog_symbol_visible_from(helper_sym, &app, "b.jog"));
    CHECK(!jog_symbol_visible_from(helper_sym, &store, "db.jog"));

    CHECK(local_a && local_a->kind == JOG_SYM_CONSTANT && local_a->decl == local_val);
    CHECK(jog_package_scope_lookup(app_scope, "local") == NULL);
    CHECK(local_b && local_b->kind == JOG_SYM_FUNCTION && local_b->decl == local_fn && local_b != local_a);
    CHECK(jog_symbol_visible_from(local_a, &app, "a.jog"));
    CHECK(!jog_symbol_visible_from(local_a, &app, "b.jog"));
    CHECK(!jog_symbol_visible_from(local_b, &app, "a.jog"));
    CHECK(scratch_sym && scratch_sym->kind == JOG_SYM_VARIABLE && scratch_sym->decl == scratch);
    CHECK(jog_package_scope_lookup(app_scope, "scratch") == NULL);
    CHECK(jog_file_lookup(app_scope, 1, "scratch") == NULL);
    CHECK(jog_symbol_visible_from(scratch_sym, &app, "a.jog"));
    CHECK(!jog_symbol_visible_from(scratch_sym, &app, "b.jog"));

    CHECK(first_sym && first_sym->kind == JOG_SYM_CONSTANT && first_sym->decl == first && first->symbol == first_sym);
    CHECK(jog_file_lookup(app_scope, 0, "_") == NULL);
    CHECK(jog_package_scope_lookup(app_scope, "len") == NULL);
    CHECK(jog_file_lookup(app_scope, 0, "len") == NULL);
    CHECK(method->symbol && method->symbol->kind == JOG_SYM_METHOD && method->symbol->decl == method);
    CHECK(other_sym && other_sym->decl == other && jog_file_lookup(app_scope, 0, "other") == other_sym);

    CHECK(malloc_sym && malloc_sym->kind == JOG_SYM_FUNCTION && malloc_sym->decl == malloc_fn);
    CHECK(malloc_fn->visibility == JOG_VIS_PUBLIC);
    CHECK(jog_symbol_visible_from(malloc_sym, &store, "db.jog"));
    CHECK(secret_sym && secret_sym->kind == JOG_SYM_FUNCTION);
    CHECK(secret_sym && secret_sym->decl && secret_sym->decl->visibility == JOG_VIS_PACKAGE);
    CHECK(jog_package_scope_lookup(app_scope, "secret") == secret_sym);
    CHECK(jog_symbol_visible_from(secret_sym, &app, "b.jog"));
    CHECK(!jog_symbol_visible_from(secret_sym, &store, "db.jog"));

    CHECK(open_sym && open_sym->kind == JOG_SYM_FUNCTION && open_sym->decl == open_fn);
    CHECK(jog_symbol_visible_from(open_sym, &app, "a.jog"));
    CHECK(hidden_sym && hidden_sym->decl == hidden_fn);
    CHECK(jog_symbol_visible_from(hidden_sym, &store, "db.jog"));
    CHECK(!jog_symbol_visible_from(hidden_sym, &app, "a.jog"));
    CHECK(jog_file_lookup(app_scope, 0, "hidden") == NULL);
    CHECK(jog_file_lookup(app_scope, 0, "open") == NULL);
    CHECK(conn_sym && conn_sym->kind == JOG_SYM_TYPE && conn_sym->decl == conn);
    CHECK(jog_package_scope_lookup((JogPackageScope *)io.scope, "print") != NULL);
    CHECK(jog_package_scope_lookup((JogPackageScope *)io.scope, "scratch") != NULL);
    CHECK(!jog_symbol_visible_from(jog_package_scope_lookup((JogPackageScope *)io.scope, "scratch"), &app, "a.jog"));

    CHECK(shadow_sym && shadow_sym->kind == JOG_SYM_FUNCTION && shadow_sym->decl == shadow);
    CHECK(shadow_sym != jog_package_scope_lookup(universe, "i32"));
    CHECK(jog_block_scope_lookup(app_scope->file_scopes[0], "i64") == NULL);
    CHECK(void_sym == jog_package_scope_lookup(universe, "void"));
    CHECK(void_sym && void_sym->kind == JOG_SYM_TYPE && void_sym->type != jog_type_builtin(types, JOG_BUILTIN_UNIT));
    CHECK(i64_sym == jog_package_scope_lookup(universe, "i64"));
    CHECK(jog_file_lookup(app_scope, 0, "missing") == NULL);
    CHECK(jog_file_lookup(app_scope, 2, "main") == NULL);

    jog_types_release(types);
    ct_arena_destroy(arena);
}

static void test_package_scope_errors(void) {
    CtArena *arena = ct_arena_create();
    CtDiag *diag;
    CtIntern *intern;
    JogTypes *types;
    JogPackageScope *universe;
    JogProgram program;
    JogPackage app;
    JogPackage io;
    JogPackage other;
    JogPackage *pkgs[3];
    JogSourceFile app_file;
    JogSourceFile *app_files[1];
    JogNode *missing;
    JogNode *self_import;
    JogNode *import_io;
    JogNode *import_other;
    JogNode *main_a;
    JogNode *main_b;
    JogNode *exposed;
    JogNode *exposed_file;
    JogNode *imports[4];
    JogNode *decls[4];
    char text[4096];

    CHECK(arena != NULL);
    if (!arena) {
        return;
    }
    diag = ct_diag_create(arena);
    intern = ct_intern_create(arena);
    types = jog_types_create(arena, 8);
    universe = jog_universe_scope(arena, intern, types);
    CHECK(diag && intern && types && universe);
    if (!diag || !intern || !types || !universe) {
        jog_types_release(types);
        ct_arena_destroy(arena);
        return;
    }

    missing = import_of(arena, intern, at_file("a.jog", 1), "gone", "pkg", NULL);
    self_import = import_of(arena, intern, at_file("a.jog", 2), "app", NULL, NULL);
    import_io = import_of(arena, intern, at_file("a.jog", 3), "std", "io", NULL);
    import_other = import_of(arena, intern, at_file("a.jog", 4), "other", "io", NULL);
    main_a = fn_of(arena, intern, at_file("a.jog", 5), "main", JOG_VIS_PUBLIC, false);
    main_b = fn_of(arena, intern, at_file("a.jog", 6), "main", JOG_VIS_PUBLIC, false);
    exposed = fn_of(arena, intern, at_file("a.jog", 7), "exposed", JOG_VIS_PUBLIC, false);
    exposed_file = fn_of(arena, intern, at_file("a.jog", 8), "exposed", JOG_VIS_FILE, false);
    if (!missing || !self_import || !import_io || !import_other || !main_a || !main_b || !exposed || !exposed_file) {
        jog_types_release(types);
        ct_arena_destroy(arena);
        return;
    }
    imports[0] = missing;
    imports[1] = self_import;
    imports[2] = import_io;
    imports[3] = import_other;
    decls[0] = main_a;
    decls[1] = main_b;
    decls[2] = exposed;
    decls[3] = exposed_file;

    memset(&app_file, 0, sizeof(app_file));
    app_file.ast = file_of(arena, at_file("a.jog", 1), imports, 4, decls, 4);
    app_files[0] = &app_file;
    memset(&app, 0, sizeof(app));
    memset(&io, 0, sizeof(io));
    memset(&other, 0, sizeof(other));
    app.path = "app";
    app.name = "app";
    app.files = app_files;
    app.file_count = 1;
    io.path = "std.io";
    io.name = "io";
    other.path = "other.io";
    other.name = "io";
    pkgs[0] = &io;
    pkgs[1] = &other;
    pkgs[2] = &app;
    memset(&program, 0, sizeof(program));
    program.arena = arena;
    program.diag = diag;
    program.intern = intern;
    program.types = types;
    program.packages = pkgs;
    program.package_count = 3;

    CHECK(!jog_resolve_packages(&program, universe));
    CHECK(ct_diag_error_count(diag) == 5);
    diag_text(diag, text, sizeof(text));
    CHECK(strstr(text, "a.jog:1:1: error: cannot find package 'gone.pkg'") != NULL);
    CHECK(strstr(text, "a.jog:2:1: error: a package cannot import itself") != NULL);
    CHECK(strstr(text, "a.jog:4:1: error: duplicate declaration of 'io'") != NULL);
    CHECK(strstr(text, "a.jog:6:1: error: duplicate declaration of 'main'") != NULL);
    CHECK(strstr(text, "a.jog:8:1: error: duplicate declaration of 'exposed'") != NULL);
    CHECK(strstr(text, "note: previous declaration is here") != NULL);

    jog_types_release(types);
    ct_arena_destroy(arena);
}

static JogType *builtin(const Fix *f, JogBuiltin which) {
    return jog_type_builtin(f->types, which);
}

static void test_demand_order(void) {
    Fix f;
    JogPackage pkg;
    JogSourceFile file;
    JogSourceFile *files[1];
    JogPackage *pkgs[1];
    JogPos p1 = at_file("a.jog", 1);
    JogPos p2 = at_file("a.jog", 2);
    JogPos p3 = at_file("a.jog", 3);
    JogPos p4 = at_file("a.jog", 4);
    JogPos p5 = at_file("a.jog", 5);
    JogPos p6 = at_file("a.jog", 6);
    JogPos p7 = at_file("a.jog", 7);
    JogPos p8 = at_file("a.jog", 8);
    JogPos p9 = at_file("a.jog", 9);
    JogPos p10 = at_file("a.jog", 10);
    JogNode *alias_a;
    JogNode *alias_b;
    JogNode *alias_c;
    JogNode *fn;
    JogNode *param;
    JogNode *params[1];
    JogNode *point;
    JogNode *ptr_point;
    JogNode *ptr_void;
    JogNode *celsius;
    JogNode *arr;
    JogNode *slice;
    JogNode *opt;
    JogNode *tuple;
    JogNode *tuple_elems[2];
    JogNode *decls[10];
    JogType *i64;
    JogType *i32;
    JogType *boolean;
    JogType *f64;

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    alias_a = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p1, "A", JOG_VIS_PUBLIC);
    alias_b = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p2, "B", JOG_VIS_PUBLIC);
    alias_c = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p3, "C", JOG_VIS_PUBLIC);
    fn = fn_of(f.arena, f.intern, p4, "f", JOG_VIS_PUBLIC, false);
    param = param_of(f.arena, f.intern, p4, "x", tname(f.arena, f.intern, p4, "A", NULL));
    point = type_of(f.arena, f.intern, JOG_NODE_STRUCT, p5, "Point", JOG_VIS_PUBLIC);
    ptr_point = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p6, "Ptr", JOG_VIS_PUBLIC);
    ptr_void = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p7, "Raw", JOG_VIS_PUBLIC);
    celsius = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p8, "Celsius", JOG_VIS_PUBLIC);
    arr = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p9, "Quad", JOG_VIS_PUBLIC);
    slice = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p10, "Nums", JOG_VIS_PUBLIC);
    if (!alias_a || !alias_b || !alias_c || !fn || !param || !point || !ptr_point || !ptr_void || !celsius || !arr ||
        !slice) {
        fix_end(&f);
        return;
    }
    alias_a->as.type_decl.target = tname(f.arena, f.intern, p1, "B", NULL);
    alias_b->as.type_decl.target = tname(f.arena, f.intern, p2, "C", NULL);
    alias_c->as.type_decl.target = tname(f.arena, f.intern, p3, "i64", NULL);
    params[0] = param;
    fn->as.fn.params = nodes_of(f.arena, params, 1);
    fn->as.fn.ret = tname(f.arena, f.intern, p4, "A", NULL);
    ptr_point->as.type_decl.target = tptr(f.arena, p6, tname(f.arena, f.intern, p6, "Point", NULL), false);
    ptr_void->as.type_decl.target = tptr(f.arena, p7, tname(f.arena, f.intern, p7, "void", NULL), true);
    celsius->as.type_decl.distinct = true;
    celsius->as.type_decl.target = tname(f.arena, f.intern, p8, "f64", NULL);
    arr->as.type_decl.target = mk(f.arena, JOG_NODE_TYPE_ARRAY, p9);
    if (arr->as.type_decl.target) {
        arr->as.type_decl.target->as.type_array.len = lit(f.arena, p9, "4");
        arr->as.type_decl.target->as.type_array.elem = tname(f.arena, f.intern, p9, "i32", NULL);
    }
    slice->as.type_decl.target = mk(f.arena, JOG_NODE_TYPE_SLICE, p10);
    if (slice->as.type_decl.target) {
        slice->as.type_decl.target->as.type_elem.elem = tname(f.arena, f.intern, p10, "i32", NULL);
    }
    /* Dependents are declared before the names they use. */
    decls[0] = alias_a;
    decls[1] = alias_b;
    decls[2] = fn;
    decls[3] = ptr_point;
    decls[4] = ptr_void;
    decls[5] = celsius;
    decls[6] = arr;
    decls[7] = slice;
    decls[8] = alias_c;
    decls[9] = point;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 10));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(run_ok(&f, pkgs, 1));
    i64 = builtin(&f, JOG_BUILTIN_I64);
    i32 = builtin(&f, JOG_BUILTIN_I32);
    f64 = builtin(&f, JOG_BUILTIN_F64);
    CHECK(alias_a->symbol && alias_a->symbol->type == i64);
    CHECK(alias_b->symbol && alias_b->symbol->type == i64);
    CHECK(alias_c->symbol && alias_c->symbol->type == i64);
    CHECK(alias_a->type == i64);
    CHECK(fn->as.fn.ret && fn->as.fn.ret->symbol == alias_a->symbol);
    CHECK(fn->as.fn.ret->type == i64);
    CHECK(param->type == i64);
    CHECK(fn->type && fn->type->kind == JOG_TY_FN);
    CHECK(fn->type->as.fn.param_count == 1);
    CHECK(fn->type->as.fn.params && fn->type->as.fn.params[0] == i64);
    CHECK(fn->type->as.fn.ret == i64);
    CHECK(point->type && point->type->kind == JOG_TY_STRUCT);
    CHECK(point->type->as.nominal.name && strcmp(point->type->as.nominal.name, "Point") == 0);
    CHECK(point->symbol && point->symbol->type == point->type);
    CHECK(ptr_point->type && ptr_point->type->kind == JOG_TY_PTR);
    CHECK(!ptr_point->type->is_const);
    CHECK(ptr_point->type->as.elem.elem == point->type);
    CHECK(ptr_void->type && ptr_void->type->kind == JOG_TY_PTR && ptr_void->type->is_const);
    CHECK(ptr_void->type->as.elem.elem && ptr_void->type->as.elem.elem->kind == JOG_TY_STRUCT);
    CHECK(ptr_void->type->as.elem.elem->as.nominal.name &&
          strcmp(ptr_void->type->as.elem.elem->as.nominal.name, "void") == 0);
    CHECK(ptr_void->type->as.elem.elem != builtin(&f, JOG_BUILTIN_UNIT));
    CHECK(celsius->type && celsius->type->kind == JOG_TY_DISTINCT);
    CHECK(celsius->type->as.nominal.base == f64);
    CHECK(celsius->type->has_layout);
    CHECK(arr->type && arr->type->kind == JOG_TY_ARRAY);
    CHECK(arr->type->as.array.len == 4);
    CHECK(arr->type->as.array.elem == i32);
    CHECK(slice->type && slice->type->kind == JOG_TY_SLICE);
    CHECK(slice->type->as.elem.elem == i32);
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    opt = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p1, "Maybe", JOG_VIS_PUBLIC);
    tuple = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p2, "Pair", JOG_VIS_PUBLIC);
    if (!opt || !tuple) {
        fix_end(&f);
        return;
    }
    opt->as.type_decl.target = mk(f.arena, JOG_NODE_TYPE_OPTION, p1);
    if (opt->as.type_decl.target) {
        opt->as.type_decl.target->as.type_elem.elem = tname(f.arena, f.intern, p1, "i32", NULL);
    }
    tuple_elems[0] = tname(f.arena, f.intern, p2, "i32", NULL);
    tuple_elems[1] = tname(f.arena, f.intern, p2, "bool", NULL);
    tuple->as.type_decl.target = mk(f.arena, JOG_NODE_TYPE_TUPLE, p2);
    if (tuple->as.type_decl.target) {
        tuple->as.type_decl.target->as.type_tuple.elems = nodes_of(f.arena, tuple_elems, 2);
    }
    decls[0] = opt;
    decls[1] = tuple;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 2));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(run_ok(&f, pkgs, 1));
    i32 = builtin(&f, JOG_BUILTIN_I32);
    boolean = builtin(&f, JOG_BUILTIN_BOOL);
    CHECK(opt->type && opt->type->kind == JOG_TY_OPTION);
    CHECK(opt->type->as.elem.elem == i32);
    CHECK(tuple->type && tuple->type->kind == JOG_TY_TUPLE);
    CHECK(tuple->type->as.tuple.count == 2);
    CHECK(tuple->type->as.tuple.elems[0] == i32);
    CHECK(tuple->type->as.tuple.elems[1] == boolean);
    fix_end(&f);
}

static void test_demand_cycles(void) {
    Fix f;
    JogPackage pkg;
    JogSourceFile file;
    JogSourceFile *files[1];
    JogPackage *pkgs[1];
    JogNode *decls[4];
    JogNode *a;
    JogNode *b;
    JogNode *c;
    JogNode *d;
    JogPos p1 = at_file("a.jog", 1);
    JogPos p2 = at_file("a.jog", 2);
    JogPos p3 = at_file("a.jog", 3);
    JogPos p4 = at_file("a.jog", 4);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    a = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p1, "A", JOG_VIS_PUBLIC);
    b = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p2, "B", JOG_VIS_PUBLIC);
    c = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p3, "C", JOG_VIS_PUBLIC);
    if (!a || !b || !c) {
        fix_end(&f);
        return;
    }
    a->as.type_decl.target = tname(f.arena, f.intern, p1, "B", NULL);
    b->as.type_decl.target = tname(f.arena, f.intern, p2, "A", NULL);
    c->as.type_decl.target = tname(f.arena, f.intern, p3, "i64", NULL);
    decls[0] = a;
    decls[1] = b;
    decls[2] = c;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 3));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(!run_pkgs(&f, pkgs, 1));
    expect_diag(&f, "a.jog:1:1: error: declaration cycle: A -> B -> A");
    CHECK(ct_diag_error_count(f.diag) == 1);
    CHECK(c->symbol && c->symbol->type == builtin(&f, JOG_BUILTIN_I64));
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    a = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p1, "A", JOG_VIS_PUBLIC);
    b = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p2, "B", JOG_VIS_PUBLIC);
    c = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p3, "C", JOG_VIS_PUBLIC);
    if (!a || !b || !c) {
        fix_end(&f);
        return;
    }
    a->as.type_decl.target = tname(f.arena, f.intern, p1, "B", NULL);
    b->as.type_decl.target = tname(f.arena, f.intern, p2, "C", NULL);
    c->as.type_decl.target = tname(f.arena, f.intern, p3, "A", NULL);
    decls[0] = a;
    decls[1] = b;
    decls[2] = c;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 3));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(!run_pkgs(&f, pkgs, 1));
    expect_diag(&f, "declaration cycle: A -> B -> C -> A");
    CHECK(ct_diag_error_count(f.diag) == 1);
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    a = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p1, "A", JOG_VIS_PUBLIC);
    if (!a) {
        fix_end(&f);
        return;
    }
    a->as.type_decl.target = tname(f.arena, f.intern, p1, "A", NULL);
    decls[0] = a;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 1));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(!run_pkgs(&f, pkgs, 1));
    expect_diag(&f, "declaration cycle: A -> A");
    CHECK(ct_diag_error_count(f.diag) == 1);
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    a = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p1, "A", JOG_VIS_PUBLIC);
    b = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p2, "B", JOG_VIS_PUBLIC);
    c = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p3, "C", JOG_VIS_PUBLIC);
    d = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p4, "D", JOG_VIS_PUBLIC);
    if (!a || !b || !c || !d) {
        fix_end(&f);
        return;
    }
    a->as.type_decl.target = tname(f.arena, f.intern, p1, "B", NULL);
    b->as.type_decl.target = tname(f.arena, f.intern, p2, "A", NULL);
    c->as.type_decl.target = tname(f.arena, f.intern, p3, "D", NULL);
    d->as.type_decl.target = tname(f.arena, f.intern, p4, "C", NULL);
    decls[0] = a;
    decls[1] = b;
    decls[2] = c;
    decls[3] = d;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 4));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(!run_pkgs(&f, pkgs, 1));
    expect_diag(&f, "declaration cycle: A -> B -> A");
    expect_diag(&f, "declaration cycle: C -> D -> C");
    CHECK(ct_diag_error_count(f.diag) == 2);
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    a = bind_of(f.arena, f.intern, JOG_NODE_VAL, p1, JOG_VIS_PUBLIC, "a", NULL);
    b = bind_of(f.arena, f.intern, JOG_NODE_VAL, p2, JOG_VIS_PUBLIC, "b", NULL);
    if (!a || !b) {
        fix_end(&f);
        return;
    }
    a->as.binding.type = tname(f.arena, f.intern, p1, "i64", NULL);
    b->as.binding.type = tname(f.arena, f.intern, p2, "i64", NULL);
    a->as.binding.init = ident_of(f.arena, f.intern, p1, "b");
    b->as.binding.init = ident_of(f.arena, f.intern, p2, "a");
    decls[0] = a;
    decls[1] = b;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 2));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(!run_pkgs(&f, pkgs, 1));
    expect_diag(&f, "declaration cycle: a -> b -> a");
    CHECK(ct_diag_error_count(f.diag) == 1);
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    a = bind_of(f.arena, f.intern, JOG_NODE_VAL, p1, JOG_VIS_PUBLIC, "a", NULL);
    b = bind_of(f.arena, f.intern, JOG_NODE_VAL, p2, JOG_VIS_PUBLIC, "b", NULL);
    if (!a || !b) {
        fix_end(&f);
        return;
    }
    a->as.binding.type = tname(f.arena, f.intern, p1, "i64", NULL);
    b->as.binding.type = tname(f.arena, f.intern, p2, "i64", NULL);
    a->as.binding.init = bin(f.arena, p1, JOG_TOK_PLUS, ident_of(f.arena, f.intern, p1, "b"), lit(f.arena, p1, "1"));
    b->as.binding.init = ident_of(f.arena, f.intern, p2, "a");
    decls[0] = a;
    decls[1] = b;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 2));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(!run_pkgs(&f, pkgs, 1));
    expect_diag(&f, "declaration cycle: a -> b -> a");
    fix_end(&f);
}

static void test_demand_values(void) {
    Fix f;
    JogPackage pkg;
    JogSourceFile file;
    JogSourceFile *files[1];
    JogPackage *pkgs[1];
    JogNode *decls[4];
    JogNode *a;
    JogNode *b;
    JogNode *c;
    JogNode *body;
    JogNode *stmt;
    JogNode *bogus;
    JogNode *stmts[1];
    JogNode *fn;
    JogNode *block;
    JogNode *malloc_fn;
    JogNode *extern_fns[1];
    JogNode *method;
    JogNode *point;
    JogPos p1 = at_file("a.jog", 1);
    JogPos p2 = at_file("a.jog", 2);
    JogPos p3 = at_file("a.jog", 3);
    JogPackageScope *scope;

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    a = bind_of(f.arena, f.intern, JOG_NODE_VAL, p1, JOG_VIS_PUBLIC, "a", NULL);
    b = bind_of(f.arena, f.intern, JOG_NODE_VAL, p2, JOG_VIS_PUBLIC, "b", NULL);
    if (!a || !b) {
        fix_end(&f);
        return;
    }
    a->as.binding.init = ident_of(f.arena, f.intern, p1, "b");
    b->as.binding.type = tname(f.arena, f.intern, p2, "i64", NULL);
    decls[0] = a;
    decls[1] = b;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 2));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(run_ok(&f, pkgs, 1));
    CHECK(a->symbol && a->symbol->type == builtin(&f, JOG_BUILTIN_I64));
    CHECK(b->symbol && b->symbol->type == a->symbol->type);
    CHECK(a->as.binding.init->symbol == b->symbol);
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    a = bind_of(f.arena, f.intern, JOG_NODE_VAL, p1, JOG_VIS_PUBLIC, "flag", NULL);
    if (!a) {
        fix_end(&f);
        return;
    }
    a->as.binding.init = ident_of(f.arena, f.intern, p1, "true");
    decls[0] = a;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 1));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(run_ok(&f, pkgs, 1));
    CHECK(a->symbol && a->symbol->type == builtin(&f, JOG_BUILTIN_BOOL));
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    a = bind_of(f.arena, f.intern, JOG_NODE_VAL, p1, JOG_VIS_PUBLIC, "a", NULL);
    b = bind_of(f.arena, f.intern, JOG_NODE_VAL, p2, JOG_VIS_PUBLIC, "b", NULL);
    c = bind_of(f.arena, f.intern, JOG_NODE_VAL, p3, JOG_VIS_PUBLIC, "c", NULL);
    if (!a || !b || !c) {
        fix_end(&f);
        return;
    }
    b->as.binding.type = tname(f.arena, f.intern, p2, "i64", NULL);
    c->as.binding.type = tname(f.arena, f.intern, p3, "i64", NULL);
    a->as.binding.init = bin(f.arena, p1, JOG_TOK_PLUS, ident_of(f.arena, f.intern, p1, "b"),
                             ident_of(f.arena, f.intern, p1, "c"));
    decls[0] = a;
    decls[1] = b;
    decls[2] = c;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 3));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(run_ok(&f, pkgs, 1));
    CHECK(a->symbol && a->symbol->type == builtin(&f, JOG_BUILTIN_I64));
    CHECK(a->as.binding.init && a->as.binding.init->type == a->symbol->type);
    CHECK(b->symbol && b->symbol->type == builtin(&f, JOG_BUILTIN_I64));
    CHECK(ct_diag_error_count(f.diag) == 0);
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    a = bind_of(f.arena, f.intern, JOG_NODE_VAL, p1, JOG_VIS_PUBLIC, "a", "b");
    if (!a) {
        fix_end(&f);
        return;
    }
    {
        JogNode *elems[2];
        JogNode *vals[2];
        JogNode *ty = mk(f.arena, JOG_NODE_TYPE_TUPLE, p1);
        JogNode *init = mk(f.arena, JOG_NODE_TUPLE, p1);
        elems[0] = tname(f.arena, f.intern, p1, "i64", NULL);
        elems[1] = tname(f.arena, f.intern, p1, "i64", NULL);
        vals[0] = lit(f.arena, p1, "1");
        vals[1] = lit(f.arena, p1, "2");
        if (!ty || !init || !elems[0] || !elems[1] || !vals[0] || !vals[1]) {
            fix_end(&f);
            return;
        }
        ty->as.type_tuple.elems = nodes_of(f.arena, elems, 2);
        init->as.tuple.elems = nodes_of(f.arena, vals, 2);
        a->as.binding.type = ty;
        a->as.binding.init = init;
    }
    decls[0] = a;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 1));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(run_ok(&f, pkgs, 1));
    scope = pkg.scope;
    CHECK(a->symbol && a->symbol->type == builtin(&f, JOG_BUILTIN_I64));
    CHECK(scope && jog_package_scope_lookup(scope, a->as.binding.names.items[1]) &&
          jog_package_scope_lookup(scope, a->as.binding.names.items[1])->type == a->symbol->type);
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    fn = fn_of(f.arena, f.intern, p1, "f", JOG_VIS_PUBLIC, false);
    bogus = ident_of(f.arena, f.intern, p1, "not_a_name");
    stmt = mk(f.arena, JOG_NODE_EXPR_STMT, p1);
    body = mk(f.arena, JOG_NODE_BLOCK, p1);
    if (!fn || !bogus || !stmt || !body) {
        fix_end(&f);
        return;
    }
    stmt->as.expr_stmt.expr = bogus;
    stmts[0] = stmt;
    body->as.block.stmts = nodes_of(f.arena, stmts, 1);
    fn->as.fn.body = body;
    fn->as.fn.ret = tname(f.arena, f.intern, p1, "i32", NULL);
    decls[0] = fn;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 1));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(!run_pkgs(&f, pkgs, 1));
    expect_diag(&f, "undefined name 'not_a_name'");
    expect_diag(&f, "missing return");
    CHECK(bogus->symbol == NULL);
    CHECK(fn->type && fn->type->kind == JOG_TY_FN);
    CHECK(fn->type->as.fn.param_count == 0);
    CHECK(fn->type->as.fn.ret == builtin(&f, JOG_BUILTIN_I32));
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    malloc_fn = fn_of(f.arena, f.intern, p1, "malloc", JOG_VIS_PUBLIC, false);
    block = mk(f.arena, JOG_NODE_EXTERN_BLOCK, p1);
    if (!malloc_fn || !block) {
        fix_end(&f);
        return;
    }
    malloc_fn->as.fn.is_extern = true;
    malloc_fn->as.fn.ret = tname(f.arena, f.intern, p1, "i32", NULL);
    extern_fns[0] = malloc_fn;
    block->visibility = JOG_VIS_PUBLIC;
    block->as.extern_block.fns = nodes_of(f.arena, extern_fns, 1);
    decls[0] = block;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 1));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(run_ok(&f, pkgs, 1));
    CHECK(malloc_fn->type && malloc_fn->type->kind == JOG_TY_FN);
    CHECK(malloc_fn->type->as.fn.is_extern);
    CHECK(malloc_fn->type->as.fn.ret == builtin(&f, JOG_BUILTIN_I32));
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    method = fn_of(f.arena, f.intern, p1, "mag", JOG_VIS_PUBLIC, false);
    point = type_of(f.arena, f.intern, JOG_NODE_STRUCT, p2, "Point", JOG_VIS_PUBLIC);
    if (!method || !point) {
        fix_end(&f);
        return;
    }
    method->as.fn.receiver = tname(f.arena, f.intern, p1, "Point", NULL);
    method->as.fn.ret = tname(f.arena, f.intern, p1, "i32", NULL);
    decls[0] = method;
    decls[1] = point;
    prep_file(&file, file_of(f.arena, p1, NULL, 0, decls, 2));
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    CHECK(run_ok(&f, pkgs, 1));
    CHECK(method->symbol && method->symbol->kind == JOG_SYM_METHOD);
    CHECK(method->as.fn.receiver->type == point->type);
    CHECK(method->type && method->type->as.fn.ret == builtin(&f, JOG_BUILTIN_I32));
    CHECK(jog_package_scope_lookup(pkg.scope, method->as.fn.name) == NULL);
    fix_end(&f);
}

static void test_demand_names(void) {
    Fix f;
    JogPackage app;
    JogPackage other;
    JogSourceFile app_file;
    JogSourceFile other_file;
    JogSourceFile *app_files[1];
    JogSourceFile *other_files[1];
    JogPackage *pkgs[2];
    JogNode *imports[1];
    JogNode *app_decls[1];
    JogNode *other_decls[1];
    JogNode *a;
    JogNode *b;
    JogNode *fn;
    JogNode *imp;
    JogPos p1 = at_file("a.jog", 1);
    JogPos p2 = at_file("a.jog", 2);
    JogPos o1 = at_file("b.jog", 1);
    JogProgram bare;

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    b = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, o1, "B", JOG_VIS_PUBLIC);
    imp = import_of(f.arena, f.intern, p1, "other", NULL, NULL);
    a = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p2, "A", JOG_VIS_PUBLIC);
    if (!b || !imp || !a) {
        fix_end(&f);
        return;
    }
    b->as.type_decl.target = tname(f.arena, f.intern, o1, "i64", NULL);
    a->as.type_decl.target = tname(f.arena, f.intern, p2, "other", "B");
    other_decls[0] = b;
    imports[0] = imp;
    app_decls[0] = a;
    prep_file(&other_file, file_of(f.arena, o1, NULL, 0, other_decls, 1));
    prep_file(&app_file, file_of(f.arena, p1, imports, 1, app_decls, 1));
    other_files[0] = &other_file;
    app_files[0] = &app_file;
    prep_pkg(&app, "app", app_files, 1);
    prep_pkg(&other, "other", other_files, 1);
    pkgs[0] = &app;
    pkgs[1] = &other;
    CHECK(run_ok(&f, pkgs, 2));
    CHECK(a->symbol && a->symbol->type == builtin(&f, JOG_BUILTIN_I64));
    CHECK(a->type == b->symbol->type);
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    b = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, o1, "B", JOG_VIS_PACKAGE);
    imp = import_of(f.arena, f.intern, p1, "other", NULL, NULL);
    a = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p2, "A", JOG_VIS_PUBLIC);
    if (!b || !imp || !a) {
        fix_end(&f);
        return;
    }
    b->as.type_decl.target = tname(f.arena, f.intern, o1, "i64", NULL);
    a->as.type_decl.target = tname(f.arena, f.intern, p2, "other", "B");
    other_decls[0] = b;
    imports[0] = imp;
    app_decls[0] = a;
    prep_file(&other_file, file_of(f.arena, o1, NULL, 0, other_decls, 1));
    prep_file(&app_file, file_of(f.arena, p1, imports, 1, app_decls, 1));
    other_files[0] = &other_file;
    app_files[0] = &app_file;
    prep_pkg(&app, "app", app_files, 1);
    prep_pkg(&other, "other", other_files, 1);
    pkgs[0] = &app;
    pkgs[1] = &other;
    CHECK(!run_pkgs(&f, pkgs, 2));
    expect_diag(&f, "type 'B' is not visible");
    CHECK(b->symbol && b->symbol->type == builtin(&f, JOG_BUILTIN_I64));
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    a = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p1, "A", JOG_VIS_PUBLIC);
    if (!a) {
        fix_end(&f);
        return;
    }
    a->as.type_decl.target = tname(f.arena, f.intern, p1, "Missing", NULL);
    app_decls[0] = a;
    prep_file(&app_file, file_of(f.arena, p1, NULL, 0, app_decls, 1));
    app_files[0] = &app_file;
    prep_pkg(&app, "app", app_files, 1);
    pkgs[0] = &app;
    CHECK(!run_pkgs(&f, pkgs, 1));
    expect_diag(&f, "undefined type 'Missing'");
    CHECK(ct_diag_error_count(f.diag) == 1);
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    fn = fn_of(f.arena, f.intern, p1, "f", JOG_VIS_PUBLIC, false);
    a = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p2, "A", JOG_VIS_PUBLIC);
    if (!fn || !a) {
        fix_end(&f);
        return;
    }
    fn->as.fn.ret = tname(f.arena, f.intern, p1, "i32", NULL);
    a->as.type_decl.target = tname(f.arena, f.intern, p2, "f", NULL);
    app_decls[0] = a;
    other_decls[0] = fn;
    /* one file, two decls */
    {
        JogNode *both[2];
        both[0] = a;
        both[1] = fn;
        prep_file(&app_file, file_of(f.arena, p1, NULL, 0, both, 2));
    }
    app_files[0] = &app_file;
    prep_pkg(&app, "app", app_files, 1);
    pkgs[0] = &app;
    CHECK(!run_pkgs(&f, pkgs, 1));
    expect_diag(&f, "'f' is not a type");
    CHECK(fn->type && fn->type->kind == JOG_TY_FN);
    CHECK(fn->type->as.fn.ret == builtin(&f, JOG_BUILTIN_I32));
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    a = fn_of(f.arena, f.intern, p1, "main", JOG_VIS_PUBLIC, false);
    b = fn_of(f.arena, f.intern, p2, "main", JOG_VIS_PUBLIC, false);
    if (!a || !b) {
        fix_end(&f);
        return;
    }
    app_decls[0] = a;
    other_decls[0] = b;
    {
        JogNode *both[2];
        both[0] = a;
        both[1] = b;
        prep_file(&app_file, file_of(f.arena, p1, NULL, 0, both, 2));
    }
    app_files[0] = &app_file;
    prep_pkg(&app, "app", app_files, 1);
    pkgs[0] = &app;
    CHECK(!run_pkgs(&f, pkgs, 1));
    {
        char text[8192];
        diag_text(f.diag, text, sizeof(text));
        CHECK(strstr(text, "duplicate declaration of 'main'") != NULL);
        CHECK(strstr(text, "declaration cycle") == NULL);
    }
    fix_end(&f);

    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    a = type_of(f.arena, f.intern, JOG_NODE_TYPE_DECL, p1, "A", JOG_VIS_PUBLIC);
    if (!a) {
        fix_end(&f);
        return;
    }
    a->as.type_decl.target = tname(f.arena, f.intern, p1, "i64", NULL);
    app_decls[0] = a;
    prep_file(&app_file, file_of(f.arena, p1, NULL, 0, app_decls, 1));
    app_files[0] = &app_file;
    prep_pkg(&app, "app", app_files, 1);
    pkgs[0] = &app;
    CHECK(run_ok(&f, pkgs, 1));
    CHECK(a->symbol && a->symbol->type == builtin(&f, JOG_BUILTIN_I64));
    CHECK(run_ok(&f, pkgs, 1));
    fix_end(&f);

    CHECK(!jog_check(NULL));
    memset(&bare, 0, sizeof(bare));
    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    bare.arena = f.arena;
    bare.diag = f.diag;
    bare.intern = f.intern;
    CHECK(!jog_check(&bare));
    expect_diag(&f, "checker is missing program state");
    fix_end(&f);
}

static size_t collect_expected(CtArena *arena, const char *src, const char **out, size_t cap) {
    size_t n = 0;
    const char *p = src ? src : "";
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        size_t i;
        for (i = 0; i + 8 < len; i++) {
            const char *s;
            size_t rest;
            char *copy;
            if (p[i] != '/' || p[i + 1] != '/') {
                continue;
            }
            s = p + i + 2;
            while (s < p + len && (*s == ' ' || *s == '\t')) {
                s++;
            }
            rest = (size_t)(p + len - s);
            if (rest < 6 || strncmp(s, "error:", 6) != 0) {
                continue;
            }
            s += 6;
            while (s < p + len && (*s == ' ' || *s == '\t')) {
                s++;
            }
            rest = (size_t)(p + len - s);
            while (rest > 0 && (s[rest - 1] == '\r' || s[rest - 1] == ' ' || s[rest - 1] == '\t')) {
                rest--;
            }
            if (n >= cap) {
                return n + 1;
            }
            copy = ct_arena_alloc(arena, rest + 1, 1);
            if (!copy) {
                return n;
            }
            memcpy(copy, s, rest);
            copy[rest] = '\0';
            out[n++] = copy;
            break;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return n;
}

static JogNode *fn_named(JogNode *file, const char *name) {
    size_t i;
    if (!file || file->kind != JOG_NODE_FILE) {
        return NULL;
    }
    for (i = 0; i < file->as.file.decls.count; i++) {
        JogNode *decl = file->as.file.decls.items ? file->as.file.decls.items[i] : NULL;
        size_t j;
        if (!decl) {
            continue;
        }
        if (decl->kind == JOG_NODE_EXTERN_BLOCK) {
            for (j = 0; j < decl->as.extern_block.fns.count; j++) {
                JogNode *fn = decl->as.extern_block.fns.items ? decl->as.extern_block.fns.items[j] : NULL;
                if (fn && fn->kind == JOG_NODE_FN && fn->as.fn.name && strcmp(fn->as.fn.name, name) == 0) {
                    return fn;
                }
            }
        }
        if (decl->kind == JOG_NODE_FN && decl->as.fn.name && strcmp(decl->as.fn.name, name) == 0) {
            return decl;
        }
    }
    return NULL;
}

static void assert_hello(Fix *f, JogNode *root) {
    JogNode *main_fn;
    JogNode *puts_fn;
    JogNode *greeting;
    JogNode *iff;
    JogNode *call;
    JogType *i8;
    JogType *i32;
    JogType *cstr;
    JogType *boolean;
    JogNode *body;
    JogNode *stmt;
    i8 = builtin(f, JOG_BUILTIN_I8);
    i32 = builtin(f, JOG_BUILTIN_I32);
    boolean = builtin(f, JOG_BUILTIN_BOOL);
    cstr = jog_type_ptr(f->types, i8, true);
    main_fn = fn_named(root, "main");
    puts_fn = fn_named(root, "puts");
    CHECK(main_fn && puts_fn && cstr);
    CHECK(puts_fn->type && puts_fn->type->kind == JOG_TY_FN);
    CHECK(puts_fn->type->as.fn.is_extern);
    CHECK(puts_fn->type->as.fn.param_count == 1);
    CHECK(puts_fn->type->as.fn.params && puts_fn->type->as.fn.params[0] == cstr);
    CHECK(puts_fn->type->as.fn.ret == i32);
    body = main_fn->as.fn.body;
    CHECK(body && body->kind == JOG_NODE_BLOCK && body->as.block.stmts.count >= 2);
    if (!body || body->as.block.stmts.count < 2 || !body->as.block.stmts.items) {
        return;
    }
    greeting = body->as.block.stmts.items[0];
    stmt = body->as.block.stmts.items[1];
    CHECK(greeting && greeting->kind == JOG_NODE_VAL);
    CHECK(greeting->type == cstr);
    CHECK(greeting->symbol && greeting->symbol->type == cstr);
    CHECK(greeting->as.binding.init && greeting->as.binding.init->kind == JOG_NODE_CSTRING_LIT);
    CHECK(greeting->as.binding.init->type == cstr);
    CHECK(stmt && stmt->kind == JOG_NODE_EXPR_STMT);
    iff = stmt ? stmt->as.expr_stmt.expr : NULL;
    CHECK(iff && iff->kind == JOG_NODE_IF);
    CHECK(iff && iff->as.if_.cond && iff->as.if_.cond->kind == JOG_NODE_BINARY);
    CHECK(iff && iff->as.if_.cond && iff->as.if_.cond->type == boolean);
    call = iff && iff->as.if_.cond ? iff->as.if_.cond->as.binary.lhs : NULL;
    CHECK(call && call->kind == JOG_NODE_CALL);
    CHECK(call && call->type == i32);
    CHECK(call && call->symbol == puts_fn->symbol);
}

static void check_one_file(const char *path) {
    Fix f;
    CtFile src;
    CtDa tokens;
    JogNode *root;
    JogSourceFile file;
    JogSourceFile *files[1];
    JogPackage pkg;
    JogPackage *pkgs[1];
    const char *expected[32];
    size_t nexpect;
    size_t i;
    int ok;
    char text[8192];
    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    memset(&src, 0, sizeof(src));
    if (!ct_da_init(&tokens, sizeof(JogToken))) {
        CHECK(0);
        fix_end(&f);
        return;
    }
    if (!ct_file_read(f.arena, f.diag, path, &src) || !src.data) {
        fprintf(stderr, "cannot read %s\n", path);
        CHECK(0);
        ct_da_free(&tokens);
        fix_end(&f);
        return;
    }
    if (!jog_lex(f.arena, f.diag, f.intern, &src, &tokens) || ct_diag_error_count(f.diag) != 0) {
        fprintf(stderr, "lex errors in %s\n", path);
        ct_diag_print(f.diag, stderr);
        CHECK(0);
        ct_da_free(&tokens);
        fix_end(&f);
        return;
    }
    root = jog_parse(f.arena, f.diag, f.intern, &src, &tokens);
    nexpect = collect_expected(f.arena, src.data, expected, 32);
    CHECK(nexpect <= 32);
    if (!root || (ct_diag_error_count(f.diag) != 0 && nexpect == 0)) {
        fprintf(stderr, "parse errors in %s\n", path);
        ct_diag_print(f.diag, stderr);
        CHECK(0);
        ct_da_free(&tokens);
        fix_end(&f);
        return;
    }
    if (ct_diag_error_count(f.diag) != 0) {
        diag_text(f.diag, text, sizeof(text));
        if (ct_diag_error_count(f.diag) != nexpect) {
            fprintf(stderr, "error count %zu, expected %zu, in %s\n%s\n", ct_diag_error_count(f.diag), nexpect, path,
                    text);
            CHECK(0);
        }
        for (i = 0; i < nexpect && i < 32; i++) {
            if (!strstr(text, expected[i])) {
                fprintf(stderr, "missing '%s' in %s\n%s\n", expected[i], path, text);
                CHECK(0);
            }
        }
        ct_da_free(&tokens);
        fix_end(&f);
        return;
    }
    prep_file(&file, root);
    files[0] = &file;
    prep_pkg(&pkg, "app", files, 1);
    pkgs[0] = &pkg;
    ok = run_pkgs(&f, pkgs, 1);
    diag_text(f.diag, text, sizeof(text));
    if (nexpect == 0) {
        if (!ok || ct_diag_error_count(f.diag) != 0) {
            fprintf(stderr, "unexpected errors in %s\n%s\n", path, text);
            CHECK(0);
        }
        if (strstr(path, "ok_hello.jog")) {
            assert_hello(&f, root);
        }
    } else {
        if (ok || ct_diag_error_count(f.diag) != nexpect) {
            fprintf(stderr, "error count %zu, expected %zu, in %s\n%s\n", ct_diag_error_count(f.diag), nexpect, path,
                    text);
            CHECK(0);
        }
        for (i = 0; i < nexpect && i < 32; i++) {
            if (!strstr(text, expected[i])) {
                fprintf(stderr, "missing '%s' in %s\n%s\n", expected[i], path, text);
                CHECK(0);
            }
        }
    }
    ct_da_free(&tokens);
    fix_end(&f);
}

static char *save_env(const char *value) {
    size_t n = strlen(value);
    char *copy = malloc(n + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, n + 1);
    return copy;
}

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

static void free_pkg_tokens(JogPackage *pkg) {
    size_t i;
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
    size_t i;
    if (!program) {
        return;
    }
    for (i = 0; i < program->package_count; i++) {
        free_pkg_tokens(program->packages ? program->packages[i] : NULL);
    }
    free_pkg_tokens(program->root);
}

static JogNode *fn_flagged(JogPackage *pkg, const char *name, int want_expect) {
    size_t f;
    size_t d;
    if (!pkg || !pkg->files) {
        return NULL;
    }
    for (f = 0; f < pkg->file_count; f++) {
        JogNode *file = pkg->files[f] ? pkg->files[f]->ast : NULL;
        if (!file || file->kind != JOG_NODE_FILE) {
            continue;
        }
        for (d = 0; d < file->as.file.decls.count; d++) {
            JogNode *decl = file->as.file.decls.items ? file->as.file.decls.items[d] : NULL;
            if (!decl || decl->kind != JOG_NODE_FN || !decl->as.fn.name || strcmp(decl->as.fn.name, name) != 0) {
                continue;
            }
            if (want_expect && decl->as.fn.is_expect) {
                return decl;
            }
            if (!want_expect && decl->as.fn.is_actual) {
                return decl;
            }
        }
    }
    return NULL;
}

static void assert_ok_expect(JogPackage *pkg) {
    JogNode *expect = fn_flagged(pkg, "hidden", 1);
    JogNode *actual = fn_flagged(pkg, "hidden", 0);
    CHECK(expect && expect->as.fn.is_expect && !expect->symbol);
    CHECK(actual && actual->symbol && actual->symbol->decl == actual);
    CHECK(actual && actual->visibility == JOG_VIS_PACKAGE);
    CHECK(actual && actual->as.fn.body);
    expect = fn_flagged(pkg, "answer", 1);
    actual = fn_flagged(pkg, "answer", 0);
    CHECK(expect && !expect->symbol);
    CHECK(actual && actual->symbol && actual->symbol->decl == actual);
    CHECK(actual && actual->visibility == JOG_VIS_PUBLIC);
    CHECK(actual && actual->as.fn.body);
}

static size_t collect_dir_expected(Fix *f, const char *dir, const char **out, size_t cap) {
    CtDa names;
    size_t i;
    size_t n = 0;
    if (!ct_da_init(&names, sizeof(const char *))) {
        return cap + 1;
    }
    if (!ct_fs_list(f->arena, f->diag, dir, CT_FS_FILES, ".jog", &names)) {
        ct_da_free(&names);
        return cap + 1;
    }
    for (i = 0; i < names.count && n <= cap; i++) {
        const char *name = *(const char **)ct_da_at(&names, i);
        const char *path = name ? ct_path_join(f->arena, dir, name) : NULL;
        CtFile src;
        size_t got;
        memset(&src, 0, sizeof(src));
        if (!path || !ct_file_read(f->arena, f->diag, path, &src) || !src.data) {
            n = cap + 1;
            break;
        }
        got = collect_expected(f->arena, src.data, out + n, cap - n);
        if (got > cap - n) {
            n = cap + 1;
            break;
        }
        n += got;
    }
    ct_da_free(&names);
    return n;
}

static void check_one_dir(const char *path) {
    Fix f;
    const char *saved = NULL;
    int had = 0;
    const char *expected[32];
    size_t nexpect;
    size_t i;
    int loaded;
    int ok;
    char text[8192];
    const char *env = getenv("JOG_ROOT");
    if (!fix_begin(&f)) {
        CHECK(0);
        return;
    }
    if (env && env[0] != '\0') {
        had = 1;
        saved = save_env(env);
        if (!saved) {
            CHECK(0);
            fix_end(&f);
            return;
        }
    }
    if (!env_set(CT_SOURCE_DIR)) {
        CHECK(0);
        free((void *)saved);
        fix_end(&f);
        return;
    }
    f.program.options.ptr_size = 8;
    loaded = jog_load(&f.program, path);
    ok = loaded && jog_check(&f.program);
    nexpect = collect_dir_expected(&f, path, expected, 32);
    diag_text(f.diag, text, sizeof(text));
    if (nexpect > 32) {
        fprintf(stderr, "too many expected errors in %s\n", path);
        CHECK(0);
    } else if (nexpect == 0) {
        if (!ok || ct_diag_error_count(f.diag) != 0) {
            fprintf(stderr, "unexpected errors in %s\n%s\n", path, text);
            CHECK(0);
        } else if (strstr(path, "ok_expect")) {
            assert_ok_expect(f.program.root);
        }
    } else {
        if (ok || ct_diag_error_count(f.diag) != nexpect) {
            fprintf(stderr, "error count %zu, expected %zu, in %s\n%s\n", ct_diag_error_count(f.diag), nexpect, path,
                    text);
            CHECK(0);
        }
        for (i = 0; i < nexpect; i++) {
            if (!strstr(text, expected[i])) {
                fprintf(stderr, "missing '%s' in %s\n%s\n", expected[i], path, text);
                CHECK(0);
            }
        }
    }
    free_program_tokens(&f.program);
    if (had) {
        env_set(saved);
    } else {
        env_set(NULL);
    }
    free((void *)saved);
    fix_end(&f);
}

static void test_check_sources(void) {
    CtArena *arena = ct_arena_create();
    CtDiag *diag;
    CtDa names;
    const char *dir;
    size_t i;
    CHECK(arena != NULL);
    if (!arena) {
        return;
    }
    diag = ct_diag_create(arena);
    dir = ct_path_join(arena, CT_SOURCE_DIR, "tests");
    dir = dir ? ct_path_join(arena, dir, "check") : NULL;
    CHECK(diag && dir);
    if (!diag || !dir || !ct_da_init(&names, sizeof(const char *))) {
        ct_arena_destroy(arena);
        return;
    }
    if (!ct_fs_list(arena, diag, dir, CT_FS_FILES, ".jog", &names)) {
        fprintf(stderr, "cannot list %s\n", dir);
        CHECK(0);
        ct_da_free(&names);
        ct_arena_destroy(arena);
        return;
    }
    CHECK(names.count >= 8);
    for (i = 0; i < names.count; i++) {
        const char *name = *(const char **)ct_da_at(&names, i);
        const char *path = ct_path_join(arena, dir, name);
        CHECK(path != NULL);
        if (path) {
            check_one_file(path);
        }
    }
    ct_da_free(&names);
    if (!ct_da_init(&names, sizeof(const char *))) {
        ct_arena_destroy(arena);
        return;
    }
    if (!ct_fs_list(arena, diag, dir, CT_FS_DIRS, NULL, &names)) {
        fprintf(stderr, "cannot list directories in %s\n", dir);
        CHECK(0);
        ct_da_free(&names);
        ct_arena_destroy(arena);
        return;
    }
    for (i = 0; i < names.count; i++) {
        const char *name = *(const char **)ct_da_at(&names, i);
        const char *path = ct_path_join(arena, dir, name);
        CHECK(path != NULL);
        if (path) {
            check_one_dir(path);
        }
    }
    ct_da_free(&names);
    ct_arena_destroy(arena);
}

int main(void) {
    test_symbols();
    test_universe();
    test_const_eval();
    test_package_scopes();
    test_package_scope_errors();
    test_demand_order();
    test_demand_cycles();
    test_demand_values();
    test_demand_names();
    test_check_sources();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
