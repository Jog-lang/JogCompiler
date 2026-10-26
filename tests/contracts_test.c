#include "ast.h"
#include "ir.h"
#include "types.h"

#include "arena.h"
#include "da.h"
#include "diag.h"
#include "str.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

static int prints_as(const JogType *type, const char *expected) {
    CtSb sb;
    ct_sb_init(&sb);
    jog_type_print(type, &sb);
    int same = strcmp(ct_sb_cstr(&sb), expected) == 0;
    if (!same) {
        fprintf(stderr, "  printed '%s', expected '%s'\n", ct_sb_cstr(&sb), expected);
    }
    ct_sb_free(&sb);
    return same;
}

static void test_types(void) {
    CtArena *arena = ct_arena_create();
    JogTypes *types = jog_types_create(arena, 8);
    CHECK(types != NULL);
    CHECK(jog_types_create(arena, 3) == NULL);

    JogType *i32 = jog_type_builtin(types, JOG_BUILTIN_I32);
    JogType *u8 = jog_type_builtin(types, JOG_BUILTIN_U8);
    JogType *i64 = jog_type_builtin(types, JOG_BUILTIN_I64);
    JogType *f64 = jog_type_builtin(types, JOG_BUILTIN_F64);
    JogType *usize = jog_type_builtin(types, JOG_BUILTIN_USIZE);
    JogType *str = jog_type_builtin(types, JOG_BUILTIN_CONST_BYTES);
    JogType *unit = jog_type_builtin(types, JOG_BUILTIN_UNIT);

    CHECK(i32->size == 4 && i32->align == 4);
    CHECK(usize->size == 8 && usize->as.integer.is_size);
    CHECK(str->size == 16 && str->is_const);

    /* Interning: the same shape is the same pointer. */
    JogType *p1 = jog_type_ptr(types, i32, false);
    CHECK(p1 == jog_type_ptr(types, i32, false));
    CHECK(p1 != jog_type_ptr(types, i32, true));
    CHECK(jog_type_slice(types, u8, true) == jog_type_slice(types, u8, true));
    CHECK(jog_type_array(types, i32, 4) != jog_type_array(types, i32, 5));
    JogType *pair_elems[2] = {i32, str};
    JogType *pair = jog_type_tuple(types, pair_elems, 2);
    CHECK(pair == jog_type_tuple(types, pair_elems, 2));
    CHECK(jog_type_bytes_n(types, 4) == jog_type_bytes_n(types, 4));

    /* Layout. */
    CHECK(jog_type_array(types, i32, 4)->size == 16);
    CHECK(pair->size == 24 && pair->align == 8 && pair->as.tuple.offsets[1] == 8);
    JogType *opt_ptr = jog_type_option(types, p1);
    CHECK(jog_type_option_is_niche(opt_ptr) && opt_ptr->size == 8);
    JogType *opt_i32 = jog_type_option(types, i32);
    CHECK(!jog_type_option_is_niche(opt_i32) && opt_i32->size == 8 && opt_i32->align == 4);
    CHECK(jog_type_bytes_n(types, 3)->size == 3 && jog_type_bytes_n(types, 3)->align == 1);

    /* struct Point { x: i8, y: i64, z: i8 } has C layout: 1 + 7 + 8 + 1 + 7 = 24. */
    JogType *point = jog_type_new_nominal(types, JOG_TY_STRUCT, "Point", "", NULL);
    JogField fields[3] = {{"x", u8, 0, false, NULL}, {"y", i64, 0, false, NULL}, {"z", u8, 0, false, NULL}};
    point->as.nominal.fields = fields;
    point->as.nominal.field_count = 3;
    CHECK(jog_type_layout(types, point));
    CHECK(point->size == 24 && point->align == 8);
    CHECK(fields[1].offset == 8 && fields[2].offset == 16);

    /* enum Shape { Circle(r: f64), Empty }: u8 tag, payload at 8, size 16. */
    JogType *shape = jog_type_new_nominal(types, JOG_TY_ENUM, "Shape", "", NULL);
    JogField circle_fields[1] = {{"r", f64, 0, false, NULL}};
    JogVariant variants[2] = {{"Circle", circle_fields, 1, 0, NULL}, {"Empty", NULL, 0, 1, NULL}};
    shape->as.nominal.variants = variants;
    shape->as.nominal.variant_count = 2;
    CHECK(jog_type_layout(types, shape));
    CHECK(shape->as.nominal.base == u8);
    CHECK(shape->as.nominal.payload_offset == 8 && shape->size == 16);

    JogType *celsius = jog_type_new_nominal(types, JOG_TY_DISTINCT, "Celsius", "", NULL);
    celsius->as.nominal.base = f64;
    CHECK(jog_type_layout(types, celsius) && celsius->size == 8);
    CHECK(jog_type_underlying(celsius) == f64);

    /* Printing. */
    CHECK(prints_as(jog_type_ptr(types, jog_type_array(types, i32, 4), true), "*const [4]i32"));
    CHECK(prints_as(pair, "(i32, const bytes)"));
    CHECK(prints_as(opt_ptr, "*i32?"));
    JogType *params[1] = {str};
    JogType *fn = jog_type_fn(types, params, 1, unit, NULL, 0, false, false, true);
    CHECK(fn == jog_type_fn(types, params, 1, unit, NULL, 0, false, false, true));
    CHECK(fn->size == 8);
    CHECK(prints_as(fn, "extern (const bytes) -> ()"));
    CHECK(prints_as(usize, "usize"));

    JogTypes *wasm = jog_types_create(arena, 4);
    CHECK(jog_type_builtin(wasm, JOG_BUILTIN_USIZE)->size == 4);
    CHECK(jog_type_builtin(wasm, JOG_BUILTIN_CONST_BYTES)->size == 8);

    jog_types_release(wasm);
    jog_types_release(types);
    ct_arena_destroy(arena);
}

