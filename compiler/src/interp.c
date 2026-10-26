#include "phases.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Runs JIR at compile time. Memory is a byte arena and a pointer is an
   offset into it; offset 0 is null. Integers are target-width and
   little-endian. Each call has its own memory. A panic or a step-budget
   overrun is a compile error plus one note per jog name on the stack.
   The bytes written to *out live in module->arena and use the return
   type's target layout. Pointers in those bytes are not relocated.

   jog_interp_set_step_budget(n) with n > 0 replaces the budget argument.
   The driver sets n from --comptime-steps. Zero keeps the caller's budget,
   which lowering passes as 10 million. */

enum { JOG_INTERP_MEM = 64u * 1024u * 1024u, JOG_INTERP_DEPTH = 4096u };

typedef struct GSlot {
    uint32_t off;
    uint32_t size;
    int is_const;
} GSlot;

typedef struct Frame {
    JirFunc *func;
    uint32_t block;
    uint32_t ip;
    uint32_t used_at;
    uint32_t *off;
    uint32_t nlocal;
    int32_t ret_dst;
} Frame;

typedef struct Interp {
    const JirModule *module;
    CtDiag *diag;
    uint64_t budget;
    uint64_t steps;
    uint32_t ptr_size;
    uint8_t *mem;
    size_t mem_cap;
    uint32_t used;
    GSlot *globals;
    size_t nglobal;
    Frame *frames;
    size_t frame_cap;
    size_t depth;
    uint8_t *result;
    size_t result_n;
    int failed;
} Interp;

static CtPos cpos(JogPos pos) {
    CtPos out;
    out.file = pos.file;
    out.line = pos.line;
    out.column = pos.column;
    return out;
}

static void note_stack(Interp *ip) {
    size_t i;
    if (!ip) {
        return;
    }
    i = ip->depth;
    while (i > 0) {
        Frame *fr;
        const char *name;
        CtPos pos;
        i--;
        fr = &ip->frames[i];
        name = fr->func && fr->func->jog_name ? fr->func->jog_name : "?";
        memset(&pos, 0, sizeof(pos));
        ct_diag_emit(ip->diag, CT_SEVERITY_NOTE, pos, "at %s", name);
    }
}

static void error_msg(Interp *ip, JogPos pos, const char *msg) {
    if (!ip || ip->failed) {
        return;
    }
    ip->failed = 1;
    ct_diag_emit(ip->diag, CT_SEVERITY_ERROR, cpos(pos), "%s", msg);
    note_stack(ip);
}

static void panic_msg(Interp *ip, JogPos pos, const char *msg) {
    char buf[160];
    if (!msg) {
        msg = "panic";
    }
    (void)snprintf(buf, sizeof(buf), "compile-time panic: %s", msg);
    error_msg(ip, pos, buf);
}

static JogType *underlying(JogType *type) {
    return jog_type_underlying(type);
}

static size_t ty_size(JogType *type) {
    if (!type || !type->has_layout || type->size > SIZE_MAX) {
        return 0;
    }
    return (size_t)type->size;
}

static uint32_t ty_align(JogType *type) {
    if (!type || !type->has_layout || type->align == 0) {
        return 1;
    }
    return type->align;
}

static int is_float_ty(JogType *type) {
    type = underlying(type);
    return type && (type->kind == JOG_TY_FLOAT || type->kind == JOG_TY_UNTYPED_FLOAT);
}

static unsigned int_width(JogType *type) {
    type = underlying(type);
    if (!type) {
        return 64;
    }
    if (type->kind == JOG_TY_BOOL) {
        return 1;
    }
    if (type->kind == JOG_TY_UNTYPED_INT) {
        return 64;
    }
    if (type->kind == JOG_TY_INT && type->as.integer.bits > 0 && type->as.integer.bits <= 64) {
        return type->as.integer.bits;
    }
    return 64;
}

static int int_signed(JogType *type) {
    type = underlying(type);
    if (!type || type->kind == JOG_TY_BOOL) {
        return 0;
    }
    if (type->kind == JOG_TY_UNTYPED_INT) {
        return 1;
    }
    return type->kind == JOG_TY_INT && type->as.integer.is_signed;
}

static uint64_t mask_of(unsigned width) {
    if (width == 0) {
        return 0;
    }
    if (width >= 64) {
        return ~(uint64_t)0;
    }
    return ((uint64_t)1 << width) - 1;
}

static int64_t sext(uint64_t bits, unsigned width) {
    uint64_t sign;
    uint64_t mask;
    uint64_t v;
    if (width == 0) {
        return 0;
    }
    if (width >= 64) {
        return (int64_t)bits;
    }
    sign = (uint64_t)1 << (width - 1);
    mask = (sign << 1) - 1;
    v = bits & mask;
    if (v & sign) {
        v |= ~mask;
    }
    return (int64_t)v;
}

static int64_t signed_min(unsigned width) {
    if (width == 0 || width > 64) {
        return 0;
    }
    if (width == 64) {
        return INT64_MIN;
    }
    return -((int64_t)1 << (width - 1));
}

static int64_t signed_max(unsigned width) {
    if (width == 0 || width > 64) {
        return 0;
    }
    if (width == 64) {
        return INT64_MAX;
    }
    return ((int64_t)1 << (width - 1)) - 1;
}

static int align_up(uint32_t v, uint32_t a, uint32_t *out) {
    uint64_t x;
    if (a <= 1) {
        *out = v;
        return 1;
    }
    x = (uint64_t)v + (uint64_t)(a - 1);
    x &= ~((uint64_t)a - 1);
    if (x > UINT32_MAX) {
        return 0;
    }
    *out = (uint32_t)x;
    return 1;
}

static int ensure_cap(Interp *ip, size_t need) {
    uint8_t *mem;
    size_t cap;
    if (need <= ip->mem_cap) {
        return 1;
    }
    cap = ip->mem_cap ? ip->mem_cap : 4096;
    while (cap < need) {
        if (cap > JOG_INTERP_MEM / 2) {
            cap = JOG_INTERP_MEM;
            break;
        }
        cap *= 2;
    }
    if (need > cap || cap > JOG_INTERP_MEM) {
        return 0;
    }
    mem = realloc(ip->mem, cap);
    if (!mem) {
        return 0;
    }
    ip->mem = mem;
    ip->mem_cap = cap;
    return 1;
}

static int bump(Interp *ip, uint32_t align, uint32_t size, uint32_t *off) {
    uint32_t at;
    uint64_t end;
    if (!align_up(ip->used, align, &at)) {
        error_msg(ip, (JogPos){0}, "compile-time memory limit exceeded");
        return 0;
    }
    end = (uint64_t)at + (uint64_t)size;
    if (end > JOG_INTERP_MEM || end > UINT32_MAX) {
        error_msg(ip, (JogPos){0}, "compile-time memory limit exceeded");
        return 0;
    }
    if (!ensure_cap(ip, (size_t)end)) {
        error_msg(ip, (JogPos){0}, "out of memory");
        return 0;
    }
    if ((uint32_t)end > ip->used) {
        memset(ip->mem + ip->used, 0, (size_t)end - ip->used);
    }
    ip->used = (uint32_t)end;
    *off = at;
    return !ip->failed;
}

static void mem_write(Interp *ip, uint32_t off, uint32_t n, uint64_t bits) {
    uint32_t i;
    if ((uint64_t)off + n > ip->used || !ip->mem) {
        return;
    }
    for (i = 0; i < n && i < 8; i++) {
        ip->mem[off + i] = (uint8_t)((bits >> (8 * i)) & 0xffu);
    }
    for (; i < n; i++) {
        ip->mem[off + i] = 0;
    }
}

static int range_ok(const Interp *ip, uint32_t off, uint32_t n) {
    return (uint64_t)off + (uint64_t)n <= ip->used;
}

