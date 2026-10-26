#include "float_lit.h"

#include <string.h>

/* Float literal trees. The value is computed in binary64, which is the
   default type of an untyped float, then rounded to f32 when that type wins.
   Remainder matches integer division: the quotient truncates toward zero
   and the remainder takes the sign of the dividend. */

static int finite_d(double v) {
    return v == v && (v == 0.0 || v + v != v);
}

static int is_float_arith(JogTokenKind op) {
    return op == JOG_TOK_PLUS || op == JOG_TOK_MINUS || op == JOG_TOK_STAR || op == JOG_TOK_SLASH ||
           op == JOG_TOK_PERCENT;
}

int jog_float_lit_tree(const JogNode *node) {
    if (!node) {
        return 0;
    }
    if (node->kind == JOG_NODE_FLOAT_LIT) {
        return 1;
    }
    if (node->kind == JOG_NODE_UNARY) {
        return node->as.unary.op == JOG_TOK_MINUS && jog_float_lit_tree(node->as.unary.operand);
    }
    if (node->kind != JOG_NODE_BINARY || !is_float_arith(node->as.binary.op)) {
        return 0;
    }
    return jog_float_lit_tree(node->as.binary.lhs) && jog_float_lit_tree(node->as.binary.rhs);
}

int jog_char_codepoint(const char *text, size_t len, uint32_t *out) {
    const unsigned char *p;
    uint32_t cp;
    size_t need;
    size_t i;
    if (!text || !out || len == 0 || len > 4) {
        return 0;
    }
    p = (const unsigned char *)text;
    if (p[0] < 0x80) {
        if (len != 1) {
            return 0;
        }
        *out = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0) {
        need = 2;
        cp = (uint32_t)(p[0] & 0x1F);
    } else if ((p[0] & 0xF0) == 0xE0) {
        need = 3;
        cp = (uint32_t)(p[0] & 0x0F);
    } else if ((p[0] & 0xF8) == 0xF0) {
        need = 4;
        cp = (uint32_t)(p[0] & 0x07);
    } else {
        return 0;
    }
    if (len != need) {
        return 0;
    }
    for (i = 1; i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3F);
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    if (need == 2 && cp < 0x80) {
        return 0;
    }
    if (need == 3 && cp < 0x800) {
        return 0;
    }
    if (need == 4 && cp < 0x10000) {
        return 0;
    }
    *out = cp;
    return 1;
}

static JogFloatLitStatus parse_decimal(const char *text, size_t len, double *out) {
    char buf[512];
    size_t n = 0;
    size_t i;
    double v = 0.0;
    int exp = 0;
    int saw = 0;
    int esign;
    int e;
    const char *p;
    if (!text) {
        return JOG_FLOAT_LIT_BAD;
    }
    for (i = 0; i < len; i++) {
        if (text[i] == '_') {
            continue;
        }
        if (n + 1 >= sizeof(buf)) {
            return JOG_FLOAT_LIT_BAD;
        }
        buf[n++] = text[i];
    }
    if (n == 0) {
        return JOG_FLOAT_LIT_BAD;
    }
    buf[n] = '\0';
    p = buf;
    while (*p >= '0' && *p <= '9') {
        v = v * 10.0 + (double)(*p - '0');
        if (!finite_d(v)) {
            return JOG_FLOAT_LIT_FIT;
        }
        p++;
        saw = 1;
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            v = v * 10.0 + (double)(*p - '0');
            if (!finite_d(v)) {
                return JOG_FLOAT_LIT_FIT;
            }
            exp--;
            p++;
            saw = 1;
        }
    }
    if (!saw) {
        return JOG_FLOAT_LIT_BAD;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        esign = 1;
        e = 0;
        if (*p == '+' || *p == '-') {
            if (*p == '-') {
                esign = -1;
            }
            p++;
        }
        if (*p < '0' || *p > '9') {
            return JOG_FLOAT_LIT_BAD;
        }
        while (*p >= '0' && *p <= '9') {
            if (e > 1000000) {
                return JOG_FLOAT_LIT_FIT;
            }
            e = e * 10 + (*p - '0');
            p++;
        }
        if (esign < 0) {
            if (exp < -1000000 + e) {
                exp = -1000000;
            } else {
                exp -= e;
            }
        } else if (exp > 1000000 - e) {
            return JOG_FLOAT_LIT_FIT;
        } else {
            exp += e;
        }
    }
    if (*p != '\0') {
        return JOG_FLOAT_LIT_BAD;
    }
    if (exp > 400) {
        return JOG_FLOAT_LIT_FIT;
    }
    if (exp < -400) {
        v = 0.0;
        exp = 0;
    }
    while (exp > 0) {
        v *= 10.0;
        if (!finite_d(v)) {
            return JOG_FLOAT_LIT_FIT;
        }
        exp--;
    }
    while (exp < 0) {
        v /= 10.0;
        exp++;
    }
    *out = v;
    return JOG_FLOAT_LIT_OK;
}

