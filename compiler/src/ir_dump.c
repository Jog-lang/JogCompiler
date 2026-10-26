#include "ir.h"

#include <inttypes.h>
#include <string.h>

/* Stable text form of a JirModule for golden tests and `jog emit-ir`.
   Indices identify functions (fN), blocks (bN), locals (lN), and globals
   (gN). Source names are labels next to those indices.

   main fN | main none
   init fN | init none
   global gN var|const <name>: <type> = zero | hex:<bytes> | missing:<len>
       [reloc <offset> str|cstr "<bytes>" | global gN[+addend] | func fN]
   fn fN <linkage> <name> "<jog name>": <type>
     lN[< name>]: <type>[ param][ addr]
   bN[< label>]:
     [lD = ][checked ]<instruction>
     br | cond_br | switch | ret | unreachable | unterminated

   Integer constants use the low `width` bits of the type (two's complement
   when signed); bits above that width are ignored. Bool is true, false, or
   a decimal when the bits are neither 0 nor 1. Floats that are integers in
   the int64 range, including -2^63, print as N.0. Other finite floats are
   hexadecimal (0x1.8p0 is 1.5), so the text does not depend on libc printf.
   nan, -nan, inf, and -inf do not keep a payload. Strings are quoted, with
   \\ \" \n \r \t and \xHH. A position is "  ; file:line:column" when
   pos.file is set. addr is the local's address_taken flag; jir_verify sets
   it, this printer does not. */

enum {
    USE_A = 1u << 0,
    USE_B = 1u << 1,
    USE_AUX = 1u << 2,
    USE_AUX2 = 1u << 3,
    USE_ARGS = 1u << 4
};

_Static_assert(sizeof(double) == 8, "JIR float constants are IEEE-754 binary64");
_Static_assert(JIR_GE == 15, "binop names follow JirBinOp");
_Static_assert(JIR_BITNOT == 2, "unop names follow JirUnOp");

static uint64_t low_bits(uint64_t bits, unsigned width) {
    if (width >= 64) {
        return bits;
    }
    return bits & ((UINT64_C(1) << width) - 1);
}

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static bool is_plain_ident(const char *s) {
    if (!s || !is_ident_start(s[0])) {
        return false;
    }
    for (size_t i = 1; s[i] != '\0'; i++) {
        if (!is_ident_char(s[i])) {
            return false;
        }
    }
    return true;
}

static void append_escaped(CtSb *out, const char *ptr, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)ptr[i];
        switch (c) {
        case '\\':
            ct_sb_append_cstr(out, "\\\\");
            break;
        case '"':
            ct_sb_append_cstr(out, "\\\"");
            break;
        case '\n':
            ct_sb_append_cstr(out, "\\n");
            break;
        case '\r':
            ct_sb_append_cstr(out, "\\r");
            break;
        case '\t':
            ct_sb_append_cstr(out, "\\t");
            break;
        default:
            if (c >= 32 && c < 127) {
                ct_sb_putc(out, (char)c);
            } else {
                ct_sb_appendf(out, "\\x%02x", (unsigned)c);
            }
            break;
        }
    }
}

static void append_quoted(CtSb *out, const char *ptr, size_t len) {
    ct_sb_putc(out, '"');
    if (ptr) {
        append_escaped(out, ptr, len);
    }
    ct_sb_putc(out, '"');
}

/* Bare when the name is an identifier. NULL is <anon>. Anything else is quoted. */
static void append_name(CtSb *out, const char *name) {
    if (!name) {
        ct_sb_append_cstr(out, "<anon>");
        return;
    }
    if (is_plain_ident(name)) {
        ct_sb_append_cstr(out, name);
        return;
    }
    append_quoted(out, name, strlen(name));
}

static void append_jog_name(CtSb *out, const char *name) {
    if (!name) {
        ct_sb_append_cstr(out, "\"\"");
        return;
    }
    append_quoted(out, name, strlen(name));
}

static void append_pos(CtSb *out, JogPos pos) {
    if (!pos.file) {
        return;
    }
    ct_sb_appendf(out, "  ; %s:%" PRIu32 ":%" PRIu32, pos.file, pos.line, pos.column);
}

