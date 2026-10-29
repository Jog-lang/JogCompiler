#ifndef JOG_SCALAR_OP_H
#define JOG_SCALAR_OP_H

#include "ast.h"
#include "types.h"

/* A parsed @intrinsic(...) attribute. kind is JOG_INTRINSIC_* or 0.
   unknown is the positional name when it is not int, float, or bool. */
typedef struct JogIntrinsicSpec {
    int kind;
    int has_signed;
    int is_signed;
    int has_bits;
    int bits_ptr;
    uint64_t bits;
    const char *unknown;
} JogIntrinsicSpec;

/* 1 when attr is @intrinsic(...) with one positional and well-formed arguments.
   0 when the shape is wrong. */
int jog_parse_intrinsic(const JogNode *attr, JogIntrinsicSpec *out);

/* 1 when the spec names a width the compiler has. 0 when a required field is
   missing. -1 when the width is not one the compiler has. *bits is the width.
   *is_ptr is 1 for bits: ptr. Bool reports bits 8. */
int jog_intrinsic_width_ok(const JogIntrinsicSpec *spec, uint32_t ptr_size, unsigned *bits, int *is_ptr);

/* 1 when target is byte, [N]byte, or [ptr]byte for that width. */
int jog_intrinsic_base_ok(const JogNode *target, unsigned bits, int is_ptr);

/* "op+", or NULL when op is not a scalar operator. */
const char *jog_op_method_name(JogTokenKind op);

/* "add" or "neg", or NULL when op is not that arity. unary is 1 for no value parameter. */
const char *jog_op_intrinsic_name(JogTokenKind op, int unary);

/* 1 when this intrinsic exists for JOG_INTRINSIC_INT, FLOAT, or BOOL. */
int jog_intrinsic_allowed(int kind, const char *intrinsic);

/* 1 when the intrinsic's result is bool. */
int jog_intrinsic_returns_bool(const char *intrinsic);

/* 1 when name is add, sub, mul, or another operator intrinsic. */
int jog_is_op_intrinsic_name(const char *name);

/* 1 when fn is an expression-bodied operator whose body is that one intrinsic call.
   *intrinsic_out receives the intrinsic name. */
int jog_fn_intrinsic_body(const JogNode *fn, const char **intrinsic_out);

/* When decl is a distinct type with a matching @intrinsic, set the flag and
   the machine alignment. Does not report. Returns 1 when the flag was set. */
int jog_type_apply_intrinsic(JogTypes *types, JogType *type, const JogNode *decl);

#endif