static int overlaps(uint32_t a, uint32_t asz, uint32_t b, uint32_t bsz) {
    uint64_t ae;
    uint64_t be;
    if (asz == 0 || bsz == 0) {
        return 0;
    }
    ae = (uint64_t)a + asz;
    be = (uint64_t)b + bsz;
    return (uint64_t)a < be && (uint64_t)b < ae;
}

static int hits_var(const Interp *ip, uint32_t off, uint32_t n) {
    size_t i;
    for (i = 0; i < ip->nglobal; i++) {
        if (ip->globals[i].is_const) {
            continue;
        }
        if (overlaps(off, n, ip->globals[i].off, ip->globals[i].size)) {
            return 1;
        }
    }
    return 0;
}

static int hits_global(const Interp *ip, uint32_t off, uint32_t n) {
    size_t i;
    for (i = 0; i < ip->nglobal; i++) {
        if (overlaps(off, n, ip->globals[i].off, ip->globals[i].size)) {
            return 1;
        }
    }
    return 0;
}

static int local_at(const Frame *fr, uint32_t index, uint32_t *off, uint32_t *sz) {
    JirLocal *loc;
    if (!fr || !fr->func || index >= fr->nlocal || index >= fr->func->locals.count) {
        return 0;
    }
    loc = jir_local_at(fr->func, index);
    if (!loc || !fr->off) {
        return 0;
    }
    *off = fr->off[index];
    *sz = (uint32_t)ty_size(loc->type);
    return 1;
}

static int place_bytes(Interp *ip, const void *bytes, size_t n, uint32_t *off) {
    uint32_t at;
    if (n > UINT32_MAX) {
        error_msg(ip, (JogPos){0}, "compile-time memory limit exceeded");
        return 0;
    }
    if (!bump(ip, 1, (uint32_t)n, &at)) {
        return 0;
    }
    if (n && bytes) {
        memcpy(ip->mem + at, bytes, n);
    }
    *off = at;
    return 1;
}

static int read_op(Interp *ip, Frame *fr, const JirOperand *op, uint8_t *dst, size_t n, JogPos pos) {
    size_t i;
    if (!dst) {
        return 0;
    }
    memset(dst, 0, n);
    if (!op || n == 0) {
        return !ip->failed;
    }
    if (is_float_ty(op->type) || op->kind == JIR_OPND_FLOAT) {
        error_msg(ip, pos, "float values are not available at compile time");
        return 0;
    }
    switch (op->kind) {
    case JIR_OPND_NONE:
    case JIR_OPND_NULL:
        return 1;
    case JIR_OPND_INT: {
        uint64_t bits = op->as.bits;
        size_t w = n < 8 ? n : 8;
        for (i = 0; i < w; i++) {
            dst[i] = (uint8_t)((bits >> (8 * i)) & 0xffu);
        }
        return 1;
    }
    case JIR_OPND_LOCAL: {
        uint32_t off = 0;
        uint32_t sz = 0;
        size_t m;
        if (!fr || !local_at(fr, op->as.local, &off, &sz) || !range_ok(ip, off, sz)) {
            error_msg(ip, pos, "compile-time local is out of range");
            return 0;
        }
        m = n < sz ? n : sz;
        if (m) {
            memcpy(dst, ip->mem + off, m);
        }
        return 1;
    }
    case JIR_OPND_GLOBAL: {
        uint64_t bits;
        size_t w;
        if (op->as.global >= ip->nglobal) {
            error_msg(ip, pos, "compile-time global is out of range");
            return 0;
        }
        bits = ip->globals[op->as.global].off;
        w = n < 8 ? n : 8;
        for (i = 0; i < w; i++) {
            dst[i] = (uint8_t)((bits >> (8 * i)) & 0xffu);
        }
        return 1;
    }
    case JIR_OPND_STRING: {
        uint32_t off = 0;
        size_t len = op->as.str.len;
        uint64_t bits;
        if (ip->ptr_size == 0 || n < (size_t)ip->ptr_size * 2) {
            error_msg(ip, pos, "compile-time string has a bad size");
            return 0;
        }
        if (!place_bytes(ip, op->as.str.ptr, len, &off)) {
            return 0;
        }
        bits = off;
        for (i = 0; i < ip->ptr_size; i++) {
            dst[i] = (uint8_t)((bits >> (8 * i)) & 0xffu);
        }
        bits = len;
        for (i = 0; i < ip->ptr_size; i++) {
            dst[ip->ptr_size + i] = (uint8_t)((bits >> (8 * i)) & 0xffu);
        }
        return 1;
    }
    case JIR_OPND_CSTRING: {
        uint32_t off = 0;
        size_t len = op->as.str.len;
        uint8_t *raw;
        uint64_t bits;
        if (ip->ptr_size == 0 || n < ip->ptr_size) {
            error_msg(ip, pos, "compile-time string has a bad size");
            return 0;
        }
        if (len == SIZE_MAX) {
            error_msg(ip, pos, "compile-time memory limit exceeded");
            return 0;
        }
        raw = malloc(len + 1);
        if (!raw) {
            error_msg(ip, pos, "out of memory");
            return 0;
        }
        if (len && op->as.str.ptr) {
            memcpy(raw, op->as.str.ptr, len);
        }
        raw[len] = 0;
        if (!place_bytes(ip, raw, len + 1, &off)) {
            free(raw);
            return 0;
        }
        free(raw);
        bits = off;
        for (i = 0; i < ip->ptr_size && i < n; i++) {
            dst[i] = (uint8_t)((bits >> (8 * i)) & 0xffu);
        }
        return 1;
    }
    case JIR_OPND_FUNC:
        error_msg(ip, pos, "function values are not available at compile time");
        return 0;
    default:
        error_msg(ip, pos, "compile-time operand is not supported");
        return 0;
    }
}

static int read_int(Interp *ip, Frame *fr, const JirOperand *op, JogPos pos, uint64_t *out) {
    uint8_t buf[8];
    size_t n;
    size_t i;
    uint64_t v = 0;
    memset(buf, 0, sizeof(buf));
    n = ty_size(op ? op->type : NULL);
    if (n == 0 || n > 8) {
        n = 8;
    }
    if (!read_op(ip, fr, op, buf, n, pos)) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        v |= (uint64_t)buf[i] << (8 * i);
    }
    *out = v;
    return 1;
}

static int write_local(Interp *ip, Frame *fr, int32_t dst, const uint8_t *src, size_t n, JogPos pos) {
    uint32_t off = 0;
    uint32_t sz = 0;
    if (dst < 0) {
        return 1;
    }
    if (!local_at(fr, (uint32_t)dst, &off, &sz) || !range_ok(ip, off, sz)) {
        error_msg(ip, pos, "compile-time local is out of range");
        return 0;
    }
    memset(ip->mem + off, 0, sz);
    if (src && n) {
        memcpy(ip->mem + off, src, n < sz ? n : sz);
    }
    return 1;
}

static int write_local_bits(Interp *ip, Frame *fr, int32_t dst, uint64_t bits, JogPos pos) {
    uint8_t buf[8];
    uint32_t off = 0;
    uint32_t sz = 0;
    uint32_t i;
    if (dst < 0) {
        return 1;
    }
    if (!local_at(fr, (uint32_t)dst, &off, &sz)) {
        error_msg(ip, pos, "compile-time local is out of range");
        return 0;
    }
    for (i = 0; i < 8; i++) {
        buf[i] = (uint8_t)((bits >> (8 * i)) & 0xffu);
    }
    return write_local(ip, fr, dst, buf, 8, pos);
}

static int pop_frame(Interp *ip) {
    Frame *fr;
    if (ip->depth == 0) {
        return 0;
    }
    fr = &ip->frames[ip->depth - 1];
    ip->used = fr->used_at;
    free(fr->off);
    fr->off = NULL;
    ip->depth--;
    return 1;
}