static void append_bytes_literal(CtSb *out, const char *ptr, size_t len, bool cstring) {
    if (cstring) {
        ct_sb_putc(out, 'c');
    }
    if (!ptr) {
        if (len == 0) {
            ct_sb_append_cstr(out, "\"\"");
        } else {
            ct_sb_appendf(out, "\"\" missing:%" PRIu64, (uint64_t)len);
        }
        return;
    }
    append_quoted(out, ptr, len);
}

static void append_int_const(CtSb *out, const JogType *type, uint64_t bits) {
    if (type && type->kind == JOG_TY_BOOL) {
        if (bits == 0) {
            ct_sb_append_cstr(out, "false");
        } else if (bits == 1) {
            ct_sb_append_cstr(out, "true");
        } else {
            ct_sb_appendf(out, "%" PRIu64, bits);
        }
        return;
    }

    bool is_signed = false;
    unsigned width = 64;
    if (type && type->kind == JOG_TY_INT) {
        is_signed = type->as.integer.is_signed;
        width = type->as.integer.bits;
        if (width == 0 || width > 64) {
            width = 64;
        }
    } else if (type && type->kind == JOG_TY_UNTYPED_INT) {
        is_signed = true;
    } else {
        ct_sb_appendf(out, "%" PRIu64, bits);
        return;
    }

    uint64_t v = low_bits(bits, width);
    if (is_signed) {
        uint64_t sign = width == 64 ? (UINT64_C(1) << 63) : (UINT64_C(1) << (width - 1));
        if ((v & sign) != 0) {
            uint64_t mag = ~v + 1;
            if (width < 64) {
                mag = low_bits(mag, width);
            }
            ct_sb_appendf(out, "-%" PRIu64, mag);
            return;
        }
    }
    ct_sb_appendf(out, "%" PRIu64, v);
}

/* True when the finite value is an integer in the int64 range. */
static bool float_as_i64(unsigned sign, unsigned exp, uint64_t frac, int64_t *out) {
    if (exp == 0 || exp == 0x7ff) {
        return false;
    }
    int e = (int)exp - 1023;
    if (e < 0 || e > 63) {
        return false;
    }
    if (e == 63) {
        if (!sign || frac != 0) {
            return false;
        }
        *out = INT64_MIN;
        return true;
    }
    uint64_t sig = (UINT64_C(1) << 52) | frac;
    uint64_t mag;
    if (e >= 52) {
        unsigned sh = (unsigned)(e - 52);
        if (sh > 0 && sig > (UINT64_MAX >> sh)) {
            return false;
        }
        mag = sig << sh;
    } else {
        unsigned sh = (unsigned)(52 - e);
        if ((sig & ((UINT64_C(1) << sh) - 1)) != 0) {
            return false;
        }
        mag = sig >> sh;
    }
    if (sign) {
        if (mag > (uint64_t)INT64_MAX + 1) {
            return false;
        }
        *out = mag == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)mag;
        return true;
    }
    if (mag > (uint64_t)INT64_MAX) {
        return false;
    }
    *out = (int64_t)mag;
    return true;
}

static void append_hex_frac(CtSb *out, uint64_t frac) {
    char digits[13];
    for (int i = 0; i < 13; i++) {
        unsigned shift = (unsigned)(48 - i * 4);
        unsigned d = (unsigned)((frac >> shift) & 0xfu);
        digits[i] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
    }
    int n = 13;
    while (n > 0 && digits[n - 1] == '0') {
        n--;
    }
    if (n > 0) {
        ct_sb_putc(out, '.');
        ct_sb_append(out, digits, (size_t)n);
    }
}

static void append_float(CtSb *out, double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof bits);
    unsigned sign = (unsigned)(bits >> 63);
    unsigned exp = (unsigned)((bits >> 52) & 0x7ffu);
    uint64_t frac = bits & ((UINT64_C(1) << 52) - 1);

    if (exp == 0x7ff) {
        if (frac == 0) {
            ct_sb_append_cstr(out, sign ? "-inf" : "inf");
        } else {
            ct_sb_append_cstr(out, sign ? "-nan" : "nan");
        }
        return;
    }
    if (exp == 0 && frac == 0) {
        ct_sb_append_cstr(out, sign ? "-0.0" : "0.0");
        return;
    }
    int64_t as_int = 0;
    if (float_as_i64(sign, exp, frac, &as_int)) {
        ct_sb_appendf(out, "%" PRId64 ".0", as_int);
        return;
    }
    if (sign) {
        ct_sb_putc(out, '-');
    }
    if (exp == 0) {
        ct_sb_append_cstr(out, "0x0");
        append_hex_frac(out, frac);
        ct_sb_append_cstr(out, "p-1022");
        return;
    }
    ct_sb_append_cstr(out, "0x1");
    append_hex_frac(out, frac);
    ct_sb_appendf(out, "p%d", (int)exp - 1023);
}

