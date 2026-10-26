#include "const_eval.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

/* Sign-magnitude integer, little-endian limbs, no leading zero limbs.
   Magnitude is at most CONST_MAX_BITS. Zero is non-negative. */
enum { CONST_LIMB_BITS = 32, CONST_MAX_BITS = 4096, CONST_MAX_LIMBS = CONST_MAX_BITS / CONST_LIMB_BITS };

typedef struct Big {
    uint32_t limb[CONST_MAX_LIMBS];
    uint16_t n;
    bool neg;
} Big;

enum { PARSE_OK = 0, PARSE_INVALID = 1, PARSE_BIG = 2 };

static void big_trim(Big *b) {
    while (b->n > 1 && b->limb[b->n - 1] == 0) {
        b->n--;
    }
    if (b->n == 1 && b->limb[0] == 0) {
        b->neg = false;
    }
}

static void big_zero(Big *b) {
    memset(b, 0, sizeof(*b));
    b->n = 1;
}

static bool big_is_zero(const Big *b) {
    uint16_t i;
    for (i = 0; i < b->n; i++) {
        if (b->limb[i] != 0) {
            return false;
        }
    }
    return true;
}

static void big_set_u64(Big *b, uint64_t v) {
    big_zero(b);
    b->limb[0] = (uint32_t)v;
    b->limb[1] = (uint32_t)(v >> 32);
    b->n = b->limb[1] != 0 ? 2 : 1;
}

static uint32_t big_bit_len(const Big *b) {
    uint32_t top;
    uint32_t bits;
    if (big_is_zero(b)) {
        return 0;
    }
    top = b->limb[b->n - 1];
    bits = 0;
    while (top != 0) {
        bits++;
        top >>= 1;
    }
    return ((uint32_t)b->n - 1u) * (uint32_t)CONST_LIMB_BITS + bits;
}

static int big_cmp_mag(const Big *a, const Big *b) {
    uint16_t i;
    if (a->n != b->n) {
        return a->n < b->n ? -1 : 1;
    }
    for (i = a->n; i-- > 0;) {
        if (a->limb[i] != b->limb[i]) {
            return a->limb[i] < b->limb[i] ? -1 : 1;
        }
    }
    return 0;
}

static int big_cmp(const Big *a, const Big *b) {
    int mag;
    if (big_is_zero(a) && big_is_zero(b)) {
        return 0;
    }
    if (a->neg != b->neg) {
        return a->neg ? -1 : 1;
    }
    mag = big_cmp_mag(a, b);
    return a->neg ? -mag : mag;
}

static bool big_is_pow2(const Big *b) {
    bool seen = false;
    uint16_t i;
    if (big_is_zero(b)) {
        return false;
    }
    for (i = 0; i < b->n; i++) {
        uint32_t x = b->limb[i];
        if (x == 0) {
            continue;
        }
        if (seen || (x & (x - 1u)) != 0) {
            return false;
        }
        seen = true;
    }
    return seen;
}

/* bits is 8, 16, 32, or 64. */
static bool big_fits(const Big *b, unsigned bits, bool is_signed) {
    uint32_t len;
    if (bits == 0 || bits > 64) {
        return false;
    }
    len = big_bit_len(b);
    if (!is_signed) {
        return !b->neg && len <= bits;
    }
    if (!b->neg) {
        return len < bits;
    }
    if (len < bits) {
        return true;
    }
    return len == bits && big_is_pow2(b);
}

static uint64_t big_to_bits(const Big *b) {
    uint64_t mag = b->limb[0];
    if (b->n > 1) {
        mag |= (uint64_t)b->limb[1] << 32;
    }
    if (!b->neg) {
        return mag;
    }
    return ~mag + 1ull;
}

static bool big_add_mag(Big *dst, const Big *a, const Big *b) {
    Big tmp;
    uint16_t n = a->n > b->n ? a->n : b->n;
    uint16_t i;
    uint64_t carry = 0;
    big_zero(&tmp);
    for (i = 0; i < n; i++) {
        uint64_t av = i < a->n ? a->limb[i] : 0;
        uint64_t bv = i < b->n ? b->limb[i] : 0;
        uint64_t sum = av + bv + carry;
        tmp.limb[i] = (uint32_t)sum;
        carry = sum >> 32;
    }
    if (carry != 0) {
        if (n >= CONST_MAX_LIMBS) {
            return false;
        }
        tmp.limb[n++] = (uint32_t)carry;
    }
    tmp.n = n == 0 ? 1 : n;
    big_trim(&tmp);
    *dst = tmp;
    return true;
}

