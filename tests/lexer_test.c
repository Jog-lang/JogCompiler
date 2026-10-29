#include "lexer.h"

#include "arena.h"
#include "da.h"
#include "diag.h"
#include "intern.h"

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

typedef struct Lexed {
    CtArena *arena;
    CtDiag *diag;
    CtIntern *intern;
    CtDa tokens;
    bool ok;
} Lexed;

static void lex(Lexed *out, const char *source) {
    out->arena = ct_arena_create();
    out->diag = ct_diag_create(out->arena);
    out->intern = ct_intern_create(out->arena);
    ct_da_init(&out->tokens, sizeof(JogToken));
    CtFile file = {"test.jog", source, strlen(source)};
    out->ok = jog_lex(out->arena, out->diag, out->intern, &file, &out->tokens);
}

static void release(Lexed *lexed) {
    ct_da_free(&lexed->tokens);
    ct_arena_destroy(lexed->arena);
}

static const JogToken *tok(Lexed *lexed, size_t index) {
    return ct_da_at(&lexed->tokens, index);
}

/* Compares the token kinds against expected, which ends with EOF. Prints
   both sequences on a mismatch. */
static void expect_kinds(const char *source, const JogTokenKind *expected, const char *file, int line) {
    Lexed lexed;
    lex(&lexed, source);
    size_t n = 0;
    while (expected[n] != JOG_TOK_EOF) {
        n++;
    }
    n++;
    int same = lexed.ok && lexed.tokens.count == n;
    for (size_t i = 0; same && i < n; i++) {
        same = tok(&lexed, i)->kind == expected[i];
    }
    if (!same) {
        fprintf(stderr, "FAIL %s:%d: token kinds for:\n%s\n  got:     ", file, line, source);
        for (size_t i = 0; i < lexed.tokens.count; i++) {
            fprintf(stderr, " %s", jog_token_name(tok(&lexed, i)->kind));
        }
        fprintf(stderr, "\n  expected:");
        for (size_t i = 0; i < n; i++) {
            fprintf(stderr, " %s", jog_token_name(expected[i]));
        }
        fprintf(stderr, "\n");
        ct_diag_print(lexed.diag, stderr);
        g_failures++;
    }
    release(&lexed);
}

#define EXPECT_KINDS(source, ...)                                    \
    do {                                                             \
        static const JogTokenKind expected_[] = {__VA_ARGS__};       \
        expect_kinds((source), expected_, __FILE__, __LINE__);       \
    } while (0)

static int value_is(const JogToken *t, const char *text, size_t len) {
    return t->value && t->value_len == len && memcmp(t->value, text, len) == 0;
}

#define VALUE_IS(t, literal) value_is((t), (literal), sizeof(literal) - 1)

static size_t error_count(const char *source) {
    Lexed lexed;
    lex(&lexed, source);
    size_t errors = ct_diag_error_count(lexed.diag);
    CHECK(!lexed.ok || errors == 0);
    release(&lexed);
    return errors;
}

static void test_declarations(void) {
    EXPECT_KINDS("val x = 42\n", JOG_TOK_KW_VAL, JOG_TOK_IDENT, JOG_TOK_ASSIGN, JOG_TOK_INT, JOG_TOK_SEP,
                 JOG_TOK_EOF);
    EXPECT_KINDS("fn add(a: i32, b: i32): i32 = a + b", JOG_TOK_KW_FN, JOG_TOK_IDENT, JOG_TOK_LPAREN, JOG_TOK_IDENT,
                 JOG_TOK_COLON, JOG_TOK_IDENT, JOG_TOK_COMMA, JOG_TOK_IDENT, JOG_TOK_COLON, JOG_TOK_IDENT,
                 JOG_TOK_RPAREN, JOG_TOK_COLON, JOG_TOK_IDENT, JOG_TOK_ASSIGN, JOG_TOK_IDENT, JOG_TOK_PLUS,
                 JOG_TOK_IDENT, JOG_TOK_SEP, JOG_TOK_EOF);
    EXPECT_KINDS("fn struct quote extern escaping", JOG_TOK_KW_FN, JOG_TOK_KW_STRUCT, JOG_TOK_KW_QUOTE,
                 JOG_TOK_KW_EXTERN, JOG_TOK_KW_ESCAPING, JOG_TOK_EOF);
    EXPECT_KINDS("a; b", JOG_TOK_IDENT, JOG_TOK_SEP, JOG_TOK_IDENT, JOG_TOK_SEP, JOG_TOK_EOF);
}