static const char *binop_name(uint32_t aux) {
    static const char *const names[] = {
        "add", "sub", "mul", "div", "rem", "and", "or", "xor", "shl", "shr", "eq", "ne", "lt", "le", "gt", "ge",
    };
    if (aux >= sizeof names / sizeof names[0]) {
        return NULL;
    }
    return names[aux];
}

static const char *unop_name(uint32_t aux) {
    static const char *const names[] = {"neg", "not", "bitnot"};
    if (aux >= sizeof names / sizeof names[0]) {
        return NULL;
    }
    return names[aux];
}

static void append_operand(CtSb *out, const JirOperand *op) {
    if (!op || op->kind == JIR_OPND_NONE) {
        ct_sb_append_cstr(out, "none");
        return;
    }
    switch (op->kind) {
    case JIR_OPND_LOCAL:
        ct_sb_appendf(out, "l%" PRIu32, op->as.local);
        break;
    case JIR_OPND_INT:
        jog_type_print(op->type, out);
        ct_sb_putc(out, ' ');
        append_int_const(out, op->type, op->as.bits);
        break;
    case JIR_OPND_FLOAT:
        jog_type_print(op->type, out);
        ct_sb_putc(out, ' ');
        append_float(out, op->as.f);
        break;
    case JIR_OPND_STRING:
        jog_type_print(op->type, out);
        ct_sb_putc(out, ' ');
        append_bytes_literal(out, op->as.str.ptr, op->as.str.len, false);
        break;
    case JIR_OPND_CSTRING:
        jog_type_print(op->type, out);
        ct_sb_putc(out, ' ');
        append_bytes_literal(out, op->as.str.ptr, op->as.str.len, true);
        break;
    case JIR_OPND_NULL:
        jog_type_print(op->type, out);
        ct_sb_append_cstr(out, " null");
        break;
    case JIR_OPND_GLOBAL:
        ct_sb_appendf(out, "g%" PRIu32, op->as.global);
        break;
    case JIR_OPND_FUNC:
        ct_sb_appendf(out, "f%" PRIu32, op->as.func);
        break;
    default:
        ct_sb_appendf(out, "operand %u", (unsigned)op->kind);
        break;
    }
}

static void append_args(CtSb *out, const JirInst *inst) {
    ct_sb_putc(out, '(');
    if (!inst->args && inst->arg_count > 0) {
        ct_sb_append_cstr(out, "<missing>");
    } else {
        for (size_t i = 0; i < inst->arg_count; i++) {
            if (i > 0) {
                ct_sb_append_cstr(out, ", ");
            }
            append_operand(out, &inst->args[i]);
        }
    }
    ct_sb_putc(out, ')');
}

static void append_extra(CtSb *out, const JirInst *inst, unsigned used) {
    if ((used & USE_AUX) == 0 && inst->aux != 0) {
        ct_sb_appendf(out, " aux %" PRIu32, inst->aux);
    }
    if ((used & USE_AUX2) == 0 && inst->aux2 != 0) {
        ct_sb_appendf(out, " aux2 %" PRIu32, inst->aux2);
    }
    if ((used & USE_A) == 0 && inst->a.kind != JIR_OPND_NONE) {
        ct_sb_append_cstr(out, " a ");
        append_operand(out, &inst->a);
    }
    if ((used & USE_B) == 0 && inst->b.kind != JIR_OPND_NONE) {
        ct_sb_append_cstr(out, " b ");
        append_operand(out, &inst->b);
    }
    if ((used & USE_ARGS) == 0 && inst->arg_count > 0) {
        ct_sb_append_cstr(out, " args ");
        append_args(out, inst);
    }
}

static void append_a(CtSb *out, const char *op, const JirOperand *a) {
    ct_sb_append_cstr(out, op);
    ct_sb_putc(out, ' ');
    append_operand(out, a);
}

static void append_ab(CtSb *out, const char *op, const JirOperand *a, const JirOperand *b) {
    append_a(out, op, a);
    ct_sb_append_cstr(out, ", ");
    append_operand(out, b);
}

