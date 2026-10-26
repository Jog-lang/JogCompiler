#include "arena.h"
#include "coro.h"
#include "da.h"
#include "diag.h"
#include "file.h"
#include "fs.h"
#include "intern.h"
#include "map.h"
#include "proc.h"
#include "str.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static const char *g_self;

static const char g_hex[] = "0123456789abcdef";

static int write_hex_line(FILE *fp, const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (fputc(g_hex[*p >> 4], fp) == EOF || fputc(g_hex[*p & 0xf], fp) == EOF) {
            return 0;
        }
        p++;
    }
    return fputc('\n', fp) != EOF;
}

static void append_hex_line(CtSb *sb, const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        ct_sb_putc(sb, g_hex[*p >> 4]);
        ct_sb_putc(sb, g_hex[*p & 0xf]);
        p++;
    }
    ct_sb_putc(sb, '\n');
}

/* Child mode for ct_proc_run. argv is:
   <self> --ct-proc-helper <exit-code> <out|err|both> [args...]
   stdout receives one hex line per argv entry. stderr receives "err\n". */
static int proc_helper(int argc, char **argv) {
    if (argc < 4) {
        return 2;
    }
    char *end = NULL;
    long code = strtol(argv[2], &end, 10);
    if (!end || end == argv[2] || *end != '\0' || code < 0 || code > 255) {
        return 2;
    }
    int to_stdout = strcmp(argv[3], "out") == 0 || strcmp(argv[3], "both") == 0;
    int to_stderr = strcmp(argv[3], "err") == 0 || strcmp(argv[3], "both") == 0;
    if (!to_stdout && !to_stderr) {
        return 2;
    }
    if (to_stdout) {
        for (int i = 0; i < argc; i++) {
            if (!write_hex_line(stdout, argv[i])) {
                return 2;
            }
        }
        if (fflush(stdout) != 0) {
            return 2;
        }
    }
    if (to_stderr) {
        if (fputs("err\n", stderr) == EOF || fflush(stderr) != 0) {
            return 2;
        }
    }
    return (int)code;
}

static char *read_stripped(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long length = ftell(fp);
    if (length < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = malloc((size_t)length + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)length, fp);
    fclose(fp);
    size_t written = 0;
    for (size_t i = 0; i < got; i++) {
        if (buf[i] != '\r') {
            buf[written++] = buf[i];
        }
    }
    buf[written] = '\0';
    return buf;
}

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

static int overlaps(const void *left, size_t left_size, const void *right, size_t right_size) {
    uintptr_t a = (uintptr_t)left;
    uintptr_t b = (uintptr_t)right;
    if (a <= b) {
        return b < a + left_size;
    }
    return a < b + right_size;
}

static int capture_diag(const CtDiag *diag, char *buffer, size_t buffer_size, int json) {
    const char *name = "ct_util_diag_out.txt";
    FILE *fp = fopen(name, "wb+");
    if (!fp) {
        return 0;
    }
    if (json) {
        ct_diag_print_json(diag, fp);
    } else {
        ct_diag_print(diag, fp);
    }
    if (fflush(fp) != 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        remove(name);
        return 0;
    }
    size_t nread = fread(buffer, 1, buffer_size - 1, fp);
    buffer[nread] = '\0';
    int truncated = !feof(fp);
    fclose(fp);
    remove(name);
    return !truncated;
}

