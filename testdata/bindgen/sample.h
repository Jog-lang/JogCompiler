#ifndef JOG_BINDGEN_SAMPLE_H
#define JOG_BINDGEN_SAMPLE_H

#include <stddef.h>
#include <stdint.h>

#define SAMPLE_VERSION 42
#define SAMPLE_FLAG 0x20
#define SAMPLE_NAME "sample"
#define SAMPLE_SQUARE(x) ((x) * (x))

typedef struct SampleOpaque SampleOpaque;

struct SamplePoint {
    int x;
    int y;
};

typedef struct SamplePoint SamplePoint;

enum SampleFlags {
    SAMPLE_FLAG_NONE = 0,
    SAMPLE_FLAG_READ = 1,
    SAMPLE_FLAG_WRITE = 2,
};

typedef int (*SampleCallback)(int, void *);

int sample_add(int a, int b);
void sample_log(const char *fmt, ...);
static inline int sample_inline(int x) { return x + 1; }
float sample_scale(float f);
SampleOpaque *sample_create(void);
void sample_destroy(SampleOpaque *o);
size_t sample_len(const char *s);
void sample_with_callback(SampleCallback cb);

#endif