static bool big_sub_mag(Big *dst, const Big *a, const Big *b) {
    Big tmp;
    uint16_t i;
    int borrow = 0;
    big_zero(&tmp);
    for (i = 0; i < a->n; i++) {
        int64_t av = (int64_t)a->limb[i];
        int64_t bv = i < b->n ? (int64_t)b->limb[i] : 0;
        int64_t diff = av - bv - borrow;
        if (diff < 0) {
            diff += (int64_t)1 << 32;
            borrow = 1;
        } else {
            borrow = 0;
        }
        tmp.limb[i] = (uint32_t)diff;
    }
    if (borrow != 0) {
        return false;
    }
    tmp.n = a->n == 0 ? 1 : a->n;
    big_trim(&tmp);
    *dst = tmp;
    return true;
}

static bool big_add(Big *dst, const Big *a, const Big *b) {
    int cmp;
    if (a->neg == b->neg) {
        if (!big_add_mag(dst, a, b)) {
            return false;
        }
        dst->neg = a->neg && !big_is_zero(dst);
        return true;
    }
    cmp = big_cmp_mag(a, b);
    if (cmp == 0) {
        big_zero(dst);
        return true;
    }
    if (cmp > 0) {
        if (!big_sub_mag(dst, a, b)) {
            return false;
        }
        dst->neg = a->neg && !big_is_zero(dst);
    } else {
        if (!big_sub_mag(dst, b, a)) {
            return false;
        }
        dst->neg = b->neg && !big_is_zero(dst);
    }
    return true;
}

static bool big_sub(Big *dst, const Big *a, const Big *b) {
    Big nb = *b;
    if (!big_is_zero(&nb)) {
        nb.neg = !nb.neg;
    }
    return big_add(dst, a, &nb);
}

static bool big_mul(Big *dst, const Big *a, const Big *b) {
    uint32_t prod[CONST_MAX_LIMBS];
    uint16_t i;
    bool neg;
    if (big_is_zero(a) || big_is_zero(b)) {
        big_zero(dst);
        return true;
    }
    if (big_bit_len(a) + big_bit_len(b) - 1u > (uint32_t)CONST_MAX_BITS) {
        return false;
    }
    neg = a->neg != b->neg;
    memset(prod, 0, sizeof(prod));
    for (i = 0; i < a->n; i++) {
        uint64_t carry = 0;
        uint16_t j;
        for (j = 0; j < b->n; j++) {
            size_t k = (size_t)i + (size_t)j;
            uint64_t cur;
            if (k >= CONST_MAX_LIMBS) {
                return false;
            }
            cur = (uint64_t)prod[k] + (uint64_t)a->limb[i] * (uint64_t)b->limb[j] + carry;
            prod[k] = (uint32_t)cur;
            carry = cur >> 32;
        }
        if (carry != 0) {
            size_t k = (size_t)i + (size_t)b->n;
            while (carry != 0) {
                uint64_t cur;
                if (k >= CONST_MAX_LIMBS) {
                    return false;
                }
                cur = (uint64_t)prod[k] + carry;
                prod[k] = (uint32_t)cur;
                carry = cur >> 32;
                k++;
            }
        }
    }
    big_zero(dst);
    memcpy(dst->limb, prod, sizeof(prod));
    dst->n = CONST_MAX_LIMBS;
    dst->neg = neg;
    big_trim(dst);
    return true;
}

static int mag_bit(const Big *b, uint32_t bit) {
    uint32_t li = bit / (uint32_t)CONST_LIMB_BITS;
    if (li >= b->n) {
        return 0;
    }
    return (int)((b->limb[li] >> (bit % (uint32_t)CONST_LIMB_BITS)) & 1u);
}

static bool mag_shl1(Big *b) {
    uint32_t carry = 0;
    uint16_t i;
    for (i = 0; i < b->n; i++) {
        uint32_t next = b->limb[i] >> 31;
        b->limb[i] = (b->limb[i] << 1) | carry;
        carry = next;
    }
    if (carry != 0) {
        if (b->n >= CONST_MAX_LIMBS) {
            return false;
        }
        b->limb[b->n++] = carry;
    }
    return true;
}

static bool mag_set_bit(Big *b, uint32_t bit) {
    uint32_t li = bit / (uint32_t)CONST_LIMB_BITS;
    if (li >= CONST_MAX_LIMBS) {
        return false;
    }
    while (b->n <= li) {
        b->limb[b->n++] = 0;
    }
    b->limb[li] |= 1u << (bit % (uint32_t)CONST_LIMB_BITS);
    return true;
}

/* Magnitude division. quot and rem are non-negative. b is not zero. */
static bool big_div_mag(Big *quot, Big *rem, const Big *a, const Big *b) {
    uint32_t nbits = big_bit_len(a);
    uint32_t i;
    big_zero(quot);
    big_zero(rem);
    for (i = nbits; i-- > 0;) {
        if (!mag_shl1(rem)) {
            return false;
        }
        if (mag_bit(a, i) && !big_add_mag(rem, rem, &(Big){.limb = {1}, .n = 1, .neg = false})) {
            return false;
        }
        if (big_cmp_mag(rem, b) >= 0) {
            if (!big_sub_mag(rem, rem, b) || !mag_set_bit(quot, i)) {
                return false;
            }
        }
    }
    return true;
}

