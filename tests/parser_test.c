#include "lexer.h"
#include "phases.h"

#include "arena.h"
#include "da.h"
#include "diag.h"
#include "file.h"
#include "fs.h"
#include "intern.h"
#include "str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

typedef struct Parsed {
    CtArena *arena;
    CtDiag *diag;
    CtIntern *intern;
    CtDa tokens;
    JogNode *root;
    bool lex_ok;
} Parsed;

static Parsed parse_src(const char *src) {
    Parsed parsed;
    memset(&parsed, 0, sizeof parsed);
    parsed.arena = ct_arena_create();
    parsed.diag = ct_diag_create(parsed.arena);
    parsed.intern = ct_intern_create(parsed.arena);
    ct_da_init(&parsed.tokens, sizeof(JogToken));
    CtFile file = {"test.jog", src, strlen(src)};
    parsed.lex_ok = jog_lex(parsed.arena, parsed.diag, parsed.intern, &file, &parsed.tokens);
    parsed.root = jog_parse(parsed.arena, parsed.diag, parsed.intern, &file, &parsed.tokens);
    return parsed;
}

static void close_parsed(Parsed *parsed) {
    ct_da_free(&parsed->tokens);
    ct_arena_destroy(parsed->arena);
}

static int clean(Parsed *parsed) {
    if (!parsed->root) {
        fprintf(stderr, "parse returned NULL\n");
        return 0;
    }
    if (!parsed->lex_ok || ct_diag_error_count(parsed->diag) != 0) {
        fprintf(stderr, "unexpected diagnostics:\n");
        ct_diag_print(parsed->diag, stderr);
        return 0;
    }
    return 1;
}

static JogNode *expr_at(Parsed *parsed, size_t index);

static JogNode *decl_at(Parsed *parsed, size_t index) {
    if (!parsed->root || index >= parsed->root->as.file.decls.count) {
        return NULL;
    }
    return parsed->root->as.file.decls.items[index];
}

static int path_is(const JogNode *node, const char *dotted) {
    if (!node || node->kind != JOG_NODE_TYPE_NAME) {
        return 0;
    }
    char buf[128];
    size_t n = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < node->as.type_name.path.count; i++) {
        const char *part = node->as.type_name.path.items ? node->as.type_name.path.items[i] : NULL;
        if (!part) {
            return 0;
        }
        size_t len = strlen(part);
        if (i > 0) {
            if (n + 1 >= sizeof buf) {
                return 0;
            }
            buf[n++] = '.';
        }
        if (n + len >= sizeof buf) {
            return 0;
        }
        memcpy(buf + n, part, len);
        n += len;
        buf[n] = '\0';
    }
    return strcmp(buf, dotted) == 0;
}

static const char *printed(Parsed *parsed) {
    CtSb sb;
    ct_sb_init(&sb);
    jog_ast_print(parsed->root, &sb);
    char *copy = ct_arena_strdup(parsed->arena, ct_sb_cstr(&sb));
    ct_sb_free(&sb);
    return copy ? copy : "";
}

static void test_empty(void) {
    Parsed parsed = parse_src("");
    CHECK(clean(&parsed));
    CHECK(parsed.root->kind == JOG_NODE_FILE);
    CHECK(parsed.root->as.file.imports.count == 0);
    CHECK(parsed.root->as.file.decls.count == 0);
    CHECK(strcmp(printed(&parsed), "(FILE)\n") == 0);
    close_parsed(&parsed);
}

static void test_imports(void) {
    Parsed parsed = parse_src("import std.io\nimport vendor.sdl3 as sdl\nval x = 1\nimport late\n");
    CHECK(parsed.root != NULL);
    CHECK(parsed.root->as.file.imports.count == 3);
    JogNode *io = parsed.root->as.file.imports.items[0];
    CHECK(io->kind == JOG_NODE_IMPORT);
    CHECK(io->as.import.path.count == 2);
    CHECK(strcmp(io->as.import.path.items[0], "std") == 0);
    CHECK(strcmp(io->as.import.path.items[1], "io") == 0);
    CHECK(io->as.import.alias == NULL);
    JogNode *sdl = parsed.root->as.file.imports.items[1];
    CHECK(sdl->as.import.alias && strcmp(sdl->as.import.alias, "sdl") == 0);
    CHECK(ct_diag_error_count(parsed.diag) == 1);
    CHECK(parsed.root->as.file.decls.count == 1);
    close_parsed(&parsed);
}

static void test_fn_print(void) {
    const char *expect = "(FILE\n"
                         "  (FN square\n"
                         "    (PARAM x\n"
                         "      (TYPE_NAME i32)\n"
                         "    )\n"
                         "    ->\n"
                         "      (TYPE_NAME i32)\n"
                         "    =\n"
                         "      (BINARY *\n"
                         "        (IDENT x)\n"
                         "        (IDENT x)\n"
                         "      )\n"
                         "  )\n"
                         ")\n";
    Parsed parsed = parse_src("fn square(x: i32): i32 = x * x");
    CHECK(clean(&parsed));
    const char *got = printed(&parsed);
    if (strcmp(got, expect) != 0) {
        fprintf(stderr, "print mismatch:\n%s", got);
        g_failures++;
    }
    close_parsed(&parsed);
}

static void test_functions(void) {
    Parsed parsed = parse_src("fn add(a: i32, b: i32 = 1): i32 = a + b\n"
                              "fn *Point.translate(dx: f32) { self.x = self.x + dx }\n"
                              "fn Vec2.op+(o: Vec2): Vec2 = o\n"
                              "fn *Matrix.op[]=(i: usize, v: f32) = v\n"
                              "fn List<T>.map<R>(x: T): R = x\n"
                              "fn ([]i32).sum(): i64 = 0\n"
                              "fn log(msg: str) with (l: Logger) = l.write(msg)\n"
                              "fn f(): () -> () with (l: Logger) = 0\n"
                              "private(file) fn hidden() = 1\n");
    CHECK(clean(&parsed));
    CHECK(parsed.root->as.file.decls.count == 9);

    JogNode *add = decl_at(&parsed, 0);
    CHECK(add && add->kind == JOG_NODE_FN);
    CHECK(strcmp(add->as.fn.name, "add") == 0);
    CHECK(add->as.fn.params.count == 2);
    CHECK(add->as.fn.params.items[1]->as.param.default_value != NULL);
    CHECK(add->as.fn.expr_body);
    CHECK(add->as.fn.body && add->as.fn.body->kind == JOG_NODE_BINARY);
    CHECK(add->as.fn.body->as.binary.op == JOG_TOK_PLUS);

    JogNode *method = decl_at(&parsed, 1);
    CHECK(method->as.fn.receiver_is_ptr);
    CHECK(path_is(method->as.fn.receiver, "Point"));
    CHECK(method->as.fn.body && method->as.fn.body->kind == JOG_NODE_BLOCK);
    CHECK(method->as.fn.body->as.block.stmts.count == 1);
    CHECK(method->as.fn.body->as.block.stmts.items[0]->kind == JOG_NODE_ASSIGN);

    JogNode *op = decl_at(&parsed, 2);
    CHECK(strcmp(op->as.fn.name, "op+") == 0);
    CHECK(op->as.fn.op == JOG_TOK_PLUS);
    CHECK(!op->as.fn.receiver_is_ptr);

    JogNode *index = decl_at(&parsed, 3);
    CHECK(strcmp(index->as.fn.name, "op[]=") == 0);
    CHECK(index->as.fn.op == JOG_TOK_LBRACKET);
    CHECK(index->as.fn.receiver_is_ptr);
    CHECK(index->as.fn.params.count == 2);

    JogNode *map = decl_at(&parsed, 4);
    CHECK(strcmp(map->as.fn.name, "map") == 0);
    CHECK(map->as.fn.receiver->as.type_name.args.count == 1);
    CHECK(path_is(map->as.fn.receiver->as.type_name.args.items[0], "T"));
    CHECK(map->as.fn.type_params.count == 1);
    CHECK(strcmp(map->as.fn.type_params.items[0]->as.type_param.name, "R") == 0);
    CHECK(map->as.fn.ret && map->as.fn.ret->kind == JOG_NODE_TYPE_NAME);

    JogNode *sum = decl_at(&parsed, 5);
    CHECK(sum->as.fn.receiver && sum->as.fn.receiver->kind == JOG_NODE_TYPE_SLICE);
    CHECK(!sum->as.fn.receiver_is_ptr);

    JogNode *logf = decl_at(&parsed, 6);
    CHECK(logf->as.fn.contexts.count == 1);
    CHECK(strcmp(logf->as.fn.contexts.items[0]->as.param.name, "l") == 0);
    CHECK(logf->as.fn.body && logf->as.fn.body->kind == JOG_NODE_CALL);

    JogNode *ctx = decl_at(&parsed, 7);
    CHECK(ctx->as.fn.ret && ctx->as.fn.ret->kind == JOG_NODE_TYPE_FN);
    CHECK(ctx->as.fn.ret->as.type_fn.contexts.count == 0);
    CHECK(ctx->as.fn.contexts.count == 1);

    JogNode *hidden = decl_at(&parsed, 8);
    CHECK(hidden->visibility == JOG_VIS_FILE);
    CHECK(hidden->as.fn.ret == NULL);
    close_parsed(&parsed);

    Parsed calls = parse_src("val n = obj.method<i32>(1)\n"
                             "val flag = obj.method<i32, bool>(true)\n"
                             "val cmp = obj.method < other\n");
    CHECK(clean(&calls));
    JogNode *typed = expr_at(&calls, 0);
    CHECK(typed->kind == JOG_NODE_CALL);
    CHECK(typed->as.call.callee->kind == JOG_NODE_MEMBER);
    CHECK(strcmp(typed->as.call.callee->as.member.name, "method") == 0);
    CHECK(typed->as.call.callee->as.member.type_args.count == 1);
    CHECK(typed->as.call.callee->as.member.type_args.items[0]->kind == JOG_NODE_TYPE_NAME);
    JogNode *two = expr_at(&calls, 1);
    CHECK(two->as.call.callee->as.member.type_args.count == 2);
    JogNode *cmp = expr_at(&calls, 2);
    CHECK(cmp->kind == JOG_NODE_BINARY);
    CHECK(cmp->as.binary.op == JOG_TOK_LT);
    CHECK(cmp->as.binary.lhs->kind == JOG_NODE_MEMBER);
    CHECK(cmp->as.binary.lhs->as.member.type_args.count == 0);
    close_parsed(&calls);
}