static void dump_inst(CtSb *out, const JirInst *inst) {
    unsigned used = 0;
    ct_sb_append_cstr(out, "  ");
    if (inst->dst >= 0) {
        ct_sb_appendf(out, "l%" PRId32 " = ", inst->dst);
    } else if (inst->dst != -1) {
        ct_sb_appendf(out, "dst %" PRId32 " = ", inst->dst);
    }
    if (inst->checked) {
        ct_sb_append_cstr(out, "checked ");
    }
    switch (inst->op) {
    case JIR_COPY:
        append_a(out, "copy", &inst->a);
        used = USE_A;
        break;
    case JIR_ZERO:
        ct_sb_append_cstr(out, "zero");
        break;
    case JIR_BINARY: {
        const char *name = binop_name(inst->aux);
        if (name) {
            ct_sb_append_cstr(out, name);
        } else {
            ct_sb_appendf(out, "binary %" PRIu32, inst->aux);
        }
        ct_sb_putc(out, ' ');
        append_operand(out, &inst->a);
        ct_sb_append_cstr(out, ", ");
        append_operand(out, &inst->b);
        used = USE_A | USE_B | USE_AUX;
        break;
    }
    case JIR_UNARY: {
        const char *name = unop_name(inst->aux);
        if (name) {
            append_a(out, name, &inst->a);
        } else {
            ct_sb_appendf(out, "unary %" PRIu32 " ", inst->aux);
            append_operand(out, &inst->a);
        }
        used = USE_A | USE_AUX;
        break;
    }
    case JIR_CONVERT:
        append_a(out, "convert", &inst->a);
        used = USE_A;
        break;
    case JIR_BITCAST:
        append_a(out, "bitcast", &inst->a);
        used = USE_A;
        break;
    case JIR_LOAD:
        append_a(out, "load", &inst->a);
        used = USE_A;
        break;
    case JIR_STORE:
        append_ab(out, "store", &inst->a, &inst->b);
        used = USE_A | USE_B;
        break;
    case JIR_ADDR:
        append_a(out, "addr", &inst->a);
        used = USE_A;
        break;
    case JIR_FIELD_ADDR:
        ct_sb_append_cstr(out, "field_addr ");
        append_operand(out, &inst->a);
        if (inst->aux2 != 0) {
            ct_sb_appendf(out, ", variant %" PRIu32 ", field %" PRIu32, inst->aux2 - 1, inst->aux);
        } else {
            ct_sb_appendf(out, ", %" PRIu32, inst->aux);
        }
        used = USE_A | USE_AUX | USE_AUX2;
        break;
    case JIR_ELEM_ADDR:
        append_ab(out, "elem_addr", &inst->a, &inst->b);
        used = USE_A | USE_B;
        break;
    case JIR_EXTRACT:
        ct_sb_append_cstr(out, "extract ");
        append_operand(out, &inst->a);
        ct_sb_appendf(out, ", %" PRIu32, inst->aux);
        used = USE_A | USE_AUX;
        break;
    case JIR_AGGREGATE:
        ct_sb_append_cstr(out, "aggregate ");
        append_args(out, inst);
        used = USE_ARGS;
        break;
    case JIR_ENUM_TAG:
        append_a(out, "enum_tag", &inst->a);
        used = USE_A;
        break;
    case JIR_CALL:
        ct_sb_append_cstr(out, inst->a.kind == JIR_OPND_FUNC ? "call " : "call_indirect ");
        append_operand(out, &inst->a);
        append_args(out, inst);
        used = USE_A | USE_ARGS;
        break;
    case JIR_BOUNDS_CHECK:
        append_ab(out, "bounds_check", &inst->a, &inst->b);
        used = USE_A | USE_B;
        break;
    case JIR_NULL_CHECK:
        append_a(out, "null_check", &inst->a);
        used = USE_A;
        break;
    case JIR_PANIC:
        append_a(out, "panic", &inst->a);
        used = USE_A;
        break;
    default:
        ct_sb_appendf(out, "op %u", (unsigned)inst->op);
        break;
    }
    append_extra(out, inst, used);
    append_pos(out, inst->pos);
    ct_sb_putc(out, '\n');
}

