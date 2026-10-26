#ifndef JOG_IR_H
#define JOG_IR_H

#include "ast.h"
#include "types.h"

#include "arena.h"
#include "da.h"
#include "diag.h"
#include "str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* JIR: typed three-address code over basic blocks, not SSA. See
   docs/plans/V1Compiler.md.

   - A function has typed locals and blocks. Locals 0..param_count-1 are the
     parameters in order; lowering has already appended context parameters,
     so backends never see contexts.
   - Instructions read operands (locals or constants) and write at most one
     local, dst. A local may be written many times.
   - Every block ends in exactly one terminator.
   - defer, errdefer, when, patterns, Option sugar, and error propagation are
     already lowered to plain blocks. Checks are explicit instructions.
   - Types are JogType *, interned, so backends switch on JogType.kind.

   Producers: lower.c. Consumers: emit_c.c, interp.c.
   Contract owner: see docs/plans/V1Parallel.md. */

typedef enum JirOperandKind {
    JIR_OPND_NONE = 0,
    JIR_OPND_LOCAL,   /* value of a local */
    JIR_OPND_INT,     /* integer or bool constant, two's complement in bits */
    JIR_OPND_FLOAT,
    JIR_OPND_STRING,  /* const bytes constant: points into read-only data */
    JIR_OPND_CSTRING, /* *const c.char constant: NUL-terminated */
    JIR_OPND_NULL,    /* None of an Option type, or a null C pointer */
    JIR_OPND_GLOBAL,  /* address of a global: type is *T */
    JIR_OPND_FUNC     /* a function as a value, or a direct callee */
} JirOperandKind;

typedef struct JirOperand {
    JirOperandKind kind;
    JogType *type;
    union {
        uint32_t local;
        uint64_t bits;
        double f;
        struct {
            const char *ptr;
            size_t len;
        } str;
        uint32_t global;
        uint32_t func;
    } as;
} JirOperand;

typedef enum JirBinOp {
    JIR_ADD, JIR_SUB, JIR_MUL, JIR_DIV, JIR_REM,
    JIR_AND, JIR_OR, JIR_XOR, JIR_SHL, JIR_SHR, /* SHR is arithmetic for signed types */
    JIR_EQ, JIR_NE, JIR_LT, JIR_LE, JIR_GT, JIR_GE /* result is bool; signedness from operand type */
} JirBinOp;

typedef enum JirUnOp {
    JIR_NEG, JIR_NOT, JIR_BITNOT
} JirUnOp;

typedef enum JirOp {
    JIR_COPY,         /* dst = a */
    JIR_ZERO,         /* dst = zero value of dst's type */
    JIR_BINARY,       /* dst = a <aux: JirBinOp> b; checked: panic on overflow, division by zero */
    JIR_UNARY,        /* dst = <aux: JirUnOp> a; checked NEG panics on overflow */
    JIR_CONVERT,      /* dst = a converted to dst's type (int<->int, int<->float);
                         checked: panic if the value does not fit */
    JIR_BITCAST,      /* dst = a reinterpreted: pointer<->pointer, bytes<->[]T, bytes->const bytes, distinct<->base */
    JIR_LOAD,         /* dst = *a */
    JIR_STORE,        /* *a = b; no dst */
    JIR_ADDR,         /* dst = &local a.as.local */
    JIR_FIELD_ADDR,   /* dst = &(*a).field[aux]; a is *struct, *tuple, or *enum payload (see aux2) */
    JIR_ELEM_ADDR,    /* dst = a + b * sizeof(*a); a is *T, b is usize; unchecked */
    JIR_EXTRACT,      /* dst = a.field[aux] by value (struct, tuple, slice: 0 ptr, 1 len) */
    JIR_AGGREGATE,    /* dst = dst's type built from args in field order (struct, tuple, slice,
                         bytes: ptr then len; option: has-value then value) */
    JIR_ENUM_TAG,     /* dst = tag of enum value a */
    JIR_CALL,         /* dst = a(args); a is FUNC (direct) or a value of fn type (indirect); dst -1 for () */
    JIR_BOUNDS_CHECK, /* panic unless a < b (both usize) */
    JIR_NULL_CHECK,   /* panic unless a is not None / not null */
    JIR_PANIC         /* panic with message a (STRING); does not return, the block still needs a terminator */
} JirOp;

typedef struct JirInst {
    JirOp op;
    uint32_t aux;  /* JirBinOp, JirUnOp, field index */
    uint32_t aux2; /* FIELD_ADDR on an enum: variant index + 1, otherwise 0 */
    bool checked;
    int32_t dst;   /* local index, or -1 */
    JirOperand a;
    JirOperand b;
    JirOperand *args; /* arena array */
    size_t arg_count;
    JogPos pos;
} JirInst;

typedef enum JirTermKind {
    JIR_TERM_NONE = 0, /* not terminated yet; invalid in a finished function */
    JIR_TERM_BR,
    JIR_TERM_COND_BR,
    JIR_TERM_SWITCH,
    JIR_TERM_RET,
    JIR_TERM_UNREACHABLE
} JirTermKind;

typedef struct JirCase {
    uint64_t value;
    uint32_t target;
} JirCase;

typedef struct JirTerm {
    JirTermKind kind;
    JirOperand value;         /* COND_BR: bool condition; SWITCH: integer; RET: result or NONE */
    uint32_t target;          /* BR target; COND_BR when true; SWITCH default */
    uint32_t else_target;     /* COND_BR when false */
    JirCase *cases;           /* SWITCH, arena array */
    size_t case_count;
    JogPos pos;
} JirTerm;

