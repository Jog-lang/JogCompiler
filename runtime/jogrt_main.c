#include "jogrt.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>

/* The C runtime's argv is in the ANSI code page. Jog arguments are UTF-8,
   so they come from the UTF-16 command line. NULL keeps the C argv. The
   blocks live until the process exits. */
static char **jogrt_utf8_argv(int *argc_out) {
    int count = 0;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &count);
    char **out = NULL;
    int ok = 1;
    if (!wide || count < 0) {
        return NULL;
    }
    out = calloc((size_t)count + 1, sizeof(char *));
    ok = out != NULL;
    for (int i = 0; ok && i < count; i++) {
        int need = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        out[i] = need > 0 ? malloc((size_t)need) : NULL;
        ok = out[i] && WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, out[i], need, NULL, NULL) == need;
    }
    LocalFree(wide);
    if (!ok) {
        return NULL;
    }
    *argc_out = count;
    return out;
}
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
    int wide_argc = 0;
    char **wide_argv = jogrt_utf8_argv(&wide_argc);
    if (wide_argv) {
        argc = wide_argc;
        argv = wide_argv;
    }
#endif
    jogrt_start(argc, argv);
    jog_entry_init();
    jog_entry_main();
    return jogrt_finish();
}