static int push_frame(Interp *ip, JirFunc *func, int32_t ret_dst) {
    Frame *fr;
    uint32_t i;
    uint32_t nlocal;
    uint32_t *offs;
    uint32_t saved;
    if (!func) {
        error_msg(ip, (JogPos){0}, "compile-time function is missing");
        return 0;
    }
    if (ip->depth >= JOG_INTERP_DEPTH) {
        error_msg(ip, func->pos, "compile-time call stack exceeded");
        return 0;
    }
    if (ip->depth >= ip->frame_cap) {
        size_t cap = ip->frame_cap ? ip->frame_cap * 2 : 8;
        Frame *grown = realloc(ip->frames, cap * sizeof(*grown));
        if (!grown) {
            error_msg(ip, func->pos, "out of memory");
            return 0;
        }
        ip->frames = grown;
        ip->frame_cap = cap;
    }
    nlocal = (uint32_t)func->locals.count;
    offs = calloc(nlocal ? nlocal : 1, sizeof(*offs));
    if (!offs) {
        error_msg(ip, func->pos, "out of memory");
        return 0;
    }
    fr = &ip->frames[ip->depth];
    memset(fr, 0, sizeof(*fr));
    fr->func = func;
    fr->ret_dst = ret_dst;
    fr->used_at = ip->used;
    fr->off = offs;
    fr->nlocal = nlocal;
    saved = ip->used;
    ip->depth++;
    for (i = 0; i < nlocal; i++) {
        JirLocal *loc = jir_local_at(func, i);
        uint32_t sz = loc ? (uint32_t)ty_size(loc->type) : 0;
        uint32_t al = loc ? ty_align(loc->type) : 1;
        if (!bump(ip, al, sz, &offs[i])) {
            ip->used = saved;
            ip->depth--;
            free(offs);
            fr->off = NULL;
            return 0;
        }
    }
    if (func->blocks.count == 0) {
        error_msg(ip, func->pos, "compile-time function has no body");
        pop_frame(ip);
        return 0;
    }
    return 1;
}

static int value_fits(uint64_t bits, JogType *src, JogType *dst) {
    unsigned sw = int_width(src);
    unsigned dw = int_width(dst);
    int ss = int_signed(src);
    int ds = int_signed(dst);
    JogType *db = underlying(dst);
    JogType *sb = underlying(src);
    if (db && db->kind == JOG_TY_BOOL) {
        uint64_t v = bits & mask_of(sw ? sw : 8);
        return v == 0 || v == 1;
    }
    if (sb && sb->kind == JOG_TY_BOOL) {
        return 1;
    }
    if (ss == ds && dw >= sw) {
        return 1;
    }
    if (!ss && ds && dw > sw) {
        return 1;
    }
    if (ss && !ds) {
        int64_t v = sext(bits, sw);
        if (v < 0) {
            return 0;
        }
        if (dw >= sw) {
            return 1;
        }
        return (uint64_t)v <= mask_of(dw);
    }
    if (!ss && ds) {
        uint64_t v = bits & mask_of(sw);
        return v <= (uint64_t)signed_max(dw);
    }
    if (!ss && !ds) {
        return (bits & mask_of(sw)) <= mask_of(dw);
    }
    {
        int64_t v = sext(bits, sw);
        return v >= signed_min(dw) && v <= signed_max(dw);
    }
}

static int mul_i64(uint64_t ua, uint64_t ub, uint64_t *out) {
    int64_t a = (int64_t)ua;
    int64_t b = (int64_t)ub;
    if (a == 0 || b == 0) {
        *out = 0;
        return 0;
    }
    if (a == INT64_MIN) {
        if (b == 1) {
            *out = (uint64_t)INT64_MIN;
            return 0;
        }
        return 1;
    }
    if (b == INT64_MIN) {
        if (a == 1) {
            *out = (uint64_t)INT64_MIN;
            return 0;
        }
        return 1;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b) {
                return 1;
            }
        } else if (b < INT64_MIN / a) {
            return 1;
        }
    } else if (b > 0) {
        if (a < INT64_MIN / b) {
            return 1;
        }
    } else if (b < INT64_MAX / a) {
        return 1;
    }
    *out = (uint64_t)(a * b);
    return 0;
}

static int field_loc(JogType *type, uint32_t aux, uint32_t aux2, uint32_t ptr, uint64_t *off, uint64_t *sz) {
    type = underlying(type);
    if (!type) {
        return 0;
    }
    if (type->kind == JOG_TY_SLICE || type->kind == JOG_TY_BYTES || type->kind == JOG_TY_INTERFACE) {
        if (aux > 1 || ptr == 0) {
            return 0;
        }
        *off = aux == 0 ? 0 : ptr;
        *sz = ptr;
        return 1;
    }
    if (type->kind == JOG_TY_TUPLE) {
        if (!type->as.tuple.elems || aux >= type->as.tuple.count || !type->as.tuple.offsets) {
            return 0;
        }
        *off = type->as.tuple.offsets[aux];
        *sz = ty_size(type->as.tuple.elems[aux]);
        return 1;
    }
    if (type->kind == JOG_TY_STRUCT) {
        if (!type->as.nominal.fields || aux >= type->as.nominal.field_count) {
            return 0;
        }
        *off = type->as.nominal.fields[aux].offset;
        *sz = ty_size(type->as.nominal.fields[aux].type);
        return 1;
    }
    if (type->kind == JOG_TY_ENUM && aux2 > 0) {
        size_t variant = (size_t)aux2 - 1;
        JogVariant *payload;
        if (!type->as.nominal.variants || variant >= type->as.nominal.variant_count) {
            return 0;
        }
        payload = &type->as.nominal.variants[variant];
        if (!payload->fields || aux >= payload->field_count) {
            return 0;
        }
        *off = type->as.nominal.payload_offset + payload->fields[aux].offset;
        *sz = ty_size(payload->fields[aux].type);
        return 1;
    }
    if (type->kind == JOG_TY_OPTION && !jog_type_option_is_niche(type)) {
        JogType *elem = type->as.elem.elem;
        uint32_t al = ty_align(elem);
        uint64_t value_off = al <= 1 ? 1 : ((uint64_t)1 + al - 1) & ~((uint64_t)al - 1);
        if (aux == 0) {
            *off = 0;
            *sz = 1;
            return 1;
        }
        if (aux == 1) {
            *off = value_off;
            *sz = ty_size(elem);
            return 1;
        }
    }
    return 0;
}

static int pointee_size(JogType *ptr_ty, uint32_t *sz) {
    JogType *type = underlying(ptr_ty);
    JogType *elem;
    if (!type || type->kind != JOG_TY_PTR) {
        return 0;
    }
    elem = type->as.elem.elem;
    *sz = (uint32_t)ty_size(elem);
    return 1;
}

