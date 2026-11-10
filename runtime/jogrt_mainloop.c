#include "jogrt.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* Returning mainloop: the app passes an @export tick that returns 0 to
   stop. Desktop runs a while loop; Emscripten drives requestAnimationFrame.

   A weak jog_app_iterate stub remains so binaries that never call
   jog_host_run_loop still link if an older object references the symbol. */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
int jog_app_iterate(void) {
    return 0;
}

#ifdef __EMSCRIPTEN__
static jog_host_tick_fn jog_host_tick;

static void jog_host_frame_trampoline(void) {
    if (!jog_host_tick || !jog_host_tick()) {
        emscripten_cancel_main_loop();
    }
}

void jog_host_run_loop(jog_host_tick_fn tick) {
    jog_host_tick = tick;
    emscripten_set_main_loop(jog_host_frame_trampoline, 0, 1);
}
#else
void jog_host_run_loop(jog_host_tick_fn tick) {
    if (!tick) {
        return;
    }
    while (tick()) {
    }
}
#endif