static void test_bindings_and_types(void) {
    Parsed parsed = parse_src("val limit = 10\n"
                              "val (q, r) = divmod(7, 2)\n"
                              "var buf: [256]u8\n"
                              "context alloc = defaultAllocator\n"
                              "type Celsius: f64\n"
                              "type Handle = bytes[8]\n"
                              "type Opt = *T?\n"
                              "type Inner = *(T?)\n"
                              "type View = []T?\n"
                              "type MapT = Map<str, List<i32>>\n"
                              "type Deep = F<G<H<i32>>>\n"
                              "type Fn = (str) -> () with Logger\n"
                              "type Esc = escaping () -> ()\n"
                              "type Cfn = extern (c.int) -> c.int\n"
                              "type Group = (i32)\n");
    CHECK(clean(&parsed));

    JogNode *limit = decl_at(&parsed, 0);
    CHECK(limit->kind == JOG_NODE_VAL && !limit->as.binding.destructure);
    CHECK(limit->as.binding.init->kind == JOG_NODE_INT_LIT);
    CHECK(strcmp(limit->as.binding.init->as.literal.text, "10") == 0);

    JogNode *pair = decl_at(&parsed, 1);
    CHECK(pair->as.binding.destructure);
    CHECK(pair->as.binding.names.count == 2);
    CHECK(pair->as.binding.init->kind == JOG_NODE_CALL);
    CHECK(pair->as.binding.init->as.call.args.count == 2);

    JogNode *buf = decl_at(&parsed, 2);
    CHECK(buf->kind == JOG_NODE_VAR && buf->as.binding.init == NULL);
    CHECK(buf->as.binding.type->kind == JOG_NODE_TYPE_ARRAY);
    CHECK(buf->as.binding.type->as.type_array.len->kind == JOG_NODE_INT_LIT);

    CHECK(decl_at(&parsed, 3)->kind == JOG_NODE_CONTEXT);

    JogNode *celsius = decl_at(&parsed, 4);
    CHECK(celsius->as.type_decl.distinct);
    CHECK(path_is(celsius->as.type_decl.target, "f64"));
    CHECK(!decl_at(&parsed, 5)->as.type_decl.distinct);
    CHECK(decl_at(&parsed, 5)->as.type_decl.target->kind == JOG_NODE_TYPE_BYTES);
    CHECK(decl_at(&parsed, 5)->as.type_decl.target->as.type_bytes.len != NULL);

    JogNode *opt = decl_at(&parsed, 6)->as.type_decl.target;
    CHECK(opt->kind == JOG_NODE_TYPE_OPTION);
    CHECK(opt->as.type_elem.elem->kind == JOG_NODE_TYPE_PTR);

    JogNode *inner = decl_at(&parsed, 7)->as.type_decl.target;
    CHECK(inner->kind == JOG_NODE_TYPE_PTR);
    CHECK(inner->as.type_elem.elem->kind == JOG_NODE_TYPE_OPTION);

    JogNode *view = decl_at(&parsed, 8)->as.type_decl.target;
    CHECK(view->kind == JOG_NODE_TYPE_SLICE);
    CHECK(view->as.type_elem.elem->kind == JOG_NODE_TYPE_OPTION);

    JogNode *map = decl_at(&parsed, 9)->as.type_decl.target;
    CHECK(path_is(map, "Map"));
    CHECK(map->as.type_name.args.count == 2);
    JogNode *list = map->as.type_name.args.items[1];
    CHECK(path_is(list, "List"));
    CHECK(list->as.type_name.args.count == 1);
    CHECK(path_is(list->as.type_name.args.items[0], "i32"));

    JogNode *deep = decl_at(&parsed, 10)->as.type_decl.target;
    CHECK(path_is(deep, "F"));
    CHECK(path_is(deep->as.type_name.args.items[0], "G"));
    CHECK(path_is(deep->as.type_name.args.items[0]->as.type_name.args.items[0], "H"));

    JogNode *fn = decl_at(&parsed, 11)->as.type_decl.target;
    CHECK(fn->kind == JOG_NODE_TYPE_FN);
    CHECK(fn->as.type_fn.params.count == 1);
    CHECK(fn->as.type_fn.ret->kind == JOG_NODE_TYPE_UNIT);
    CHECK(fn->as.type_fn.contexts.count == 1);
    CHECK(path_is(fn->as.type_fn.contexts.items[0], "Logger"));

    CHECK(decl_at(&parsed, 12)->as.type_decl.target->as.type_fn.escaping);
    JogNode *cfn = decl_at(&parsed, 13)->as.type_decl.target;
    CHECK(cfn->as.type_fn.is_extern && !cfn->as.type_fn.escaping);
    CHECK(path_is(decl_at(&parsed, 14)->as.type_decl.target, "i32"));
    close_parsed(&parsed);
}

static void test_aggregates(void) {
    Parsed parsed = parse_src("@packed\n"
                              "private(file) struct Header { magic: bytes[4], len: u32 = 0 }\n"
                              "struct Player { Entity\n name: str }\n"
                              "interface Shape {\n"
                              "    Named\n"
                              "    var pos: Point\n"
                              "    fn area(): f64\n"
                              "    fn *move(dx: f32, dy: f32)\n"
                              "}\n"
                              "interface Signed { ~i8 | ~i16 | ~i32 }\n"
                              "enum Ordering: i8 { Less = -1, Equal = 0, Greater }\n"
                              "enum Bad { Circle(r: f64) = 1\n Empty }\n"
                              "@link(\"SDL3\")\n"
                              "extern {\n"
                              "    fn SDL_Quit()\n"
                              "    fn SDL_Log(fmt: *const c.char, ...)\n"
                              "}\n"
                              "@opaque struct SDL_Window\n");
    CHECK(parsed.root != NULL);
    CHECK(ct_diag_error_count(parsed.diag) == 1);

    JogNode *header = decl_at(&parsed, 0);
    CHECK(header->kind == JOG_NODE_STRUCT);
    CHECK(header->visibility == JOG_VIS_FILE);
    CHECK(header->attrs.count == 1);
    CHECK(strcmp(header->attrs.items[0]->as.attribute.name, "packed") == 0);
    CHECK(header->as.aggregate.members.count == 2);
    JogNode *magic = header->as.aggregate.members.items[0];
    CHECK(magic->kind == JOG_NODE_FIELD);
    CHECK(magic->as.field.type->kind == JOG_NODE_TYPE_BYTES);
    CHECK(header->as.aggregate.members.items[1]->as.field.default_value != NULL);

    JogNode *player = decl_at(&parsed, 1);
    CHECK(player->as.aggregate.members.items[0]->kind == JOG_NODE_EMBED);
    CHECK(path_is(player->as.aggregate.members.items[0]->as.embed.type, "Entity"));

    JogNode *shape = decl_at(&parsed, 2);
    CHECK(shape->kind == JOG_NODE_INTERFACE);
    CHECK(shape->as.aggregate.members.count == 4);
    CHECK(shape->as.aggregate.members.items[0]->kind == JOG_NODE_EMBED);
    CHECK(shape->as.aggregate.members.items[1]->as.field.is_var);
    JogNode *move = shape->as.aggregate.members.items[3];
    CHECK(move->kind == JOG_NODE_FN && move->as.fn.receiver_is_ptr && move->as.fn.receiver == NULL);
    CHECK(move->as.fn.body == NULL);

    JogNode *set = decl_at(&parsed, 3)->as.aggregate.members.items[0];
    CHECK(set->kind == JOG_NODE_TYPE_SET);
    CHECK(set->as.type_set.terms.count == 3);
    CHECK(set->as.type_set.approximate && set->as.type_set.approximate[0]);
    CHECK(set->as.type_set.approximate[2]);

    JogNode *ordering = decl_at(&parsed, 4);
    CHECK(path_is(ordering->as.aggregate.tag_type, "i8"));
    JogNode *less = ordering->as.aggregate.members.items[0];
    CHECK(less->as.variant.value && less->as.variant.value->kind == JOG_NODE_UNARY);
    CHECK(ordering->as.aggregate.members.items[2]->as.variant.value == NULL);

    JogNode *bad = decl_at(&parsed, 5);
    CHECK(bad->as.aggregate.members.count == 2);
    CHECK(bad->as.aggregate.members.items[0]->as.variant.fields.count == 1);
    CHECK(bad->as.aggregate.members.items[0]->as.variant.value != NULL);

    JogNode *ext = decl_at(&parsed, 6);
    CHECK(ext->kind == JOG_NODE_EXTERN_BLOCK);
    CHECK(ext->attrs.count == 1);
    CHECK(ext->attrs.items[0]->as.attribute.has_parens);
    CHECK(ext->as.extern_block.fns.count == 2);
    JogNode *logf = ext->as.extern_block.fns.items[1];
    CHECK(logf->as.fn.is_extern && logf->as.fn.variadic);
    CHECK(logf->as.fn.params.count == 1);
    CHECK(logf->as.fn.params.items[0]->as.param.type->kind == JOG_NODE_TYPE_PTR);
    CHECK(logf->as.fn.params.items[0]->as.param.type->as.type_elem.is_const);
    CHECK(ext->as.extern_block.fns.items[0]->attrs.count == 0);

    JogNode *window = decl_at(&parsed, 7);
    CHECK(window->kind == JOG_NODE_STRUCT);
    CHECK(window->as.aggregate.members.count == 0);
    CHECK(strcmp(window->attrs.items[0]->as.attribute.name, "opaque") == 0);
    close_parsed(&parsed);
}