static int exec_binary(Interp *ip, Frame *fr, const JirInst *inst) {
    JirBinOp op = (JirBinOp)inst->aux;
    uint64_t ua = 0;
    uint64_t ub = 0;
    uint64_t out = 0;
    unsigned width;
    int sgn;
    int checked = inst->checked;
    int ov = 0;
    if (!read_int(ip, fr, &inst->a, inst->pos, &ua) || !read_int(ip, fr, &inst->b, inst->pos, &ub)) {
        return 0;
    }
    width = int_width(inst->a.type);
    sgn = int_signed(inst->a.type);
    ua &= mask_of(width);
    ub &= mask_of(int_width(inst->b.type));
    switch (op) {
    case JIR_ADD:
    case JIR_SUB:
    case JIR_MUL:
        if (!sgn && width >= 64) {
            if (op == JIR_ADD) {
                out = ua + ub;
                ov = out < ua;
            } else if (op == JIR_SUB) {
                out = ua - ub;
                ov = ua < ub;
            } else if (ub != 0 && ua > UINT64_MAX / ub) {
                ov = 1;
            } else {
                out = ua * ub;
            }
        } else if (!sgn) {
            uint64_t m = mask_of(width);
            if (op == JIR_ADD) {
                out = ua + ub;
                ov = out > m;
                out &= m;
            } else if (op == JIR_SUB) {
                ov = ua < ub;
                out = (ua - ub) & m;
            } else {
                out = ua * ub;
                ov = out > m;
                out &= m;
            }
        } else if (width >= 64) {
            if (op == JIR_ADD) {
                out = ua + ub;
                ov = ((~(ua ^ ub) & (ua ^ out)) & (uint64_t)1 << 63) != 0;
            } else if (op == JIR_SUB) {
                out = ua - ub;
                ov = ((ua ^ ub) & (ua ^ out) & ((uint64_t)1 << 63)) != 0;
            } else {
                ov = mul_i64(ua, ub, &out);
            }
        } else {
            int64_t sa = sext(ua, width);
            int64_t sb = sext(ub, width);
            int64_t r;
            if (op == JIR_ADD) {
                r = sa + sb;
            } else if (op == JIR_SUB) {
                r = sa - sb;
            } else {
                r = sa * sb;
            }
            ov = r < signed_min(width) || r > signed_max(width);
            out = (uint64_t)r & mask_of(width);
        }
        if (checked && ov) {
            panic_msg(ip, inst->pos, "overflow");
            return 0;
        }
        break;
    case JIR_DIV:
    case JIR_REM: {
        unsigned bw = int_width(inst->b.type);
        int bs = int_signed(inst->b.type);
        if (!sgn && !bs) {
            if (ub == 0) {
                panic_msg(ip, inst->pos, "divide by zero");
                return 0;
            }
            out = op == JIR_DIV ? ua / ub : ua % ub;
            out &= mask_of(width);
        } else {
            int64_t sa = sext(ua, width);
            int64_t sb = sext(ub, bw);
            int64_t r;
            if (sb == 0) {
                panic_msg(ip, inst->pos, "divide by zero");
                return 0;
            }
            if (width >= 64 && sa == INT64_MIN && sb == -1) {
                if (checked || op == JIR_DIV) {
                    panic_msg(ip, inst->pos, "overflow");
                    return 0;
                }
                out = 0;
                break;
            }
            r = op == JIR_DIV ? sa / sb : sa % sb;
            if (checked && width < 64 && (r < signed_min(width) || r > signed_max(width))) {
                panic_msg(ip, inst->pos, "overflow");
                return 0;
            }
            out = (uint64_t)r & mask_of(width);
        }
        break;
    }
    case JIR_AND:
        out = ua & ub;
        break;
    case JIR_OR:
        out = ua | ub;
        break;
    case JIR_XOR:
        out = ua ^ ub;
        break;
    case JIR_SHL:
    case JIR_SHR: {
        unsigned sw = int_width(inst->b.type);
        int ss = int_signed(inst->b.type);
        uint64_t sh = ub;
        int neg = 0;
        if (ss && sext(ub, sw) < 0) {
            neg = 1;
        }
        if (checked && (neg || sh >= width)) {
            panic_msg(ip, inst->pos, "overflow");
            return 0;
        }
        if (neg || width == 0 || sh >= 64 || sh >= width) {
            if (op == JIR_SHR && sgn && width > 0 && (ua & ((uint64_t)1 << (width - 1)))) {
                out = mask_of(width);
            } else {
                out = 0;
            }
            break;
        }
        if (op == JIR_SHL) {
            out = (ua << sh) & mask_of(width);
        } else if (!sgn) {
            out = (ua >> sh) & mask_of(width);
        } else {
            uint64_t logical = ua >> sh;
            uint64_t low = mask_of(width - (unsigned)sh);
            uint64_t ones = mask_of(width);
            if (ua & ((uint64_t)1 << (width - 1))) {
                out = (logical & low) | (ones & ~low);
            } else {
                out = logical & low;
            }
        }
        break;
    }
    case JIR_EQ:
    case JIR_NE:
    case JIR_LT:
    case JIR_LE:
    case JIR_GT:
    case JIR_GE: {
        int cmp_s = sgn;
        int less = 0;
        int eq = 0;
        if (cmp_s) {
            int64_t sa = sext(ua, width);
            int64_t sb = sext(ub, int_width(inst->b.type));
            less = sa < sb;
            eq = sa == sb;
        } else {
            less = ua < ub;
            eq = ua == ub;
        }
        if (op == JIR_EQ) {
            out = eq;
        } else if (op == JIR_NE) {
            out = !eq;
        } else if (op == JIR_LT) {
            out = less;
        } else if (op == JIR_GT) {
            out = !less && !eq;
        } else if (op == JIR_LE) {
            out = less || eq;
        } else {
            out = !less;
        }
        out = out ? 1 : 0;
        break;
    }
    default:
        error_msg(ip, inst->pos, "compile-time operator is not supported");
        return 0;
    }
    return write_local_bits(ip, fr, inst->dst, out, inst->pos);
}

static int exec_unary(Interp *ip, Frame *fr, const JirInst *inst) {
    uint64_t ua = 0;
    uint64_t out = 0;
    unsigned width;
    if (!read_int(ip, fr, &inst->a, inst->pos, &ua)) {
        return 0;
    }
    width = int_width(inst->a.type);
    ua &= mask_of(width);
    if (inst->aux == JIR_NEG) {
        JirInst sub;
        JirOperand zero;
        memset(&sub, 0, sizeof(sub));
        zero = jir_int(inst->a.type, 0);
        sub.op = JIR_BINARY;
        sub.aux = JIR_SUB;
        sub.checked = inst->checked;
        sub.dst = inst->dst;
        sub.a = zero;
        sub.b = inst->a;
        sub.pos = inst->pos;
        return exec_binary(ip, fr, &sub);
    }
    if (inst->aux == JIR_NOT) {
        out = ua == 0 ? 1 : 0;
    } else if (inst->aux == JIR_BITNOT) {
        out = (~ua) & mask_of(width);
    } else {
        error_msg(ip, inst->pos, "compile-time operator is not supported");
        return 0;
    }
    return write_local_bits(ip, fr, inst->dst, out, inst->pos);
}

static int exec_convert(Interp *ip, Frame *fr, const JirInst *inst) {
    uint64_t bits = 0;
    uint32_t off = 0;
    uint32_t sz = 0;
    JirLocal *dst_loc;
    JogType *dst_ty;
    JogType *src_ty;
    unsigned sw;
    unsigned dw;
    uint64_t written;
    if (inst->dst < 0) {
        error_msg(ip, inst->pos, "compile-time convert has no destination");
        return 0;
    }
    dst_loc = jir_local_at(fr->func, (uint32_t)inst->dst);
    dst_ty = dst_loc ? dst_loc->type : NULL;
    src_ty = inst->a.type;
    if (is_float_ty(src_ty) || is_float_ty(dst_ty)) {
        error_msg(ip, inst->pos, "float values are not available at compile time");
        return 0;
    }
    if (!read_int(ip, fr, &inst->a, inst->pos, &bits)) {
        return 0;
    }
    if (inst->checked && !value_fits(bits, src_ty, dst_ty)) {
        panic_msg(ip, inst->pos, "convert");
        return 0;
    }
    sw = int_width(src_ty);
    dw = int_width(dst_ty);
    if (underlying(dst_ty) && underlying(dst_ty)->kind == JOG_TY_BOOL) {
        written = (bits & mask_of(sw ? sw : 8)) ? 1 : 0;
    } else if (int_signed(dst_ty)) {
        int64_t v;
        if (underlying(src_ty) && underlying(src_ty)->kind == JOG_TY_BOOL) {
            v = bits & 1;
        } else if (int_signed(src_ty)) {
            v = sext(bits, sw);
        } else {
            v = (int64_t)(bits & mask_of(sw));
        }
        written = (uint64_t)v;
    } else if (int_signed(src_ty)) {
        written = (uint64_t)sext(bits, sw);
    } else {
        written = bits & mask_of(sw);
    }
    (void)dw;
    if (!local_at(fr, (uint32_t)inst->dst, &off, &sz)) {
        error_msg(ip, inst->pos, "compile-time local is out of range");
        return 0;
    }
    mem_write(ip, off, sz, written & (sz >= 8 ? ~(uint64_t)0 : mask_of(sz * 8)));
    return 1;
}

