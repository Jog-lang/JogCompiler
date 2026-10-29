#ifndef JOG_TYPES_H
#define JOG_TYPES_H

#include "arena.h"
#include "str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Semantic types. Structural types (pointers, slices, arrays, options,
   tuples, functions, [N]byte) are interned: asking twice for the same
   shape returns the same JogType *, so equality is pointer equality.
   Nominal types (struct, enum, interface, distinct) are created once per
   declaration (or per generic instance) and are unique by construction.

   The scalar names are bound from core/scalar.jog onto these builtins.
   str is a distinct type over the const-bytes view, created when the
   prelude is bound. rune is a distinct type over the u32 builtin.
   Option<T> stays JOG_TY_OPTION; the prelude enum is that constructor.
   error and void are the core interface and the incomplete struct.
   Operator methods from that file are stored beside the builtins, not as
   scope names. A user distinct type may set intrinsic when its @intrinsic
   matches a machine type.

   Contract owner: see docs/plans/V1Parallel.md. */

typedef struct JogNode JogNode;
typedef struct JogType JogType;

typedef enum JogTypeKind {
    JOG_TY_ERROR = 0,     /* poison after a reported error; compatible with everything */
    JOG_TY_UNIT,          /* () */
    JOG_TY_BOOL,
    JOG_TY_INT,
    JOG_TY_FLOAT,
    JOG_TY_UNTYPED_INT,   /* integer constant before it gets a type (defaults to i64) */
    JOG_TY_UNTYPED_FLOAT, /* float constant before it gets a type (defaults to f64) */
    JOG_TY_NULL,          /* the type of null before it gets an Option type */
    JOG_TY_BYTES_N,       /* [N]byte, stored inline */
    JOG_TY_BYTES,         /* bytes / const bytes: pointer + length */
    JOG_TY_PTR,           /* *T, *const T; never null */
    JOG_TY_SLICE,         /* []T, []const T */
    JOG_TY_ARRAY,         /* [N]T */
    JOG_TY_OPTION,        /* T? */
    JOG_TY_TUPLE,
    JOG_TY_FN,
    JOG_TY_STRUCT,
    JOG_TY_ENUM,
    JOG_TY_INTERFACE,
    JOG_TY_DISTINCT,      /* type A: B */
    JOG_TY_TYPE_PARAM     /* T inside a generic body before instantiation */
} JogTypeKind;

typedef struct JogField {
    const char *name;
    JogType *type;
    uint64_t offset;   /* set by jog_type_layout */
    bool embedded;
    JogNode *decl;     /* FIELD or EMBED node, may be NULL for synthesized fields */
} JogField;

typedef struct JogVariant {
    const char *name;
    JogField *fields;  /* payload, laid out like a struct starting at payload_offset */
    size_t field_count;
    int64_t tag;
    JogNode *decl;
} JogVariant;

struct JogType {
    JogTypeKind kind;
    uint32_t id;      /* unique, dense, assigned at creation */
    bool is_const;    /* PTR, SLICE, BYTES */
    bool has_layout;  /* size and align are valid */
    uint64_t size;
    uint32_t align;
    /* 0, or JOG_INTRINSIC_*. Set on a distinct type declared with @intrinsic.
       Builtin integers, floats, and bool keep their kind and leave this 0. */
    uint8_t intrinsic;
    uint8_t intrinsic_bits;
    uint8_t intrinsic_signed;
    uint8_t intrinsic_ptr;
    union {
        struct {
            uint8_t bits; /* 8, 16, 32, 64 */
            bool is_signed;
            bool is_size; /* usize / isize: bits follow the target pointer size */
        } integer;
        struct {
            uint8_t bits; /* 32, 64 */
        } flt;
        struct {
            uint64_t n;
        } bytes_n;
        struct {
            JogType *elem; /* PTR, SLICE, OPTION */
        } elem;
        struct {
            JogType *elem;
            uint64_t len;
        } array;
        struct {
            JogType **elems;
            size_t count;
            uint64_t *offsets;
        } tuple;
        struct {
            JogType **params;
            size_t param_count;
            JogType *ret;          /* UNIT when there is no result */
            JogType **contexts;
            size_t context_count;
            bool variadic;         /* C variadic, extern only */
            bool escaping;
            bool is_extern;        /* plain C function pointer, no environment */
        } fn;
        struct {
            const char *name;      /* declared name, for messages */
            const char *package;   /* package path, e.g. "net.http" */
            JogNode *decl;         /* STRUCT, ENUM, INTERFACE, or TYPE_DECL */
            JogType **type_args;   /* generic instance arguments, NULL otherwise */
            size_t type_arg_count;
            JogType *base;         /* DISTINCT: the base type; ENUM: the tag type */
            JogField *fields;      /* STRUCT */
            size_t field_count;
            JogVariant *variants;  /* ENUM */
            size_t variant_count;
            uint64_t payload_offset; /* ENUM: where payloads start */
        } nominal;
        struct {
            const char *name;
            JogNode *decl;         /* TYPE_PARAM */
            JogType *constraint;   /* INTERFACE or NULL */
        } param;
    } as;
};

enum {
    JOG_INTRINSIC_NONE = 0,
    JOG_INTRINSIC_INT = 1,
    JOG_INTRINSIC_FLOAT = 2,
    JOG_INTRINSIC_BOOL = 3
};

