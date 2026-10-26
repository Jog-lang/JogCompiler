#ifndef JOG_FLOAT_LIT_H
#define JOG_FLOAT_LIT_H

#include "ast.h"

/* A float literal, unary minus of one, or + - * / % of those.
   An integer literal is not a float tree. */
int jog_float_lit_tree(const JogNode *node);

/* Decode the UTF-8 bytes of one char literal. 0 when they are not one
   Unicode scalar value. */
int jog_char_codepoint(const char *text, size_t len, uint32_t *out);

typedef enum JogFloatLitStatus {
    JOG_FLOAT_LIT_OK = 0,
    JOG_FLOAT_LIT_BAD,
    JOG_FLOAT_LIT_FIT,
    JOG_FLOAT_LIT_DIV0
} JogFloatLitStatus;

/* Evaluate the tree as IEEE binary64, then round to `bits` (32 or 64).
   A 32-bit result is the float32 value promoted back to double.
   Overflow of that format is JOG_FLOAT_LIT_FIT. Division or remainder by
   zero is JOG_FLOAT_LIT_DIV0. */
JogFloatLitStatus jog_float_lit_eval(const JogNode *node, unsigned bits, double *out);

#endif
