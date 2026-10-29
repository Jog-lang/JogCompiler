#ifndef JOG_AST_H
#define JOG_AST_H

#include "token.h"

#include "arena.h"
#include "da.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The syntax tree for one file. Every node lives in the program arena and
   is never freed. Names are interned (CtIntern), so they compare by
   pointer. The parser fills the syntax fields; the resolver and checker
   fill `type` and `symbol` and must not change the shape of the tree.
   Lowering reads the checked tree.

   Contract owner: see docs/plans/V1Parallel.md. Changing this header
   affects every stream. */

typedef struct JogType JogType;
typedef struct JogSymbol JogSymbol;
typedef struct JogNode JogNode;

typedef struct JogPos {
    const char *file;
    uint32_t line;
    uint32_t column;
} JogPos;

/* Fixed array of nodes in the arena. */
typedef struct JogList {
    JogNode **items;
    size_t count;
} JogList;

/* Fixed array of interned names. "_" is interned like any other name. */
typedef struct JogNames {
    const char **items;
    size_t count;
} JogNames;

typedef enum JogVisibility {
    JOG_VIS_PUBLIC = 0,
    JOG_VIS_PACKAGE, /* private(package) */
    JOG_VIS_FILE,    /* private, private(file) */
    JOG_VIS_SCOPE    /* private(scope) */
} JogVisibility;

/* X(kind, union member). The member says which field of JogNode.as holds
   the node's data; "none" means the node has no data of its own. */
#define JOG_NODE_KINDS(X)                                                       \
    /* files and declarations */                                                \
    X(FILE, file)                                                               \
    X(IMPORT, import)                                                           \
    X(FN, fn)                                                                   \
    X(VAR, binding)                                                             \
    X(VAL, binding)                                                             \
    X(CONTEXT, binding)       /* context x: T = e; names has one entry */      \
    X(TYPE_DECL, type_decl)                                                     \
    X(STRUCT, aggregate)                                                        \
    X(INTERFACE, aggregate)                                                     \
    X(ENUM, aggregate)                                                          \
    X(EXTERN_BLOCK, extern_block)                                               \
    X(ATTRIBUTE, attribute)                                                     \
    /* members */                                                               \
    X(FIELD, field)           /* struct field, interface val/var property */   \
    X(EMBED, embed)           /* struct embedding, interface embedding */      \
    X(TYPE_SET, type_set)     /* interface type set: ~int | ~uint */           \
    X(VARIANT, variant)                                                         \
    X(PARAM, param)           /* fn, lambda, context, variant payload */       \
    X(TYPE_PARAM, type_param)                                                   \
    /* type expressions */                                                      \
    X(TYPE_NAME, type_name)                                                     \
    X(TYPE_PTR, type_elem)                                                      \
    X(TYPE_SLICE, type_elem)                                                    \
    X(TYPE_ARRAY, type_array)                                                   \
    X(TYPE_OPTION, type_elem)                                                   \
    X(TYPE_TUPLE, type_tuple)                                                   \
    X(TYPE_FN, type_fn)                                                         \
    X(TYPE_UNIT, none)                                                          \
    /* statements */                                                            \
    X(BLOCK, block)           /* also a block used as an expression */         \
    X(EXPR_STMT, expr_stmt)                                                     \
    X(ASSIGN, assign)                                                           \
    X(DESTRUCTURE, destructure) /* (a, b) = f() */                             \
    X(WITH, with)                                                               \
    X(DEFER, defer)                                                             \
    X(ERRDEFER, defer)                                                          \
    X(RETURN, ret)                                                              \
    X(FOR, for_)                                                                \
    X(BREAK, none)                                                              \
    X(CONTINUE, none)                                                           \
    X(ATTR_BLOCK, attr_block) /* @unchecked { ... }; attrs on the node */      \
    /* expressions */                                                           \
    X(INT_LIT, literal)       /* text is the lexeme, e.g. "0xFF_00" */         \
    X(FLOAT_LIT, literal)                                                       \
    X(STRING_LIT, literal)    /* text is the decoded value */                  \
    X(CSTRING_LIT, literal)                                                     \
    X(CHAR_LIT, literal)      /* UTF-8 bytes of one code point */              \
    X(HEXBLOB_LIT, literal)                                                     \
    X(BOOL_LIT, literal)                                                        \
    X(NULL_LIT, none)                                                           \
    X(INTERP, interp)         /* "a ${x} b" */                                 \
    X(IDENT, ident)                                                             \
    X(DOT_VARIANT, ident)     /* .Empty, type from context */                  \
    X(TUPLE, tuple)                                                             \
    X(UNARY, unary)           /* - ! ~ * (deref) & (address) */                \
    X(BINARY, binary)         /* arithmetic, logic, compare, <=>, ?:, .. */    \
    X(CAST, cast)             /* as, as? */                                    \
    X(IS, is)                                                                   \
    X(CALL, call)                                                               \
    X(ARG, arg)                                                                 \
    X(INDEX, index)                                                             \
    X(SLICE, slice)           /* a[lo..hi], either bound optional */           \
    X(MEMBER, member)         /* a.b, a?.b, a.0 */                             \
    X(FORCE, unary)           /* a!! */                                        \
    X(IF, if_)                                                                  \
    X(WHEN, when)                                                               \
    X(WHEN_ARM, when_arm)                                                       \
    X(LAMBDA, lambda)                                                           \
    X(GO, go)                                                                   \
    X(ARRAY_LIT, array_lit)   /* [4]i32(1, 2), [_]T(...), []T(...) */          \
    X(BUILTIN, builtin)       /* @comptime(e), @embed("f") */                  \
    /* patterns, only inside WHEN_ARM.conds */                                  \
    X(PAT_VARIANT, pat_variant) /* Circle(r), Rect(w: 1, h) */                 \
    X(PAT_FIELD, pat_field)                                                     \
    X(PAT_BIND, ident)        /* bare name; resolver may turn it into a variant */ \
    X(PAT_WILDCARD, none)     /* _ */                                          \
    X(PAT_IN, pat_expr)       /* in 0..10 */                                   \
    X(PAT_IS, pat_is)         /* is T */                                       \
    X(PAT_EXPR, pat_expr)     /* a constant compared with == */