static int exec_copy_op(Interp *ip, Frame *fr, int32_t dst, const JirOperand *src, JogPos pos) {
    uint32_t off = 0;
    uint32_t sz = 0;
    uint8_t *tmp;
    if (dst < 0) {
        return 1;
    }
    if (!local_at(fr, (uint32_t)dst, &off, &sz)) {
        error_msg(ip, pos, "compile-time local is out of range");
        return 0;
    }
    if (sz == 0) {
        return 1;
    }
    if (src && src->kind == JIR_OPND_LOCAL) {
        uint32_t soff = 0;
        uint32_t ssz = 0;
        if (!local_at(fr, src->as.local, &soff, &ssz) || !range_ok(ip, soff, ssz) || !range_ok(ip, off, sz)) {
            error_msg(ip, pos, "compile-time local is out of range");
            return 0;
        }
        memset(ip->mem + off, 0, sz);
        memcpy(ip->mem + off, ip->mem + soff, ssz < sz ? ssz : sz);
        return 1;
    }
    tmp = malloc(sz);
    if (!tmp) {
        error_msg(ip, pos, "out of memory");
        return 0;
    }
    if (!read_op(ip, fr, src, tmp, sz, pos)) {
        free(tmp);
        return 0;
    }
    if (!range_ok(ip, off, sz)) {
        free(tmp);
        error_msg(ip, pos, "compile-time local is out of range");
        return 0;
    }
    memcpy(ip->mem + off, tmp, sz);
    free(tmp);
    return 1;
}

static int load_ptr(Interp *ip, Frame *fr, const JirOperand *op, JogPos pos, uint64_t *addr) {
    uint64_t bits = 0;
    if (!read_int(ip, fr, op, pos, &bits)) {
        return 0;
    }
    *addr = bits & mask_of(ip->ptr_size * 8);
    return 1;
}

static int mem_copy_checked(Interp *ip, uint32_t off, uint32_t n, uint8_t *dst, int store, const uint8_t *src, JogPos pos) {
    if (off == 0) {
        panic_msg(ip, pos, "null pointer");
        return 0;
    }
    if (!range_ok(ip, off, n)) {
        panic_msg(ip, pos, "bounds");
        return 0;
    }
    if (!store && hits_var(ip, off, n)) {
        error_msg(ip, pos, "cannot read a var at compile time");
        return 0;
    }
    if (store && hits_global(ip, off, n)) {
        error_msg(ip, pos, "cannot write a global at compile time");
        return 0;
    }
    if (n == 0) {
        return 1;
    }
    if (store) {
        memcpy(ip->mem + off, src, n);
    } else {
        memcpy(dst, ip->mem + off, n);
    }
    return 1;
}

static int exec_aggregate(Interp *ip, Frame *fr, const JirInst *inst) {
    JirLocal *dst_loc;
    JogType *type;
    uint32_t off = 0;
    uint32_t sz = 0;
    size_t argi = 0;
    if (inst->dst < 0 || !local_at(fr, (uint32_t)inst->dst, &off, &sz) || !range_ok(ip, off, sz)) {
        error_msg(ip, inst->pos, "compile-time aggregate has no destination");
        return 0;
    }
    memset(ip->mem + off, 0, sz);
    dst_loc = jir_local_at(fr->func, (uint32_t)inst->dst);
    type = underlying(dst_loc ? dst_loc->type : NULL);
    if (!type) {
        error_msg(ip, inst->pos, "compile-time aggregate has no type");
        return 0;
    }
    if (type->kind == JOG_TY_OPTION && jog_type_option_is_niche(type)) {
        uint64_t tag = 0;
        if (inst->arg_count < 2 || !inst->args) {
            error_msg(ip, inst->pos, "compile-time aggregate is missing a field");
            return 0;
        }
        if (!read_int(ip, fr, &inst->args[0], inst->pos, &tag)) {
            return 0;
        }
        if (tag) {
            return exec_copy_op(ip, fr, inst->dst, &inst->args[1], inst->pos);
        }
        return 1;
    }
    if (type->kind == JOG_TY_SLICE || type->kind == JOG_TY_BYTES || type->kind == JOG_TY_INTERFACE) {
        if (inst->arg_count < 2 || !inst->args) {
            error_msg(ip, inst->pos, "compile-time aggregate is missing a field");
            return 0;
        }
        if (!read_op(ip, fr, &inst->args[0], ip->mem + off, ip->ptr_size, inst->pos)) {
            return 0;
        }
        return read_op(ip, fr, &inst->args[1], ip->mem + off + ip->ptr_size, ip->ptr_size, inst->pos);
    }
    if (type->kind == JOG_TY_ENUM) {
        const JogVariant *variant;
        size_t field;
        if (!type->as.nominal.variants || inst->aux >= type->as.nominal.variant_count) {
            error_msg(ip, inst->pos, "compile-time aggregate is missing a field");
            return 0;
        }
        variant = &type->as.nominal.variants[inst->aux];
        mem_write(ip, off, (uint32_t)ty_size(type->as.nominal.base), (uint64_t)variant->tag);
        for (field = 0; field < variant->field_count; field++) {
            JogType *ft = variant->fields ? variant->fields[field].type : NULL;
            uint64_t foff = type->as.nominal.payload_offset + (variant->fields ? variant->fields[field].offset : 0);
            size_t fsz;
            if (ty_size(ft) == 0) {
                continue;
            }
            while (argi < inst->arg_count && inst->args && ty_size(inst->args[argi].type) == 0) {
                argi++;
            }
            if (argi >= inst->arg_count || !inst->args) {
                error_msg(ip, inst->pos, "compile-time aggregate is missing a field");
                return 0;
            }
            fsz = ty_size(ft);
            if (foff + fsz > sz) {
                error_msg(ip, inst->pos, "compile-time aggregate is missing a field");
                return 0;
            }
            if (!read_op(ip, fr, &inst->args[argi], ip->mem + off + (uint32_t)foff, fsz, inst->pos)) {
                return 0;
            }
            argi++;
        }
        return 1;
    }
    if (type->kind == JOG_TY_OPTION) {
        JogType *elem = type->as.elem.elem;
        uint32_t al = ty_align(elem);
        uint64_t value_off = al <= 1 ? 1 : ((uint64_t)1 + al - 1) & ~((uint64_t)al - 1);
        if (inst->arg_count < 1 || !inst->args) {
            error_msg(ip, inst->pos, "compile-time aggregate is missing a field");
            return 0;
        }
        if (!read_op(ip, fr, &inst->args[0], ip->mem + off, 1, inst->pos)) {
            return 0;
        }
        if (ty_size(elem) == 0) {
            return 1;
        }
        if (inst->arg_count < 2) {
            error_msg(ip, inst->pos, "compile-time aggregate is missing a field");
            return 0;
        }
        return read_op(ip, fr, &inst->args[1], ip->mem + off + (uint32_t)value_off, ty_size(elem), inst->pos);
    }
    {
        size_t fields = 0;
        size_t field;
        if (type->kind == JOG_TY_STRUCT) {
            fields = type->as.nominal.field_count;
        } else if (type->kind == JOG_TY_TUPLE) {
            fields = type->as.tuple.count;
        } else {
            error_msg(ip, inst->pos, "compile-time aggregate is not supported");
            return 0;
        }
        for (field = 0; field < fields; field++) {
            JogType *ft = NULL;
            uint64_t foff = 0;
            size_t fsz;
            if (type->kind == JOG_TY_STRUCT && type->as.nominal.fields) {
                ft = type->as.nominal.fields[field].type;
                foff = type->as.nominal.fields[field].offset;
            } else if (type->as.tuple.elems && type->as.tuple.offsets) {
                ft = type->as.tuple.elems[field];
                foff = type->as.tuple.offsets[field];
            }
            if (ty_size(ft) == 0) {
                continue;
            }
            while (argi < inst->arg_count && inst->args && ty_size(inst->args[argi].type) == 0) {
                argi++;
            }
            if (argi >= inst->arg_count || !inst->args) {
                error_msg(ip, inst->pos, "compile-time aggregate is missing a field");
                return 0;
            }
            fsz = ty_size(ft);
            if (!read_op(ip, fr, &inst->args[argi], ip->mem + off + (uint32_t)foff, fsz, inst->pos)) {
                return 0;
            }
            argi++;
        }
    }
    return 1;
}