static void test_precedence_and_exprs(void) {
    Parsed parsed = parse_src("val a = x & MASK == 0\n"
                              "val b = n?.len ?: 0\n"
                              "val c = a <=> b == .Equal\n"
                              "val d = Point(x: 1, y: 2)\n"
                              "val e = \"hi ${name}\"\n"
                              "val f = items[1..]\n"
                              "val g = p!!\n"
                              "val h = n as bytes[4]\n"
                              "val i = a < b < c\n");
    CHECK(ct_diag_error_count(parsed.diag) == 1);
    JogNode *a = decl_at(&parsed, 0)->as.binding.init;
    CHECK(a->kind == JOG_NODE_BINARY && a->as.binary.op == JOG_TOK_EQ);
    CHECK(a->as.binary.lhs->as.binary.op == JOG_TOK_AMP);

    JogNode *b = decl_at(&parsed, 1)->as.binding.init;
    CHECK(b->as.binary.op == JOG_TOK_ELVIS);
    CHECK(b->as.binary.lhs->kind == JOG_NODE_MEMBER && b->as.binary.lhs->as.member.safe);

    JogNode *c = decl_at(&parsed, 2)->as.binding.init;
    CHECK(c->as.binary.op == JOG_TOK_EQ);
    CHECK(c->as.binary.lhs->as.binary.op == JOG_TOK_CMP3);
    CHECK(c->as.binary.rhs->kind == JOG_NODE_DOT_VARIANT);

    JogNode *d = decl_at(&parsed, 3)->as.binding.init;
    CHECK(d->kind == JOG_NODE_CALL);
    CHECK(d->as.call.args.items[0]->as.arg.name && strcmp(d->as.call.args.items[0]->as.arg.name, "x") == 0);
    CHECK(d->as.call.args.items[1]->as.arg.name && strcmp(d->as.call.args.items[1]->as.arg.name, "y") == 0);

    JogNode *e = decl_at(&parsed, 4)->as.binding.init;
    CHECK(e->kind == JOG_NODE_INTERP);
    CHECK(e->as.interp.parts.count == 3);
    CHECK(e->as.interp.parts.items[1]->kind == JOG_NODE_IDENT);

    CHECK(decl_at(&parsed, 5)->as.binding.init->kind == JOG_NODE_SLICE);
    CHECK(decl_at(&parsed, 6)->as.binding.init->kind == JOG_NODE_FORCE);
    JogNode *h = decl_at(&parsed, 7)->as.binding.init;
    CHECK(h->kind == JOG_NODE_CAST && h->as.cast.op == JOG_TOK_KW_AS);
    CHECK(h->as.cast.type->kind == JOG_NODE_TYPE_BYTES);
    close_parsed(&parsed);
}

static void test_recovery(void) {
    Parsed parsed = parse_src("fn broken\nval x = 1\nstruct S {\n    123\n    y: i32\n}\n");
    CHECK(parsed.root != NULL);
    CHECK(ct_diag_error_count(parsed.diag) >= 2);
    CHECK(parsed.root->as.file.decls.count == 3);
    CHECK(decl_at(&parsed, 0)->kind == JOG_NODE_FN);
    CHECK(strcmp(decl_at(&parsed, 0)->as.fn.name, "broken") == 0);
    CHECK(decl_at(&parsed, 1)->kind == JOG_NODE_VAL);
    CHECK(strcmp(decl_at(&parsed, 1)->as.binding.names.items[0], "x") == 0);
    JogNode *s = decl_at(&parsed, 2);
    CHECK(s->kind == JOG_NODE_STRUCT);
    CHECK(s->as.aggregate.members.count == 1);
    CHECK(strcmp(s->as.aggregate.members.items[0]->as.field.name, "y") == 0);
    close_parsed(&parsed);

    /* quote is post-V1. Recovery still keeps the next statement. */
    Parsed later = parse_src("fn f() {\n    quote { return 1 }\n    val y = 2\n}\n");
    CHECK(later.root != NULL);
    CHECK(ct_diag_error_count(later.diag) >= 1);
    JogNode *body = decl_at(&later, 0)->as.fn.body;
    CHECK(body && body->kind == JOG_NODE_BLOCK);
    CHECK(body->as.block.stmts.count == 1);
    CHECK(body->as.block.stmts.items[0]->kind == JOG_NODE_VAL);
    close_parsed(&later);
}

static JogNode *fn_block(Parsed *parsed, size_t index) {
    JogNode *fn = decl_at(parsed, index);
    if (!fn || fn->kind != JOG_NODE_FN) {
        return NULL;
    }
    return fn->as.fn.body;
}

static JogNode *block_stmt(const JogNode *block, size_t index) {
    if (!block || block->kind != JOG_NODE_BLOCK || index >= block->as.block.stmts.count) {
        return NULL;
    }
    return block->as.block.stmts.items[index];
}

