#ifndef JOG_OS_SUFFIX_H
#define JOG_OS_SUFFIX_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* File-name OS tags shared by the loader and the platform check.
   The tag is the single dot-segment immediately before .jog or
   .test.jog. posix matches linux and macos. Any other segment,
   including a.b.jog, is an ordinary file. */

static inline int jog_os_name_is(const char *text, size_t len, const char *word) {
    size_t n = word ? strlen(word) : 0;
    return word && n == len && text && memcmp(text, word, len) == 0;
}

static inline int jog_os_tag_is_known(const char *text, size_t len) {
    return jog_os_name_is(text, len, "windows") || jog_os_name_is(text, len, "linux") ||
           jog_os_name_is(text, len, "macos") || jog_os_name_is(text, len, "wasm") ||
           jog_os_name_is(text, len, "posix");
}

/* A jog.mod `targets` name. posix is a file tag, not a target. */
static inline int jog_os_target_name(const char *text, size_t len) {
    return jog_os_name_is(text, len, "windows") || jog_os_name_is(text, len, "linux") ||
           jog_os_name_is(text, len, "macos") || jog_os_name_is(text, len, "wasm");
}

static inline int jog_os_tag_matches(const char *tag, size_t len, const char *target) {
    if (!target) {
        return 0;
    }
    if (jog_os_name_is(tag, len, "posix")) {
        return strcmp(target, "linux") == 0 || strcmp(target, "macos") == 0;
    }
    return jog_os_name_is(tag, len, target);
}

static inline const char *jog_host_os(void) {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

/* wasm32 is pointer size 4. Anything else is the host. */
static inline const char *jog_target_os(uint32_t ptr_size) {
    if (ptr_size == 4) {
        return "wasm";
    }
    return jog_host_os();
}

static inline const char *jog_path_base(const char *path) {
    const char *base = path ? path : "";
    const char *p;
    for (p = base; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

/* 1 when the basename's last dot-segment is a known OS tag.
   *is_test is set when the name ends in .test.jog. *tag points into name. */
static inline int jog_file_os_tag(const char *name, const char **tag, size_t *tag_len, int *is_test) {
    size_t n;
    const char *stem_end;
    const char *dot = NULL;
    const char *p;
    if (tag) {
        *tag = NULL;
    }
    if (tag_len) {
        *tag_len = 0;
    }
    if (is_test) {
        *is_test = 0;
    }
    if (!name) {
        return 0;
    }
    n = strlen(name);
    if (n >= 9 && memcmp(name + n - 9, ".test.jog", 9) == 0) {
        if (is_test) {
            *is_test = 1;
        }
        stem_end = name + n - 9;
    } else if (n >= 4 && memcmp(name + n - 4, ".jog", 4) == 0) {
        stem_end = name + n - 4;
    } else {
        return 0;
    }
    for (p = name; p < stem_end; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (!dot || dot == name) {
        return 0;
    }
    if (tag) {
        *tag = dot + 1;
    }
    if (tag_len) {
        *tag_len = (size_t)(stem_end - (dot + 1));
    }
    return jog_os_tag_is_known(dot + 1, (size_t)(stem_end - (dot + 1)));
}

#endif