static int exec_call(Interp *ip, Frame *fr, const JirInst *inst) {
    JirFunc *callee;
    uint8_t **bufs = NULL;
    size_t nparam;
    size_t i;
    int32_t ret_dst;
    if (inst->a.kind != JIR_OPND_FUNC) {
        error_msg(ip, inst->pos, "indirect calls are not available at compile time");
        return 0;
    }
    callee = jir_func(ip->module, inst->a.as.func);
    if (!callee) {
        error_msg(ip, inst->pos, "compile-time function is missing");
        return 0;
    }
    if (callee->linkage == JIR_LINK_EXTERN) {
        char buf[160];
        const char *name = callee->jog_name ? callee->jog_name : (callee->name ? callee->name : "?");
        (void)snprintf(buf, sizeof(buf), "cannot call an extern function '%s' at compile time", name);
        error_msg(ip, inst->pos, buf);
        return 0;
    }
    nparam = callee->param_count;
    if (inst->arg_count != nparam) {
        error_msg(ip, inst->pos, "compile-time call has the wrong number of arguments");
        return 0;
    }
    if (nparam && !inst->args) {
        error_msg(ip, inst->pos, "compile-time call has the wrong number of arguments");
        return 0;
    }
    if (nparam) {
        bufs = calloc(nparam, sizeof(*bufs));
        if (!bufs) {
            error_msg(ip, inst->pos, "out of memory");
            return 0;
        }
        for (i = 0; i < nparam; i++) {
            JirLocal *param = jir_local_at(callee, (uint32_t)i);
            size_t sz = param ? ty_size(param->type) : 0;
            bufs[i] = malloc(sz ? sz : 1);
            if (!bufs[i]) {
                error_msg(ip, inst->pos, "out of memory");
                break;
            }
            memset(bufs[i], 0, sz ? sz : 1);
            if (sz && !read_op(ip, fr, &inst->args[i], bufs[i], sz, inst->pos)) {
                break;
            }
        }
        if (ip->failed || i < nparam) {
            for (i = 0; i < nparam; i++) {
                free(bufs[i]);
            }
            free(bufs);
            return 0;
        }
    }
    ret_dst = inst->dst;
    if (!push_frame(ip, callee, ret_dst)) {
        if (bufs) {
            for (i = 0; i < nparam; i++) {
                free(bufs[i]);
            }
            free(bufs);
        }
        return 0;
    }
    {
        Frame *child = &ip->frames[ip->depth - 1];
        for (i = 0; i < nparam; i++) {
            JirLocal *param = jir_local_at(callee, (uint32_t)i);
            size_t sz = param ? ty_size(param->type) : 0;
            if (sz) {
                memcpy(ip->mem + child->off[i], bufs[i], sz);
            }
        }
    }
    if (bufs) {
        for (i = 0; i < nparam; i++) {
            free(bufs[i]);
        }
        free(bufs);
    }
    return 1;
}

