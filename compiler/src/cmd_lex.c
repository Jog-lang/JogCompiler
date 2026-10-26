#include "commands.h"
#include "lexer.h"

#include "arena.h"
#include "da.h"
#include "diag.h"
#include "file.h"
#include "intern.h"

#include <stdio.h>

/* Writes bytes with C-style escapes so every token prints on one line. */
static void print_escaped(FILE *out, const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        switch (c) {
        case '\n': fputs("\\n", out); break;
        case '\t': fputs("\\t", out); break;
        case '\r': fputs("\\r", out); break;
        case '\\': fputs("\\\\", out); break;
        case '"': fputs("\\\"", out); break;
        default:
            if (c < 0x20 || c == 0x7F) {
                fprintf(out, "\\x%02X", c);
            } else {
                fputc(c, out);
            }
        }
    }
}

static void print_token(FILE *out, const JogToken *tok) {
    fprintf(out, "%4u:%-4u %-14s", (unsigned)tok->line, (unsigned)tok->column, jog_token_name(tok->kind));
    switch (tok->kind) {
    case JOG_TOK_SEP:
    case JOG_TOK_EOF:
        break;
    case JOG_TOK_STRING:
    case JOG_TOK_STR_HEAD:
    case JOG_TOK_STR_MID:
    case JOG_TOK_STR_TAIL:
    case JOG_TOK_CSTRING:
    case JOG_TOK_CHAR:
    case JOG_TOK_HEXBLOB:
        fputc('"', out);
        print_escaped(out, tok->value, tok->value_len);
        fputc('"', out);
        break;
    default:
        fwrite(tok->start, 1, tok->length, out);
        break;
    }
    fputc('\n', out);
}

int jog_cmd_lex(int argc, char **argv) {
    if (argc != 1) {
        fprintf(stderr, "usage: jog lex <file>\n");
        return 2;
    }
    CtArena *arena = ct_arena_create();
    CtDiag *diag = ct_diag_create(arena);
    CtIntern *intern = ct_intern_create(arena);
    if (!arena || !diag || !intern) {
        fprintf(stderr, "jog: out of memory\n");
        ct_arena_destroy(arena);
        return 1;
    }

    int status = 0;
    CtFile file;
    CtDa tokens;
    ct_da_init(&tokens, sizeof(JogToken));
    if (!ct_file_read(arena, diag, argv[0], &file)) {
        status = 1;
    } else {
        if (!jog_lex(arena, diag, intern, &file, &tokens)) {
            status = 1;
        }
        for (size_t i = 0; i < tokens.count; i++) {
            print_token(stdout, ct_da_at(&tokens, i));
        }
    }
    fflush(stdout);
    ct_diag_print(diag, stderr);

    ct_da_free(&tokens);
    ct_arena_destroy(arena);
    return status;
}