typedef enum JogNodeKind {
#define JOG_NODE_ENUM(kind, member) JOG_NODE_##kind,
    JOG_NODE_KINDS(JOG_NODE_ENUM)
#undef JOG_NODE_ENUM
    JOG_NODE_COUNT
} JogNodeKind;

typedef enum JogForKind {
    JOG_FOR_FOREVER = 0, /* for { } */
    JOG_FOR_COND,        /* for cond { } */
    JOG_FOR_IN           /* for i in xs { }, for i, x in xs { } */
} JogForKind;

struct JogNode {
    JogNodeKind kind;
    JogPos pos;
    JogList attrs;            /* ATTRIBUTE nodes written before the node */
    JogVisibility visibility; /* declarations and members only */

    /* Filled by the checker. type is the type of an expression, of a type
       expression, or of a declared entity. symbol is what an IDENT,
       TYPE_NAME, or MEMBER resolved to. */
    JogType *type;
    JogSymbol *symbol;

    union {
        struct {
            JogList imports; /* IMPORT */
            JogList decls;   /* everything else at top level, in source order */
        } file;
        struct {
            JogNames path;     /* net.http -> {"net", "http"} */
            const char *alias; /* NULL means the last path segment */
        } import;
        struct {
            const char *name;         /* for operators: "op+", "op<=>", "op[]=" */
            JogTokenKind op;          /* the operator token, 0 for ordinary functions */
            JogNode *receiver;        /* type expression, NULL for free functions */
            bool receiver_is_ptr;     /* fn *T.m() */
            JogList type_params;      /* TYPE_PARAM */
            JogList params;           /* PARAM */
            JogList contexts;         /* PARAM, from with (...) */
            JogNode *ret;             /* type expression, NULL means () */
            JogNode *body;            /* BLOCK, an expression for "= expr", NULL for extern */
            bool expr_body;
            bool is_extern;           /* declared inside an extern block */
            bool variadic;            /* extern only: trailing ... */
            /* expect fn has no body. actual fn has the body and takes
               visibility from its expect. No existing field changed meaning. */
            bool is_expect;
            bool is_actual;
        } fn;
        struct {
            JogNames names;  /* one name, or several for val (a, b) = ... */
            bool destructure;
            JogNode *type;   /* NULL when inferred */
            JogNode *init;   /* NULL when absent (var only) */
        } binding;
        struct {
            const char *name;
            JogList type_params;
            JogNode *target;
            bool distinct; /* type A: B rather than type A = B */
        } type_decl;
        struct {
            const char *name;
            JogList type_params;
            JogNode *tag_type; /* enum only: enum E: u8 */
            JogList members;   /* FIELD, EMBED, FN (interface methods), TYPE_SET, VARIANT */
        } aggregate;
        struct {
            JogList fns; /* FN with is_extern set */
        } extern_block;
        struct {
            const char *name; /* without '@' */
            JogList args;     /* ARG */
            bool has_parens;
        } attribute;
        struct {
            const char *name;
            JogNode *type;
            JogNode *default_value; /* NULL if none */
            bool is_var;            /* interface property declared with var */
        } field;
        struct {
            JogNode *type;
        } embed;
        struct {
            JogList terms;      /* type expressions */
            bool *approximate;  /* parallel to terms: true for ~T */
        } type_set;
        struct {
            const char *name;
            JogList fields;  /* PARAM; empty for payload-free variants */
            JogNode *value;  /* explicit tag value, or NULL */
        } variant;
        struct {
            const char *name;
            JogNode *type;          /* NULL for untyped lambda parameters.
                                       A pack stores the element type here. */
            JogNode *default_value;
            /* True when written `...T: name`. The checked type is []T.
               No existing field changed meaning. */
            bool pack;
        } param;
        struct {
            const char *name;
            JogNode *constraint; /* NULL means any */
        } type_param;

