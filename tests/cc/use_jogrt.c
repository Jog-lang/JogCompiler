#include "jogrt.h"

/* Wrong on purpose. size_t is wider than unsigned int on x64, so gcc and
   clang warn about this libc builtin unless the driver passes its
   mismatch flag. The declaration is not called. */
void *memcpy(void *dst, const void *src, unsigned int n);

void jog_entry_init(void) {
}

void jog_entry_main(void) {
#if defined(__clang__)
    static const char cc[] = "clang ";
#elif defined(__GNUC__)
    static const char cc[] = "gcc ";
#else
    static const char cc[] = "other ";
#endif
#if defined(__OPTIMIZE__)
    static const char opt[] = "opt\n";
#else
    static const char opt[] = "dbg\n";
#endif
    if (!jogrt_write(1, "jogrt ", 6) || !jogrt_write(1, cc, sizeof(cc) - 1) || !jogrt_write(1, opt, sizeof(opt) - 1)) {
        jogrt_exit(1);
    }
}