static void test_arena(void) {
    ct_arena_destroy(NULL);
    ct_arena_reset(NULL);
    CHECK(ct_arena_alloc(NULL, 8, 8) == NULL);
    CHECK(ct_arena_strdup(NULL, "x") == NULL);
    CHECK(ct_arena_copy(NULL, "x", 1) == NULL);

    CtArena *arena = ct_arena_create();
    CHECK(arena != NULL);
    if (!arena) {
        return;
    }

    CHECK(ct_arena_alloc(arena, 8, 0) == NULL);
    CHECK(ct_arena_alloc(arena, 8, 3) == NULL);
    CHECK(ct_arena_strdup(arena, NULL) == NULL);

    void *first = ct_arena_alloc(arena, 24, 8);
    void *second = ct_arena_alloc(arena, 24, 8);
    CHECK(first != NULL);
    CHECK(second != NULL);
    CHECK(first != second);
    CHECK(first && ((uintptr_t)first % 8) == 0);
    CHECK(second && ((uintptr_t)second % 8) == 0);
    CHECK(first && second && !overlaps(first, 24, second, 24));

    void *align16 = ct_arena_alloc(arena, 1, 16);
    void *align32 = ct_arena_alloc(arena, 1, 32);
    CHECK(align16 != NULL && ((uintptr_t)align16 % 16) == 0);
    CHECK(align32 != NULL && ((uintptr_t)align32 % 32) == 0);

    void *previous = NULL;
    for (int i = 0; i < 64; i++) {
        void *block = ct_arena_alloc(arena, 200, 8);
        CHECK(block != NULL);
        if (!block) {
            break;
        }
        CHECK(((uintptr_t)block % 8) == 0);
        if (previous) {
            CHECK(!overlaps(previous, 200, block, 200));
        }
        memset(block, 0x5A, 200);
        previous = block;
    }

    char original[] = "arena-string";
    char *copied_text = ct_arena_strdup(arena, original);
    original[0] = 'A';
    CHECK(copied_text != NULL);
    CHECK(copied_text && strcmp(copied_text, "arena-string") == 0);

    int numbers[] = {1, 2, 3};
    int *copied_numbers = ct_arena_copy(arena, numbers, sizeof(numbers));
    numbers[0] = 9;
    CHECK(copied_numbers != NULL);
    CHECK(copied_numbers && copied_numbers[0] == 1 && copied_numbers[1] == 2 && copied_numbers[2] == 3);

    ct_arena_reset(arena);
    void *rewound = ct_arena_alloc(arena, 24, 8);
    void *rewound_next = ct_arena_alloc(arena, 24, 8);
    CHECK(rewound == first);
    CHECK(rewound_next == second);

    ct_arena_destroy(arena);
}

static void test_diag(void) {
    CtArena *arena = ct_arena_create();
    CtDiag *diag = ct_diag_create(arena);
    CHECK(arena != NULL && diag != NULL);
    if (!arena || !diag) {
        ct_arena_destroy(arena);
        return;
    }

    CHECK(ct_diag_error_count(NULL) == 0);
    ct_diag_print(NULL, stdout);
    ct_diag_print(diag, NULL);

    CtPos note_pos = {0};
    note_pos.file = "main.ct";
    ct_diag_emit(diag, CT_SEVERITY_NOTE, note_pos, "see previous");

    CtPos warning_pos = {0};
    warning_pos.file = "main.ct";
    warning_pos.line = 4;
    warning_pos.column = 2;
    ct_diag_emit(diag, CT_SEVERITY_WARNING, warning_pos, "unused name '%s'", "foo");

    CtPos error_pos = {0};
    error_pos.file = "main.ct";
    error_pos.line = 9;
    error_pos.column = 1;
    ct_diag_emit(diag, CT_SEVERITY_ERROR, error_pos, "bad token '%s'", "@@");

    CHECK(ct_diag_error_count(diag) == 1);

    char first_print[2048];
    char second_print[2048];
    CHECK(capture_diag(diag, first_print, sizeof(first_print), 0));
    CHECK(capture_diag(diag, second_print, sizeof(second_print), 0));
    CHECK(strcmp(first_print, second_print) == 0);
    CHECK(ct_diag_error_count(diag) == 1);
    CHECK(strstr(first_print, "main.ct: note: see previous") != NULL);
    CHECK(strstr(first_print, "main.ct:4:2: warning: unused name 'foo'") != NULL);
    CHECK(strstr(first_print, "main.ct:9:1: error: bad token '@@'") != NULL);
    CHECK(strstr(first_print, "main.ct") != NULL);

    ct_arena_destroy(arena);
}