static void append_target(CtSb *out, const JirFunc *func, uint32_t target) {
    ct_sb_appendf(out, "b%" PRIu32, target);
    JirBlock *block = func ? jir_block_at(func, target) : NULL;
    if (block && block->label) {
        ct_sb_putc(out, ' ');
        append_name(out, block->label);
    }
}

static void dump_term(CtSb *out, const JirFunc *func, const JirTerm *term) {
    int value_used = 0;
    switch (term->kind) {
    case JIR_TERM_NONE:
        ct_sb_append_cstr(out, "  unterminated");
        break;
    case JIR_TERM_BR:
        ct_sb_append_cstr(out, "  br ");
        append_target(out, func, term->target);
        break;
    case JIR_TERM_COND_BR:
        ct_sb_append_cstr(out, "  cond_br ");
        append_operand(out, &term->value);
        ct_sb_append_cstr(out, ", ");
        append_target(out, func, term->target);
        ct_sb_append_cstr(out, ", ");
        append_target(out, func, term->else_target);
        value_used = 1;
        break;
    case JIR_TERM_SWITCH:
        ct_sb_append_cstr(out, "  switch ");
        append_operand(out, &term->value);
        ct_sb_append_cstr(out, ", ");
        append_target(out, func, term->target);
        append_pos(out, term->pos);
        ct_sb_putc(out, '\n');
        if (!term->cases && term->case_count > 0) {
            ct_sb_append_cstr(out, "    <missing>\n");
            return;
        }
        for (size_t i = 0; i < term->case_count; i++) {
            ct_sb_append_cstr(out, "    ");
            append_int_const(out, term->value.type, term->cases[i].value);
            ct_sb_append_cstr(out, " -> ");
            append_target(out, func, term->cases[i].target);
            ct_sb_putc(out, '\n');
        }
        return;
    case JIR_TERM_RET:
        ct_sb_append_cstr(out, "  ret");
        if (term->value.kind != JIR_OPND_NONE) {
            ct_sb_putc(out, ' ');
            append_operand(out, &term->value);
        }
        value_used = 1;
        break;
    case JIR_TERM_UNREACHABLE:
        ct_sb_append_cstr(out, "  unreachable");
        break;
    default:
        ct_sb_appendf(out, "  term %u", (unsigned)term->kind);
        break;
    }
    if (!value_used && term->value.kind != JIR_OPND_NONE) {
        ct_sb_append_cstr(out, " value ");
        append_operand(out, &term->value);
    }
    append_pos(out, term->pos);
    ct_sb_putc(out, '\n');
}

static void dump_block(CtSb *out, const JirFunc *func, JirBlock *block, uint32_t index) {
    if (!block) {
        ct_sb_appendf(out, "b%" PRIu32 " <null>\n", index);
        return;
    }
    ct_sb_appendf(out, "b%" PRIu32, index);
    if (block->label) {
        ct_sb_putc(out, ' ');
        append_name(out, block->label);
    }
    ct_sb_append_cstr(out, ":\n");
    for (size_t i = 0; i < block->insts.count; i++) {
        JirInst *inst = ct_da_at(&block->insts, i);
        if (!inst) {
            ct_sb_append_cstr(out, "  <null inst>\n");
            continue;
        }
        dump_inst(out, inst);
    }
    dump_term(out, func, &block->term);
}

static void append_global_data(CtSb *out, const JirGlobal *global) {
    if (!global->data) {
        if (global->data_len == 0) {
            ct_sb_append_cstr(out, "zero");
        } else {
            ct_sb_appendf(out, "missing:%" PRIu64, (uint64_t)global->data_len);
        }
        return;
    }
    ct_sb_append_cstr(out, "hex:");
    for (size_t i = 0; i < global->data_len; i++) {
        if (i > 0) {
            ct_sb_putc(out, ' ');
        }
        ct_sb_appendf(out, "%02x", (unsigned)global->data[i]);
    }
    for (size_t r = 0; r < global->reloc_count; r++) {
        const JirReloc *rel = &global->relocs[r];
        ct_sb_appendf(out, " reloc %" PRIu32 " ", rel->offset);
        if (rel->kind == JIR_RELOC_STRING || rel->kind == JIR_RELOC_CSTRING) {
            ct_sb_append_cstr(out, rel->kind == JIR_RELOC_CSTRING ? "cstr " : "str ");
            append_bytes_literal(out, rel->ptr, rel->len, rel->kind == JIR_RELOC_CSTRING);
        } else if (rel->kind == JIR_RELOC_GLOBAL) {
            ct_sb_appendf(out, "global g%" PRIu32, rel->index);
            if (rel->addend != 0) {
                ct_sb_appendf(out, "+%" PRIu32, rel->addend);
            }
        } else if (rel->kind == JIR_RELOC_FUNC) {
            ct_sb_appendf(out, "func f%" PRIu32, rel->index);
        } else {
            ct_sb_appendf(out, "kind %u", (unsigned)rel->kind);
        }
    }
}