static void test_statements(void) {
    Parsed hello = parse_src("fn main() {\n"
                             "    val greeting = c\"Hello from Jog!\"\n"
                             "    if puts(greeting) < 0 {\n"
                             "        return\n"
                             "    }\n"
                             "}\n");
    CHECK(clean(&hello));
    JogNode *hi = block_stmt(fn_block(&hello, 0), 1);
    CHECK(hi && hi->kind == JOG_NODE_EXPR_STMT);
    JogNode *iff = hi->as.expr_stmt.expr;
    CHECK(iff && iff->kind == JOG_NODE_IF);
    CHECK(iff->as.if_.cond->kind == JOG_NODE_BINARY);
    CHECK(iff->as.if_.cond->as.binary.op == JOG_TOK_LT);
    CHECK(iff->as.if_.cond->as.binary.lhs->kind == JOG_NODE_CALL);
    CHECK(iff->as.if_.then_block->as.block.stmts.items[0]->kind == JOG_NODE_RETURN);
    CHECK(iff->as.if_.else_branch == NULL);
    close_parsed(&hello);

    Parsed maxf = parse_src("fn max(a: i32, b: i32): i32 = if a > b { a } else { b }\n"
                            "val m = if a > 0 { a }\n"
                            "else { 0 }\n");
    CHECK(clean(&maxf));
    JogNode *maxfn = decl_at(&maxf, 0);
    CHECK(maxfn->as.fn.expr_body);
    CHECK(maxfn->as.fn.body->kind == JOG_NODE_IF);
    CHECK(maxfn->as.fn.body->as.if_.else_branch->kind == JOG_NODE_BLOCK);
    JogNode *m = decl_at(&maxf, 1)->as.binding.init;
    CHECK(m->kind == JOG_NODE_IF);
    CHECK(m->as.if_.else_branch && m->as.if_.else_branch->kind == JOG_NODE_BLOCK);
    close_parsed(&maxf);

    Parsed chain = parse_src("fn f() {\n"
                             "    if a > 0 {\n"
                             "        return 1\n"
                             "    } else if a < 0 {\n"
                             "        return -1\n"
                             "    } else {\n"
                             "        return 0\n"
                             "    }\n"
                             "}\n");
    CHECK(clean(&chain));
    JogNode *top = block_stmt(fn_block(&chain, 0), 0)->as.expr_stmt.expr;
    CHECK(top->kind == JOG_NODE_IF);
    JogNode *mid = top->as.if_.else_branch;
    CHECK(mid && mid->kind == JOG_NODE_IF);
    CHECK(mid->as.if_.else_branch->kind == JOG_NODE_BLOCK);
    close_parsed(&chain);

    Parsed loops = parse_src("fn f() {\n"
                             "    for { break }\n"
                             "    for ready { continue }\n"
                             "    for i in 0..n { }\n"
                             "    for i, x in items { }\n"
                             "    defer free(buf)\n"
                             "    defer { free(a) }\n"
                             "    errdefer free(buf)\n"
                             "    with (logger) { log(msg) }\n"
                             "    x = 1\n"
                             "    x += 2\n"
                             "    *p = 3\n"
                             "    (n, err) = g()\n"
                             "    (n, _) = g()\n"
                             "}\n");
    CHECK(clean(&loops));
    JogNode *body = fn_block(&loops, 0);
    JogNode *forever = block_stmt(body, 0);
    CHECK(forever->kind == JOG_NODE_FOR && forever->as.for_.form == JOG_FOR_FOREVER);
    CHECK(forever->as.for_.expr == NULL);
    CHECK(block_stmt(forever->as.for_.body, 0)->kind == JOG_NODE_BREAK);
    JogNode *cond = block_stmt(body, 1);
    CHECK(cond->as.for_.form == JOG_FOR_COND);
    CHECK(cond->as.for_.expr->kind == JOG_NODE_IDENT);
    CHECK(block_stmt(cond->as.for_.body, 0)->kind == JOG_NODE_CONTINUE);
    JogNode *one = block_stmt(body, 2);
    CHECK(one->as.for_.form == JOG_FOR_IN);
    CHECK(strcmp(one->as.for_.first, "i") == 0);
    CHECK(one->as.for_.second == NULL);
    CHECK(one->as.for_.expr->as.binary.op == JOG_TOK_DOTDOT);
    JogNode *two = block_stmt(body, 3);
    CHECK(strcmp(two->as.for_.first, "i") == 0 && strcmp(two->as.for_.second, "x") == 0);
    JogNode *d1 = block_stmt(body, 4);
    CHECK(d1->kind == JOG_NODE_DEFER && d1->as.defer.body->kind == JOG_NODE_EXPR_STMT);
    JogNode *d2 = block_stmt(body, 5);
    CHECK(d2->kind == JOG_NODE_DEFER && d2->as.defer.body->kind == JOG_NODE_BLOCK);
    CHECK(block_stmt(body, 6)->kind == JOG_NODE_ERRDEFER);
    JogNode *with = block_stmt(body, 7);
    CHECK(with->kind == JOG_NODE_WITH);
    CHECK(with->as.with.values.count == 1);
    CHECK(with->as.with.body->kind == JOG_NODE_BLOCK);
    JogNode *set = block_stmt(body, 8);
    CHECK(set->kind == JOG_NODE_ASSIGN && set->as.assign.op == JOG_TOK_ASSIGN);
    CHECK(block_stmt(body, 9)->as.assign.op == JOG_TOK_PLUS_ASSIGN);
    CHECK(block_stmt(body, 10)->as.assign.target->kind == JOG_NODE_UNARY);
    JogNode *dest = block_stmt(body, 11);
    CHECK(dest->kind == JOG_NODE_DESTRUCTURE);
    CHECK(dest->as.destructure.targets.count == 2);
    CHECK(strcmp(dest->as.destructure.targets.items[0]->as.ident.name, "n") == 0);
    CHECK(strcmp(block_stmt(body, 12)->as.destructure.targets.items[1]->as.ident.name, "_") == 0);
    close_parsed(&loops);

    Parsed lits = parse_src("val v = [4]i32(1, 2, 3, 4)\n"
                            "val w = [_]i32(1, 2, 3)\n"
                            "val names = []const str(\"a\", \"b\")\n"
                            "val nested = [2][2]f32((1, 0), (0, 1))\n"
                            "val block = { 1 }\n");
    CHECK(clean(&lits));
    JogNode *v = decl_at(&lits, 0)->as.binding.init;
    CHECK(v->kind == JOG_NODE_ARRAY_LIT);
    CHECK(v->as.array_lit.type->kind == JOG_NODE_TYPE_ARRAY);
    CHECK(v->as.array_lit.type->as.type_array.infer_len == false);
    CHECK(v->as.array_lit.elems.count == 4);
    JogNode *w = decl_at(&lits, 1)->as.binding.init;
    CHECK(w->as.array_lit.type->as.type_array.infer_len);
    CHECK(w->as.array_lit.elems.count == 3);
    JogNode *names = decl_at(&lits, 2)->as.binding.init;
    CHECK(names->as.array_lit.type->kind == JOG_NODE_TYPE_SLICE);
    CHECK(names->as.array_lit.type->as.type_elem.is_const);
    CHECK(names->as.array_lit.elems.count == 2);
    JogNode *nested = decl_at(&lits, 3)->as.binding.init;
    CHECK(nested->as.array_lit.type->as.type_array.elem->kind == JOG_NODE_TYPE_ARRAY);
    CHECK(nested->as.array_lit.elems.items[0]->as.arg.value->kind == JOG_NODE_TUPLE);
    JogNode *block = decl_at(&lits, 4)->as.binding.init;
    CHECK(block->kind == JOG_NODE_BLOCK);
    CHECK(block->as.block.stmts.items[0]->kind == JOG_NODE_EXPR_STMT);
    close_parsed(&lits);

    Parsed lams = parse_src("fn f() {\n"
                            "    items.each { print(it) }\n"
                            "    repeat(3) { i -> print(i) }\n"
                            "    items.fold(0) { acc, x -> acc + x }\n"
                            "    log(msg) with (logger) { write(msg) }\n"
                            "    if items.each { return }\n"
                            "    if (items.each { print(it) }) { return }\n"
                            "    items.each\n"
                            "    { print(it) }\n"
                            "}\n");
    CHECK(clean(&lams));
    JogNode *lb = fn_block(&lams, 0);
    JogNode *each = block_stmt(lb, 0)->as.expr_stmt.expr;
    CHECK(each->kind == JOG_NODE_CALL);
    CHECK(each->as.call.args.count == 0);
    CHECK(each->as.call.callee->kind == JOG_NODE_MEMBER);
    CHECK(each->as.call.trailing->kind == JOG_NODE_LAMBDA);
    CHECK(each->as.call.trailing->as.lambda.implicit_it);
    CHECK(each->as.call.trailing->as.lambda.params.count == 0);
    JogNode *rep = block_stmt(lb, 1)->as.expr_stmt.expr;
    CHECK(rep->as.call.args.count == 1);
    CHECK(rep->as.call.trailing->as.lambda.implicit_it == false);
    CHECK(strcmp(rep->as.call.trailing->as.lambda.params.items[0]->as.param.name, "i") == 0);
    JogNode *fold = block_stmt(lb, 2)->as.expr_stmt.expr;
    CHECK(fold->as.call.trailing->as.lambda.params.count == 2);
    CHECK(block_stmt(fold->as.call.trailing->as.lambda.body, 0)->as.expr_stmt.expr->kind == JOG_NODE_BINARY);
    JogNode *logged = block_stmt(lb, 3)->as.expr_stmt.expr;
    CHECK(logged->as.call.contexts.count == 1);
    CHECK(logged->as.call.trailing->as.lambda.implicit_it);
    JogNode *head = block_stmt(lb, 4)->as.expr_stmt.expr;
    CHECK(head->kind == JOG_NODE_IF);
    CHECK(head->as.if_.cond->kind == JOG_NODE_MEMBER);
    CHECK(block_stmt(head->as.if_.then_block, 0)->kind == JOG_NODE_RETURN);
    JogNode *wrapped = block_stmt(lb, 5)->as.expr_stmt.expr;
    CHECK(wrapped->as.if_.cond->kind == JOG_NODE_CALL);
    CHECK(wrapped->as.if_.cond->as.call.trailing->kind == JOG_NODE_LAMBDA);
    JogNode *split = block_stmt(lb, 6)->as.expr_stmt.expr;
    CHECK(split->kind == JOG_NODE_MEMBER);
    CHECK(block_stmt(lb, 7)->as.expr_stmt.expr->kind == JOG_NODE_BLOCK);
    close_parsed(&lams);

    Parsed attrs = parse_src("fn f() {\n"
                             "    @unchecked {\n"
                             "        x = 1\n"
                             "    }\n"
                             "    @comptime(x)\n"
                             "}\n");
    CHECK(clean(&attrs));
    JogNode *ab = block_stmt(fn_block(&attrs, 0), 0);
    CHECK(ab->kind == JOG_NODE_ATTR_BLOCK);
    CHECK(ab->attrs.count == 1);
    CHECK(strcmp(ab->attrs.items[0]->as.attribute.name, "unchecked") == 0);
    CHECK(block_stmt(ab->as.attr_block.body, 0)->kind == JOG_NODE_ASSIGN);
    JogNode *built = block_stmt(fn_block(&attrs, 0), 1)->as.expr_stmt.expr;
    CHECK(built->kind == JOG_NODE_BUILTIN);
    CHECK(strcmp(built->as.builtin.name, "comptime") == 0);
    close_parsed(&attrs);

    /* quote does not end a line, so the following 'x' is part of that
       statement. when and go parse; val is the next statement. */
    Parsed rejected = parse_src("fn f() {\n"
                                "    when x { else -> 1 }\n"
                                "    go x\n"
                                "    quote\n"
                                "    x\n"
                                "    val y = 2\n"
                                "}\n"
                                "val z = when y\n");
    CHECK(rejected.root != NULL);
    CHECK(ct_diag_error_count(rejected.diag) >= 2);
    JogNode *rejected_body = fn_block(&rejected, 0);
    CHECK(block_stmt(rejected_body, 0)->as.expr_stmt.expr->kind == JOG_NODE_WHEN);
    CHECK(block_stmt(rejected_body, 1)->as.expr_stmt.expr->kind == JOG_NODE_GO);
    JogNode *kept = block_stmt(rejected_body, 2);
    CHECK(kept && kept->kind == JOG_NODE_VAL);
    CHECK(decl_at(&rejected, 1)->kind == JOG_NODE_VAL);
    close_parsed(&rejected);
}

static void test_attributes_and_block(void) {
    Parsed parsed = parse_src("@a()\nfn f() = 1\nfn g() {\n    return a / b, a % b\n}\n");
    CHECK(clean(&parsed));
    CHECK(decl_at(&parsed, 0)->attrs.count == 1);
    CHECK(decl_at(&parsed, 0)->attrs.items[0]->as.attribute.has_parens);
    JogNode *ret = decl_at(&parsed, 1)->as.fn.body->as.block.stmts.items[0];
    CHECK(ret->kind == JOG_NODE_RETURN);
    CHECK(ret->as.ret.values.count == 2);
    close_parsed(&parsed);
}

static int same_text(const char *got, const char *expect) {
    size_t gi = 0;
    size_t ei = 0;
    while (got[gi] || expect[ei]) {
        if (got[gi] == '\r') {
            gi++;
            continue;
        }
        if (expect[ei] == '\r') {
            ei++;
            continue;
        }
        if (got[gi] != expect[ei]) {
            return 0;
        }
        if (got[gi] == '\0') {
            return 1;
        }
        gi++;
        ei++;
    }
    return 1;
}

static JogNode *expr_at(Parsed *parsed, size_t index) {
    JogNode *decl = decl_at(parsed, index);
    if (!decl || (decl->kind != JOG_NODE_VAL && decl->kind != JOG_NODE_VAR)) {
        return NULL;
    }
    return decl->as.binding.init;
}

static JogNode *arm_cond(const JogNode *arm, size_t index) {
    if (!arm || arm->kind != JOG_NODE_WHEN_ARM || index >= arm->as.when_arm.conds.count) {
        return NULL;
    }
    return arm->as.when_arm.conds.items[index];
}