static bool big_div_mod(Big *quot, Big *rem, const Big *a, const Big *b) {
    Big am = *a;
    Big bm = *b;
    bool aneg;
    bool bneg;
    am.neg = false;
    bm.neg = false;
    if (!big_div_mag(quot, rem, &am, &bm)) {
        return false;
    }
    aneg = a->neg && !big_is_zero(a);
    bneg = b->neg && !big_is_zero(b);
    quot->neg = (aneg != bneg) && !big_is_zero(quot);
    rem->neg = aneg && !big_is_zero(rem);
    return true;
}

static bool big_shl(Big *dst, const Big *src, uint32_t bits) {
    Big tmp;
    uint32_t limb_shift;
    uint32_t bit_shift;
    uint16_t i;
    if (big_is_zero(src) || bits == 0) {
        *dst = *src;
        dst->neg = src->neg && !big_is_zero(src);
        return true;
    }
    if (bits >= (uint32_t)CONST_MAX_BITS || big_bit_len(src) > (uint32_t)CONST_MAX_BITS - bits) {
        return false;
    }
    limb_shift = bits / (uint32_t)CONST_LIMB_BITS;
    bit_shift = bits % (uint32_t)CONST_LIMB_BITS;
    big_zero(&tmp);
    for (i = 0; i < src->n; i++) {
        uint64_t v = (uint64_t)src->limb[i] << bit_shift;
        size_t idx = (size_t)i + (size_t)limb_shift;
        if (idx >= CONST_MAX_LIMBS) {
            return false;
        }
        tmp.limb[idx] |= (uint32_t)v;
        if (bit_shift != 0 && (v >> 32) != 0) {
            if (idx + 1 >= CONST_MAX_LIMBS) {
                return false;
            }
            tmp.limb[idx + 1] |= (uint32_t)(v >> 32);
        }
    }
    tmp.n = (uint16_t)(src->n + limb_shift + (bit_shift != 0 ? 1u : 0u));
    if (tmp.n > CONST_MAX_LIMBS) {
        tmp.n = CONST_MAX_LIMBS;
    }
    tmp.neg = src->neg;
    big_trim(&tmp);
    *dst = tmp;
    return true;
}

static void mag_shr(Big *dst, const Big *src, uint32_t bits) {
    uint32_t limb_shift = bits / (uint32_t)CONST_LIMB_BITS;
    uint32_t bit_shift = bits % (uint32_t)CONST_LIMB_BITS;
    uint16_t n = 0;
    uint16_t i;
    big_zero(dst);
    if (limb_shift >= src->n) {
        return;
    }
    for (i = (uint16_t)limb_shift; i < src->n; i++) {
        uint32_t v = src->limb[i] >> bit_shift;
        if (bit_shift != 0 && (uint16_t)(i + 1u) < src->n) {
            v |= src->limb[i + 1u] << (32u - bit_shift);
        }
        dst->limb[n++] = v;
    }
    dst->n = n == 0 ? 1 : n;
    big_trim(dst);
}

static bool low_bits_set(const Big *src, uint32_t bits) {
    uint32_t lim = bits;
    uint32_t have = big_bit_len(src);
    uint32_t i;
    if (lim > have) {
        lim = have;
    }
    for (i = 0; i < lim; i++) {
        if (mag_bit(src, i)) {
            return true;
        }
    }
    return false;
}

/* Arithmetic right shift: floor division by 2^bits. */
static bool big_shr(Big *dst, const Big *src, uint32_t bits) {
    Big mag;
    Big shifted;
    Big one;
    if (bits == 0) {
        *dst = *src;
        return true;
    }
    if (big_is_zero(src)) {
        big_zero(dst);
        return true;
    }
    if (!src->neg) {
        if (bits >= big_bit_len(src)) {
            big_zero(dst);
            return true;
        }
        mag_shr(dst, src, bits);
        return true;
    }
    mag = *src;
    mag.neg = false;
    if (bits >= big_bit_len(&mag)) {
        big_set_u64(dst, 1);
        dst->neg = true;
        return true;
    }
    mag_shr(&shifted, &mag, bits);
    if (low_bits_set(&mag, bits)) {
        big_set_u64(&one, 1);
        if (!big_add(dst, &shifted, &one)) {
            return false;
        }
    } else {
        *dst = shifted;
    }
    dst->neg = !big_is_zero(dst);
    return true;
}

static bool big_not(Big *dst, const Big *a) {
    Big neg = *a;
    Big one;
    if (!big_is_zero(&neg)) {
        neg.neg = !neg.neg;
    }
    big_set_u64(&one, 1);
    return big_sub(dst, &neg, &one);
}