static void test_newlines(void) {
    /* Ignored inside parentheses. */
    EXPECT_KINDS("f(a,\n  b)\n", JOG_TOK_IDENT, JOG_TOK_LPAREN, JOG_TOK_IDENT, JOG_TOK_COMMA, JOG_TOK_IDENT,
                 JOG_TOK_RPAREN, JOG_TOK_SEP, JOG_TOK_EOF);
    /* Significant again inside a lambda nested in parentheses. */
    EXPECT_KINDS("f(xs.map {\n  it\n})", JOG_TOK_IDENT, JOG_TOK_LPAREN, JOG_TOK_IDENT, JOG_TOK_DOT, JOG_TOK_IDENT,
                 JOG_TOK_LBRACE, JOG_TOK_IDENT, JOG_TOK_SEP, JOG_TOK_RBRACE, JOG_TOK_RPAREN, JOG_TOK_SEP,
                 JOG_TOK_EOF);
    /* Not after an operator. */
    EXPECT_KINDS("a +\n  b\n", JOG_TOK_IDENT, JOG_TOK_PLUS, JOG_TOK_IDENT, JOG_TOK_SEP, JOG_TOK_EOF);
    /* Leading-dot continuation, also across a comment line. */
    EXPECT_KINDS("val n = items\n    .filter { it }\n    // note\n    ?.count()\n", JOG_TOK_KW_VAL, JOG_TOK_IDENT,
                 JOG_TOK_ASSIGN, JOG_TOK_IDENT, JOG_TOK_DOT, JOG_TOK_IDENT, JOG_TOK_LBRACE, JOG_TOK_IDENT,
                 JOG_TOK_RBRACE, JOG_TOK_QDOT, JOG_TOK_IDENT, JOG_TOK_LPAREN, JOG_TOK_RPAREN, JOG_TOK_SEP,
                 JOG_TOK_EOF);
    EXPECT_KINDS("a\n?: b", JOG_TOK_IDENT, JOG_TOK_ELVIS, JOG_TOK_IDENT, JOG_TOK_SEP, JOG_TOK_EOF);
    /* Types ending in '>', '>>', '?', and bytes end a statement. */
    EXPECT_KINDS("var m: Map<str, List<i32>>\nvar b: bytes\nvar p: *T?\n", JOG_TOK_KW_VAR, JOG_TOK_IDENT,
                 JOG_TOK_COLON, JOG_TOK_IDENT, JOG_TOK_LT, JOG_TOK_IDENT, JOG_TOK_COMMA, JOG_TOK_IDENT, JOG_TOK_LT,
                 JOG_TOK_IDENT, JOG_TOK_SHR, JOG_TOK_SEP, JOG_TOK_KW_VAR, JOG_TOK_IDENT, JOG_TOK_COLON,
                 JOG_TOK_IDENT, JOG_TOK_SEP, JOG_TOK_KW_VAR, JOG_TOK_IDENT, JOG_TOK_COLON, JOG_TOK_STAR,
                 JOG_TOK_IDENT, JOG_TOK_QUESTION, JOG_TOK_SEP, JOG_TOK_EOF);
    /* A block comment that spans lines acts as a newline. */
    EXPECT_KINDS("a /* x /* nested */\n */ b", JOG_TOK_IDENT, JOG_TOK_SEP, JOG_TOK_IDENT, JOG_TOK_SEP, JOG_TOK_EOF);
    EXPECT_KINDS("a /* same line */ b", JOG_TOK_IDENT, JOG_TOK_IDENT, JOG_TOK_SEP, JOG_TOK_EOF);
    /* Blank lines give one SEP. */
    EXPECT_KINDS("return\n\n\nbreak", JOG_TOK_KW_RETURN, JOG_TOK_SEP, JOG_TOK_KW_BREAK, JOG_TOK_SEP, JOG_TOK_EOF);
    /* An attribute line followed by a declaration. */
    EXPECT_KINDS("@test\nfn t()", JOG_TOK_ATTR, JOG_TOK_KW_FN, JOG_TOK_IDENT, JOG_TOK_LPAREN, JOG_TOK_RPAREN,
                 JOG_TOK_SEP, JOG_TOK_EOF);
}