static void test_forms(void) {
    Parsed wh = parse_src("val kind = when code {\n"
                          "    200, 204 -> \"ok\"\n"
                          "    in 300..400 -> \"redirect\"\n"
                          "    else -> \"error\"\n"
                          "}\n"
                          "val area = when shape {\n"
                          "    Circle(r) -> r * r\n"
                          "    Rect(w, h) -> w * h\n"
                          "    Rect(h: height) -> height\n"
                          "    Empty -> 0\n"
                          "    else -> { 1 }\n"
                          "}\n"
                          "val nested = when event {\n"
                          "    Some(Click(Point(x: 0, y))) -> y\n"
                          "    Some(Key(code: in 'a'..'z')) -> code\n"
                          "    Some(_) -> 0\n"
                          "    None -> 1\n"
                          "}\n"
                          "val cond = when {\n"
                          "    x < 0 -> \"negative\"\n"
                          "    x == 0 -> \"zero\"\n"
                          "    else -> \"positive\"\n"
                          "}\n"
                          "val typed = when s {\n"
                          "    is Circle -> s.r\n"
                          "    is Rect -> 0\n"
                          "    else -> 1\n"
                          "}\n");
    CHECK(clean(&wh));
    JogNode *kind = expr_at(&wh, 0);
    CHECK(kind && kind->kind == JOG_NODE_WHEN);
    CHECK(strcmp(kind->as.when.subject->as.ident.name, "code") == 0);
    CHECK(kind->as.when.arms.count == 3);
    JogNode *nums = kind->as.when.arms.items[0];
    CHECK(nums->as.when_arm.conds.count == 2);
    CHECK(arm_cond(nums, 0)->kind == JOG_NODE_PAT_EXPR);
    CHECK(arm_cond(nums, 0)->as.pat_expr.expr->kind == JOG_NODE_INT_LIT);
    CHECK(arm_cond(kind->as.when.arms.items[1], 0)->kind == JOG_NODE_PAT_IN);
    CHECK(kind->as.when.arms.items[1]->as.when_arm.conds.items[0]->as.pat_expr.expr->as.binary.op == JOG_TOK_DOTDOT);
    CHECK(kind->as.when.arms.items[2]->as.when_arm.conds.count == 0);
    CHECK(kind->as.when.arms.items[2]->as.when_arm.body->kind == JOG_NODE_STRING_LIT);

    JogNode *area = expr_at(&wh, 1);
    JogNode *circle = area->as.when.arms.items[0];
    CHECK(arm_cond(circle, 0)->kind == JOG_NODE_PAT_VARIANT);
    CHECK(strcmp(arm_cond(circle, 0)->as.pat_variant.name, "Circle") == 0);
    CHECK(arm_cond(circle, 0)->as.pat_variant.fields.items[0]->as.pat_field.pattern->kind == JOG_NODE_PAT_BIND);
    JogNode *named = arm_cond(area->as.when.arms.items[2], 0);
    CHECK(named->kind == JOG_NODE_PAT_VARIANT);
    CHECK(named->as.pat_variant.fields.items[0]->as.pat_field.name != NULL);
    CHECK(strcmp(named->as.pat_variant.fields.items[0]->as.pat_field.name, "h") == 0);
    CHECK(arm_cond(area->as.when.arms.items[3], 0)->kind == JOG_NODE_PAT_BIND);
    CHECK(area->as.when.arms.items[4]->as.when_arm.body->kind == JOG_NODE_BLOCK);

    JogNode *nested = expr_at(&wh, 2);
    JogNode *click = arm_cond(nested->as.when.arms.items[0], 0);
    CHECK(click->kind == JOG_NODE_PAT_VARIANT);
    JogNode *point = click->as.pat_variant.fields.items[0]->as.pat_field.pattern;
    CHECK(point->kind == JOG_NODE_PAT_VARIANT);
    CHECK(strcmp(point->as.pat_variant.name, "Click") == 0);
    JogNode *inner = point->as.pat_variant.fields.items[0]->as.pat_field.pattern;
    CHECK(strcmp(inner->as.pat_variant.name, "Point") == 0);
    CHECK(inner->as.pat_variant.fields.items[0]->as.pat_field.pattern->kind == JOG_NODE_PAT_EXPR);
    CHECK(inner->as.pat_variant.fields.items[1]->as.pat_field.pattern->kind == JOG_NODE_PAT_BIND);
    JogNode *key = arm_cond(nested->as.when.arms.items[1], 0);
    JogNode *code = key->as.pat_variant.fields.items[0]->as.pat_field.pattern;
    code = code->as.pat_variant.fields.items[0]->as.pat_field.pattern;
    CHECK(code->kind == JOG_NODE_PAT_IN);
    CHECK(arm_cond(nested->as.when.arms.items[2], 0)->as.pat_variant.fields.items[0]->as.pat_field.pattern->kind ==
          JOG_NODE_PAT_WILDCARD);

    JogNode *cond = expr_at(&wh, 3);
    CHECK(cond->as.when.subject == NULL);
    CHECK(arm_cond(cond->as.when.arms.items[0], 0)->kind == JOG_NODE_PAT_EXPR);
    CHECK(arm_cond(cond->as.when.arms.items[0], 0)->as.pat_expr.expr->as.binary.op == JOG_TOK_LT);
    CHECK(arm_cond(expr_at(&wh, 4)->as.when.arms.items[0], 0)->kind == JOG_NODE_PAT_IS);
    close_parsed(&wh);

    Parsed lam = parse_src("val double = (x: i32) -> { x * 2 }\n"
                           "val add = (a: i32, b: i32): i32 -> a + b\n"
                           "val noop = () -> {}\n"
                           "val paren = (a + b) * c\n"
                           "val pair = (a, b)\n"
                           "fn f() {\n"
                           "    val g = () -> { return 1 }\n"
                           "}\n");
    CHECK(clean(&lam));
    JogNode *dbl = expr_at(&lam, 0);
    CHECK(dbl->kind == JOG_NODE_LAMBDA);
    CHECK(dbl->as.lambda.implicit_it == false);
    CHECK(dbl->as.lambda.expr_body == false);
    CHECK(strcmp(dbl->as.lambda.params.items[0]->as.param.name, "x") == 0);
    CHECK(dbl->as.lambda.params.items[0]->as.param.type->kind == JOG_NODE_TYPE_NAME);
    JogNode *add = expr_at(&lam, 1);
    CHECK(add->as.lambda.expr_body);
    CHECK(add->as.lambda.params.count == 2);
    CHECK(add->as.lambda.ret->kind == JOG_NODE_TYPE_NAME);
    CHECK(add->as.lambda.body->kind == JOG_NODE_BINARY);
    CHECK(expr_at(&lam, 2)->kind == JOG_NODE_LAMBDA);
    CHECK(expr_at(&lam, 2)->as.lambda.params.count == 0);
    CHECK(expr_at(&lam, 2)->as.lambda.body->kind == JOG_NODE_BLOCK);
    CHECK(expr_at(&lam, 3)->kind == JOG_NODE_BINARY);
    CHECK(expr_at(&lam, 4)->kind == JOG_NODE_TUPLE);
    JogNode *ret = block_stmt(block_stmt(fn_block(&lam, 5), 0)->as.binding.init->as.lambda.body, 0);
    CHECK(ret->kind == JOG_NODE_RETURN);
    close_parsed(&lam);

    Parsed go = parse_src("fn f() {\n"
                          "    go fetch(url)\n"
                          "    go { process(item) }\n"
                          "    go with (requestArena, reqLogger) { handle(conn) }\n"
                          "    go(stack: 1 << 20) { work() }\n"
                          "    go(stack: 8) with (logger) fetch(url)\n"
                          "}\n"
                          "val t = go { compute() }\n");
    CHECK(clean(&go));
    JogNode *gb = fn_block(&go, 0);
    JogNode *call = block_stmt(gb, 0)->as.expr_stmt.expr;
    CHECK(call->kind == JOG_NODE_GO);
    CHECK(call->as.go.body->kind == JOG_NODE_CALL);
    CHECK(call->as.go.args.count == 0);
    JogNode *block = block_stmt(gb, 1)->as.expr_stmt.expr;
    CHECK(block->as.go.body->kind == JOG_NODE_BLOCK);
    JogNode *withgo = block_stmt(gb, 2)->as.expr_stmt.expr;
    CHECK(withgo->as.go.contexts.count == 2);
    CHECK(withgo->as.go.body->kind == JOG_NODE_BLOCK);
    JogNode *stack = block_stmt(gb, 3)->as.expr_stmt.expr;
    CHECK(stack->as.go.args.count == 1);
    CHECK(strcmp(stack->as.go.args.items[0]->as.arg.name, "stack") == 0);
    CHECK(stack->as.go.args.items[0]->as.arg.value->as.binary.op == JOG_TOK_SHL);
    JogNode *both = block_stmt(gb, 4)->as.expr_stmt.expr;
    CHECK(both->as.go.args.count == 1);
    CHECK(both->as.go.contexts.count == 1);
    CHECK(both->as.go.body->kind == JOG_NODE_CALL);
    CHECK(expr_at(&go, 1)->kind == JOG_NODE_GO);
    close_parsed(&go);

    Parsed gen = parse_src("val xs = List<i32>()\n"
                           "val ys = List<List<i32>>()\n"
                           "val ch = Chan<i32>.init(16)\n"
                           "val n = max<f32>(1, 2.5)\n"
                           "val cmp = a < b\n"
                           "val call = a < b > (c)\n"
                           "val chain = a < b > c\n"
                           "val words = @comptime(makeCrcTable())\n"
                           "val blob = @embed(\"table.bin\")\n"
                           "val sum = @add(self, o)\n"
                           "fn f(): (bytes, error?) { return bytes(), err }\n"
                           "val p = Point(x: 1, y: 2)\n");
    CHECK(ct_diag_error_count(gen.diag) == 1);
    JogNode *xs = expr_at(&gen, 0);
    CHECK(xs->kind == JOG_NODE_CALL);
    CHECK(xs->as.call.callee->kind == JOG_NODE_IDENT);
    CHECK(xs->as.call.callee->as.ident.type_args.count == 1);
    JogNode *ys = expr_at(&gen, 1);
    CHECK(ys->as.call.callee->as.ident.type_args.count == 1);
    JogNode *nested_arg = ys->as.call.callee->as.ident.type_args.items[0];
    CHECK(nested_arg->kind == JOG_NODE_TYPE_NAME);
    CHECK(nested_arg->as.type_name.args.count == 1);
    JogNode *ch = expr_at(&gen, 2);
    CHECK(ch->kind == JOG_NODE_CALL);
    CHECK(ch->as.call.callee->kind == JOG_NODE_MEMBER);
    CHECK(ch->as.call.callee->as.member.base->as.ident.type_args.count == 1);
    JogNode *mx = expr_at(&gen, 3);
    CHECK(mx->kind == JOG_NODE_CALL);
    CHECK(mx->as.call.callee->as.ident.type_args.items[0]->kind == JOG_NODE_TYPE_NAME);
    CHECK(expr_at(&gen, 4)->kind == JOG_NODE_BINARY);
    CHECK(expr_at(&gen, 4)->as.binary.op == JOG_TOK_LT);
    JogNode *generic_call = expr_at(&gen, 5);
    CHECK(generic_call->kind == JOG_NODE_CALL);
    CHECK(strcmp(generic_call->as.call.callee->as.ident.name, "a") == 0);
    CHECK(generic_call->as.call.callee->as.ident.type_args.count == 1);
    CHECK(expr_at(&gen, 7)->kind == JOG_NODE_BUILTIN);
    CHECK(strcmp(expr_at(&gen, 7)->as.builtin.name, "comptime") == 0);
    CHECK(strcmp(expr_at(&gen, 8)->as.builtin.name, "embed") == 0);
    CHECK(expr_at(&gen, 9)->kind == JOG_NODE_BUILTIN);
    JogNode *retb = decl_at(&gen, 10)->as.fn.body->as.block.stmts.items[0];
    CHECK(retb->kind == JOG_NODE_RETURN);
    CHECK(retb->as.ret.values.items[0]->kind == JOG_NODE_CALL);
    CHECK(strcmp(retb->as.ret.values.items[0]->as.call.callee->as.ident.name, "bytes") == 0);
    CHECK(expr_at(&gen, 11)->kind == JOG_NODE_CALL);
    close_parsed(&gen);
}