        struct {
            JogNames path; /* std.io.Reader -> {"std", "io", "Reader"} */
            JogList args;  /* type expressions */
            bool is_const; /* const Name: a const pointer, slice, or bytes view */
        } type_name;
        struct {
            JogNode *elem;
            bool is_const;
        } type_elem;
        struct {
            JogNode *len; /* expression; NULL with infer_len for [_] */
            bool infer_len;
            JogNode *elem;
        } type_array;
        struct {
            JogList elems;
        } type_tuple;
        struct {
            JogList params;   /* type expressions */
            JogNode *ret;
            JogList contexts; /* type expressions */
            bool escaping;
            bool is_extern;
        } type_fn;

        struct {
            JogList stmts;
        } block;
        struct {
            JogNode *expr;
        } expr_stmt;
        struct {
            JogTokenKind op; /* ASSIGN, PLUS_ASSIGN, ... */
            JogNode *target;
            JogNode *value;
        } assign;
        struct {
            JogList targets; /* expressions; IDENT "_" discards */
            JogNode *value;
        } destructure;
        struct {
            JogList values;
            JogNode *body;
        } with;
        struct {
            JogNode *body; /* a statement or a BLOCK */
        } defer;
        struct {
            JogList values;
        } ret;
        struct {
            JogForKind form;
            const char *first;  /* JOG_FOR_IN: index, or the element when second is NULL */
            const char *second; /* JOG_FOR_IN: element when two names are given */
            JogNode *expr;      /* condition or iterated expression */
            JogNode *body;
        } for_;
        struct {
            JogNode *body;
        } attr_block;