static void test_numbers(void) {
    EXPECT_KINDS("0xDEAD_BEEF 1_000 3.14 1e-9 2.5E+3 0b1010 0o755", JOG_TOK_INT, JOG_TOK_INT, JOG_TOK_FLOAT,
                 JOG_TOK_FLOAT, JOG_TOK_FLOAT, JOG_TOK_INT, JOG_TOK_INT, JOG_TOK_SEP, JOG_TOK_EOF);
    EXPECT_KINDS("0..n", JOG_TOK_INT, JOG_TOK_DOTDOT, JOG_TOK_IDENT, JOG_TOK_SEP, JOG_TOK_EOF);
    EXPECT_KINDS("pair.0.1", JOG_TOK_IDENT, JOG_TOK_DOT, JOG_TOK_INT, JOG_TOK_DOT, JOG_TOK_INT, JOG_TOK_SEP,
                 JOG_TOK_EOF);
    EXPECT_KINDS("timeout(5.s)", JOG_TOK_IDENT, JOG_TOK_LPAREN, JOG_TOK_INT, JOG_TOK_DOT, JOG_TOK_IDENT,
                 JOG_TOK_RPAREN, JOG_TOK_SEP, JOG_TOK_EOF);
    CHECK(error_count("1abc") == 1);
    CHECK(error_count("0x") == 1);
    CHECK(error_count("0b102") == 1);
}

static void test_operators(void) {
    EXPECT_KINDS("<=> <<= >>= ... ?. ?: !! -> != == <= >= && || += %= ^= |= &=", JOG_TOK_CMP3, JOG_TOK_SHL_ASSIGN,
                 JOG_TOK_SHR_ASSIGN, JOG_TOK_ELLIPSIS, JOG_TOK_QDOT, JOG_TOK_ELVIS, JOG_TOK_BANGBANG, JOG_TOK_ARROW,
                 JOG_TOK_NEQ, JOG_TOK_EQ, JOG_TOK_LE, JOG_TOK_GE, JOG_TOK_AMPAMP, JOG_TOK_PIPEPIPE,
                 JOG_TOK_PLUS_ASSIGN, JOG_TOK_PERCENT_ASSIGN, JOG_TOK_CARET_ASSIGN, JOG_TOK_PIPE_ASSIGN,
                 JOG_TOK_AMP_ASSIGN, JOG_TOK_EOF);
    EXPECT_KINDS("x as? T", JOG_TOK_IDENT, JOG_TOK_AS_Q, JOG_TOK_IDENT, JOG_TOK_SEP, JOG_TOK_EOF);
    EXPECT_KINDS("x as T?", JOG_TOK_IDENT, JOG_TOK_KW_AS, JOG_TOK_IDENT, JOG_TOK_QUESTION, JOG_TOK_SEP, JOG_TOK_EOF);
    EXPECT_KINDS("maybe!!", JOG_TOK_IDENT, JOG_TOK_BANGBANG, JOG_TOK_SEP, JOG_TOK_EOF);
}