static int exec_inst(Interp *ip, Frame *fr, const JirInst *inst, int *advance) {
    *advance = 1;
    if (!inst) {
        error_msg(ip, fr->func->pos, "compile-time instruction is missing");
        return 0;
    }
    switch (inst->op) {
    case JIR_COPY:
        return exec_copy_op(ip, fr, inst->dst, &inst->a, inst->pos);
    case JIR_ZERO: {
        uint32_t off = 0;
        uint32_t sz = 0;
        if (inst->dst < 0 || !local_at(fr, (uint32_t)inst->dst, &off, &sz) || !range_ok(ip, off, sz)) {
            error_msg(ip, inst->pos, "compile-time local is out of range");
            return 0;
        }
        memset(ip->mem + off, 0, sz);
        return 1;
    }
    case JIR_BINARY:
        return exec_binary(ip, fr, inst);
    case JIR_UNARY:
        return exec_unary(ip, fr, inst);
    case JIR_CONVERT:
        return exec_convert(ip, fr, inst);
    case JIR_BITCAST:
        return exec_copy_op(ip, fr, inst->dst, &inst->a, inst->pos);
    case JIR_LOAD: {
        uint64_t addr = 0;
        uint32_t off = 0;
        uint32_t sz = 0;
        uint8_t *tmp;
        if (inst->dst < 0 || !local_at(fr, (uint32_t)inst->dst, &off, &sz)) {
            error_msg(ip, inst->pos, "compile-time load has no destination");
            return 0;
        }
        if (!load_ptr(ip, fr, &inst->a, inst->pos, &addr)) {
            return 0;
        }
        if (sz == 0) {
            return 1;
        }
        if (addr > UINT32_MAX) {
            panic_msg(ip, inst->pos, "bounds");
            return 0;
        }
        tmp = malloc(sz);
        if (!tmp) {
            error_msg(ip, inst->pos, "out of memory");
            return 0;
        }
        if (!mem_copy_checked(ip, (uint32_t)addr, sz, tmp, 0, NULL, inst->pos)) {
            free(tmp);
            return 0;
        }
        memcpy(ip->mem + off, tmp, sz);
        free(tmp);
        return 1;
    }
    case JIR_STORE: {
        uint64_t addr = 0;
        size_t n = ty_size(inst->b.type);
        uint8_t *tmp;
        if (n == 0) {
            uint32_t psz = 0;
            if (!pointee_size(inst->a.type, &psz)) {
                return 1;
            }
            n = psz;
        }
        if (!load_ptr(ip, fr, &inst->a, inst->pos, &addr)) {
            return 0;
        }
        if (n == 0) {
            return 1;
        }
        tmp = malloc(n);
        if (!tmp) {
            error_msg(ip, inst->pos, "out of memory");
            return 0;
        }
        if (!read_op(ip, fr, &inst->b, tmp, n, inst->pos)) {
            free(tmp);
            return 0;
        }
        if (addr > UINT32_MAX || !mem_copy_checked(ip, (uint32_t)addr, (uint32_t)n, NULL, 1, tmp, inst->pos)) {
            free(tmp);
            return 0;
        }
        free(tmp);
        return 1;
    }
    case JIR_ADDR: {
        uint32_t off = 0;
        uint32_t sz = 0;
        if (inst->a.kind != JIR_OPND_LOCAL || !local_at(fr, inst->a.as.local, &off, &sz)) {
            error_msg(ip, inst->pos, "compile-time addr needs a local");
            return 0;
        }
        return write_local_bits(ip, fr, inst->dst, off, inst->pos);
    }
    case JIR_FIELD_ADDR: {
        uint64_t addr = 0;
        uint64_t foff = 0;
        uint64_t fsz = 0;
        JogType *ptr = underlying(inst->a.type);
        JogType *base = ptr && ptr->kind == JOG_TY_PTR ? ptr->as.elem.elem : NULL;
        if (!load_ptr(ip, fr, &inst->a, inst->pos, &addr)) {
            return 0;
        }
        if (!field_loc(base, inst->aux, inst->aux2, ip->ptr_size, &foff, &fsz)) {
            error_msg(ip, inst->pos, "compile-time field is out of range");
            return 0;
        }
        (void)fsz;
        return write_local_bits(ip, fr, inst->dst, addr + foff, inst->pos);
    }
    case JIR_ELEM_ADDR: {
        uint64_t addr = 0;
        uint64_t index = 0;
        uint32_t esz = 0;
        if (!load_ptr(ip, fr, &inst->a, inst->pos, &addr) || !read_int(ip, fr, &inst->b, inst->pos, &index)) {
            return 0;
        }
        if (!pointee_size(inst->a.type, &esz)) {
            error_msg(ip, inst->pos, "compile-time element address has no type");
            return 0;
        }
        return write_local_bits(ip, fr, inst->dst, addr + index * esz, inst->pos);
    }
    case JIR_EXTRACT: {
        uint32_t off = 0;
        uint32_t sz = 0;
        uint64_t foff = 0;
        uint64_t fsz = 0;
        uint8_t *tmp;
        if (inst->a.kind != JIR_OPND_LOCAL || !local_at(fr, inst->a.as.local, &off, &sz)) {
            error_msg(ip, inst->pos, "compile-time extract needs a local");
            return 0;
        }
        if (!field_loc(inst->a.type, inst->aux, inst->aux2, ip->ptr_size, &foff, &fsz)) {
            error_msg(ip, inst->pos, "compile-time field is out of range");
            return 0;
        }
        if ((uint64_t)off + foff + fsz > ip->used) {
            panic_msg(ip, inst->pos, "bounds");
            return 0;
        }
        if (fsz == 0) {
            return write_local(ip, fr, inst->dst, NULL, 0, inst->pos);
        }
        tmp = malloc((size_t)fsz);
        if (!tmp) {
            error_msg(ip, inst->pos, "out of memory");
            return 0;
        }
        memcpy(tmp, ip->mem + off + (uint32_t)foff, (size_t)fsz);
        if (!write_local(ip, fr, inst->dst, tmp, (size_t)fsz, inst->pos)) {
            free(tmp);
            return 0;
        }
        free(tmp);
        return 1;
    }
    case JIR_AGGREGATE:
        return exec_aggregate(ip, fr, inst);
    case JIR_ENUM_TAG: {
        uint32_t off = 0;
        uint32_t sz = 0;
        JogType *type = underlying(inst->a.type);
        size_t tsz;
        uint8_t *tmp;
        if (inst->a.kind != JIR_OPND_LOCAL || !local_at(fr, inst->a.as.local, &off, &sz) || !type ||
            type->kind != JOG_TY_ENUM) {
            error_msg(ip, inst->pos, "compile-time enum tag needs an enum");
            return 0;
        }
        tsz = ty_size(type->as.nominal.base);
        tmp = calloc(tsz ? tsz : 1, 1);
        if (!tmp) {
            error_msg(ip, inst->pos, "out of memory");
            return 0;
        }
        if (tsz) {
            memcpy(tmp, ip->mem + off, tsz);
        }
        if (!write_local(ip, fr, inst->dst, tmp, tsz, inst->pos)) {
            free(tmp);
            return 0;
        }
        free(tmp);
        return 1;
    }
    case JIR_CALL:
        if (!exec_call(ip, fr, inst)) {
            return 0;
        }
        *advance = 0;
        return 1;
    case JIR_BOUNDS_CHECK: {
        uint64_t a = 0;
        uint64_t b = 0;
        if (!read_int(ip, fr, &inst->a, inst->pos, &a) || !read_int(ip, fr, &inst->b, inst->pos, &b)) {
            return 0;
        }
        a &= mask_of(int_width(inst->a.type));
        b &= mask_of(int_width(inst->b.type));
        if (!(a < b)) {
            panic_msg(ip, inst->pos, "bounds");
            return 0;
        }
        return 1;
    }
    case JIR_NULL_CHECK: {
        uint64_t bits = 0;
        JogType *type = underlying(inst->a.type);
        if (!read_int(ip, fr, &inst->a, inst->pos, &bits)) {
            return 0;
        }
        if (type && type->kind == JOG_TY_OPTION && !jog_type_option_is_niche(type)) {
            if ((bits & 0xffu) == 0) {
                panic_msg(ip, inst->pos, "null pointer");
                return 0;
            }
            return 1;
        }
        if ((bits & mask_of(ip->ptr_size * 8)) == 0) {
            panic_msg(ip, inst->pos, "null pointer");
            return 0;
        }
        return 1;
    }
    case JIR_PANIC: {
        const char *msg = "panic";
        if (inst->a.kind == JIR_OPND_STRING && inst->a.as.str.ptr && inst->a.as.str.ptr[0]) {
            msg = inst->a.as.str.ptr;
        }
        panic_msg(ip, inst->pos, msg);
        return 0;
    }
    default:
        error_msg(ip, inst->pos, "compile-time instruction is not supported");
        return 0;
    }
}

static int jump_block(Interp *ip, Frame *fr, uint32_t target, JogPos pos) {
    if (!fr->func || target >= fr->func->blocks.count) {
        error_msg(ip, pos, "compile-time branch is out of range");
        return 0;
    }
    fr->block = target;
    fr->ip = 0;
    return 1;
}

static int exec_term(Interp *ip, Frame *fr, const JirTerm *term) {
    JogPos pos = term ? term->pos : fr->func->pos;
    if (!term || term->kind == JIR_TERM_NONE) {
        error_msg(ip, pos, "compile-time function is missing a terminator");
        return 0;
    }
    switch (term->kind) {
    case JIR_TERM_BR:
        return jump_block(ip, fr, term->target, pos);
    case JIR_TERM_COND_BR: {
        uint64_t cond = 0;
        if (!read_int(ip, fr, &term->value, pos, &cond)) {
            return 0;
        }
        return jump_block(ip, fr, cond ? term->target : term->else_target, pos);
    }
    case JIR_TERM_SWITCH: {
        uint64_t value = 0;
        size_t i;
        unsigned width = int_width(term->value.type);
        if (!read_int(ip, fr, &term->value, pos, &value)) {
            return 0;
        }
        value &= mask_of(width);
        for (i = 0; i < term->case_count; i++) {
            uint64_t arm = term->cases[i].value & mask_of(width);
            if (arm == value) {
                return jump_block(ip, fr, term->cases[i].target, pos);
            }
        }
        return jump_block(ip, fr, term->target, pos);
    }
    case JIR_TERM_RET: {
        int32_t ret_dst = fr->ret_dst;
        JogType *ret_ty = NULL;
        size_t n = 0;
        uint8_t *buf = NULL;
        int root = ip->depth == 1;
        if (fr->func && fr->func->type && fr->func->type->kind == JOG_TY_FN) {
            ret_ty = fr->func->type->as.fn.ret;
        }
        n = ty_size(ret_ty);
        if (n) {
            buf = malloc(n);
            if (!buf) {
                error_msg(ip, pos, "out of memory");
                return 0;
            }
            memset(buf, 0, n);
            if (term->value.kind != JIR_OPND_NONE && !read_op(ip, fr, &term->value, buf, n, pos)) {
                free(buf);
                return 0;
            }
        }
        if (!pop_frame(ip)) {
            free(buf);
            return 0;
        }
        if (root) {
            size_t m = n < ip->result_n ? n : ip->result_n;
            if (m && buf && ip->result) {
                memcpy(ip->result, buf, m);
            }
            free(buf);
            return 1;
        }
        if (ret_dst >= 0 && buf) {
            Frame *caller = &ip->frames[ip->depth - 1];
            if (!write_local(ip, caller, ret_dst, buf, n, pos)) {
                free(buf);
                return 0;
            }
        }
        free(buf);
        if (ip->depth > 0) {
            ip->frames[ip->depth - 1].ip++;
        }
        return 1;
    }
    case JIR_TERM_UNREACHABLE:
        panic_msg(ip, pos, "unreachable");
        return 0;
    default:
        error_msg(ip, pos, "compile-time terminator is not supported");
        return 0;
    }
}