static bool to_tc(const Big *v, uint32_t *limbs, size_t nlimbs) {
    size_t i;
    uint64_t carry;
    memset(limbs, 0, nlimbs * sizeof(uint32_t));
    if (v->n > nlimbs) {
        return false;
    }
    for (i = 0; i < v->n; i++) {
        limbs[i] = v->limb[i];
    }
    if (!v->neg) {
        return true;
    }
    carry = 1;
    for (i = 0; i < nlimbs; i++) {
        uint64_t inv = (uint64_t)(~limbs[i]) + carry;
        limbs[i] = (uint32_t)inv;
        carry = inv >> 32;
    }
    return true;
}

static bool from_tc(Big *dst, const uint32_t *limbs, size_t nlimbs) {
    uint32_t tmp[CONST_MAX_LIMBS];
    bool neg;
    size_t i;
    uint64_t carry;
    if (nlimbs == 0 || nlimbs > CONST_MAX_LIMBS) {
        return false;
    }
    neg = (limbs[nlimbs - 1] & 0x80000000u) != 0;
    memcpy(tmp, limbs, nlimbs * sizeof(uint32_t));
    if (neg) {
        carry = 1;
        for (i = 0; i < nlimbs; i++) {
            uint64_t inv = (uint64_t)(~tmp[i]) + carry;
            tmp[i] = (uint32_t)inv;
            carry = inv >> 32;
        }
    }
    big_zero(dst);
    memcpy(dst->limb, tmp, nlimbs * sizeof(uint32_t));
    dst->n = (uint16_t)nlimbs;
    dst->neg = neg;
    big_trim(dst);
    return true;
}

static bool big_bitop(Big *dst, const Big *a, const Big *b, int op) {
    uint32_t la = big_bit_len(a);
    uint32_t lb = big_bit_len(b);
    uint32_t width = la > lb ? la : lb;
    size_t nlimbs;
    uint32_t aa[CONST_MAX_LIMBS];
    uint32_t bb[CONST_MAX_LIMBS];
    uint32_t rr[CONST_MAX_LIMBS];
    size_t i;
    width += 1u;
    nlimbs = ((size_t)width + (CONST_LIMB_BITS - 1)) / CONST_LIMB_BITS;
    if (nlimbs == 0) {
        nlimbs = 1;
    }
    if (nlimbs > CONST_MAX_LIMBS) {
        return false;
    }
    if (!to_tc(a, aa, nlimbs) || !to_tc(b, bb, nlimbs)) {
        return false;
    }
    for (i = 0; i < nlimbs; i++) {
        if (op == 0) {
            rr[i] = aa[i] & bb[i];
        } else if (op == 1) {
            rr[i] = aa[i] | bb[i];
        } else {
            rr[i] = aa[i] ^ bb[i];
        }
    }
    return from_tc(dst, rr, nlimbs);
}

static bool big_mul_add_small(Big *b, uint32_t mul, uint32_t add) {
    uint64_t carry = add;
    uint16_t i;
    for (i = 0; i < b->n; i++) {
        uint64_t cur = (uint64_t)b->limb[i] * (uint64_t)mul + carry;
        b->limb[i] = (uint32_t)cur;
        carry = cur >> 32;
    }
    while (carry != 0) {
        if (b->n >= CONST_MAX_LIMBS) {
            return false;
        }
        b->limb[b->n++] = (uint32_t)carry;
        carry >>= 32;
    }
    big_trim(b);
    return true;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int parse_literal(const char *text, size_t len, Big *out) {
    size_t i = 0;
    int base = 10;
    bool any = false;
    if (!text || len == 0 || text[0] < '0' || text[0] > '9') {
        return PARSE_INVALID;
    }
    big_zero(out);
    if (len >= 2 && text[0] == '0') {
        char mark = text[1];
        if (mark == 'x' || mark == 'X') {
            base = 16;
            i = 2;
        } else if (mark == 'b' || mark == 'B') {
            base = 2;
            i = 2;
        } else if (mark == 'o' || mark == 'O') {
            base = 8;
            i = 2;
        }
    }
    for (; i < len; i++) {
        char c = text[i];
        int digit;
        if (c == '_') {
            continue;
        }
        digit = hex_val(c);
        if (digit < 0 || digit >= base) {
            return PARSE_INVALID;
        }
        any = true;
        if (!big_mul_add_small(out, (uint32_t)base, (uint32_t)digit)) {
            return PARSE_BIG;
        }
    }
    return any ? PARSE_OK : PARSE_INVALID;
}

static bool is_hex_literal(const char *text, size_t len) {
    return text && len >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
}

static CtPos at_node(const JogNode *node) {
    CtPos pos;
    pos.file = NULL;
    pos.line = 0;
    pos.column = 0;
    if (node) {
        pos.file = node->pos.file;
        pos.line = node->pos.line;
        pos.column = node->pos.column;
    }
    return pos;
}

static bool node_prefers_bool(const JogNode *node) {
    JogTokenKind op;
    if (!node) {
        return false;
    }
    if (node->kind == JOG_NODE_BOOL_LIT) {
        return true;
    }
    if (node->kind == JOG_NODE_UNARY && node->as.unary.op == JOG_TOK_BANG) {
        return true;
    }
    if (node->kind != JOG_NODE_BINARY) {
        return false;
    }
    op = node->as.binary.op;
    return op == JOG_TOK_AMPAMP || op == JOG_TOK_PIPEPIPE || op == JOG_TOK_EQ || op == JOG_TOK_NEQ || op == JOG_TOK_LT ||
           op == JOG_TOK_LE || op == JOG_TOK_GT || op == JOG_TOK_GE;
}

static JogType *preset_int(const JogNode *node) {
    if (node && node->type && node->type->kind == JOG_TY_INT) {
        return node->type;
    }
    return NULL;
}

static bool concrete_int(const JogType *type) {
    return type && type->kind == JOG_TY_INT;
}

static void fail_fit_literal(CtDiag *diag, const JogNode *node) {
    ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "integer constant does not fit");
}