static void dump_global(CtSb *out, const JirGlobal *global, uint32_t index) {
    if (!global) {
        ct_sb_appendf(out, "global g%" PRIu32 " <null>\n", index);
        return;
    }
    ct_sb_appendf(out, "global g%" PRIu32 " %s ", index, global->is_const ? "const" : "var");
    append_name(out, global->name);
    ct_sb_append_cstr(out, ": ");
    jog_type_print(global->type, out);
    ct_sb_append_cstr(out, " = ");
    append_global_data(out, global);
    append_pos(out, global->pos);
    ct_sb_putc(out, '\n');
}

static const char *linkage_name(JirLinkage linkage) {
    static const char *const names[] = {"internal", "public", "export", "extern"};
    if ((unsigned)linkage >= sizeof names / sizeof names[0]) {
        return NULL;
    }
    return names[linkage];
}

static void dump_func(CtSb *out, JirFunc *func, uint32_t index) {
    if (!func) {
        ct_sb_appendf(out, "fn f%" PRIu32 " <null>\n", index);
        return;
    }
    const char *link = linkage_name(func->linkage);
    ct_sb_appendf(out, "fn f%" PRIu32 " ", index);
    if (link) {
        ct_sb_append_cstr(out, link);
    } else {
        ct_sb_appendf(out, "link %d", (int)func->linkage);
    }
    ct_sb_putc(out, ' ');
    append_name(out, func->name);
    ct_sb_putc(out, ' ');
    append_jog_name(out, func->jog_name);
    ct_sb_append_cstr(out, ": ");
    jog_type_print(func->type, out);
    append_pos(out, func->pos);
    ct_sb_putc(out, '\n');
    for (size_t i = 0; i < func->locals.count; i++) {
        JirLocal *local = jir_local_at(func, (uint32_t)i);
        ct_sb_appendf(out, "  l%" PRIu64, (uint64_t)i);
        if (local && local->name) {
            ct_sb_putc(out, ' ');
            append_name(out, local->name);
        }
        ct_sb_append_cstr(out, ": ");
        jog_type_print(local ? local->type : NULL, out);
        if (local && local->is_param) {
            ct_sb_append_cstr(out, " param");
        }
        if (local && local->address_taken) {
            ct_sb_append_cstr(out, " addr");
        }
        ct_sb_putc(out, '\n');
    }
    for (size_t b = 0; b < func->blocks.count; b++) {
        dump_block(out, func, jir_block_at(func, (uint32_t)b), (uint32_t)b);
    }
}

static void append_entry(CtSb *out, const char *label, int32_t index) {
    if (index == -1) {
        ct_sb_appendf(out, "%s none\n", label);
    } else if (index < 0) {
        ct_sb_appendf(out, "%s %" PRId32 "\n", label, index);
    } else {
        ct_sb_appendf(out, "%s f%" PRId32 "\n", label, index);
    }
}

void jir_dump(const JirModule *module, CtSb *out) {
    if (!out) {
        return;
    }
    if (!module) {
        ct_sb_append_cstr(out, "module <null>\n");
        return;
    }
    append_entry(out, "main", module->main_func);
    append_entry(out, "init", module->init_func);
    int have_globals = module->globals.count > 0;
    int have_funcs = module->funcs.count > 0;
    if (have_globals || have_funcs) {
        ct_sb_putc(out, '\n');
    }
    for (size_t i = 0; i < module->globals.count; i++) {
        dump_global(out, jir_global(module, (uint32_t)i), (uint32_t)i);
    }
    if (have_globals && have_funcs) {
        ct_sb_putc(out, '\n');
    }
    for (size_t i = 0; i < module->funcs.count; i++) {
        if (i > 0) {
            ct_sb_putc(out, '\n');
        }
        dump_func(out, jir_func(module, (uint32_t)i), (uint32_t)i);
    }
}