static void test_diag_json(void) {
    CtArena *arena = ct_arena_create();
    CtDiag *diag = ct_diag_create(arena);
    CtPos error_pos = {0};
    CtPos note_pos = {0};
    char printed[2048];
    const char *expect =
        "{\"file\":\"a\\\\b.jog\",\"line\":3,\"column\":1,\"severity\":\"error\",\"message\":\"say "
        "\\\"hi\\\"\\n\\t\\\\\\u0001 caf\xc3\xa9\"}\n"
        "{\"file\":\"\",\"line\":0,\"column\":0,\"severity\":\"note\",\"message\":\"see\"}\n";
    CHECK(arena != NULL && diag != NULL);
    if (!arena || !diag) {
        ct_arena_destroy(arena);
        return;
    }
    ct_diag_print_json(NULL, stdout);
    ct_diag_print_json(diag, NULL);
    CHECK(capture_diag(diag, printed, sizeof(printed), 1));
    CHECK(strcmp(printed, "") == 0);

    error_pos.file = "a\\b.jog";
    error_pos.line = 3;
    error_pos.column = 1;
    ct_diag_emit(diag, CT_SEVERITY_ERROR, error_pos, "say \"hi\"\n\t\\\x01 caf\xc3\xa9");
    ct_diag_emit(diag, CT_SEVERITY_NOTE, note_pos, "see");
    CHECK(capture_diag(diag, printed, sizeof(printed), 1));
    CHECK(strcmp(printed, expect) == 0);
    if (strcmp(printed, expect) != 0) {
        fprintf(stderr, "json diag:\n%s\nexpected:\n%s\n", printed, expect);
    }
    ct_arena_destroy(arena);
}

static void test_file(void) {
    CtArena *arena = ct_arena_create();
    CtDiag *diag = ct_diag_create(arena);
    CHECK(arena != NULL && diag != NULL);
    if (!arena || !diag) {
        ct_arena_destroy(arena);
        return;
    }

    const char *name = "ct_util_roundtrip.txt";
    char path[64];
    CHECK(strlen(name) + 1 <= sizeof(path));
    memcpy(path, name, strlen(name) + 1);
    const char *payload = "alpha\nbeta";
    size_t payload_len = strlen(payload);

    FILE *fp = fopen(name, "wb");
    CHECK(fp != NULL);
    if (fp) {
        CHECK(fwrite(payload, 1, payload_len, fp) == payload_len);
        CHECK(fclose(fp) == 0);
    }

    CtFile file;
    int read_ok = fp && ct_file_read(arena, diag, path, &file);
    CHECK(read_ok);
    CHECK(ct_diag_error_count(diag) == 0);
    CHECK(file.size == payload_len);
    CHECK(file.data != NULL && memcmp(file.data, payload, payload_len) == 0);
    CHECK(file.data != NULL && file.data[file.size] == '\0');
    path[0] = 'X';
    CHECK(file.path != NULL && strcmp(file.path, name) == 0);
    remove(name);

    size_t errors_before = ct_diag_error_count(diag);
    CtFile missing;
    int missing_ok = ct_file_read(arena, diag, "ct_util_no_such_dir/missing.txt", &missing);
    CHECK(!missing_ok);
    CHECK(ct_diag_error_count(diag) == errors_before + 1);
    CHECK(missing.data == NULL);
    CHECK(missing.path == NULL);
    CHECK(missing.size == 0);

    ct_arena_destroy(arena);
}

