/* JOG-232: coverage-guided fuzz runtime (native). Linked when fuzz instrumentation is on. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { JOGRT_FUZZ_MAP = 1 << 16 };

static uint8_t jogrt_fuzz_cov[JOGRT_FUZZ_MAP];
static unsigned char jogrt_fuzz_buf[1 << 20];
static int64_t jogrt_fuzz_buflen;
static uint64_t jogrt_fuzz_rng = 0x9e3779b97f4a7c15ULL;

void jogrt_fuzz_hit(int64_t id) {
    uint32_t i = (uint32_t)id & (JOGRT_FUZZ_MAP - 1);
    jogrt_fuzz_cov[i] = 1;
}

void jogrt_fuzz_reset(void) {
    memset(jogrt_fuzz_cov, 0, sizeof jogrt_fuzz_cov);
}

void jogrt_fuzz_seed_rng(int64_t seed) {
    jogrt_fuzz_rng = (uint64_t)seed;
    if (jogrt_fuzz_rng == 0) jogrt_fuzz_rng = 0x9e3779b97f4a7c15ULL;
}

int64_t jogrt_fuzz_rand(void) {
    uint64_t x = jogrt_fuzz_rng;
    x ^= x << 7;
    x ^= x >> 9;
    x ^= x << 8;
    jogrt_fuzz_rng = x;
    return (int64_t)x;
}

/* Load argv[1] into the static buffer. Returns length, or -1 on error. */
extern const char *jogrt_arg_ptr(int i);

int64_t jogrt_fuzz_prepare(void) {
    const char *path = jogrt_arg_ptr(1);
    jogrt_fuzz_buflen = 0;
    if (!path || !path[0]) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(jogrt_fuzz_buf, 1, sizeof jogrt_fuzz_buf, f);
    fclose(f);
    jogrt_fuzz_buflen = (int64_t)n;
    jogrt_fuzz_reset();
    return jogrt_fuzz_buflen;
}

void *jogrt_fuzz_ptr(void) {
    return jogrt_fuzz_buf;
}

int64_t jogrt_fuzz_len(void) {
    return jogrt_fuzz_buflen;
}

/* Print coverage marker for the driver: __JOG_COV__ <popcount> <hash64>\n.
 * The driver keeps hashes across process runs (each run starts with a fresh map). */
void jogrt_fuzz_report(void) {
    int64_t n = 0;
    uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < JOGRT_FUZZ_MAP; i++) {
        if (jogrt_fuzz_cov[i]) {
            n++;
            h ^= (uint64_t)(i + 1);
            h *= 1099511628211ULL;
        }
    }
    printf("__JOG_COV__ %lld %llu\n", (long long)n, (unsigned long long)h);
    fflush(stdout);
}