/* What a value occupies: a builtin integer, float, or bool, or a distinct
   type built on one of those. bits is 1 for bool. is_ptr is isize or usize. */
typedef struct JogMachine {
    int kind;
    unsigned bits;
    int is_signed;
    int is_ptr;
} JogMachine;

typedef struct JogTypes JogTypes;

typedef enum JogBuiltin {
    JOG_BUILTIN_I8, JOG_BUILTIN_I16, JOG_BUILTIN_I32, JOG_BUILTIN_I64,
    JOG_BUILTIN_U8, JOG_BUILTIN_U16, JOG_BUILTIN_U32, JOG_BUILTIN_U64,
    JOG_BUILTIN_ISIZE, JOG_BUILTIN_USIZE,
    JOG_BUILTIN_F32, JOG_BUILTIN_F64,
    JOG_BUILTIN_BOOL,
    JOG_BUILTIN_UNIT,
    JOG_BUILTIN_BYTES,       /* bytes */
    JOG_BUILTIN_CONST_BYTES, /* []const byte, the base of str */
    JOG_BUILTIN_UNTYPED_INT,
    JOG_BUILTIN_UNTYPED_FLOAT,
    JOG_BUILTIN_NULL,
    JOG_BUILTIN_ERROR,
    JOG_BUILTIN_COUNT
} JogBuiltin;

/* ptr_size is 8 for x64 and 4 for wasm32. NULL on bad arguments or out of
   memory. */
JogTypes *jog_types_create(CtArena *arena, uint32_t ptr_size);

/* Frees the table's scratch buffer. The types themselves live in the arena.
   NULL is ignored. */
void jog_types_release(JogTypes *types);

uint32_t jog_types_ptr_size(const JogTypes *types);

JogType *jog_type_builtin(JogTypes *types, JogBuiltin which);

/* Interned structural types. NULL only on out of memory. Arrays of element
   types are copied, so callers may pass temporary buffers. */
JogType *jog_type_bytes_n(JogTypes *types, uint64_t n);
JogType *jog_type_ptr(JogTypes *types, JogType *elem, bool is_const);
JogType *jog_type_slice(JogTypes *types, JogType *elem, bool is_const);
JogType *jog_type_array(JogTypes *types, JogType *elem, uint64_t len);
JogType *jog_type_option(JogTypes *types, JogType *elem);
JogType *jog_type_tuple(JogTypes *types, JogType *const *elems, size_t count);
JogType *jog_type_fn(JogTypes *types, JogType *const *params, size_t param_count, JogType *ret,
                     JogType *const *contexts, size_t context_count, bool variadic, bool escaping, bool is_extern);

/* A fresh nominal type: STRUCT, ENUM, INTERFACE, or DISTINCT. The caller
   fills as.nominal (fields, variants, base, type_args) and then calls
   jog_type_layout. */
JogType *jog_type_new_nominal(JogTypes *types, JogTypeKind kind, const char *name, const char *package,
                              JogNode *decl);

JogType *jog_type_new_param(JogTypes *types, const char *name, JogNode *decl, JogType *constraint);

/* Computes size, align, and field offsets with C layout rules. Structural
   types get their layout at creation when their parts already have one;
   call this for nominal types once their fields are known, and again for
   structural types created before that. Returns false if a part has no
   layout yet (the checker orders declarations so that does not happen, and
   reports recursive value types itself). Idempotent. */
bool jog_type_layout(JogTypes *types, JogType *type);

/* Strips DISTINCT layers. */
JogType *jog_type_underlying(JogType *type);

/* The distinct str from the core prelude, or NULL before it is bound. */
JogType *jog_type_core_str(const JogTypes *types);
void jog_types_set_core_str(JogTypes *types, JogType *type);

/* 1 when type is that distinct str. */
int jog_type_is_core_str(const JogTypes *types, const JogType *type);

/* BYTES itself, or the BYTES under distinct layers. NULL otherwise. */
JogType *jog_type_bytes_view(JogType *type);

/* 1 when got converts to core str: bytes, []byte, []const byte, []u8,
   []const u8. The reverse is not this conversion. */
int jog_type_converts_to_str(const JogTypes *types, const JogType *got, const JogType *want);

/* 1 when type is an integer, float, or bool, including a distinct type whose
   underlying type is one, or whose own @intrinsic says so. */
int jog_type_machine(const JogType *type, JogMachine *out);

/* Record a user @intrinsic and raise align to the machine alignment. */
void jog_type_set_intrinsic(JogType *type, int kind, unsigned bits, int is_signed, int is_ptr);

/* Prelude operator methods. Not scope bindings. value_params is 0 or 1,
   not counting the receiver. 1 when recorded, 0 on a duplicate or OOM. */
int jog_types_add_scalar_op(JogTypes *types, JogType *on, const char *name, int value_params, JogType *ret);

/* Result type of that prelude operator, or NULL. */
JogType *jog_types_scalar_op(const JogTypes *types, const JogType *on, const char *name, int value_params);

/* True for *T? and other options whose None is an all-zero niche, which are
   represented as a plain nullable pointer. */
bool jog_type_option_is_niche(const JogType *type);

/* Appends the Jog spelling, e.g. "*const [4]i32", "(i32, str)", "Map<str, i32>". */
void jog_type_print(const JogType *type, CtSb *out);

#endif
