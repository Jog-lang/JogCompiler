#ifndef JOG_LEXER_H
#define JOG_LEXER_H

#include "token.h"

#include "arena.h"
#include "da.h"
#include "diag.h"
#include "file.h"
#include "intern.h"

#include <stdbool.h>

/* Splits a whole file into tokens and appends them to tokens, which must be
   initialized with item_size sizeof(JogToken). The last token is always
   EOF. Newlines become SEP tokens by the rule in docs/design/language/
   Grammar.md. A string with interpolation becomes STR_HEAD, the tokens of
   each ${...} expression, STR_MID between expressions, and STR_TAIL.

   Errors go to diag, and lexing continues after each one. Returns false if
   any error was emitted or memory ran out. Token values point into arena
   and into file->data, so both must outlive the tokens. */
bool jog_lex(CtArena *arena, CtDiag *diag, CtIntern *intern, const CtFile *file, CtDa *tokens);

#endif
