/* POSIX IO helpers referenced by std.io / std.fs (libc builds only).
 * Mirrors seed runtime/jogrt.c for errno, stdin read, and binary stdio. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

int jogrt_errno(void) {
    return errno;
}

intptr_t jogrt_stdin_read(void *buf, size_t cap) {
    if (!buf || cap == 0) {
        return -1;
    }
    if (cap > (size_t)INTPTR_MAX) {
        cap = (size_t)INTPTR_MAX;
    }
    for (;;) {
        ssize_t got = read(0, buf, cap);
        if (got >= 0) {
            return (intptr_t)got;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

/* No text-mode translation on POSIX; flush stdout to match seed. */
_Bool jogrt_stdio_binary(void) {
    return fflush(stdout) == 0;
}