static void fail_overflow(CtDiag *diag, const JogNode *node) {
    ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "integer overflow");
}

static bool apply_result_type(CtDiag *diag, JogTypes *types, JogNode *node, JogType *want, JogType *forced, Big *value,
                              bool literal, JogType **produced) {
    JogType *ty = forced ? forced : want;
    if (concrete_int(ty) && !big_fits(value, ty->as.integer.bits, ty->as.integer.is_signed)) {
        if (literal) {
            fail_fit_literal(diag, node);
        } else {
            fail_overflow(diag, node);
        }
        return false;
    }
    if (want && ty && want != ty) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "integer types do not match");
        return false;
    }
    if (!ty) {
        ty = jog_type_builtin(types, JOG_BUILTIN_UNTYPED_INT);
        if (!ty) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "out of memory");
            return false;
        }
    }
    node->type = ty;
    *produced = ty;
    return true;
}

static bool fold_int(CtDiag *diag, JogTypes *types, JogNode *node, JogType *want, Big *out, JogType **produced);

static bool fold_int_child(CtDiag *diag, JogTypes *types, JogNode *parent, JogTokenKind op, JogNode *child, Big *out,
                           JogType **produced) {
    if (!child || node_prefers_bool(child)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(parent), "operator '%s' expects an integer", jog_token_spelling(op));
        return false;
    }
    return fold_int(diag, types, child, preset_int(child), out, produced);
}

static bool shift_amount(CtDiag *diag, const JogNode *node, const Big *count, uint32_t *bits, bool *huge) {
    if (count->neg && !big_is_zero(count)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "shift count is negative");
        return false;
    }
    if (big_bit_len(count) > 32) {
        *huge = true;
        *bits = 0;
        return true;
    }
    *huge = false;
    *bits = count->limb[0];
    return true;
}

/* 1 when node is size(T) or align(T) and *out is that layout number. 0 when
   it is not that call. -1 after a diagnostic. The argument type is filled
   by the checker. use_align reads JogType.align; otherwise JogType.size.
   noun is "size" or "alignment". */
static int fold_layout_call(CtDiag *diag, JogTypes *types, JogNode *node, JogType *want, Big *out, JogType **produced,
                            const char *builtin, const char *noun, int use_align) {
    const JogNode *callee;
    const JogNode *arg;
    const JogNode *value;
    JogType *ty;
    JogType *usize;
    const char *param_name;
    if (!node || node->kind != JOG_NODE_CALL || node->as.call.args.count != 1 || node->as.call.trailing ||
        node->as.call.contexts.count != 0 || !builtin || !noun) {
        return 0;
    }
    callee = node->as.call.callee;
    if (!callee || callee->kind != JOG_NODE_IDENT || callee->symbol || callee->as.ident.type_args.count != 0 ||
        !callee->as.ident.name || strcmp(callee->as.ident.name, builtin) != 0) {
        return 0;
    }
    arg = node->as.call.args.items ? node->as.call.args.items[0] : NULL;
    value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : NULL;
    ty = value ? value->type : NULL;
    if (!ty || !ty->has_layout) {
        if (ty && ty->kind == JOG_TY_TYPE_PARAM) {
            param_name = ty->as.param.name ? ty->as.param.name : "T";
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "%s of '%s' is not known yet", noun, param_name);
        } else if (ty) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "%s is not known yet", noun);
        } else {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "not a constant expression");
        }
        return -1;
    }
    usize = jog_type_builtin(types, JOG_BUILTIN_USIZE);
    if (!usize) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "out of memory");
        return -1;
    }
    big_set_u64(out, use_align ? (uint64_t)ty->align : ty->size);
    if (!apply_result_type(diag, types, node, want, usize, out, false, produced)) {
        return -1;
    }
    return 1;
}