static void test_rules(void) {
    /* Rule 1: '{' in a head opens the body. A trailing lambda must be parenthesized. */
    Parsed heads = parse_src("fn f() {\n"
                             "    if items.each { return }\n"
                             "    when items.each { else -> 1 }\n"
                             "    for items.each { }\n"
                             "    with (items.each) { }\n"
                             "    if (items.each { print(it) }) { return }\n"
                             "    with ((items.each { print(it) })) { }\n"
                             "}\n");
    CHECK(clean(&heads));
    JogNode *hb = fn_block(&heads, 0);
    JogNode *iff = block_stmt(hb, 0)->as.expr_stmt.expr;
    CHECK(iff->kind == JOG_NODE_IF);
    CHECK(iff->as.if_.cond->kind == JOG_NODE_MEMBER);
    JogNode *when = block_stmt(hb, 1)->as.expr_stmt.expr;
    CHECK(when->kind == JOG_NODE_WHEN);
    CHECK(when->as.when.subject->kind == JOG_NODE_MEMBER);
    CHECK(block_stmt(hb, 2)->as.for_.expr->kind == JOG_NODE_MEMBER);
    CHECK(block_stmt(hb, 3)->kind == JOG_NODE_WITH);
    JogNode *wrapped = block_stmt(hb, 4)->as.expr_stmt.expr;
    CHECK(wrapped->as.if_.cond->kind == JOG_NODE_CALL);
    CHECK(wrapped->as.if_.cond->as.call.trailing->kind == JOG_NODE_LAMBDA);
    JogNode *with = block_stmt(hb, 5);
    CHECK(with->kind == JOG_NODE_WITH);
    CHECK(with->as.with.values.items[0]->kind == JOG_NODE_CALL);
    close_parsed(&heads);

    Parsed bare = parse_src("fn f() {\n    with (items.each { print(it) }) { }\n}\n");
    CHECK(ct_diag_error_count(bare.diag) >= 1);
    close_parsed(&bare);

    /* Rule 2 is covered by test_forms (type arguments versus '<', including '>>'). */

    /* Rule 3 is the parenthesized lambda versus '(a + b)' in test_forms. */

    /* Rule 4: a '{' after a newline is a block, not a trailing lambda. */
    Parsed split = parse_src("fn f() {\n    items.each\n    { print(it) }\n}\n");
    CHECK(clean(&split));
    CHECK(block_stmt(fn_block(&split, 0), 0)->as.expr_stmt.expr->kind == JOG_NODE_MEMBER);
    CHECK(block_stmt(fn_block(&split, 0), 1)->as.expr_stmt.expr->kind == JOG_NODE_BLOCK);
    close_parsed(&split);

    /* Rule 5: call 'with' is postfix; a statement 'with (' is a with statement. */
    Parsed withs = parse_src("fn f() {\n    log(msg) with (logger)\n    with (logger) { log(msg) }\n}\n");
    CHECK(clean(&withs));
    JogNode *post = block_stmt(fn_block(&withs, 0), 0)->as.expr_stmt.expr;
    CHECK(post->kind == JOG_NODE_CALL);
    CHECK(post->as.call.contexts.count == 1);
    CHECK(block_stmt(fn_block(&withs, 0), 1)->kind == JOG_NODE_WITH);
    close_parsed(&withs);

    /* Rule 6 is semantic: return inside a lambda is that lambda's return.
       test_forms checks the RETURN node sits in the lambda body. */

    /* Rule 7: the parser always builds a variant pattern for IDENT(...). */
    Parsed pats = parse_src("val v = when shape {\n"
                            "    Circle(r) -> r\n"
                            "    Empty -> 0\n"
                            "    Point(1, 2) -> 3\n"
                            "    Color.Red -> 4\n"
                            "    null -> 5\n"
                            "}\n");
    CHECK(clean(&pats));
    JogNode *pv = expr_at(&pats, 0);
    CHECK(arm_cond(pv->as.when.arms.items[0], 0)->kind == JOG_NODE_PAT_VARIANT);
    CHECK(arm_cond(pv->as.when.arms.items[1], 0)->kind == JOG_NODE_PAT_BIND);
    CHECK(arm_cond(pv->as.when.arms.items[2], 0)->kind == JOG_NODE_PAT_VARIANT);
    CHECK(arm_cond(pv->as.when.arms.items[3], 0)->kind == JOG_NODE_PAT_EXPR);
    CHECK(arm_cond(pv->as.when.arms.items[3], 0)->as.pat_expr.expr->kind == JOG_NODE_MEMBER);
    CHECK(arm_cond(pv->as.when.arms.items[4], 0)->as.pat_expr.expr->kind == JOG_NODE_NULL_LIT);
    close_parsed(&pats);

    /* Rule 8: a leading dot continues the previous line. when arms omit the dot. */
    Parsed dots = parse_src("val n = items\n"
                            "    .count()\n"
                            "val e = .Empty\n");
    CHECK(clean(&dots));
    CHECK(expr_at(&dots, 0)->kind == JOG_NODE_CALL);
    CHECK(expr_at(&dots, 0)->as.call.callee->kind == JOG_NODE_MEMBER);
    CHECK(expr_at(&dots, 1)->kind == JOG_NODE_DOT_VARIANT);
    close_parsed(&dots);

    /* Rule 9: '[' starts an array literal where an operand is expected, and an index after one. */
    Parsed brackets = parse_src("val v = [4]i32(1, 2)\n"
                                "val w = a[1]\n"
                                "val s = a[1..]\n");
    CHECK(clean(&brackets));
    CHECK(expr_at(&brackets, 0)->kind == JOG_NODE_ARRAY_LIT);
    CHECK(expr_at(&brackets, 1)->kind == JOG_NODE_INDEX);
    CHECK(expr_at(&brackets, 2)->kind == JOG_NODE_SLICE);
    close_parsed(&brackets);
}

/* Language-note fences are fragments. Placeholders are scrubbed, a bodyless
   top-level signature grows an empty body, and declarations are parsed
   separately from statements. Statements are wrapped in a function.
   Lines that are only function types become type aliases. */
typedef struct NoteScan {
    int brace;
    int paren;
    int bracket;
    int saw_brace;
    int saw_base_eq;
    int ends_base_eq;
} NoteScan;

static int note_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int note_starts_word(const char *s, const char *end, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(end - s) < n || memcmp(s, word, n) != 0) {
        return 0;
    }
    return s + n >= end || !note_ident_char(s[n]);
}

static const char *note_skip_ws(const char *s, const char *end) {
    while (s < end && (*s == ' ' || *s == '\t' || *s == '\r')) {
        s++;
    }
    return s;
}

static int note_bare_eq(char prev, char next) {
    if (next == '=') {
        return 0;
    }
    return prev != '=' && prev != '!' && prev != '<' && prev != '>' && prev != '+' && prev != '-' && prev != '*' &&
           prev != '/' && prev != '%' && prev != '&' && prev != '|' && prev != '^';
}

static char note_prev(const char *s, size_t i) {
    while (i > 0) {
        i--;
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r') {
            return s[i];
        }
    }
    return 0;
}

static void note_skip_quote(const char *s, size_t len, size_t *i, char quote) {
    (*i)++;
    while (*i < len) {
        if (s[*i] == '\\' && *i + 1 < len) {
            *i += 2;
            continue;
        }
        if (s[*i] == quote) {
            (*i)++;
            return;
        }
        (*i)++;
    }
}

static NoteScan note_scan_line(const char *s, size_t len, int brace, int paren, int bracket) {
    NoteScan sc;
    sc.brace = brace;
    sc.paren = paren;
    sc.bracket = bracket;
    sc.saw_brace = 0;
    sc.saw_base_eq = 0;
    sc.ends_base_eq = 0;
    size_t i = 0;
    while (i < len) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            i++;
            continue;
        }
        if (c == '/' && i + 1 < len && s[i + 1] == '/') {
            break;
        }
        if (c == '"' || c == '\'') {
            note_skip_quote(s, len, &i, c);
            sc.ends_base_eq = 0;
            continue;
        }
        if (c == '{') {
            sc.brace++;
            sc.saw_brace = 1;
            sc.ends_base_eq = 0;
            i++;
            continue;
        }
        if (c == '}') {
            if (sc.brace > 0) {
                sc.brace--;
            }
            sc.ends_base_eq = 0;
            i++;
            continue;
        }
        if (c == '(') {
            sc.paren++;
            sc.ends_base_eq = 0;
            i++;
            continue;
        }
        if (c == ')') {
            if (sc.paren > 0) {
                sc.paren--;
            }
            sc.ends_base_eq = 0;
            i++;
            continue;
        }
        if (c == '[') {
            sc.bracket++;
            sc.ends_base_eq = 0;
            i++;
            continue;
        }
        if (c == ']') {
            if (sc.bracket > 0) {
                sc.bracket--;
            }
            sc.ends_base_eq = 0;
            i++;
            continue;
        }
        if (c == '=' && note_bare_eq(note_prev(s, i), i + 1 < len ? s[i + 1] : 0)) {
            int at_base = sc.brace == 0 && sc.paren == 0 && sc.bracket == 0;
            sc.ends_base_eq = at_base;
            if (at_base) {
                sc.saw_base_eq = 1;
            }
            i++;
            continue;
        }
        sc.ends_base_eq = 0;
        i++;
    }
    return sc;
}

static int note_line_is_ellipsis(const char *s, size_t len) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    if (i + 3 > len || s[i] != '.' || s[i + 1] != '.' || s[i + 2] != '.') {
        return 0;
    }
    i += 3;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    if (i >= len) {
        return 1;
    }
    return i + 1 < len && s[i] == '/' && s[i + 1] == '/';
}