static void test_map(void) {
    void *out = (void *)1;

    CHECK(ct_map_create(NULL) == NULL);
    CHECK(!ct_map_put(NULL, "k", NULL));
    CHECK(!ct_map_put(NULL, NULL, NULL));
    CHECK(!ct_map_get(NULL, "k", &out));
    CHECK(!ct_map_get(NULL, NULL, &out));
    CHECK(!ct_map_remove(NULL, "k"));
    CHECK(!ct_map_remove(NULL, NULL));
    CHECK(ct_map_count(NULL) == 0);

    CtArena *arena = ct_arena_create();
    CHECK(arena != NULL);
    if (!arena) {
        return;
    }

    CtMap *map = ct_map_create(arena);
    CHECK(map != NULL);
    if (!map) {
        ct_arena_destroy(arena);
        return;
    }

    CHECK(!ct_map_put(map, NULL, NULL));
    CHECK(!ct_map_get(map, NULL, &out));
    CHECK(!ct_map_get(map, "missing", NULL));
    CHECK(!ct_map_remove(map, NULL));
    CHECK(!ct_map_remove(map, "missing"));
    CHECK(ct_map_count(map) == 0);

    out = (void *)1;
    CHECK(!ct_map_get(map, "missing", &out));
    CHECK(out == (void *)1);

    CHECK(ct_map_put(map, "nil", NULL));
    CHECK(ct_map_count(map) == 1);
    out = (void *)1;
    CHECK(ct_map_get(map, "nil", &out));
    CHECK(out == NULL);

    char caller_key[] = "alpha";
    int alpha = 1;
    CHECK(ct_map_put(map, caller_key, &alpha));
    CHECK(ct_map_count(map) == 2);
    caller_key[0] = 'Z';
    out = NULL;
    CHECK(ct_map_get(map, "alpha", &out));
    CHECK(out == &alpha);
    CHECK(!ct_map_get(map, caller_key, &out));

    char replace_key[] = "alpha";
    int alpha_replaced = 2;
    size_t count_before_replace = ct_map_count(map);
    CHECK(ct_map_put(map, replace_key, &alpha_replaced));
    CHECK(ct_map_count(map) == count_before_replace);
    replace_key[0] = 'Q';
    CHECK(ct_map_get(map, "alpha", &out));
    CHECK(out == &alpha_replaced);
    CHECK(!ct_map_get(map, replace_key, &out));

    int empty_value = 3;
    CHECK(ct_map_put(map, "", &empty_value));
    CHECK(ct_map_count(map) == 3);
    CHECK(ct_map_get(map, "", &out));
    CHECK(out == &empty_value);
    CHECK(ct_map_get(map, "alpha", &out));
    CHECK(out == &alpha_replaced);
    CHECK(!ct_map_get(map, " ", &out));

    enum { MAP_KEYS = 40 };
    int payloads[MAP_KEYS];
    char names[MAP_KEYS][16];
    for (int i = 0; i < MAP_KEYS; i++) {
        payloads[i] = 1000 + i;
        CHECK(snprintf(names[i], sizeof(names[i]), "id-%d", i) > 0);
        CHECK(ct_map_put(map, names[i], &payloads[i]));
    }
    CHECK(ct_map_count(map) == 3u + (size_t)MAP_KEYS);

    for (int i = 0; i < MAP_KEYS; i++) {
        names[i][0] = 'X';
    }
    for (int i = 0; i < MAP_KEYS; i++) {
        char expected[16];
        CHECK(snprintf(expected, sizeof(expected), "id-%d", i) > 0);
        out = NULL;
        CHECK(ct_map_get(map, expected, &out));
        CHECK(out == &payloads[i]);
    }
    CHECK(ct_map_get(map, "nil", &out));
    CHECK(out == NULL);
    CHECK(ct_map_get(map, "", &out));
    CHECK(out == &empty_value);
    CHECK(ct_map_get(map, "alpha", &out));
    CHECK(out == &alpha_replaced);

    size_t before_replace_grown = ct_map_count(map);
    CHECK(ct_map_put(map, "id-1", &payloads[1]));
    CHECK(ct_map_count(map) == before_replace_grown);

    size_t before_remove = ct_map_count(map);
    CHECK(ct_map_remove(map, "id-0"));
    CHECK(ct_map_count(map) == before_remove - 1);
    CHECK(!ct_map_get(map, "id-0", &out));
    CHECK(!ct_map_remove(map, "id-0"));
    CHECK(ct_map_count(map) == before_remove - 1);
    CHECK(ct_map_get(map, "id-1", &out));
    CHECK(out == &payloads[1]);

    int id0_again = 77;
    CHECK(ct_map_put(map, "id-0", &id0_again));
    CHECK(ct_map_count(map) == before_remove);
    CHECK(ct_map_get(map, "id-0", &out));
    CHECK(out == &id0_again);

    CHECK(ct_map_remove(map, ""));
    CHECK(!ct_map_get(map, "", &out));
    CHECK(ct_map_get(map, "nil", &out));
    CHECK(out == NULL);
    CHECK(ct_map_get(map, "alpha", &out));
    CHECK(out == &alpha_replaced);

    size_t before_tombstones = ct_map_count(map);
    CHECK(ct_map_remove(map, "id-2"));
    CHECK(ct_map_remove(map, "id-3"));
    CHECK(ct_map_remove(map, "id-4"));
    CHECK(ct_map_count(map) == before_tombstones - 3);
    CHECK(!ct_map_get(map, "id-3", &out));
    CHECK(ct_map_get(map, "id-5", &out));
    CHECK(out == &payloads[5]);

    int extra = 5;
    CHECK(ct_map_put(map, "extra", &extra));
    CHECK(ct_map_count(map) == before_tombstones - 2);
    CHECK(ct_map_get(map, "extra", &out));
    CHECK(out == &extra);
    CHECK(!ct_map_get(map, "id-2", &out));

    ct_arena_destroy(arena);
}

