#ifndef JOG_LOWER_STRING_H
#define JOG_LOWER_STRING_H

#include "lower_internal.h"

/* Scalar fmt and allocating interpolation. Pieces call jogrt_fmt_*.
   A user format method is called with the remaining buffer.
   Not a contract header. */

/* 1 when call is the fmt builtin. *out is the (str, error?) result.
   0 when it is not. */
int jog_lower_string_call(Lower *L, JogNode *node, JirOperand *out);

/* Lower an interpolated string to a str. */
JirOperand jog_lower_string_interp(Lower *L, JogNode *node);

#endif
