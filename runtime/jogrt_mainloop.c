#include "jogrt.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* Returning mainloop helper for browser builds. Desktop uses a plain
   while loop so one Jog program can call the same jog_host_run_loop. */

typedef int (*JogHostFrameFn)(void);

#ifdef __EMSCRIPTEN__
static JogHostFrameFn g_frame;

static void jog_host_frame_trampoline(void) {
    if (!g_frame || !g_frame()) {
        emscripten_cancel_main_loop();
    }
}

void jog_host_run_loop(JogHostFrameFn frame) {
    g_frame = frame;
    emscripten_set_main_loop(jog_host_frame_trampoline, 0, 1);
}
#else
void jog_host_run_loop(JogHostFrameFn frame) {
    if (!frame) {
        return;
    }
    while (frame()) {
    }
}
#endif