static int run(Interp *ip) {
    while (!ip->failed && ip->depth > 0) {
        Frame *fr = &ip->frames[ip->depth - 1];
        JirBlock *block = jir_block_at(fr->func, fr->block);
        size_t ninst;
        if (!block) {
            error_msg(ip, fr->func->pos, "compile-time block is missing");
            break;
        }
        if (ip->steps >= ip->budget) {
            error_msg(ip, block->term.pos.line ? block->term.pos : fr->func->pos, "compile-time step budget exceeded");
            break;
        }
        ip->steps++;
        ninst = block->insts.count;
        if (fr->ip < ninst) {
            JirInst *inst = ct_da_at(&block->insts, fr->ip);
            int advance = 1;
            if (!exec_inst(ip, fr, inst, &advance)) {
                break;
            }
            if (!ip->failed && advance && ip->depth > 0) {
                ip->frames[ip->depth - 1].ip++;
            }
        } else if (!exec_term(ip, fr, &block->term)) {
            break;
        }
    }
    return !ip->failed && ip->depth == 0;
}

static int place_globals(Interp *ip) {
    size_t i;
    size_t n = ip->module->globals.count;
    ip->globals = calloc(n ? n : 1, sizeof(*ip->globals));
    if (!ip->globals) {
        error_msg(ip, (JogPos){0}, "out of memory");
        return 0;
    }
    ip->nglobal = n;
    ip->used = 16;
    if (!ensure_cap(ip, 16)) {
        error_msg(ip, (JogPos){0}, "out of memory");
        return 0;
    }
    memset(ip->mem, 0, 16);
    for (i = 0; i < n; i++) {
        JirGlobal *g = jir_global(ip->module, (uint32_t)i);
        uint32_t sz = g ? (uint32_t)ty_size(g->type) : 0;
        uint32_t al = g ? ty_align(g->type) : 1;
        uint32_t off = 0;
        if (!g) {
            error_msg(ip, (JogPos){0}, "compile-time global is missing");
            return 0;
        }
        if (!bump(ip, al, sz, &off)) {
            return 0;
        }
        ip->globals[i].off = off;
        ip->globals[i].size = sz;
        ip->globals[i].is_const = g->is_const;
        if (g->data && g->data_len && sz) {
            size_t m = g->data_len < sz ? g->data_len : sz;
            memcpy(ip->mem + off, g->data, m);
        }
    }
    return !ip->failed;
}

static void interp_free(Interp *ip) {
    while (ip->depth > 0) {
        pop_frame(ip);
    }
    free(ip->frames);
    free(ip->globals);
    free(ip->mem);
    free(ip->result);
    ip->frames = NULL;
    ip->globals = NULL;
    ip->mem = NULL;
    ip->result = NULL;
}

static uint64_t g_driver_step_budget;

void jog_interp_set_step_budget(uint64_t budget) {
    g_driver_step_budget = budget;
}

bool jir_interp_call(const JirModule *module, uint32_t func_index, const JirOperand *args, size_t arg_count,
                     uint64_t step_budget, const uint8_t **out, CtDiag *diag) {
    Interp ip;
    JirFunc *func;
    JogType *ret = NULL;
    size_t n = 0;
    size_t i;
    uint8_t **bufs = NULL;
    uint8_t *published = NULL;
    if (out) {
        *out = NULL;
    }
    if (!module || !out || !diag || !module->arena) {
        return 0;
    }
    memset(&ip, 0, sizeof(ip));
    if (g_driver_step_budget != 0) {
        step_budget = g_driver_step_budget;
    }
    ip.module = module;
    ip.diag = diag;
    ip.budget = step_budget;
    ip.ptr_size = module->types ? jog_types_ptr_size(module->types) : 8;
    if (ip.ptr_size == 0 || ip.ptr_size > 8) {
        ip.ptr_size = 8;
    }
    func = jir_func(module, func_index);
    if (!func) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, cpos((JogPos){0}), "compile-time function is missing");
        return 0;
    }
    if (func->linkage == JIR_LINK_EXTERN) {
        const char *name = func->jog_name ? func->jog_name : (func->name ? func->name : "?");
        ct_diag_emit(diag, CT_SEVERITY_ERROR, cpos(func->pos), "cannot call an extern function '%s' at compile time",
                     name);
        return 0;
    }
    if (func->type && func->type->kind == JOG_TY_FN) {
        ret = func->type->as.fn.ret;
    }
    n = ty_size(ret);
    if (arg_count != func->param_count || (arg_count && !args)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, cpos(func->pos), "compile-time call has the wrong number of arguments");
        return 0;
    }
    if (!place_globals(&ip)) {
        interp_free(&ip);
        return 0;
    }
    ip.result_n = n;
    ip.result = calloc(n ? n : 1, 1);
    if (!ip.result) {
        error_msg(&ip, func->pos, "out of memory");
        interp_free(&ip);
        return 0;
    }
    if (arg_count) {
        bufs = calloc(arg_count, sizeof(*bufs));
        if (!bufs) {
            error_msg(&ip, func->pos, "out of memory");
            interp_free(&ip);
            return 0;
        }
        for (i = 0; i < arg_count; i++) {
            JirLocal *param = jir_local_at(func, (uint32_t)i);
            size_t sz = param ? ty_size(param->type) : 0;
            if (args[i].kind == JIR_OPND_LOCAL || args[i].kind == JIR_OPND_GLOBAL) {
                error_msg(&ip, func->pos, "compile-time argument is not a constant");
                break;
            }
            bufs[i] = malloc(sz ? sz : 1);
            if (!bufs[i]) {
                error_msg(&ip, func->pos, "out of memory");
                break;
            }
            memset(bufs[i], 0, sz ? sz : 1);
            if (sz && !read_op(&ip, NULL, &args[i], bufs[i], sz, func->pos)) {
                break;
            }
        }
        if (ip.failed) {
            for (i = 0; i < arg_count; i++) {
                free(bufs[i]);
            }
            free(bufs);
            interp_free(&ip);
            return 0;
        }
    }
    if (!push_frame(&ip, func, -1)) {
        if (bufs) {
            for (i = 0; i < arg_count; i++) {
                free(bufs[i]);
            }
            free(bufs);
        }
        interp_free(&ip);
        return 0;
    }
    for (i = 0; i < arg_count; i++) {
        JirLocal *param = jir_local_at(func, (uint32_t)i);
        size_t sz = param ? ty_size(param->type) : 0;
        if (sz) {
            memcpy(ip.mem + ip.frames[0].off[i], bufs[i], sz);
        }
        free(bufs[i]);
    }
    free(bufs);
    if (!run(&ip)) {
        interp_free(&ip);
        return 0;
    }
    published = ct_arena_alloc(module->arena, n ? n : 1, 1);
    if (!published) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, cpos(func->pos), "out of memory");
        interp_free(&ip);
        return 0;
    }
    memset(published, 0, n ? n : 1);
    if (n) {
        memcpy(published, ip.result, n);
    }
    *out = published;
    interp_free(&ip);
    return 1;
}
