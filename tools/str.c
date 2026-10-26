#include "str.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CtStr ct_str(const char *cstr) {
    CtStr s = {0};
    if (cstr) {
        s.ptr = cstr;
        s.len = strlen(cstr);
    }
    return s;
}

CtStr ct_str_n(const char *ptr, size_t len) {
    CtStr s = {ptr, ptr ? len : 0};
    return s;
}

bool ct_str_eq(CtStr a, CtStr b) {
    if (a.len != b.len) {
        return false;
    }
    return a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0;
}

bool ct_str_eq_cstr(CtStr a, const char *b) {
    return ct_str_eq(a, ct_str(b));
}

bool ct_str_starts_with(CtStr s, CtStr prefix) {
    if (prefix.len > s.len) {
        return false;
    }
    return prefix.len == 0 || memcmp(s.ptr, prefix.ptr, prefix.len) == 0;
}

bool ct_str_ends_with(CtStr s, CtStr suffix) {
    if (suffix.len > s.len) {
        return false;
    }
    return suffix.len == 0 || memcmp(s.ptr + (s.len - suffix.len), suffix.ptr, suffix.len) == 0;
}

char *ct_str_dup(CtArena *arena, CtStr s) {
    if (!arena || s.len == SIZE_MAX) {
        return NULL;
    }
    char *copy = ct_arena_alloc(arena, s.len + 1, 1);
    if (!copy) {
        return NULL;
    }
    if (s.len > 0) {
        memcpy(copy, s.ptr, s.len);
    }
    copy[s.len] = '\0';
    return copy;
}

void ct_sb_init(CtSb *sb) {
    if (!sb) {
        return;
    }
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->failed = false;
}

void ct_sb_free(CtSb *sb) {
    if (!sb) {
        return;
    }
    free(sb->data);
    ct_sb_init(sb);
}

void ct_sb_clear(CtSb *sb) {
    if (!sb) {
        return;
    }
    sb->len = 0;
    if (sb->data) {
        sb->data[0] = '\0';
    }
}

/* Makes room for extra bytes plus the terminating NUL. */
static bool sb_reserve(CtSb *sb, size_t extra) {
    if (extra > SIZE_MAX - 1 - sb->len) {
        return false;
    }
    size_t needed = sb->len + extra + 1;
    if (needed <= sb->cap) {
        return true;
    }
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2) {
            cap = needed;
            break;
        }
        cap *= 2;
    }
    char *grown = realloc(sb->data, cap);
    if (!grown) {
        return false;
    }
    sb->data = grown;
    sb->cap = cap;
    return true;
}

bool ct_sb_append(CtSb *sb, const char *data, size_t len) {
    if (!sb) {
        return false;
    }
    if (len > 0 && !data) {
        sb->failed = true;
        return false;
    }
    if (!sb_reserve(sb, len)) {
        sb->failed = true;
        return false;
    }
    if (len > 0) {
        memcpy(sb->data + sb->len, data, len);
    }
    sb->len += len;
    sb->data[sb->len] = '\0';
    return true;
}

bool ct_sb_append_cstr(CtSb *sb, const char *cstr) {
    CtStr s = ct_str(cstr);
    return ct_sb_append(sb, s.ptr, s.len);
}

bool ct_sb_append_str(CtSb *sb, CtStr s) {
    return ct_sb_append(sb, s.ptr, s.len);
}

bool ct_sb_putc(CtSb *sb, char c) {
    return ct_sb_append(sb, &c, 1);
}

bool ct_sb_appendf(CtSb *sb, const char *fmt, ...) {
    if (!sb || !fmt) {
        return false;
    }
    va_list args;
    va_start(args, fmt);
    va_list sizing;
    va_copy(sizing, args);
    int needed = vsnprintf(NULL, 0, fmt, sizing);
    va_end(sizing);
    if (needed < 0 || !sb_reserve(sb, (size_t)needed)) {
        va_end(args);
        sb->failed = true;
        return false;
    }
    vsnprintf(sb->data + sb->len, (size_t)needed + 1, fmt, args);
    va_end(args);
    sb->len += (size_t)needed;
    return true;
}

const char *ct_sb_cstr(const CtSb *sb) {
    if (!sb || !sb->data) {
        return "";
    }
    return sb->data;
}

CtStr ct_sb_view(const CtSb *sb) {
    if (!sb || !sb->data) {
        return ct_str_n(NULL, 0);
    }
    return ct_str_n(sb->data, sb->len);
}
