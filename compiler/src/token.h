#ifndef JOG_TOKEN_H
#define JOG_TOKEN_H

#include <stddef.h>
#include <stdint.h>

/* X(name, spelling). Keywords and punctuation spell as they are written;
   the other kinds have a descriptive name for messages. */
#define JOG_TOKEN_KINDS(X)              \
    X(EOF, "end of file")               \
    X(SEP, "newline")                   \
    X(IDENT, "identifier")              \
    X(INT, "integer literal")           \
    X(FLOAT, "float literal")           \
    X(CHAR, "char literal")             \
    X(STRING, "string literal")         \
    X(STR_HEAD, "string start")         \
    X(STR_MID, "string middle")         \
    X(STR_TAIL, "string end")           \
    X(CSTRING, "C string literal")      \
    X(HEXBLOB, "hex blob literal")      \
    X(ATTR, "attribute")                \
                                        \
    X(KW_FN, "fn")                      \
    X(KW_VAR, "var")                    \
    X(KW_VAL, "val")                    \
    X(KW_TYPE, "type")                  \
    X(KW_STRUCT, "struct")              \
    X(KW_INTERFACE, "interface")        \
    X(KW_ENUM, "enum")                  \
    X(KW_IF, "if")                      \
    X(KW_ELSE, "else")                  \
    X(KW_WHEN, "when")                  \
    X(KW_FOR, "for")                    \
    X(KW_IN, "in")                      \
    X(KW_BREAK, "break")                \
    X(KW_CONTINUE, "continue")          \
    X(KW_RETURN, "return")              \
    X(KW_DEFER, "defer")                \
    X(KW_ERRDEFER, "errdefer")          \
    X(KW_GO, "go")                      \
    X(KW_WITH, "with")                  \
    X(KW_CONTEXT, "context")            \
    X(KW_PRIVATE, "private")            \
    X(KW_IMPORT, "import")              \
    X(KW_AS, "as")                      \
    X(KW_IS, "is")                      \
    X(KW_NULL, "null")                  \
    X(KW_TRUE, "true")                  \
    X(KW_FALSE, "false")                \
    X(KW_OP, "op")                      \
    X(KW_ESCAPING, "escaping")          \
    X(KW_CONST, "const")                \
    X(KW_EXTERN, "extern")              \
    X(KW_QUOTE, "quote")                \
                                        \
    X(LPAREN, "(")                      \
    X(RPAREN, ")")                      \
    X(LBRACE, "{")                      \
    X(RBRACE, "}")                      \
    X(LBRACKET, "[")                    \
    X(RBRACKET, "]")                    \
    X(COMMA, ",")                       \
    X(COLON, ":")                       \
    X(DOT, ".")                         \
    X(DOTDOT, "..")                     \
    X(ELLIPSIS, "...")                  \
    X(QUESTION, "?")                    \
    X(QDOT, "?.")                       \
    X(ELVIS, "?:")                      \
    X(BANG, "!")                        \
    X(BANGBANG, "!!")                   \
    X(NEQ, "!=")                        \
    X(ARROW, "->")                      \
    X(ASSIGN, "=")                      \
    X(EQ, "==")                         \
    X(LT, "<")                          \
    X(LE, "<=")                         \
    X(CMP3, "<=>")                      \
    X(SHL, "<<")                        \
    X(SHL_ASSIGN, "<<=")                \
    X(GT, ">")                          \
    X(GE, ">=")                         \
    X(SHR, ">>")                        \
    X(SHR_ASSIGN, ">>=")                \
    X(PLUS, "+")                        \
    X(PLUS_ASSIGN, "+=")                \
    X(MINUS, "-")                       \
    X(MINUS_ASSIGN, "-=")               \
    X(STAR, "*")                        \
    X(STAR_ASSIGN, "*=")                \
    X(SLASH, "/")                       \
    X(SLASH_ASSIGN, "/=")               \
    X(PERCENT, "%")                     \
    X(PERCENT_ASSIGN, "%=")             \
    X(AMP, "&")                         \
    X(AMPAMP, "&&")                     \
    X(AMP_ASSIGN, "&=")                 \
    X(PIPE, "|")                        \
    X(PIPEPIPE, "||")                   \
    X(PIPE_ASSIGN, "|=")                \
    X(CARET, "^")                       \
    X(CARET_ASSIGN, "^=")               \
    X(TILDE, "~")                       \
    X(AS_Q, "as?")

typedef enum JogTokenKind {
#define JOG_TOKEN_ENUM(name, spelling) JOG_TOK_##name,
    JOG_TOKEN_KINDS(JOG_TOKEN_ENUM)
#undef JOG_TOKEN_ENUM
    JOG_TOK_COUNT
} JogTokenKind;

/* start/length is the lexeme in the source buffer. For a SEP made from a
   newline it is that newline, for an explicit ';' it is the ';', and at
   end of file it is empty.

   value is set for these kinds and NULL otherwise:
     IDENT, ATTR    interned name (an ATTR's name has no '@')
     STRING, STR_*  decoded text of that piece, escapes applied
     CSTRING        decoded text, without the terminating NUL
     CHAR           UTF-8 bytes of the one code point
     HEXBLOB        the decoded bytes
   The decoded text lives in the lexer's arena and is NUL-terminated after
   value_len bytes. */
typedef struct JogToken {
    JogTokenKind kind;
    uint32_t line;
    uint32_t column;
    const char *start;
    size_t length;
    const char *value;
    size_t value_len;
} JogToken;

/* "identifier", "fn", "+=", ... Never NULL. */
const char *jog_token_spelling(JogTokenKind kind);

/* "IDENT", "KW_FN", "PLUS_ASSIGN", ... Never NULL. */
const char *jog_token_name(JogTokenKind kind);

#endif
