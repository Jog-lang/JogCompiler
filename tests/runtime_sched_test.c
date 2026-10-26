#include "jogrt.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

static int g_status;
static int g_got;

static void send42(void *arg) {
    int value = 42;
    g_status = jogrt_chan_send(arg, &value, sizeof(value));
}

static void recv_into(void *arg) {
    int value = 0;
    int status = jogrt_chan_recv(arg, &value, sizeof(value));
    g_got = status == 0 ? value : -1;
    g_status = status;
}

static void block_send(void *arg) {
    int value = 1;
    g_status = jogrt_chan_send(arg, &value, sizeof(value));
}

static void fail_scope(void *arg) {
    (void)arg;
    jogrt_scope_fail(jogrt_err_cancelled());
}

static void set_seven(void *arg) {
    int value = 7;
    (void)arg;
    jogrt_task_set(&value, sizeof(value));
}

static void test_buffered(void) {
    void *chan = jogrt_chan_init(sizeof(int), 1);
    int value = 9;
    int out = 0;
    CHECK(chan != NULL);
    CHECK(jogrt_chan_send(chan, &value, sizeof(value)) == 0);
    CHECK(jogrt_chan_recv(chan, &out, sizeof(out)) == 0);
    CHECK(out == 9);
    jogrt_chan_close(chan);
    CHECK(jogrt_chan_send(chan, &value, sizeof(value)) == 2);
    CHECK(jogrt_chan_recv(chan, &out, sizeof(out)) == 2);
    CHECK(out == 0);
    jogrt_chan_free(chan);
}

static void test_pipeline(void) {
    void *scope = jogrt_scope_enter(NULL);
    void *chan = jogrt_chan_init(sizeof(int), 0);
    int value = 0;
    int status;
    g_status = -1;
    CHECK(jogrt_go(send42, chan, 64 * 1024, scope, "pipe", 1) != NULL);
    status = jogrt_chan_recv(chan, &value, sizeof(value));
    CHECK(status == 0);
    CHECK(value == 42);
    CHECK(g_status == 0);
    CHECK(jogrt_scope_leave(scope) == NULL);
    jogrt_chan_free(chan);
}

static void test_reverse_pipeline(void) {
    void *scope = jogrt_scope_enter(NULL);
    void *chan = jogrt_chan_init(sizeof(int), 0);
    int value = 42;
    g_got = 0;
    g_status = -1;
    CHECK(jogrt_go(recv_into, chan, 64 * 1024, scope, "rev", 1) != NULL);
    CHECK(jogrt_chan_send(chan, &value, sizeof(value)) == 0);
    CHECK(jogrt_scope_leave(scope) == NULL);
    CHECK(g_got == 42);
    CHECK(g_status == 0);
    jogrt_chan_free(chan);
}

static void test_cancel(void) {
    void *scope = jogrt_scope_enter(NULL);
    void *chan = jogrt_chan_init(sizeof(int), 0);
    g_status = -1;
    CHECK(jogrt_go(block_send, chan, 64 * 1024, scope, NULL, 0) != NULL);
    jogrt_yield();
    jogrt_scope_cancel(scope);
    CHECK(jogrt_scope_leave(scope) == NULL);
    CHECK(g_status == 1);
    jogrt_chan_free(chan);
}

static void test_select(void) {
    void *scope = jogrt_scope_enter(NULL);
    void *chan = jogrt_chan_init(sizeof(int), 0);
    int dest = -1;
    int id;
    int chosen;
    int def;
    CHECK(jogrt_go(send42, chan, 64 * 1024, scope, NULL, 0) != NULL);
    jogrt_select_begin();
    id = jogrt_select_add_recv(chan, &dest, sizeof(dest));
    chosen = jogrt_select_wait();
    CHECK(chosen == id);
    CHECK(dest == 42);
    CHECK(jogrt_select_status() == 0);
    jogrt_select_begin();
    jogrt_select_add_recv(chan, &dest, sizeof(dest));
    def = jogrt_select_add_default();
    chosen = jogrt_select_wait();
    CHECK(chosen == def);
    CHECK(jogrt_scope_leave(scope) == NULL);
    jogrt_chan_free(chan);
}

static void test_timeout(void) {
    int id;
    int chosen;
    jogrt_select_begin();
    id = jogrt_select_add_timeout(1);
    chosen = jogrt_select_wait();
    CHECK(chosen == id);
    CHECK(jogrt_sleep(1) == 0);
}

static void test_fail_and_join(void) {
    void *scope = jogrt_scope_enter(NULL);
    void *task;
    int out = 0;
    CHECK(jogrt_go(fail_scope, NULL, 64 * 1024, scope, NULL, 0) != NULL);
    CHECK(jogrt_scope_leave(scope) == jogrt_err_cancelled());
    scope = jogrt_scope_enter(NULL);
    task = jogrt_go(set_seven, NULL, 64 * 1024, scope, NULL, 0);
    jogrt_task_join(task, &out, sizeof(out));
    CHECK(out == 7);
    CHECK(jogrt_scope_leave(scope) == NULL);
}

int main(void) {
    jogrt_sched_boot();
    test_buffered();
    test_pipeline();
    test_reverse_pipeline();
    test_cancel();
    test_select();
    test_timeout();
    test_fail_and_join();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