        struct {
            const char *text; /* NUL-terminated; see the kind comments */
            size_t len;
            bool value;       /* BOOL_LIT */
        } literal;
        struct {
            JogList parts; /* STRING_LIT pieces and expressions, in order */
        } interp;
        struct {
            const char *name;
            JogList type_args; /* explicit f<T>, usually empty */
        } ident;
        struct {
            JogList elems;
        } tuple;
        struct {
            JogTokenKind op;
            JogNode *operand;
        } unary;
        struct {
            JogTokenKind op;
            JogNode *lhs;
            JogNode *rhs;
        } binary;
        struct {
            JogTokenKind op; /* KW_AS or AS_Q */
            JogNode *expr;
            JogNode *type;
        } cast;
        struct {
            JogNode *expr;
            JogNode *type;
        } is;
        struct {
            JogNode *callee;
            JogList args;            /* ARG */
            JogNode *trailing;       /* LAMBDA, or NULL */
            JogList contexts;        /* f(x) with (a, b) */
        } call;
        struct {
            const char *name; /* NULL for positional */
            JogNode *value;
        } arg;
        struct {
            JogNode *base;
            JogNode *index;
        } index;
        struct {
            JogNode *base;
            JogNode *lo; /* NULL when omitted */
            JogNode *hi;
        } slice;
        struct {
            JogNode *base;
            const char *name; /* NULL for a tuple index */
            uint32_t tuple_index;
            bool safe;        /* ?. */
            /* Explicit type arguments on a method call: obj.method<T>(x).
               Empty when the member has none. No existing field changed meaning. */
            JogList type_args;
        } member;
        struct {
            JogNode *cond;
            JogNode *then_block;
            JogNode *else_branch; /* BLOCK, IF, or NULL */
        } if_;
        struct {
            JogNode *subject; /* NULL for a condition-only when */
            JogList arms;     /* WHEN_ARM */
        } when;
        struct {
            JogList conds;    /* patterns; empty means else */
            JogNode *body;    /* BLOCK or expression */
        } when_arm;
        struct {
            JogList params;   /* PARAM */
            JogNode *ret;     /* NULL when inferred */
            JogNode *body;    /* BLOCK or expression */
            bool expr_body;
            bool implicit_it; /* trailing lambda written without parameters */
        } lambda;
        struct {
            JogList contexts;
            JogList args;  /* go(stack: n): ARG */
            JogNode *body; /* BLOCK or a call expression */
        } go;
        struct {
            JogNode *type;  /* TYPE_ARRAY (possibly infer_len) or TYPE_SLICE */
            JogList elems;  /* ARG */
        } array_lit;
        struct {
            const char *name; /* "comptime", "embed", ... */
            JogList args;     /* ARG */
        } builtin;

        struct {
            const char *name;
            JogList fields; /* PAT_FIELD */
        } pat_variant;
        struct {
            const char *name; /* NULL for positional */
            JogNode *pattern;
        } pat_field;
        struct {
            JogNode *expr;
        } pat_expr;
        struct {
            JogNode *type;
        } pat_is;
    } as;

    /* Destructuring bindings only: one symbol per name, NULL when the name
       is discarded. NULL on every other node. Filled by the checker. */
    JogSymbol **bound_symbols;
    size_t bound_symbol_count;

    /* Filled by the checker. On a call, context_args is one expression per
       callee context parameter, in declaration order. On a with statement,
       context_symbols is one symbol per value, parallel to with.values.
       NULL when the node has none. */
    JogNode **context_args;
    size_t context_arg_count;
    JogSymbol **context_symbols;
    size_t context_symbol_count;

    /* Solved type arguments of a generic call, in type-parameter order.
       NULL when the call is not generic. An argument may still be an outer
       type parameter; lowering substitutes it. Filled by the checker. */
    JogType **solved_type_args;
    size_t solved_type_arg_count;
};

/* A zeroed node. NULL on out of memory. */
JogNode *jog_node_new(CtArena *arena, JogNodeKind kind, JogPos pos);

/* Copies the JogNode * items of da (item_size sizeof(JogNode *)) into an
   arena array. An empty da gives {NULL, 0}. On out of memory returns an
   empty list and sets *oom when oom is non-NULL. */
JogList jog_list_from_da(CtArena *arena, const CtDa *da, bool *oom);

/* The same for const char * items. */
JogNames jog_names_from_da(CtArena *arena, const CtDa *da, bool *oom);

/* "FN", "BINARY", ... Never NULL. */
const char *jog_node_kind_name(JogNodeKind kind);

#endif