static void test_da(void) {
    int sample = 1;
    CHECK(!ct_da_init(NULL, sizeof(sample)));
    CHECK(!ct_da_push(NULL, &sample));
    CHECK(!ct_da_swap_remove(NULL, 0));
    CHECK(ct_da_at(NULL, 0) == NULL);
    ct_da_free(NULL);

    CtDa zero;
    CHECK(!ct_da_init(&zero, 0));
    CHECK(!ct_da_push(&zero, &sample));
    ct_da_free(&zero);

    CtDa da;
    CHECK(ct_da_init(&da, sizeof(int)));
    CHECK(!ct_da_push(&da, NULL));
    CHECK(da.count == 0);
    CHECK(ct_da_at(&da, 0) == NULL);
    CHECK(!ct_da_swap_remove(&da, 0));

    for (int i = 0; i < 8; i++) {
        CHECK(ct_da_push(&da, &i));
    }
    CHECK(da.count == 8);
    CHECK(da.capacity == 8);

    int ninth = 8;
    CHECK(ct_da_push(&da, &ninth));
    CHECK(da.count == 9);
    CHECK(da.capacity == 16);

    for (int i = 0; i < 9; i++) {
        int *item = ct_da_at(&da, (size_t)i);
        CHECK(item != NULL && *item == i);
    }
    CHECK(ct_da_at(&da, 9) == NULL);
    CHECK(ct_da_at(&da, SIZE_MAX) == NULL);
    CHECK(!ct_da_swap_remove(&da, 9));
    CHECK(!ct_da_swap_remove(&da, SIZE_MAX));
    CHECK(da.count == 9);

    CHECK(ct_da_swap_remove(&da, 2));
    CHECK(da.count == 8);
    int expected[] = {0, 1, 8, 3, 4, 5, 6, 7};
    for (int i = 0; i < 8; i++) {
        int *item = ct_da_at(&da, (size_t)i);
        CHECK(item != NULL && *item == expected[i]);
    }
    CHECK(ct_da_at(&da, 8) == NULL);

    ct_da_free(&da);
    CHECK(da.data == NULL);
    CHECK(da.count == 0);
    CHECK(da.capacity == 0);
}

typedef struct CoroTrace {
    size_t id;
    int value;
} CoroTrace;

enum { CORO_TRACE_CAP = 8 };

