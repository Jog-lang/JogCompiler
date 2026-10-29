#include "lexer.h"

#include "str.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { JOG_MAX_NESTING = 256 };

/* An open bracket. kind is '(', '[', '{', or '$' for "${". */
typedef struct Open {
    char kind;
    uint32_t line;
    uint32_t column;
} Open;

typedef struct Lexer {
    CtArena *arena;
    CtDiag *diag;
    CtIntern *intern;
    const char *path;
    const char *end;
    const char *p;
    uint32_t line;
    const char *line_start;
    CtDa *out;
    JogTokenKind last;
    bool has_last;
    Open stack[JOG_MAX_NESTING];
    size_t depth;
    CtSb buf;
    size_t errors;
    bool out_of_memory;
} Lexer;

static const char *const g_spellings[JOG_TOK_COUNT] = {
#define JOG_TOKEN_SPELLING(name, spelling) spelling,
    JOG_TOKEN_KINDS(JOG_TOKEN_SPELLING)
#undef JOG_TOKEN_SPELLING
};

static const char *const g_names[JOG_TOK_COUNT] = {
#define JOG_TOKEN_NAME(name, spelling) #name,
    JOG_TOKEN_KINDS(JOG_TOKEN_NAME)
#undef JOG_TOKEN_NAME
};

const char *jog_token_spelling(JogTokenKind kind) {
    if ((unsigned)kind >= JOG_TOK_COUNT) {
        return "?";
    }
    return g_spellings[kind];
}

const char *jog_token_name(JogTokenKind kind) {
    if ((unsigned)kind >= JOG_TOK_COUNT) {
        return "?";
    }
    return g_names[kind];
}