/* Quotient truncates toward zero. The remainder keeps the dividend's sign. */
static JogFloatLitStatus frem(double a, double b, double *out) {
    double q;
    double t;
    if (b == 0.0 || !finite_d(a) || !finite_d(b)) {
        return b == 0.0 ? JOG_FLOAT_LIT_DIV0 : JOG_FLOAT_LIT_FIT;
    }
    q = a / b;
    if (!finite_d(q)) {
        return JOG_FLOAT_LIT_FIT;
    }
    if (q >= 0.0) {
        if (q >= 9223372036854775808.0) {
            return JOG_FLOAT_LIT_FIT;
        }
        t = (double)(long long)q;
    } else {
        if (q < -9223372036854775807.0) {
            return JOG_FLOAT_LIT_FIT;
        }
        t = q == 0.0 ? 0.0 : -(double)(long long)(-q);
    }
    *out = a - b * t;
    if (!finite_d(*out)) {
        return JOG_FLOAT_LIT_FIT;
    }
    return JOG_FLOAT_LIT_OK;
}

static JogFloatLitStatus eval_tree(const JogNode *node, double *out) {
    double lhs;
    double rhs;
    double value;
    JogFloatLitStatus st;
    JogTokenKind op;
    if (!node) {
        return JOG_FLOAT_LIT_BAD;
    }
    if (node->kind == JOG_NODE_FLOAT_LIT) {
        return parse_decimal(node->as.literal.text, node->as.literal.len, out);
    }
    if (node->kind == JOG_NODE_UNARY) {
        st = eval_tree(node->as.unary.operand, &value);
        if (st != JOG_FLOAT_LIT_OK) {
            return st;
        }
        *out = -value;
        return finite_d(*out) ? JOG_FLOAT_LIT_OK : JOG_FLOAT_LIT_FIT;
    }
    if (node->kind != JOG_NODE_BINARY) {
        return JOG_FLOAT_LIT_BAD;
    }
    st = eval_tree(node->as.binary.lhs, &lhs);
    if (st != JOG_FLOAT_LIT_OK) {
        return st;
    }
    st = eval_tree(node->as.binary.rhs, &rhs);
    if (st != JOG_FLOAT_LIT_OK) {
        return st;
    }
    op = node->as.binary.op;
    if (op == JOG_TOK_PLUS) {
        value = lhs + rhs;
    } else if (op == JOG_TOK_MINUS) {
        value = lhs - rhs;
    } else if (op == JOG_TOK_STAR) {
        value = lhs * rhs;
    } else if (op == JOG_TOK_SLASH) {
        if (rhs == 0.0) {
            return JOG_FLOAT_LIT_DIV0;
        }
        value = lhs / rhs;
    } else if (op == JOG_TOK_PERCENT) {
        return frem(lhs, rhs, out);
    } else {
        return JOG_FLOAT_LIT_BAD;
    }
    if (!finite_d(value)) {
        return JOG_FLOAT_LIT_FIT;
    }
    *out = value;
    return JOG_FLOAT_LIT_OK;
}

static JogFloatLitStatus narrow(unsigned bits, double value, double *out) {
    float narrow32;
    if (bits != 32 && bits != 64) {
        return JOG_FLOAT_LIT_BAD;
    }
    if (!finite_d(value)) {
        return JOG_FLOAT_LIT_FIT;
    }
    if (bits == 64) {
        *out = value;
        return JOG_FLOAT_LIT_OK;
    }
    narrow32 = (float)value;
    if (!finite_d((double)narrow32)) {
        return JOG_FLOAT_LIT_FIT;
    }
    *out = (double)narrow32;
    return JOG_FLOAT_LIT_OK;
}

JogFloatLitStatus jog_float_lit_eval(const JogNode *node, unsigned bits, double *out) {
    double value;
    JogFloatLitStatus st;
    if (!node || !out || !jog_float_lit_tree(node)) {
        return JOG_FLOAT_LIT_BAD;
    }
    st = eval_tree(node, &value);
    if (st != JOG_FLOAT_LIT_OK) {
        return st;
    }
    return narrow(bits == 0 ? 64 : bits, value, out);
}