static void test_strings(void) {
    Lexed lexed;
    lex(&lexed, "\"hi ${name}, ${n + 1}!\"");
    CHECK(lexed.ok);
    CHECK(lexed.tokens.count == 9);
    if (lexed.tokens.count == 9) {
        CHECK(tok(&lexed, 0)->kind == JOG_TOK_STR_HEAD && VALUE_IS(tok(&lexed, 0), "hi "));
        CHECK(tok(&lexed, 1)->kind == JOG_TOK_IDENT);
        CHECK(tok(&lexed, 2)->kind == JOG_TOK_STR_MID && VALUE_IS(tok(&lexed, 2), ", "));
        CHECK(tok(&lexed, 3)->kind == JOG_TOK_IDENT);
        CHECK(tok(&lexed, 4)->kind == JOG_TOK_PLUS);
        CHECK(tok(&lexed, 5)->kind == JOG_TOK_INT);
        CHECK(tok(&lexed, 6)->kind == JOG_TOK_STR_TAIL && VALUE_IS(tok(&lexed, 6), "!"));
        CHECK(tok(&lexed, 7)->kind == JOG_TOK_SEP);
        CHECK(tok(&lexed, 8)->kind == JOG_TOK_EOF);
    }
    release(&lexed);

    /* A string nested inside an interpolation, and a block inside one. */
    EXPECT_KINDS("\"a${f(\"b\")}c\"", JOG_TOK_STR_HEAD, JOG_TOK_IDENT, JOG_TOK_LPAREN, JOG_TOK_STRING,
                 JOG_TOK_RPAREN, JOG_TOK_STR_TAIL, JOG_TOK_SEP, JOG_TOK_EOF);
    EXPECT_KINDS("\"${xs.map { it }}\"", JOG_TOK_STR_HEAD, JOG_TOK_IDENT, JOG_TOK_DOT, JOG_TOK_IDENT,
                 JOG_TOK_LBRACE, JOG_TOK_IDENT, JOG_TOK_RBRACE, JOG_TOK_STR_TAIL, JOG_TOK_SEP, JOG_TOK_EOF);

    lex(&lexed, "\"tab\\t \\$x \\u{1F600} \\x41\" c\"hi\\n\" x\"de ad\nbe ef\" `C:\\path` 'a' '\\n' '\xC3\xA9'");
    CHECK(lexed.ok);
    CHECK(lexed.tokens.count == 9);
    if (lexed.tokens.count == 9) {
        CHECK(tok(&lexed, 0)->kind == JOG_TOK_STRING && VALUE_IS(tok(&lexed, 0), "tab\t $x \xF0\x9F\x98\x80 A"));
        CHECK(tok(&lexed, 1)->kind == JOG_TOK_CSTRING && VALUE_IS(tok(&lexed, 1), "hi\n"));
        CHECK(tok(&lexed, 2)->kind == JOG_TOK_HEXBLOB && VALUE_IS(tok(&lexed, 2), "\xDE\xAD\xBE\xEF"));
        CHECK(tok(&lexed, 2)->line == 1 && tok(&lexed, 3)->line == 2);
        CHECK(tok(&lexed, 3)->kind == JOG_TOK_STRING && VALUE_IS(tok(&lexed, 3), "C:\\path"));
        CHECK(tok(&lexed, 4)->kind == JOG_TOK_CHAR && VALUE_IS(tok(&lexed, 4), "a"));
        CHECK(tok(&lexed, 5)->kind == JOG_TOK_CHAR && VALUE_IS(tok(&lexed, 5), "\n"));
        CHECK(tok(&lexed, 6)->kind == JOG_TOK_CHAR && VALUE_IS(tok(&lexed, 6), "\xC3\xA9"));
    }
    release(&lexed);

    CHECK(error_count("\"abc") == 1);
    CHECK(error_count("\"a\\q\"") == 1);
    CHECK(error_count("x\"abc\"") == 1);
    CHECK(error_count("'ab'") == 1);
    CHECK(error_count("''") == 1);
    CHECK(error_count("\"${x\"") >= 1);
    CHECK(error_count("\"\\u{D800}\"") == 1);
}

static void test_positions_and_names(void) {
    Lexed lexed;
    lex(&lexed, "fn main() {\n    print(name, name)\n}\n");
    CHECK(lexed.ok);
    const JogToken *print = NULL;
    const JogToken *first = NULL;
    const JogToken *second = NULL;
    for (size_t i = 0; i < lexed.tokens.count; i++) {
        const JogToken *t = tok(&lexed, i);
        if (t->kind == JOG_TOK_IDENT && VALUE_IS(t, "print")) {
            print = t;
        } else if (t->kind == JOG_TOK_IDENT && VALUE_IS(t, "name")) {
            if (!first) {
                first = t;
            } else {
                second = t;
            }
        }
    }
    CHECK(print && print->line == 2 && print->column == 5);
    CHECK(first && second && first->value == second->value);
    release(&lexed);

    lex(&lexed, "@packed struct S {}");
    CHECK(lexed.ok);
    CHECK(tok(&lexed, 0)->kind == JOG_TOK_ATTR && VALUE_IS(tok(&lexed, 0), "packed"));
    release(&lexed);
}

static void test_brackets(void) {
    CHECK(error_count(")") == 1);
    CHECK(error_count("(]") >= 1);
    CHECK(error_count("f(") == 1);
    CHECK(error_count("a # b") == 1);
    CHECK(error_count("caf\xC3\xA9") == 1);
    CHECK(error_count("/* never closed") == 1);
}

int main(void) {
    test_declarations();
    test_newlines();
    test_numbers();
    test_operators();
    test_strings();
    test_positions_and_names();
    test_brackets();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
