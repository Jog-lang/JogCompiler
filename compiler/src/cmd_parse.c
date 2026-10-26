#include "commands.h"
#include "lexer.h"
#include "phases.h"

#include "arena.h"
#include "da.h"
#include "diag.h"
#include "file.h"
#include "intern.h"

#include <stdio.h>

int jog_cmd_parse(int argc, char **argv) {
    if (argc != 1) {
        fprintf(stderr, "usage: jog parse <file>\n");
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
        JogNode *tree = jog_parse(arena, diag, intern, &file, &tokens);
        if (!tree) {
            fprintf(stderr, "jog: out of memory\n");
            status = 1;
        } else {
            CtSb sb;
            ct_sb_init(&sb);
            jog_ast_print(tree, &sb);
            fputs(ct_sb_cstr(&sb), stdout);
            if (sb.failed) {
                status = 1;
            }
            ct_sb_free(&sb);
        }
    }
    if (ct_diag_error_count(diag) > 0) {
        status = 1;
    }
    fflush(stdout);
    ct_diag_print(diag, stderr);

    ct_da_free(&tokens);
    ct_arena_destroy(arena);
    return status;
}