static CoroTrace g_coro_trace[CORO_TRACE_CAP];
static size_t g_coro_trace_len;

static void coro_note(int value) {
    if (g_coro_trace_len >= CORO_TRACE_CAP) {
        return;
    }
    g_coro_trace[g_coro_trace_len].id = coroutine_id();
    g_coro_trace[g_coro_trace_len].value = value;
    g_coro_trace_len++;
}

static void coro_counter(void *arg) {
    int limit = *(const int *)arg;
    for (int value = 1; value <= limit; value++) {
        coro_note(value);
        coroutine_yield();
    }
}

static void test_coro(void) {
    int unused = 0;
    CHECK(!coroutine_go(coro_counter, &unused));
    CHECK(coroutine_alive() == 0);

    CHECK(coroutine_init());
    CHECK(coroutine_init());
    CHECK(!coroutine_go(NULL, NULL));
    CHECK(coroutine_alive() == 1);
    CHECK(coroutine_id() == 0);
    coroutine_yield();
    CHECK(coroutine_id() == 0);
    CHECK(coroutine_alive() == 1);

    g_coro_trace_len = 0;
    int limit_a = 2;
    int limit_b = 3;
    CHECK(coroutine_go(coro_counter, &limit_a));
    CHECK(coroutine_go(coro_counter, &limit_b));
    CHECK(coroutine_alive() == 3);

    while (coroutine_alive() > 1) {
        coroutine_yield();
    }

    CHECK(coroutine_alive() == 1);
    CHECK(coroutine_id() == 0);
    CHECK(g_coro_trace_len == 5);
    if (g_coro_trace_len == 5) {
        CHECK(g_coro_trace[0].id == 1 && g_coro_trace[0].value == 1);
        CHECK(g_coro_trace[1].id == 2 && g_coro_trace[1].value == 1);
        CHECK(g_coro_trace[2].id == 1 && g_coro_trace[2].value == 2);
        CHECK(g_coro_trace[3].id == 2 && g_coro_trace[3].value == 2);
        CHECK(g_coro_trace[4].id == 2 && g_coro_trace[4].value == 3);
    }

    coroutine_shutdown();
    CHECK(coroutine_alive() == 0);
}

static void test_str(void) {
    CtStr empty = ct_str(NULL);
    CHECK(empty.len == 0);
    CHECK(ct_str_eq(empty, ct_str("")));
    CHECK(ct_str_eq_cstr(ct_str_n("hello", 4), "hell"));
    CHECK(!ct_str_eq_cstr(ct_str("hello"), "hell"));
    CHECK(ct_str_starts_with(ct_str("main.test.jog"), ct_str("main")));
    CHECK(ct_str_ends_with(ct_str("main.test.jog"), ct_str(".test.jog")));
    CHECK(!ct_str_ends_with(ct_str("jog"), ct_str(".test.jog")));

    CtArena *arena = ct_arena_create();
    char *dup = ct_str_dup(arena, ct_str_n("abcdef", 3));
    CHECK(dup && strcmp(dup, "abc") == 0);
    ct_arena_destroy(arena);

    CtSb sb;
    ct_sb_init(&sb);
    CHECK(strcmp(ct_sb_cstr(&sb), "") == 0);
    CHECK(ct_sb_append_cstr(&sb, "int "));
    CHECK(ct_sb_appendf(&sb, "x%d = %s;", 7, "0"));
    CHECK(ct_sb_putc(&sb, '\n'));
    CHECK(strcmp(ct_sb_cstr(&sb), "int x7 = 0;\n") == 0);
    CHECK(sb.len == 12);
    for (int i = 0; i < 1000; i++) {
        ct_sb_append(&sb, "0123456789", 10);
    }
    CHECK(sb.len == 10012 && !sb.failed);
    CHECK(ct_sb_view(&sb).ptr[10011] == '9');
    ct_sb_clear(&sb);
    CHECK(sb.len == 0 && strcmp(ct_sb_cstr(&sb), "") == 0);
    CHECK(!ct_sb_append(&sb, NULL, 3));
    CHECK(sb.failed);
    ct_sb_free(&sb);
    CHECK(sb.data == NULL && !sb.failed);
}