static int fold_size_call(CtDiag *diag, JogTypes *types, JogNode *node, JogType *want, Big *out, JogType **produced) {
    return fold_layout_call(diag, types, node, want, out, produced, "size", "size", 0);
}

static int fold_align_call(CtDiag *diag, JogTypes *types, JogNode *node, JogType *want, Big *out, JogType **produced) {
    return fold_layout_call(diag, types, node, want, out, produced, "align", "alignment", 1);
}

static bool fold_int(CtDiag *diag, JogTypes *types, JogNode *node, JogType *want, Big *out, JogType **produced) {
    int parsed;
    if (!node || node_prefers_bool(node)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "not a constant expression");
        return false;
    }
    if (node->kind == JOG_NODE_CALL) {
        int sized = fold_size_call(diag, types, node, want, out, produced);
        if (sized == 0) {
            sized = fold_align_call(diag, types, node, want, out, produced);
        }
        if (sized == 0) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "not a constant expression");
            return false;
        }
        return sized > 0;
    }
    if (node->kind == JOG_NODE_INT_LIT) {
        parsed = parse_literal(node->as.literal.text, node->as.literal.len, out);
        if (parsed == PARSE_INVALID) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "invalid integer literal");
            return false;
        }
        if (parsed == PARSE_BIG) {
            fail_fit_literal(diag, node);
            return false;
        }
        return apply_result_type(diag, types, node, want, NULL, out, true, produced);
    }
    if (node->kind == JOG_NODE_UNARY) {
        Big operand;
        JogType *ot = NULL;
        JogTokenKind op = node->as.unary.op;
        JogType *forced = NULL;
        if (op != JOG_TOK_MINUS && op != JOG_TOK_TILDE) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "not a constant expression");
            return false;
        }
        if (!fold_int_child(diag, types, node, op, node->as.unary.operand, &operand, &ot)) {
            return false;
        }
        if (concrete_int(ot)) {
            forced = ot;
        }
        if (op == JOG_TOK_MINUS) {
            if (!big_is_zero(&operand)) {
                operand.neg = !operand.neg;
            }
            *out = operand;
        } else if (!big_not(out, &operand)) {
            fail_overflow(diag, node);
            return false;
        }
        return apply_result_type(diag, types, node, want, forced, out, false, produced);
    }
    if (node->kind == JOG_NODE_BINARY) {
        Big lv;
        Big rv;
        JogType *lt = NULL;
        JogType *rt = NULL;
        JogType *forced = NULL;
        JogTokenKind op = node->as.binary.op;
        bool logical = op == JOG_TOK_PLUS || op == JOG_TOK_MINUS || op == JOG_TOK_STAR || op == JOG_TOK_SLASH ||
                       op == JOG_TOK_PERCENT || op == JOG_TOK_AMP || op == JOG_TOK_PIPE || op == JOG_TOK_CARET ||
                       op == JOG_TOK_SHL || op == JOG_TOK_SHR;
        if (!logical) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "not a constant expression");
            return false;
        }
        if (!fold_int_child(diag, types, node, op, node->as.binary.lhs, &lv, &lt) ||
            !fold_int_child(diag, types, node, op, node->as.binary.rhs, &rv, &rt)) {
            return false;
        }
        if (concrete_int(lt) && concrete_int(rt) && lt != rt) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "integer types do not match");
            return false;
        }
        if (concrete_int(lt)) {
            forced = lt;
        } else if (concrete_int(rt)) {
            forced = rt;
        }
        if (op == JOG_TOK_PLUS) {
            if (!big_add(out, &lv, &rv)) {
                fail_overflow(diag, node);
                return false;
            }
        } else if (op == JOG_TOK_MINUS) {
            if (!big_sub(out, &lv, &rv)) {
                fail_overflow(diag, node);
                return false;
            }
        } else if (op == JOG_TOK_STAR) {
            if (!big_mul(out, &lv, &rv)) {
                fail_overflow(diag, node);
                return false;
            }
        } else if (op == JOG_TOK_SLASH || op == JOG_TOK_PERCENT) {
            Big quot;
            Big rem;
            if (big_is_zero(&rv)) {
                ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "division by zero");
                return false;
            }
            if (!big_div_mod(&quot, &rem, &lv, &rv)) {
                fail_overflow(diag, node);
                return false;
            }
            *out = op == JOG_TOK_SLASH ? quot : rem;
        } else if (op == JOG_TOK_AMP || op == JOG_TOK_PIPE || op == JOG_TOK_CARET) {
            int which = op == JOG_TOK_AMP ? 0 : op == JOG_TOK_PIPE ? 1 : 2;
            if (!big_bitop(out, &lv, &rv, which)) {
                fail_overflow(diag, node);
                return false;
            }
        } else {
            uint32_t bits = 0;
            bool huge = false;
            bool ok;
            if (!shift_amount(diag, node, &rv, &bits, &huge)) {
                return false;
            }
            if (op == JOG_TOK_SHL) {
                ok = huge ? big_is_zero(&lv) : big_shl(out, &lv, bits);
                if (huge && big_is_zero(&lv)) {
                    big_zero(out);
                    ok = true;
                }
            } else if (huge) {
                if (lv.neg && !big_is_zero(&lv)) {
                    big_set_u64(out, 1);
                    out->neg = true;
                } else {
                    big_zero(out);
                }
                ok = true;
            } else {
                ok = big_shr(out, &lv, bits);
            }
            if (!ok) {
                fail_overflow(diag, node);
                return false;
            }
        }
        return apply_result_type(diag, types, node, want, forced, out, false, produced);
    }
    ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "not a constant expression");
    return false;
}