static void note_scrub_line(CtSb *out, const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (s[i] == '"' || s[i] == '\'') {
            size_t start = i;
            note_skip_quote(s, len, &i, s[i]);
            ct_sb_append(out, s + start, i - start);
            continue;
        }
        if (s[i] == '/' && i + 1 < len && s[i + 1] == '/') {
            ct_sb_append(out, s + i, len - i);
            return;
        }
        if (s[i] == '{') {
            size_t j = i + 1;
            while (j < len && (s[j] == ' ' || s[j] == '\t')) {
                j++;
            }
            if (j + 3 <= len && s[j] == '.' && s[j + 1] == '.' && s[j + 2] == '.') {
                j += 3;
                while (j < len && (s[j] == ' ' || s[j] == '\t')) {
                    j++;
                }
                if (j < len && s[j] == '}') {
                    ct_sb_append_cstr(out, "{ }");
                    i = j + 1;
                    continue;
                }
            }
        }
        if (s[i] == '=' && note_bare_eq(note_prev(s, i), i + 1 < len ? s[i + 1] : 0)) {
            size_t j = i + 1;
            while (j < len && (s[j] == ' ' || s[j] == '\t')) {
                j++;
            }
            if (j + 3 <= len && s[j] == '.' && s[j + 1] == '.' && s[j + 2] == '.') {
                size_t k = j + 3;
                while (k < len && (s[k] == ' ' || s[k] == '\t')) {
                    k++;
                }
                if (k >= len || (k + 1 < len && s[k] == '/' && s[k + 1] == '/')) {
                    ct_sb_append_cstr(out, "= 0");
                    i = k;
                    continue;
                }
            }
        }
        ct_sb_putc(out, s[i]);
        i++;
    }
}

static void note_scrub(CtSb *out, const char *body, size_t len) {
    const char *p = body;
    const char *end = body + len;
    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') {
            nl++;
        }
        size_t line_len = (size_t)(nl - p);
        if (line_len > 0 && p[line_len - 1] == '\r') {
            line_len--;
        }
        if (!note_line_is_ellipsis(p, line_len)) {
            note_scrub_line(out, p, line_len);
            ct_sb_putc(out, '\n');
        }
        p = nl < end ? nl + 1 : end;
    }
}

static int note_is_fn_line(const char *s, const char *end) {
    s = note_skip_ws(s, end);
    if (note_starts_word(s, end, "private")) {
        s += 7;
        s = note_skip_ws(s, end);
        if (s < end && *s == '(') {
            int depth = 0;
            for (; s < end; s++) {
                if (*s == '(') {
                    depth++;
                } else if (*s == ')') {
                    depth--;
                    if (depth == 0) {
                        s++;
                        break;
                    }
                }
            }
            s = note_skip_ws(s, end);
        }
    }
    return note_starts_word(s, end, "fn");
}

static int note_is_decl_line(const char *s, const char *end) {
    static const char *words[] = {"import", "fn",    "var",    "val",     "type",   "struct",
                                  "interface", "enum", "extern", "context", "private", NULL};
    s = note_skip_ws(s, end);
    for (int i = 0; words[i]; i++) {
        if (note_starts_word(s, end, words[i])) {
            return 1;
        }
    }
    if (s < end && *s == '@') {
        NoteScan sc = note_scan_line(s, (size_t)(end - s), 0, 0, 0);
        return !sc.saw_brace;
    }
    return 0;
}

static int note_continues(const char *s, const char *end) {
    if (s < end && *s == '.') {
        return 1;
    }
    return s + 1 < end && s[0] == '?' && (s[1] == '.' || s[1] == ':');
}

static int note_blank_or_comment(const char *s, const char *end) {
    s = note_skip_ws(s, end);
    if (s >= end) {
        return 1;
    }
    return s + 1 < end && s[0] == '/' && s[1] == '/';
}

static void note_insert_body(CtSb *out, const char *s, size_t len) {
    size_t i = 0;
    size_t cut = len;
    while (i < len) {
        if (s[i] == '"' || s[i] == '\'') {
            note_skip_quote(s, len, &i, s[i]);
            continue;
        }
        if (s[i] == '/' && i + 1 < len && s[i + 1] == '/') {
            cut = i;
            break;
        }
        i++;
    }
    ct_sb_append(out, s, cut);
    ct_sb_append_cstr(out, " {}");
    if (cut < len) {
        ct_sb_append(out, s + cut, len - cut);
    }
}

static int note_has_code(const char *s) {
    while (*s) {
        if (s[0] == '/' && s[1] == '/') {
            while (*s && *s != '\n') {
                s++;
            }
            continue;
        }
        if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
            return 1;
        }
        s++;
    }
    return 0;
}