static void test_intern(void) {
    CHECK(ct_intern_create(NULL) == NULL);
    CHECK(ct_intern(NULL, "x", 1) == NULL);

    CtArena *arena = ct_arena_create();
    CtIntern *table = ct_intern_create(arena);
    CHECK(table != NULL);

    const char *a = ct_intern(table, "names", 4);
    const char *b = ct_intern_cstr(table, "name");
    CHECK(a && a == b && strcmp(a, "name") == 0);
    CHECK(ct_intern_cstr(table, "names") != a);
    CHECK(ct_intern(table, "", 0) == ct_intern_cstr(table, ""));
    CHECK(ct_intern(table, "a\0b", 3) != ct_intern(table, "a", 1));
    CHECK(ct_intern_count(table) == 5);

    /* Survives many growths with stable pointers. */
    const char *first = ct_intern_cstr(table, "k0");
    char key[16];
    for (int i = 0; i < 2000; i++) {
        snprintf(key, sizeof key, "k%d", i);
        CHECK(ct_intern_cstr(table, key) != NULL);
    }
    CHECK(ct_intern_cstr(table, "k0") == first);
    CHECK(ct_intern_count(table) == 5 + 2000);
    ct_arena_destroy(arena);
}

static void test_fs(void) {
    CtArena *arena = ct_arena_create();
    CtDiag *diag = ct_diag_create(arena);
    const char *tools = ct_path_join(arena, CT_SOURCE_DIR, "tools");
    CHECK(tools && ct_fs_is_dir(tools));
    CHECK(!ct_fs_is_file(tools));
    CHECK(ct_fs_is_file(ct_path_join(arena, tools, "fs.h")));

    CtDa names;
    ct_da_init(&names, sizeof(const char *));
    CHECK(ct_fs_list(arena, diag, tools, CT_FS_FILES, ".h", &names));
    int has_arena = 0;
    int sorted = 1;
    for (size_t i = 0; i < names.count; i++) {
        const char *name = *(const char **)ct_da_at(&names, i);
        has_arena |= strcmp(name, "arena.h") == 0;
        if (i > 0 && strcmp(*(const char **)ct_da_at(&names, i - 1), name) >= 0) {
            sorted = 0;
        }
        CHECK(strstr(name, ".c") == NULL);
    }
    CHECK(has_arena && sorted && names.count >= 8);
    ct_da_free(&names);

    ct_da_init(&names, sizeof(const char *));
    CHECK(ct_fs_list(arena, diag, CT_SOURCE_DIR, CT_FS_DIRS, NULL, &names));
    int has_tools = 0;
    for (size_t i = 0; i < names.count; i++) {
        const char *name = *(const char **)ct_da_at(&names, i);
        has_tools |= strcmp(name, "tools") == 0;
        CHECK(name[0] != '.');
    }
    CHECK(has_tools);
    ct_da_free(&names);

    ct_da_init(&names, sizeof(const char *));
    CHECK(ct_diag_error_count(diag) == 0);
    CHECK(!ct_fs_list(arena, diag, ct_path_join(arena, tools, "missing-dir"), CT_FS_FILES, NULL, &names));
    CHECK(ct_diag_error_count(diag) == 1);
    CHECK(names.count == 0);
    ct_da_free(&names);

    CHECK(strcmp(ct_path_join(arena, "a/", "b"), "a/b") == 0);
    CHECK(strcmp(ct_path_join(arena, "a", "b"), "a/b") == 0);
    CHECK(strcmp(ct_path_join(arena, "", "b"), "b") == 0);
    ct_arena_destroy(arena);
}