static bool fold_bool(CtDiag *diag, JogTypes *types, JogNode *node, bool *out);

static bool fold_bool_child(CtDiag *diag, JogTypes *types, JogNode *parent, JogTokenKind op, JogNode *child, bool *out) {
    if (!child || !node_prefers_bool(child)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(parent), "operator '%s' expects a bool", jog_token_spelling(op));
        return false;
    }
    return fold_bool(diag, types, child, out);
}

static bool fold_compare(CtDiag *diag, JogTypes *types, JogNode *node, bool *out) {
    JogTokenKind op = node->as.binary.op;
    bool relational = op == JOG_TOK_LT || op == JOG_TOK_LE || op == JOG_TOK_GT || op == JOG_TOK_GE;
    if (!relational && (node_prefers_bool(node->as.binary.lhs) || node_prefers_bool(node->as.binary.rhs))) {
        bool lb = false;
        bool rb = false;
        if (!fold_bool_child(diag, types, node, op, node->as.binary.lhs, &lb) ||
            !fold_bool_child(diag, types, node, op, node->as.binary.rhs, &rb)) {
            return false;
        }
        *out = op == JOG_TOK_EQ ? lb == rb : lb != rb;
        return true;
    }
    {
        Big lv;
        Big rv;
        JogType *lt = NULL;
        JogType *rt = NULL;
        int cmp;
        if (node_prefers_bool(node->as.binary.lhs) || node_prefers_bool(node->as.binary.rhs)) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "operator '%s' expects an integer", jog_token_spelling(op));
            return false;
        }
        if (!fold_int(diag, types, node->as.binary.lhs, preset_int(node->as.binary.lhs), &lv, &lt) ||
            !fold_int(diag, types, node->as.binary.rhs, preset_int(node->as.binary.rhs), &rv, &rt)) {
            return false;
        }
        if (concrete_int(lt) && concrete_int(rt) && lt != rt) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "integer types do not match");
            return false;
        }
        cmp = big_cmp(&lv, &rv);
        switch (op) {
        case JOG_TOK_EQ:
            *out = cmp == 0;
            break;
        case JOG_TOK_NEQ:
            *out = cmp != 0;
            break;
        case JOG_TOK_LT:
            *out = cmp < 0;
            break;
        case JOG_TOK_LE:
            *out = cmp <= 0;
            break;
        case JOG_TOK_GT:
            *out = cmp > 0;
            break;
        case JOG_TOK_GE:
            *out = cmp >= 0;
            break;
        default:
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "not a constant expression");
            return false;
        }
    }
    return true;
}

static bool fold_bool(CtDiag *diag, JogTypes *types, JogNode *node, bool *out) {
    JogType *boolean;
    if (!node) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "not a constant expression");
        return false;
    }
    boolean = jog_type_builtin(types, JOG_BUILTIN_BOOL);
    if (!boolean) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "out of memory");
        return false;
    }
    if (node->kind == JOG_NODE_BOOL_LIT) {
        *out = node->as.literal.value;
        node->type = boolean;
        return true;
    }
    if (node->kind == JOG_NODE_UNARY && node->as.unary.op == JOG_TOK_BANG) {
        bool inner = false;
        if (!fold_bool_child(diag, types, node, JOG_TOK_BANG, node->as.unary.operand, &inner)) {
            return false;
        }
        *out = !inner;
        node->type = boolean;
        return true;
    }
    if (node->kind == JOG_NODE_BINARY && (node->as.binary.op == JOG_TOK_AMPAMP || node->as.binary.op == JOG_TOK_PIPEPIPE)) {
        bool left = false;
        bool right = false;
        JogTokenKind op = node->as.binary.op;
        if (!fold_bool_child(diag, types, node, op, node->as.binary.lhs, &left)) {
            return false;
        }
        if ((op == JOG_TOK_AMPAMP && !left) || (op == JOG_TOK_PIPEPIPE && left)) {
            *out = left;
            node->type = boolean;
            return true;
        }
        if (!fold_bool_child(diag, types, node, op, node->as.binary.rhs, &right)) {
            return false;
        }
        *out = op == JOG_TOK_AMPAMP ? (left && right) : (left || right);
        node->type = boolean;
        return true;
    }
    if (node->kind == JOG_NODE_BINARY && node_prefers_bool(node)) {
        if (!fold_compare(diag, types, node, out)) {
            return false;
        }
        node->type = boolean;
        return true;
    }
    ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "not a constant expression");
    return false;
}

