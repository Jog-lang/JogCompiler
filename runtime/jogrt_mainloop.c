#include "jogrt.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* Apps that use the returning mainloop @export jog_app_iterate(): i32
   (return 0 to stop). Passing a Jog fn as extern () -> i32 is not wired
   yet (Friction); a fixed export keeps one binary on desktop + Emscripten.

   A weak stub lets programs that never call jog_host_run_loop still link. */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
int jog_app_iterate(void) {
    return 0;
}

#ifdef __EMSCRIPTEN__
static void jog_host_frame_trampoline(void) {
    if (!jog_app_iterate()) {
        emscripten_cancel_main_loop();
    }
}

void jog_host_run_loop(void) {
    emscripten_set_main_loop(jog_host_frame_trampoline, 0, 1);
}
#else
void jog_host_run_loop(void) {
    while (jog_app_iterate()) {
    }
}
#endif