static int note_directory(const char *body) {
    const char *s = body;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    const char *end = s;
    while (*end && *end != '\n' && *end != '\r') {
        end++;
    }
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    if (end <= s || end[-1] != '/') {
        return 0;
    }
    for (const char *q = s; q < end; q++) {
        char c = *q;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.' ||
              c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

static int note_nonsource(const char *body) {
    if (strstr(body, "fn ptr") || strstr(body, "ITable for") || note_directory(body)) {
        return 1;
    }
    return !strstr(body, "(") && !strstr(body, ")") && !strstr(body, "{") && !strstr(body, "}") && !strstr(body, "=") &&
           !strstr(body, "->");
}

static int note_types_from_stmts(const char *stmts, CtSb *out) {
    int any = 0;
    const char *p = stmts;
    while (*p) {
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        if (end > p && end[-1] == '\r') {
            end--;
        }
        const char *code = note_skip_ws(p, end);
        if (code < end && !(code + 1 < end && code[0] == '/' && code[1] == '/')) {
            if (*code != '(') {
                return 0;
            }
            any = 1;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    if (!any) {
        return 0;
    }
    int index = 0;
    p = stmts;
    while (*p) {
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        if (end > p && end[-1] == '\r') {
            end--;
        }
        const char *code = note_skip_ws(p, end);
        if (code < end && !(code + 1 < end && code[0] == '/' && code[1] == '/')) {
            ct_sb_appendf(out, "type __t%d = %.*s\n", index, (int)(end - code), code);
            index++;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return 1;
}

static int note_parses(const char *src, const char *id, const char *part, int *shown) {
    Parsed parsed = parse_src(src);
    int ok = parsed.root && parsed.lex_ok && ct_diag_error_count(parsed.diag) == 0;
    if (!ok) {
        fprintf(stderr, "note %s failed (%s)\n", id, part);
        if (*shown < 8) {
            (*shown)++;
            ct_diag_print(parsed.diag, stderr);
            fputs("--- source ---\n", stderr);
            fputs(src, stderr);
            fputc('\n', stderr);
        }
    }
    close_parsed(&parsed);
    return ok;
}

static int pwsh_found(void) {
#if defined(_WIN32)
    return system("where pwsh >NUL 2>NUL") == 0;
#else
    return system("command -v pwsh >/dev/null 2>&1") == 0;
#endif
}

static void test_language_notes(void) {
    char cmd[1024];
    char path[1024];
    int wrote = 0;
    if (!pwsh_found()) {
        fprintf(stderr, "skip: language notes test: pwsh not found\n");
        return;
    }
    wrote = snprintf(cmd, sizeof cmd,
                         "pwsh -NoProfile -File \"%s/tests/parse/extract-notes.ps1\" -OutFile \"%s/build/note-blocks.txt\"",
                         CT_SOURCE_DIR, CT_SOURCE_DIR);
    CHECK(wrote > 0 && (size_t)wrote < sizeof cmd);
    if (wrote < 0 || (size_t)wrote >= sizeof cmd || system(cmd) != 0) {
        fprintf(stderr, "extract-notes.ps1 failed\n");
        g_failures++;
        return;
    }
    wrote = snprintf(path, sizeof path, "%s/build/note-blocks.txt", CT_SOURCE_DIR);
    CHECK(wrote > 0 && (size_t)wrote < sizeof path);

    CtArena *arena = ct_arena_create();
    CtDiag *diag = ct_diag_create(arena);
    CtFile file;
    if (!ct_file_read(arena, diag, path, &file)) {
        fprintf(stderr, "cannot read note blocks\n");
        ct_diag_print(diag, stderr);
        g_failures++;
        ct_arena_destroy(arena);
        return;
    }
    if (strncmp(file.data, "@@@MISSING", 10) == 0) {
        const char *reason = file.data + 10;
        while (*reason == ' ') {
            reason++;
        }
        fprintf(stderr, "skip: language notes test: %s", reason);
        ct_arena_destroy(arena);
        return;
    }

    int notes = 0;
    int skips = 0;
    int nonsource = 0;
    int parsed = 0;
    int failed = 0;
    int shown = 0;
    const char *p = file.data;
    while (*p) {
        if (strncmp(p, "@@@COUNT", 8) == 0) {
            break;
        }
        if (strncmp(p, "@@@SKIP", 7) == 0) {
            skips++;
            const char *nl = strchr(p, '\n');
            p = nl ? nl + 1 : p + strlen(p);
            continue;
        }
        if (strncmp(p, "@@@NOTE ", 8) != 0) {
            const char *nl = strchr(p, '\n');
            if (!nl) {
                break;
            }
            p = nl + 1;
            continue;
        }
        notes++;
        char id[160];
        const char *nl = strchr(p, '\n');
        if (!nl) {
            failed++;
            break;
        }
        size_t idn = (size_t)(nl - (p + 8));
        if (idn >= sizeof id) {
            idn = sizeof id - 1;
        }
        memcpy(id, p + 8, idn);
        id[idn] = '\0';
        if (idn > 0 && id[idn - 1] == '\r') {
            id[idn - 1] = '\0';
        }
        p = nl + 1;
        const char *endmark = strstr(p, "\n@@@END");
        if (!endmark) {
            fprintf(stderr, "note %s has no end marker\n", id);
            failed++;
            break;
        }
        size_t body_len = (size_t)(endmark - p);
        char *body = ct_arena_alloc(arena, body_len + 1, 1);
        if (!body) {
            failed++;
            break;
        }
        memcpy(body, p, body_len);
        body[body_len] = '\0';
        p = endmark + 1;
        nl = strchr(p, '\n');
        p = nl ? nl + 1 : p + strlen(p);

        if (note_nonsource(body)) {
            nonsource++;
            continue;
        }
        CtSb scrub;
        CtSb decls;
        CtSb stmts;
        ct_sb_init(&scrub);
        ct_sb_init(&decls);
        ct_sb_init(&stmts);
        note_scrub(&scrub, body, body_len);
        const char *text = ct_sb_cstr(&scrub);
        int brace = 0;
        int paren = 0;
        int bracket = 0;
        int cont = 0;
        int which = 0;
        const char *line = text;
        while (*line) {
            const char *line_nl = strchr(line, '\n');
            const char *end = line_nl ? line_nl : line + strlen(line);
            if (end > line && end[-1] == '\r') {
                end--;
            }
            size_t len = (size_t)(end - line);
            const char *code = note_skip_ws(line, end);
            CtSb *dest = which ? &stmts : &decls;
            if (!note_blank_or_comment(line, end)) {
                int keep = cont || note_starts_word(code, end, "else") || note_continues(code, end);
                if (brace == 0 && paren == 0 && bracket == 0 && !keep) {
                    which = note_is_decl_line(line, end) ? 0 : 1;
                    dest = which ? &stmts : &decls;
                }
                NoteScan probe = note_scan_line(line, len, brace, paren, bracket);
                int add_body = brace == 0 && paren == 0 && bracket == 0 && note_is_fn_line(line, end) &&
                               probe.brace == brace && probe.paren == paren && probe.bracket == bracket &&
                               !probe.saw_brace && !probe.saw_base_eq;
                if (add_body) {
                    CtSb extra;
                    ct_sb_init(&extra);
                    note_insert_body(&extra, line, len);
                    ct_sb_append(&*dest, ct_sb_cstr(&extra), extra.len);
                    ct_sb_putc(dest, '\n');
                    probe = note_scan_line(ct_sb_cstr(&extra), extra.len, brace, paren, bracket);
                    ct_sb_free(&extra);
                } else {
                    ct_sb_append(dest, line, len);
                    ct_sb_putc(dest, '\n');
                }
                brace = probe.brace;
                paren = probe.paren;
                bracket = probe.bracket;
                cont = brace == 0 && paren == 0 && bracket == 0 && probe.ends_base_eq;
            } else {
                ct_sb_append(dest, line, len);
                ct_sb_putc(dest, '\n');
            }
            line = line_nl ? line_nl + 1 : end;
        }

        int ok = 1;
        if (!note_has_code(ct_sb_cstr(&decls)) && !note_has_code(ct_sb_cstr(&stmts))) {
            fprintf(stderr, "note %s normalized to empty\n", id);
            ok = 0;
        }
        if (ok && note_has_code(ct_sb_cstr(&decls)) && !note_parses(ct_sb_cstr(&decls), id, "declarations", &shown)) {
            ok = 0;
        }
        if (ok && note_has_code(ct_sb_cstr(&stmts))) {
            CtSb wrapped;
            ct_sb_init(&wrapped);
            if (note_types_from_stmts(ct_sb_cstr(&stmts), &wrapped)) {
                if (!note_parses(ct_sb_cstr(&wrapped), id, "types", &shown)) {
                    ok = 0;
                }
            } else {
                ct_sb_clear(&wrapped);
                ct_sb_append_cstr(&wrapped, "fn __note() {\n");
                ct_sb_append_cstr(&wrapped, ct_sb_cstr(&stmts));
                ct_sb_append_cstr(&wrapped, "}\n");
                if (!note_parses(ct_sb_cstr(&wrapped), id, "statements", &shown)) {
                    ok = 0;
                }
            }
            ct_sb_free(&wrapped);
        }
        if (scrub.failed || decls.failed || stmts.failed) {
            ok = 0;
        }
        if (ok) {
            parsed++;
        } else {
            failed++;
        }
        ct_sb_free(&scrub);
        ct_sb_free(&decls);
        ct_sb_free(&stmts);
    }

    CHECK(notes >= 100);
    CHECK(skips >= 1);
    CHECK(nonsource >= 4);
    CHECK(parsed + nonsource + failed == notes);
    CHECK(failed == 0);
    CHECK(parsed >= 90);
    ct_arena_destroy(arena);
}

static void test_golden(void) {
    const char *dir = CT_SOURCE_DIR "/tests/parse";
    CtArena *arena = ct_arena_create();
    CtDiag *diag = ct_diag_create(arena);
    CtDa names;
    ct_da_init(&names, sizeof(const char *));
    if (!ct_fs_list(arena, diag, dir, CT_FS_FILES, ".jog", &names)) {
        fprintf(stderr, "cannot list %s\n", dir);
        ct_diag_print(diag, stderr);
        g_failures++;
        ct_da_free(&names);
        ct_arena_destroy(arena);
        return;
    }
    CHECK(names.count >= 2);
    for (size_t i = 0; i < names.count; i++) {
        const char *name = *(const char **)ct_da_at(&names, i);
        size_t len = strlen(name);
        char ast_name[256];
        if (len < 5 || len >= sizeof ast_name) {
            g_failures++;
            continue;
        }
        memcpy(ast_name, name, len - 4);
        memcpy(ast_name + (len - 4), ".ast", 5);
        char *src_path = ct_path_join(arena, dir, name);
        char *ast_path = ct_path_join(arena, dir, ast_name);
        CtArena *file_arena = ct_arena_create();
        CtDiag *file_diag = ct_diag_create(file_arena);
        CtIntern *intern = ct_intern_create(file_arena);
        CtFile source;
        CtDa tokens;
        ct_da_init(&tokens, sizeof(JogToken));
        int ok = src_path && ct_file_read(file_arena, file_diag, src_path, &source);
        CHECK(ok);
        if (ok) {
            int lex_ok = jog_lex(file_arena, file_diag, intern, &source, &tokens);
            JogNode *root = jog_parse(file_arena, file_diag, intern, &source, &tokens);
            CHECK(root != NULL);
            CHECK(lex_ok);
            if (ct_diag_error_count(file_diag) != 0) {
                fprintf(stderr, "errors in %s:\n", name);
                ct_diag_print(file_diag, stderr);
                g_failures++;
            }
            CtSb sb;
            ct_sb_init(&sb);
            jog_ast_print(root, &sb);
            CtFile expect;
            if (!ast_path || !ct_file_read(file_arena, NULL, ast_path, &expect)) {
                fprintf(stderr, "missing %s, parser printed:\n%s", ast_name, ct_sb_cstr(&sb));
                g_failures++;
            } else if (!same_text(ct_sb_cstr(&sb), expect.data)) {
                fprintf(stderr, "golden mismatch for %s\n--- got ---\n%s--- expected ---\n%s", name, ct_sb_cstr(&sb),
                        expect.data);
                g_failures++;
            }
            ct_sb_free(&sb);
        }
        ct_da_free(&tokens);
        ct_arena_destroy(file_arena);
    }
    ct_da_free(&names);
    ct_arena_destroy(arena);
}

static int diag_contains(CtDiag *diag, const char *msg) {
    char buf[2048];
    FILE *file = tmpfile();
    size_t n;
    if (!file || !msg) {
        if (file) {
            fclose(file);
        }
        return 0;
    }
    ct_diag_print(diag, file);
    rewind(file);
    n = fread(buf, 1, sizeof(buf) - 1, file);
    buf[n] = '\0';
    fclose(file);
    return strstr(buf, msg) != NULL;
}

static void test_pack(void) {
    Parsed parsed = parse_src("fn f(...i32: args) {}\n"
                              "fn g(prefix: str, ...i32: rest) {}\n"
                              "fn h<T>(...T: args): usize { return args.len }\n"
                              "extern {\n"
                              "    fn printf(fmt: *const c_char, ...): c_int\n"
                              "}\n"
                              "fn both(...i32: args, x: i32) {}\n");
    CHECK(clean(&parsed));
    JogNode *f = decl_at(&parsed, 0);
    CHECK(f && f->kind == JOG_NODE_FN);
    CHECK(!f->as.fn.variadic);
    CHECK(f->as.fn.params.count == 1);
    CHECK(f->as.fn.params.items[0]->as.param.pack);
    CHECK(strcmp(f->as.fn.params.items[0]->as.param.name, "args") == 0);
    JogNode *g = decl_at(&parsed, 1);
    CHECK(g->as.fn.params.count == 2);
    CHECK(!g->as.fn.params.items[0]->as.param.pack);
    CHECK(g->as.fn.params.items[1]->as.param.pack);
    CHECK(strcmp(g->as.fn.params.items[1]->as.param.name, "rest") == 0);
    JogNode *h = decl_at(&parsed, 2);
    CHECK(h->as.fn.type_params.count == 1);
    CHECK(h->as.fn.params.items[0]->as.param.pack);
    JogNode *ext = decl_at(&parsed, 3);
    CHECK(ext->kind == JOG_NODE_EXTERN_BLOCK);
    JogNode *pf = ext->as.extern_block.fns.items[0];
    CHECK(pf->as.fn.is_extern && pf->as.fn.variadic);
    CHECK(pf->as.fn.params.count == 1);
    CHECK(!pf->as.fn.params.items[0]->as.param.pack);
    JogNode *both = decl_at(&parsed, 4);
    CHECK(both->as.fn.params.count == 2);
    CHECK(both->as.fn.params.items[0]->as.param.pack);
    CHECK(!both->as.fn.params.items[1]->as.param.pack);
    CHECK(strstr(printed(&parsed), "(PARAM args pack") != NULL);
    close_parsed(&parsed);

    Parsed bare = parse_src("fn f(...) {}\n");
    CHECK(ct_diag_error_count(bare.diag) >= 1);
    CHECK(diag_contains(bare.diag, "variadic parameters are only allowed on extern functions"));
    close_parsed(&bare);

    Parsed rejected = parse_src("extern {\n    fn f(...i32: args)\n}\n");
    CHECK(ct_diag_error_count(rejected.diag) >= 1);
    CHECK(diag_contains(rejected.diag, "an extern function cannot have a pack parameter"));
    close_parsed(&rejected);
}

int main(void) {
    test_empty();
    test_imports();
    test_fn_print();
    test_functions();
    test_bindings_and_types();
    test_aggregates();
    test_pack();
    test_precedence_and_exprs();
    test_statements();
    test_forms();
    test_rules();
    test_recovery();
    test_attributes_and_block();
    test_language_notes();
    test_golden();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