static void test_ir(void) {
    CtArena *arena = ct_arena_create();
    CtDiag *diag = ct_diag_create(arena);
    JogTypes *types = jog_types_create(arena, 8);
    JogType *i32 = jog_type_builtin(types, JOG_BUILTIN_I32);
    JogType *bool_type = jog_type_builtin(types, JOG_BUILTIN_BOOL);
    JirModule *module = jir_module_create(arena, types);
    CHECK(module && module->main_func == -1);

    /* fn max(a: i32, b: i32): i32 = if a > b { a } else { b } */
    JogType *params[2] = {i32, i32};
    JogType *fn_type = jog_type_fn(types, params, 2, i32, NULL, 0, false, false, false);
    const char *names[2] = {"a", "b"};
    int32_t index = jir_func_add(module, "jog__max", "max", JIR_LINK_INTERNAL, fn_type, names);
    CHECK(index == 0);
    JirFunc *f = jir_func(module, 0);
    CHECK(f->param_count == 2 && f->locals.count == 2);
    CHECK(jir_local_at(f, 1)->is_param && strcmp(jir_local_at(f, 1)->name, "b") == 0);

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

    JirTerm ret_a = {0};
    ret_a.kind = JIR_TERM_RET;
    ret_a.value = jir_local(f, 0);
    jir_terminate(f, then_block, ret_a);

    CHECK(!jir_verify(module, diag)); /* else block is not terminated */
    CHECK(ct_diag_error_count(diag) == 1);

    JirTerm ret_b = ret_a;
    ret_b.value = jir_local(f, 1);
    jir_terminate(f, else_block, ret_b);
    CtDiag *clean = ct_diag_create(arena);
    CHECK(jir_verify(module, clean));

    /* A global and an address-taken local. */
    int32_t g = jir_global_add(module, "jog__counter", i32, false);
    CHECK(g == 0);
    JirOperand gref = jir_global_ref(module, 0);
    CHECK(gref.type == jog_type_ptr(types, i32, false));
    uint32_t slot = jir_local_add(f, "x", i32);
    uint32_t ptr = jir_local_add(f, NULL, jog_type_ptr(types, i32, false));
    JirInst *addr = jir_emit(f, entry, JIR_ADDR, (int32_t)ptr);
    addr->a = jir_local(f, slot);
    CHECK(jir_verify(module, clean));
    CHECK(jir_local_at(f, slot)->address_taken);

    JirInst *bad = jir_emit(f, entry, JIR_COPY, -1);
    bad->a = jir_int(i32, 1);
    CHECK(!jir_verify(module, clean));

    jir_module_destroy(module);
    jog_types_release(types);
    ct_arena_destroy(arena);
}

static void test_ast(void) {
    CtArena *arena = ct_arena_create();
    JogPos pos = {"a.jog", 3, 7};
    JogNode *node = jog_node_new(arena, JOG_NODE_BINARY, pos);
    CHECK(node && node->kind == JOG_NODE_BINARY && node->pos.line == 3 && node->type == NULL);
    CHECK(strcmp(jog_node_kind_name(JOG_NODE_WHEN_ARM), "WHEN_ARM") == 0);

    CtDa da;
    ct_da_init(&da, sizeof(JogNode *));
    ct_da_push(&da, &node);
    ct_da_push(&da, &node);
    bool oom = false;
    JogList list = jog_list_from_da(arena, &da, &oom);
    CHECK(!oom && list.count == 2 && list.items[1] == node);
    ct_da_free(&da);

    CtDa empty;
    ct_da_init(&empty, sizeof(JogNode *));
    list = jog_list_from_da(arena, &empty, &oom);
    CHECK(list.count == 0 && list.items == NULL);
    ct_arena_destroy(arena);
}

int main(void) {
    test_types();
    test_ir();
    test_ast();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