static void expect_stdout(const char *const *argv, const char *path) {
    char *got = read_stripped(path);
    CtSb expect;
    ct_sb_init(&expect);
    for (size_t i = 0; argv[i]; i++) {
        append_hex_line(&expect, argv[i]);
    }
    int same = got && !expect.failed && strcmp(got, ct_sb_cstr(&expect)) == 0;
    if (!same) {
        fprintf(stderr, "stdout mismatch\n got:\n%s\n want:\n%s\n", got ? got : "(null)", ct_sb_cstr(&expect));
    }
    CHECK(same);
    free(got);
    ct_sb_free(&expect);
}

static void test_proc(void) {
    const char *out_path = "ct_util_proc_out.txt";
    const char *err_path = "ct_util_proc_err.txt";
    remove(out_path);
    remove(err_path);

    int code = 55;
    CHECK(!ct_proc_run(NULL, NULL, NULL, &code));
    CHECK(code == 55);

    const char *const empty[] = {"", NULL};
    CHECK(!ct_proc_run(empty, out_path, NULL, &code));
    CHECK(code == 55);
    CHECK(!ct_fs_is_file(out_path));
    CHECK(!ct_proc_run(empty, NULL, NULL, NULL));

    const char *const missing[] = {"ct-proc-no-such-program", NULL};
    CHECK(!ct_proc_run(missing, out_path, NULL, &code));
    CHECK(code == 55);
    CHECK(ct_fs_is_file(out_path));
    remove(out_path);

    const char *const directory[] = {CT_SOURCE_DIR, NULL};
    CHECK(!ct_proc_run(directory, NULL, NULL, &code));
    CHECK(code == 55);

    CHECK(g_self != NULL && g_self[0] != '\0');
    if (!g_self || g_self[0] == '\0') {
        remove(out_path);
        remove(err_path);
        return;
    }

    const char *const out_only[] = {g_self, "--ct-proc-helper", "0", "out", "one", "two words", NULL};
    CHECK(ct_proc_run(out_only, out_path, NULL, &code));
    CHECK(code == 0);
    expect_stdout(out_only, out_path);

    const char *const err_only[] = {g_self, "--ct-proc-helper", "3", "err", NULL};
    CHECK(ct_proc_run(err_only, NULL, err_path, &code));
    CHECK(code == 3);
    char *err = read_stripped(err_path);
    CHECK(err && strcmp(err, "err\n") == 0);
    free(err);

    const char *const both[] = {
        g_self,
        "--ct-proc-helper",
        "17",
        "both",
        "plain",
        "has space",
        "tab\there",
        "",
        "quote\"mid",
        "end\\",
        "end\\\\",
        "a\\\"b",
        "  pad  ",
        "\\",
        "\"",
        "a&b|c>d",
        "%PATH%",
        "caret^x",
        NULL,
    };
    CHECK(ct_proc_run(both, out_path, err_path, &code));
    CHECK(code == 17);
    expect_stdout(both, out_path);
    err = read_stripped(err_path);
    CHECK(err && strcmp(err, "err\n") == 0);
    free(err);

    const char *const second[] = {g_self, "--ct-proc-helper", "9", "out", "second-run", NULL};
    CHECK(ct_proc_run(second, out_path, err_path, &code));
    CHECK(code == 9);
    expect_stdout(second, out_path);
    err = read_stripped(err_path);
    CHECK(err && strcmp(err, "") == 0);
    free(err);

    const char *const shared[] = {g_self, "--ct-proc-helper", "4", "err", NULL};
    CHECK(ct_proc_run(shared, out_path, out_path, &code));
    CHECK(code == 4);
    err = read_stripped(out_path);
    CHECK(err && strcmp(err, "err\n") == 0);
    free(err);

    remove(out_path);
    remove(err_path);
}

int main(int argc, char **argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "--ct-proc-helper") == 0) {
        return proc_helper(argc, argv);
    }
    g_self = argc > 0 ? argv[0] : NULL;
    test_str();
    test_intern();
    test_fs();
    test_arena();
    test_diag();
    test_diag_json();
    test_file();
    test_map();
    test_da();
    test_coro();
    test_proc();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
