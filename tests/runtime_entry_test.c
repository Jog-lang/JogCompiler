#include "jogrt.h"

#include <stdio.h>

/* Generated programs define these two. This test defines them and then
   uses the real runtime main, which must call start, init, main, finish. */

static int g_stage;

static void fail(int code, const char *msg) {
    fprintf(stderr, "FAIL %s\n", msg);
    jogrt_exit(code);
}

void jog_entry_init(void) {
    if (g_stage != 0) {
        fail(2, "init ran twice");
    }
    if (jogrt_argc() < 1 || jogrt_argv(0, NULL) == NULL) {
        fail(3, "start did not publish argc/argv before init");
    }
    g_stage = 1;
}

void jog_entry_main(void) {
    if (g_stage != 1) {
        fail(4, "main ran before init");
    }
    const char msg[] = "entry\n";
    if (!jogrt_write(1, msg, sizeof(msg) - 1)) {
        fail(5, "entry write");
    }
}

#include "jogrt_main.c"