typedef struct JirBlock {
    const char *label; /* for dumps and C labels; may be NULL */
    CtDa insts;        /* JirInst */
    JirTerm term;
} JirBlock;

typedef struct JirLocal {
    const char *name; /* source name, or NULL for a temporary */
    JogType *type;
    bool is_param;
    bool address_taken; /* set by jir_verify for JIR_ADDR operands; backends must keep it in memory */
} JirLocal;

typedef enum JirLinkage {
    JIR_LINK_INTERNAL = 0, /* private to the program: static in C */
    JIR_LINK_PUBLIC,       /* visible Jog symbol */
    JIR_LINK_EXPORT,       /* @export: C calling convention, name used as is */
    JIR_LINK_EXTERN        /* declared only: a C function from an extern block */
} JirLinkage;

typedef struct JirFunc {
    const char *name;     /* symbol name: mangled, or the C name for EXPORT/EXTERN */
    const char *jog_name; /* for messages and dumps, e.g. "net.http.get" */
    JirLinkage linkage;
    JogType *type;        /* JOG_TY_FN, including context parameters */
    uint32_t param_count;
    CtDa locals;          /* JirLocal */
    CtDa blocks;          /* JirBlock; block 0 is the entry */
    JogPos pos;
} JirFunc;

/* A pointer-sized slot in a global image stays zero. The reloc names what
   the C backend (and only the C backend) should write there. */
typedef enum JirRelocKind {
    JIR_RELOC_STRING = 1, /* address of read-only bytes; ptr/len are the bytes */
    JIR_RELOC_CSTRING,    /* address of those bytes plus a trailing NUL */
    JIR_RELOC_GLOBAL,     /* address of global index, plus addend bytes */
    JIR_RELOC_FUNC        /* function index as a code pointer */
} JirRelocKind;

typedef struct JirReloc {
    uint32_t offset; /* byte offset of the pointer slot in the global image */
    uint32_t index;  /* global or function index; unused for strings */
    uint32_t addend;
    uint8_t kind; /* JirRelocKind */
    const char *ptr;
    size_t len;
} JirReloc;

typedef struct JirGlobal {
    const char *name;
    JogType *type;
    bool is_const;       /* read-only data (top-level val, literal backing arrays) */
    const uint8_t *data; /* constant initial bytes in target layout, or NULL for zero */
    size_t data_len;
    JogPos pos;
    const JirReloc *relocs; /* pointer slots in data; NULL when reloc_count is 0 */
    size_t reloc_count;
} JirGlobal;

typedef struct JirIfaceTable JirIfaceTable;

typedef struct JirModule {
    CtArena *arena;
    JogTypes *types;
    CtDa funcs;   /* JirFunc * */
    CtDa globals; /* JirGlobal * */
    int32_t main_func; /* the Jog main, or -1 for a library */
    int32_t init_func; /* runs global initializers in order, or -1 if there are none */
    /* Method tables built while lowering. The pointed-to struct is defined
       in iface_table.h. Both start null. */
    JirIfaceTable *iface_tables;
    size_t iface_table_count;
} JirModule;

/* ---- building ---- */

JirModule *jir_module_create(CtArena *arena, JogTypes *types);

/* Frees the growable arrays. NULL is ignored. */
void jir_module_destroy(JirModule *module);

/* Adds a function. fn_type must be JOG_TY_FN; one parameter local is
   created per parameter type, named from param_names when it is non-NULL.
   Returns the function index, or -1 on out of memory. */
int32_t jir_func_add(JirModule *module, const char *name, const char *jog_name, JirLinkage linkage,
                     JogType *fn_type, const char *const *param_names);

JirFunc *jir_func(const JirModule *module, uint32_t index);

uint32_t jir_local_add(JirFunc *func, const char *name, JogType *type);
JirLocal *jir_local_at(const JirFunc *func, uint32_t index);

uint32_t jir_block_add(JirFunc *func, const char *label);
JirBlock *jir_block_at(const JirFunc *func, uint32_t index);

/* Appends an instruction and returns it for the caller to fill. The pointer
   is valid until the next jir_emit into the same block. NULL on out of
   memory. */
JirInst *jir_emit(JirFunc *func, uint32_t block, JirOp op, int32_t dst);

/* Copies args into the module arena and attaches them to inst. */
bool jir_set_args(JirModule *module, JirInst *inst, const JirOperand *args, size_t count);

void jir_terminate(JirFunc *func, uint32_t block, JirTerm term);

int32_t jir_global_add(JirModule *module, const char *name, JogType *type, bool is_const);
JirGlobal *jir_global(const JirModule *module, uint32_t index);

/* Operand constructors. */
JirOperand jir_none(void);
JirOperand jir_local(const JirFunc *func, uint32_t index);
JirOperand jir_int(JogType *type, uint64_t bits);
JirOperand jir_float(JogType *type, double value);
JirOperand jir_string(JogType *type, const char *ptr, size_t len);
JirOperand jir_cstring(JogType *type, const char *ptr, size_t len);
JirOperand jir_null(JogType *type);
JirOperand jir_global_ref(const JirModule *module, uint32_t index); /* type *T */
JirOperand jir_func_ref(const JirModule *module, uint32_t index);

/* Structural checks: every block terminated, branch targets and local
   indices in range, dst present where an instruction produces a value.
   Reports each problem through diag and returns false if there were any.
   Type checks are the producer's job. */
bool jir_verify(const JirModule *module, CtDiag *diag);

/* Text form for tests and `jog emit-ir`. Implemented in ir_dump.c. */
void jir_dump(const JirModule *module, CtSb *out);

#endif
