/* Returning mainloop for desktop (while) and Emscripten (rAF). */
#include "jogrt.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static int64_t (*jog_tick_fn)(void);

#ifdef __EMSCRIPTEN__
static void jog_host_frame_trampoline(void) {
    if (jog_tick_fn == NULL || jog_tick_fn() == 0) {
        emscripten_cancel_main_loop();
    }
}

void jogrt_host_run_loop(int64_t (*tick)(void)) {
    jog_tick_fn = tick;
    /* fps=0 → browser rAF; simulate_infinite_loop=1 → does not return. */
    emscripten_set_main_loop(jog_host_frame_trampoline, 0, 1);
}
#else
void jogrt_host_run_loop(int64_t (*tick)(void)) {
    jog_tick_fn = tick;
    if (jog_tick_fn == NULL) return;
    while (jog_tick_fn()) {
    }
}
#endif

void jog_host_run_loop(int64_t (*tick)(void)) {
    jogrt_host_run_loop(tick);
}