/* ---- character classes ---- */

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_value(char c) {
    if (is_digit(c)) {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c - 'A' + 10;
}

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_char(char c) {
    return is_ident_start(c) || is_digit(c);
}

/* ---- state helpers ---- */

static uint32_t column_of(const Lexer *lx, const char *at) {
    return (uint32_t)(at - lx->line_start) + 1;
}

static void next_line(Lexer *lx, const char *after_newline) {
    lx->line++;
    lx->line_start = after_newline;
}

#if defined(__GNUC__)
__attribute__((format(printf, 4, 5)))
#endif
static void lex_error(Lexer *lx, uint32_t line, uint32_t column, const char *fmt, ...) {
    char message[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    CtPos pos = {lx->path, line, column};
    ct_diag_emit(lx->diag, CT_SEVERITY_ERROR, pos, "%s", message);
    lx->errors++;
}

static void emit(Lexer *lx, JogTokenKind kind, const char *start, size_t length, uint32_t line, uint32_t column,
                 const char *value, size_t value_len) {
    JogToken token;
    token.kind = kind;
    token.line = line;
    token.column = column;
    token.start = start;
    token.length = length;
    token.value = value;
    token.value_len = value_len;
    if (!ct_da_push(lx->out, &token)) {
        lx->out_of_memory = true;
    }
    lx->last = kind;
    lx->has_last = true;
}

/* Copies the decoding buffer into the arena. */
static const char *take_buffer(Lexer *lx, size_t *len) {
    if (lx->buf.failed) {
        lx->out_of_memory = true;
    }
    CtStr view = ct_sb_view(&lx->buf);
    char *copy = ct_str_dup(lx->arena, view);
    if (!copy) {
        lx->out_of_memory = true;
        *len = 0;
        return "";
    }
    *len = view.len;
    return copy;
}

static bool push_open(Lexer *lx, char kind, uint32_t line, uint32_t column) {
    if (lx->depth == JOG_MAX_NESTING) {
        lex_error(lx, line, column, "brackets nested more than %d deep", JOG_MAX_NESTING);
        return false;
    }
    Open *open = &lx->stack[lx->depth++];
    open->kind = kind;
    open->line = line;
    open->column = column;
    return true;
}

static char top_open(const Lexer *lx) {
    return lx->depth ? lx->stack[lx->depth - 1].kind : 0;
}

/* ---- the newline rule ---- */

static bool ends_statement(JogTokenKind kind) {
    switch (kind) {
    case JOG_TOK_IDENT:
    case JOG_TOK_INT:
    case JOG_TOK_FLOAT:
    case JOG_TOK_CHAR:
    case JOG_TOK_STRING:
    case JOG_TOK_STR_TAIL:
    case JOG_TOK_CSTRING:
    case JOG_TOK_HEXBLOB:
    case JOG_TOK_RPAREN:
    case JOG_TOK_RBRACKET:
    case JOG_TOK_RBRACE:
    case JOG_TOK_QUESTION:
    case JOG_TOK_BANGBANG:
    case JOG_TOK_GT:
    case JOG_TOK_SHR:
    case JOG_TOK_KW_RETURN:
    case JOG_TOK_KW_BREAK:
    case JOG_TOK_KW_CONTINUE:
    case JOG_TOK_KW_TRUE:
    case JOG_TOK_KW_FALSE:
    case JOG_TOK_KW_NULL:
        return true;
    default:
        return false;
    }
}

/* Skips a block comment starting at q ("/" "*"), honoring nesting. Returns
   the position after it, or end if it is unterminated. Does not touch the
   lexer's line counters. */
static const char *skip_block_comment_raw(const char *q, const char *end, bool *saw_newline) {
    int nesting = 0;
    while (q < end) {
        if (q[0] == '/' && q[1] == '*') {
            nesting++;
            q += 2;
        } else if (q[0] == '*' && q[1] == '/') {
            q += 2;
            if (--nesting == 0) {
                return q;
            }
        } else {
            if (*q == '\n' && saw_newline) {
                *saw_newline = true;
            }
            q++;
        }
    }
    return end;
}

/* True if the next significant text after q starts with ".", "?.", or
   "?:", which continues the previous line. */
static bool continues_line(const Lexer *lx, const char *q) {
    const char *end = lx->end;
    while (q < end) {
        char c = *q;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            q++;
        } else if (c == '/' && q[1] == '/') {
            while (q < end && *q != '\n') {
                q++;
            }
        } else if (c == '/' && q[1] == '*') {
            q = skip_block_comment_raw(q, end, NULL);
        } else {
            return c == '.' || (c == '?' && (q[1] == '.' || q[1] == ':'));
        }
    }
    return false;
}

static void newline_rule(Lexer *lx, const char *at, size_t length, uint32_t line, uint32_t column) {
    char top = top_open(lx);
    bool significant = top == 0 || top == '{';
    if (significant && lx->has_last && ends_statement(lx->last) && !continues_line(lx, lx->p)) {
        emit(lx, JOG_TOK_SEP, at, length, line, column, NULL, 0);
    }
}

/* ---- comments ---- */

static void lex_block_comment(Lexer *lx) {
    const char *start = lx->p;
    uint32_t line = lx->line;
    uint32_t column = column_of(lx, start);
    int nesting = 0;
    bool saw_newline = false;
    while (lx->p < lx->end) {
        const char *q = lx->p;
        if (q[0] == '/' && q[1] == '*') {
            nesting++;
            lx->p += 2;
        } else if (q[0] == '*' && q[1] == '/') {
            lx->p += 2;
            if (--nesting == 0) {
                break;
            }
        } else {
            lx->p++;
            if (*q == '\n') {
                saw_newline = true;
                next_line(lx, lx->p);
            }
        }
    }
    if (nesting > 0) {
        lex_error(lx, line, column, "unterminated block comment");
    }
    if (saw_newline) {
        newline_rule(lx, start, 0, line, column);
    }
}

/* ---- literals ---- */

static void append_utf8(Lexer *lx, uint32_t cp) {
    char bytes[4];
    size_t n;
    if (cp < 0x80) {
        bytes[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        bytes[0] = (char)(0xC0 | (cp >> 6));
        bytes[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        bytes[0] = (char)(0xE0 | (cp >> 12));
        bytes[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        bytes[0] = (char)(0xF0 | (cp >> 18));
        bytes[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        bytes[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    ct_sb_append(&lx->buf, bytes, n);
}

/* lx->p is at a backslash. Appends the decoded bytes to lx->buf. */
static void lex_escape(Lexer *lx) {
    uint32_t line = lx->line;
    uint32_t column = column_of(lx, lx->p);
    lx->p++;
    char c = lx->p < lx->end ? *lx->p : '\0';
    switch (c) {
    case 'n': ct_sb_putc(&lx->buf, '\n'); lx->p++; return;
    case 't': ct_sb_putc(&lx->buf, '\t'); lx->p++; return;
    case 'r': ct_sb_putc(&lx->buf, '\r'); lx->p++; return;
    case '0': ct_sb_putc(&lx->buf, '\0'); lx->p++; return;
    case '\\': ct_sb_putc(&lx->buf, '\\'); lx->p++; return;
    case '"': ct_sb_putc(&lx->buf, '"'); lx->p++; return;
    case '\'': ct_sb_putc(&lx->buf, '\''); lx->p++; return;
    case '$': ct_sb_putc(&lx->buf, '$'); lx->p++; return;
    case 'x':
        if (is_hex_digit(lx->p[1]) && is_hex_digit(lx->p[2])) {
            ct_sb_putc(&lx->buf, (char)(hex_value(lx->p[1]) * 16 + hex_value(lx->p[2])));
            lx->p += 3;
        } else {
            lex_error(lx, line, column, "'\\x' needs exactly two hex digits");
            lx->p++;
        }
        return;
    case 'u': {
        if (lx->p[1] != '{') {
            lex_error(lx, line, column, "'\\u' must be followed by '{hex digits}'");
            lx->p++;
            return;
        }
        const char *q = lx->p + 2;
        uint32_t cp = 0;
        int digits = 0;
        while (is_hex_digit(*q) && digits < 7) {
            cp = cp * 16 + (uint32_t)hex_value(*q);
            q++;
            digits++;
        }
        if (*q != '}' || digits == 0 || digits > 6) {
            lex_error(lx, line, column, "'\\u{...}' needs 1 to 6 hex digits and a closing '}'");
            while (is_hex_digit(*q)) {
                q++;
            }
            if (*q == '}') {
                q++;
            }
            lx->p = q;
            return;
        }
        lx->p = q + 1;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            lex_error(lx, line, column, "'\\u{%X}' is not a Unicode scalar value", (unsigned)cp);
            return;
        }
        append_utf8(lx, cp);
        return;
    }
    default:
        if (c == '\n' || lx->p >= lx->end) {
            lex_error(lx, line, column, "unfinished escape sequence");
        } else {
            lex_error(lx, line, column, "unknown escape sequence '\\%c'", c);
            lx->p++;
        }
        return;
    }
}

/* Reads string text after an opening '"' or after the '}' that closes an
   interpolation. Stops at the closing '"', or at "${", which opens a new
   interpolation. */
static void lex_string_body(Lexer *lx, const char *start, uint32_t line, uint32_t column, bool continuation) {
    ct_sb_clear(&lx->buf);
    for (;;) {
        if (lx->p >= lx->end || *lx->p == '\n') {
            lex_error(lx, line, column, "unterminated string literal");
            size_t len;
            const char *value = take_buffer(lx, &len);
            emit(lx, continuation ? JOG_TOK_STR_TAIL : JOG_TOK_STRING, start, (size_t)(lx->p - start), line, column,
                 value, len);
            return;
        }
        char c = *lx->p;
        if (c == '"') {
            lx->p++;
            size_t len;
            const char *value = take_buffer(lx, &len);
            emit(lx, continuation ? JOG_TOK_STR_TAIL : JOG_TOK_STRING, start, (size_t)(lx->p - start), line, column,
                 value, len);
            return;
        }
        if (c == '$' && lx->p[1] == '{') {
            uint32_t open_column = column_of(lx, lx->p);
            lx->p += 2;
            size_t len;
            const char *value = take_buffer(lx, &len);
            emit(lx, continuation ? JOG_TOK_STR_MID : JOG_TOK_STR_HEAD, start, (size_t)(lx->p - start), line, column,
                 value, len);
            push_open(lx, '$', lx->line, open_column);
            return;
        }
        if (c == '\\') {
            lex_escape(lx);
            continue;
        }
        ct_sb_putc(&lx->buf, c);
        lx->p++;
    }
}

static void lex_cstring(Lexer *lx) {
    const char *start = lx->p;
    uint32_t line = lx->line;
    uint32_t column = column_of(lx, start);
    lx->p += 2;
    ct_sb_clear(&lx->buf);
    for (;;) {
        if (lx->p >= lx->end || *lx->p == '\n') {
            lex_error(lx, line, column, "unterminated C string literal");
            break;
        }
        char c = *lx->p;
        if (c == '"') {
            lx->p++;
            break;
        }
        if (c == '\\') {
            lex_escape(lx);
            continue;
        }
        ct_sb_putc(&lx->buf, c);
        lx->p++;
    }
    size_t len;
    const char *value = take_buffer(lx, &len);
    emit(lx, JOG_TOK_CSTRING, start, (size_t)(lx->p - start), line, column, value, len);
}

static void lex_raw_string(Lexer *lx) {
    const char *start = lx->p;
    uint32_t line = lx->line;
    uint32_t column = column_of(lx, start);
    lx->p++;
    const char *text = lx->p;
    while (lx->p < lx->end && *lx->p != '`') {
        if (*lx->p == '\n') {
            next_line(lx, lx->p + 1);
        }
        lx->p++;
    }
    size_t text_len = (size_t)(lx->p - text);
    if (lx->p < lx->end) {
        lx->p++;
    } else {
        lex_error(lx, line, column, "unterminated raw string literal");
    }
    char *value = ct_str_dup(lx->arena, ct_str_n(text, text_len));
    if (!value) {
        lx->out_of_memory = true;
        value = "";
        text_len = 0;
    }
    emit(lx, JOG_TOK_STRING, start, (size_t)(lx->p - start), line, column, value, text_len);
}

static void lex_hexblob(Lexer *lx) {
    const char *start = lx->p;
    uint32_t line = lx->line;
    uint32_t column = column_of(lx, start);
    lx->p += 2;
    ct_sb_clear(&lx->buf);
    int high = -1;
    size_t digits = 0;
    bool closed = false;
    while (lx->p < lx->end) {
        char c = *lx->p;
        if (c == '"') {
            lx->p++;
            closed = true;
            break;
        }
        if (is_hex_digit(c)) {
            digits++;
            if (high < 0) {
                high = hex_value(c);
            } else {
                ct_sb_putc(&lx->buf, (char)(high * 16 + hex_value(c)));
                high = -1;
            }
        } else if (c == '\n') {
            next_line(lx, lx->p + 1);
        } else if (c != ' ' && c != '\t' && c != '\r') {
            lex_error(lx, lx->line, column_of(lx, lx->p), "'%c' is not a hex digit", c);
        }
        lx->p++;
    }
    if (!closed) {
        lex_error(lx, line, column, "unterminated hex blob literal");
    } else if (digits % 2 != 0) {
        lex_error(lx, line, column, "hex blob has an odd number of digits; each byte needs two");
    }
    size_t len;
    const char *value = take_buffer(lx, &len);
    emit(lx, JOG_TOK_HEXBLOB, start, (size_t)(lx->p - start), line, column, value, len);
}

/* Length of the UTF-8 sequence at q, or 0 if it is malformed. */
static size_t utf8_length(const char *q, const char *end) {
    unsigned char lead = (unsigned char)q[0];
    size_t n;
    if (lead < 0x80) {
        return 1;
    } else if ((lead & 0xE0) == 0xC0) {
        n = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        n = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        n = 4;
    } else {
        return 0;
    }
    if ((size_t)(end - q) < n) {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if (((unsigned char)q[i] & 0xC0) != 0x80) {
            return 0;
        }
    }
    return n;
}

static void lex_char(Lexer *lx) {
    const char *start = lx->p;
    uint32_t line = lx->line;
    uint32_t column = column_of(lx, start);
    lx->p++;
    ct_sb_clear(&lx->buf);

    if (lx->p >= lx->end || *lx->p == '\n' || *lx->p == '\'') {
        lex_error(lx, line, column, *lx->p == '\'' ? "empty char literal" : "unterminated char literal");
        if (lx->p < lx->end && *lx->p == '\'') {
            lx->p++;
        }
    } else {
        if (*lx->p == '\\') {
            lex_escape(lx);
        } else {
            size_t n = utf8_length(lx->p, lx->end);
            if (n == 0) {
                lex_error(lx, line, column, "char literal is not valid UTF-8");
                n = 1;
            }
            ct_sb_append(&lx->buf, lx->p, n);
            lx->p += n;
        }
        if (lx->p < lx->end && *lx->p == '\'') {
            lx->p++;
        } else {
            lex_error(lx, line, column, "char literal must hold exactly one code point");
            while (lx->p < lx->end && *lx->p != '\'' && *lx->p != '\n') {
                lx->p++;
            }
            if (lx->p < lx->end && *lx->p == '\'') {
                lx->p++;
            }
        }
    }
    size_t len;
    const char *value = take_buffer(lx, &len);
    emit(lx, JOG_TOK_CHAR, start, (size_t)(lx->p - start), line, column, value, len);
}

static void lex_number(Lexer *lx) {
    const char *start = lx->p;
    uint32_t line = lx->line;
    uint32_t column = column_of(lx, start);
    bool is_float = false;
    const char *p = lx->p;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X' || p[1] == 'b' || p[1] == 'B' || p[1] == 'o' || p[1] == 'O')) {
        char base = (char)(p[1] | 0x20);
        p += 2;
        const char *digits = p;
        while (*p == '_' || (base == 'x' ? is_hex_digit(*p) : base == 'b' ? (*p == '0' || *p == '1')
                                                                         : (*p >= '0' && *p <= '7'))) {
            p++;
        }
        if (p == digits) {
            lex_error(lx, line, column, "expected digits after '0%c'", base);
        }
    } else {
        while (is_digit(*p) || *p == '_') {
            p++;
        }
        bool after_dot = lx->has_last && lx->last == JOG_TOK_DOT;
        if (p[0] == '.' && is_digit(p[1]) && !after_dot) {
            is_float = true;
            p++;
            while (is_digit(*p) || *p == '_') {
                p++;
            }
        }
        if (!after_dot && (p[0] == 'e' || p[0] == 'E') &&
            (is_digit(p[1]) || ((p[1] == '+' || p[1] == '-') && is_digit(p[2])))) {
            is_float = true;
            p += (p[1] == '+' || p[1] == '-') ? 2 : 1;
            while (is_digit(*p) || *p == '_') {
                p++;
            }
        }
    }

    if (is_ident_char(*p)) {
        lex_error(lx, lx->line, column_of(lx, p), "invalid character '%c' in number literal", *p);
        while (is_ident_char(*p)) {
            p++;
        }
    }
    lx->p = p;
    emit(lx, is_float ? JOG_TOK_FLOAT : JOG_TOK_INT, start, (size_t)(p - start), line, column, NULL, 0);
}

static JogTokenKind keyword_kind(const char *start, size_t len) {
    for (int kind = JOG_TOK_KW_FN; kind <= JOG_TOK_KW_QUOTE; kind++) {
        const char *spelling = g_spellings[kind];
        if (strlen(spelling) == len && memcmp(spelling, start, len) == 0) {
            /* `op` is special only as the start of an operator method name.
               Everywhere else it is an ordinary identifier. */
            if ((JogTokenKind)kind == JOG_TOK_KW_OP) {
                return JOG_TOK_IDENT;
            }
            return (JogTokenKind)kind;
        }
    }
    return JOG_TOK_IDENT;
}

static void lex_ident(Lexer *lx) {
    const char *start = lx->p;
    uint32_t line = lx->line;
    uint32_t column = column_of(lx, start);
    while (is_ident_char(*lx->p)) {
        lx->p++;
    }
    size_t len = (size_t)(lx->p - start);
    JogTokenKind kind = keyword_kind(start, len);
    if (kind == JOG_TOK_KW_AS && lx->p[0] == '?' && lx->p[1] != '.' && lx->p[1] != ':') {
        lx->p++;
        emit(lx, JOG_TOK_AS_Q, start, len + 1, line, column, NULL, 0);
        return;
    }
    if (kind != JOG_TOK_IDENT) {
        emit(lx, kind, start, len, line, column, NULL, 0);
        return;
    }
    const char *name = ct_intern(lx->intern, start, len);
    if (!name) {
        lx->out_of_memory = true;
        name = "";
    }
    emit(lx, JOG_TOK_IDENT, start, len, line, column, name, len);
}

static void lex_attribute(Lexer *lx) {
    const char *start = lx->p;
    uint32_t line = lx->line;
    uint32_t column = column_of(lx, start);
    lx->p++;
    if (!is_ident_start(*lx->p)) {
        lex_error(lx, line, column, "expected an attribute name after '@'");
        return;
    }
    const char *name_start = lx->p;
    while (is_ident_char(*lx->p)) {
        lx->p++;
    }
    size_t len = (size_t)(lx->p - name_start);
    const char *name = ct_intern(lx->intern, name_start, len);
    if (!name) {
        lx->out_of_memory = true;
        name = "";
    }
    emit(lx, JOG_TOK_ATTR, start, (size_t)(lx->p - start), line, column, name, len);
}

/* ---- punctuation ---- */

static void close_bracket(Lexer *lx, char open, char close, uint32_t line, uint32_t column) {
    char top = top_open(lx);
    if (top == open) {
        lx->depth--;
        return;
    }
    if (top == 0) {
        lex_error(lx, line, column, "'%c' has no matching '%c'", close, open);
    } else {
        const Open *o = &lx->stack[lx->depth - 1];
        char opened[3] = {o->kind, 0, 0};
        if (o->kind == '$') {
            opened[1] = '{';
        }
        lex_error(lx, line, column, "'%c' does not match '%s' opened at %u:%u", close, opened, (unsigned)o->line,
                  (unsigned)o->column);
    }
}

/* Longest-match punctuation. Returns false for a character that starts no
   token. */
static bool lex_punct(Lexer *lx) {
    const char *p = lx->p;
    uint32_t line = lx->line;
    uint32_t column = column_of(lx, p);
    JogTokenKind kind;
    size_t len = 1;

#define IF2(c1, k2) if (p[1] == (c1)) { kind = (k2); len = 2; }

    switch (p[0]) {
    case '(': kind = JOG_TOK_LPAREN; push_open(lx, '(', line, column); break;
    case '[': kind = JOG_TOK_LBRACKET; push_open(lx, '[', line, column); break;
    case '{': kind = JOG_TOK_LBRACE; push_open(lx, '{', line, column); break;
    case ')': kind = JOG_TOK_RPAREN; close_bracket(lx, '(', ')', line, column); break;
    case ']': kind = JOG_TOK_RBRACKET; close_bracket(lx, '[', ']', line, column); break;
    case '}': kind = JOG_TOK_RBRACE; close_bracket(lx, '{', '}', line, column); break;
    case ',': kind = JOG_TOK_COMMA; break;
    case ':': kind = JOG_TOK_COLON; break;
    case '~': kind = JOG_TOK_TILDE; break;
    case '.':
        kind = JOG_TOK_DOT;
        if (p[1] == '.') {
            kind = JOG_TOK_DOTDOT;
            len = 2;
            if (p[2] == '.') {
                kind = JOG_TOK_ELLIPSIS;
                len = 3;
            }
        }
        break;
    case '?':
        kind = JOG_TOK_QUESTION;
        IF2('.', JOG_TOK_QDOT) else IF2(':', JOG_TOK_ELVIS)
        break;
    case '!':
        kind = JOG_TOK_BANG;
        IF2('!', JOG_TOK_BANGBANG) else IF2('=', JOG_TOK_NEQ)
        break;
    case '-':
        kind = JOG_TOK_MINUS;
        IF2('>', JOG_TOK_ARROW) else IF2('=', JOG_TOK_MINUS_ASSIGN)
        break;
    case '=':
        kind = JOG_TOK_ASSIGN;
        IF2('=', JOG_TOK_EQ)
        break;
    case '<':
        kind = JOG_TOK_LT;
        if (p[1] == '=' && p[2] == '>') {
            kind = JOG_TOK_CMP3;
            len = 3;
        } else if (p[1] == '<' && p[2] == '=') {
            kind = JOG_TOK_SHL_ASSIGN;
            len = 3;
        } else IF2('<', JOG_TOK_SHL) else IF2('=', JOG_TOK_LE)
        break;
    case '>':
        kind = JOG_TOK_GT;
        if (p[1] == '>' && p[2] == '=') {
            kind = JOG_TOK_SHR_ASSIGN;
            len = 3;
        } else IF2('>', JOG_TOK_SHR) else IF2('=', JOG_TOK_GE)
        break;
    case '+':
        kind = JOG_TOK_PLUS;
        IF2('=', JOG_TOK_PLUS_ASSIGN)
        break;
    case '*':
        kind = JOG_TOK_STAR;
        IF2('=', JOG_TOK_STAR_ASSIGN)
        break;
    case '/':
        kind = JOG_TOK_SLASH;
        IF2('=', JOG_TOK_SLASH_ASSIGN)
        break;
    case '%':
        kind = JOG_TOK_PERCENT;
        IF2('=', JOG_TOK_PERCENT_ASSIGN)
        break;
    case '&':
        kind = JOG_TOK_AMP;
        IF2('&', JOG_TOK_AMPAMP) else IF2('=', JOG_TOK_AMP_ASSIGN)
        break;
    case '|':
        kind = JOG_TOK_PIPE;
        IF2('|', JOG_TOK_PIPEPIPE) else IF2('=', JOG_TOK_PIPE_ASSIGN)
        break;
    case '^':
        kind = JOG_TOK_CARET;
        IF2('=', JOG_TOK_CARET_ASSIGN)
        break;
    default:
        return false;
    }
#undef IF2

    lx->p += len;
    emit(lx, kind, p, len, line, column, NULL, 0);
    return true;
}

/* ---- driver ---- */

static void finish(Lexer *lx) {
    if (lx->depth > 0) {
        const Open *o = &lx->stack[lx->depth - 1];
        if (o->kind == '$') {
            lex_error(lx, o->line, o->column, "unterminated string interpolation");
        } else {
            lex_error(lx, o->line, o->column, "'%c' is never closed", o->kind);
        }
    }
    uint32_t line = lx->line;
    uint32_t column = column_of(lx, lx->p);
    if (lx->has_last && ends_statement(lx->last)) {
        emit(lx, JOG_TOK_SEP, lx->p, 0, line, column, NULL, 0);
    }
    emit(lx, JOG_TOK_EOF, lx->p, 0, line, column, NULL, 0);
}

static void run(Lexer *lx) {
    while (lx->p < lx->end && !lx->out_of_memory) {
        const char *p = lx->p;
        char c = *p;

        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            lx->p++;
            continue;
        }
        if (c == '\n') {
            uint32_t line = lx->line;
            uint32_t column = column_of(lx, p);
            lx->p++;
            next_line(lx, lx->p);
            newline_rule(lx, p, 1, line, column);
            continue;
        }
        if (c == '/' && p[1] == '/') {
            while (lx->p < lx->end && *lx->p != '\n') {
                lx->p++;
            }
            continue;
        }
        if (c == '/' && p[1] == '*') {
            lex_block_comment(lx);
            continue;
        }
        if (c == ';') {
            emit(lx, JOG_TOK_SEP, p, 1, lx->line, column_of(lx, p), NULL, 0);
            lx->p++;
            continue;
        }
        if (c == '}' && top_open(lx) == '$') {
            uint32_t line = lx->line;
            uint32_t column = column_of(lx, p);
            lx->depth--;
            lx->p++;
            lex_string_body(lx, p, line, column, true);
            continue;
        }
        if ((c == 'c' || c == 'x') && p[1] == '"') {
            if (c == 'c') {
                lex_cstring(lx);
            } else {
                lex_hexblob(lx);
            }
            continue;
        }
        if (is_ident_start(c)) {
            lex_ident(lx);
            continue;
        }
        if (is_digit(c)) {
            lex_number(lx);
            continue;
        }
        if (c == '"') {
            uint32_t line = lx->line;
            uint32_t column = column_of(lx, p);
            lx->p++;
            lex_string_body(lx, p, line, column, false);
            continue;
        }
        if (c == '`') {
            lex_raw_string(lx);
            continue;
        }
        if (c == '\'') {
            lex_char(lx);
            continue;
        }
        if (c == '@') {
            lex_attribute(lx);
            continue;
        }
        if (lex_punct(lx)) {
            continue;
        }

        uint32_t column = column_of(lx, p);
        if ((unsigned char)c >= 0x80) {
            size_t n = utf8_length(p, lx->end);
            lex_error(lx, lx->line, column, "non-ASCII character outside a string; identifiers are ASCII");
            lx->p += n ? n : 1;
        } else if (c >= 0x20 && c < 0x7F) {
            lex_error(lx, lx->line, column, "unexpected character '%c'", c);
            lx->p++;
        } else {
            lex_error(lx, lx->line, column, "unexpected byte 0x%02X", (unsigned)(unsigned char)c);
            lx->p++;
        }
    }
    finish(lx);
}

bool jog_lex(CtArena *arena, CtDiag *diag, CtIntern *intern, const CtFile *file, CtDa *tokens) {
    if (!arena || !intern || !file || !file->data || !tokens || tokens->item_size != sizeof(JogToken)) {
        return false;
    }

    Lexer lx;
    memset(&lx, 0, sizeof lx);
    lx.arena = arena;
    lx.diag = diag;
    lx.intern = intern;
    lx.path = file->path;
    lx.p = file->data;
    lx.end = file->data + file->size;
    lx.line = 1;
    lx.line_start = file->data;
    lx.out = tokens;
    ct_sb_init(&lx.buf);

    if (file->size >= 3 && memcmp(file->data, "\xEF\xBB\xBF", 3) == 0) {
        lx.p += 3;
        lx.line_start = lx.p;
    }

    run(&lx);
    ct_sb_free(&lx.buf);
    return lx.errors == 0 && !lx.out_of_memory;
}