static bool fold_bytes(CtArena *arena, CtDiag *diag, JogNode *node, JogType *target, JogConst *out) {
    const char *text;
    size_t len;
    size_t i;
    uint64_t n;
    uint64_t digits = 0;
    uint64_t need;
    uint8_t *buf;
    size_t filled = 0;
    int hi = -1;
    if (!node || node->kind != JOG_NODE_INT_LIT) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "constant does not fit");
        return false;
    }
    text = node->as.literal.text;
    len = node->as.literal.len;
    if (!is_hex_literal(text, len)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "hex literal required for bytes[N]");
        return false;
    }
    for (i = 2; i < len; i++) {
        if (text[i] == '_') {
            continue;
        }
        if (hex_val(text[i]) < 0) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "invalid integer literal");
            return false;
        }
        digits++;
    }
    if (digits == 0) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "invalid integer literal");
        return false;
    }
    n = target->as.bytes_n.n;
    if (n > UINT64_MAX / 2u) {
        fail_fit_literal(diag, node);
        return false;
    }
    need = n * 2u;
    if (digits != need) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node),
                     "hex literal has %" PRIu64 " digits, bytes[%" PRIu64 "] needs %" PRIu64, digits, n, need);
        return false;
    }
    if (n > SIZE_MAX) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "out of memory");
        return false;
    }
    buf = ct_arena_alloc(arena, (size_t)n == 0 ? 1 : (size_t)n, 1);
    if (!buf) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "out of memory");
        return false;
    }
    for (i = 2; i < len; i++) {
        int digit;
        if (text[i] == '_') {
            continue;
        }
        digit = hex_val(text[i]);
        if (hi < 0) {
            hi = digit;
        } else {
            buf[filled++] = (uint8_t)((hi << 4) | digit);
            hi = -1;
        }
    }
    node->type = target;
    out->kind = JOG_CONST_BYTES;
    out->type = target;
    out->bytes = buf;
    out->len = (size_t)n;
    return true;
}

static bool finish_int(CtDiag *diag, JogTypes *types, JogNode *node, JogType *target, JogConst *out) {
    Big value;
    JogType *produced = NULL;
    JogType *want = NULL;
    if (target && target->kind == JOG_TY_INT) {
        want = target;
    }
    if (!fold_int(diag, types, node, want, &value, &produced)) {
        return false;
    }
    if (produced && produced->kind == JOG_TY_UNTYPED_INT) {
        JogType *i64 = jog_type_builtin(types, JOG_BUILTIN_I64);
        if (!i64) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "out of memory");
            return false;
        }
        if (!big_fits(&value, 64, true)) {
            node->type = NULL;
            if (node->kind == JOG_NODE_INT_LIT) {
                fail_fit_literal(diag, node);
            } else {
                fail_overflow(diag, node);
            }
            return false;
        }
        produced = i64;
        node->type = i64;
    }
    out->kind = JOG_CONST_INT;
    out->type = produced;
    out->bits = big_to_bits(&value);
    return true;
}

bool jog_const_eval(CtArena *arena, CtDiag *diag, JogTypes *types, JogNode *node, JogType *target, JogConst *out) {
    JogType *natural = target;
    if (!arena || !diag || !types || !node || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (natural && natural->kind == JOG_TY_UNTYPED_INT) {
        natural = NULL;
    }
    if (natural && natural->kind == JOG_TY_BYTES_N) {
        return fold_bytes(arena, diag, node, natural, out);
    }
    if (node_prefers_bool(node)) {
        bool bit = false;
        JogType *boolean;
        if (natural && natural->kind != JOG_TY_BOOL) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "constant does not fit");
            return false;
        }
        if (!fold_bool(diag, types, node, &bit)) {
            return false;
        }
        boolean = jog_type_builtin(types, JOG_BUILTIN_BOOL);
        out->kind = JOG_CONST_BOOL;
        out->type = boolean;
        out->bits = bit ? 1u : 0u;
        return true;
    }
    if (natural && natural->kind != JOG_TY_INT) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, at_node(node), "constant does not fit");
        return false;
    }
    return finish_int(diag, types, node, natural, out);
}
