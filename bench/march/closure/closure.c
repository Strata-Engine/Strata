/* The C of closure.march. VARIANT 0: the rule through a function pointer with
 * a context struct. VARIANT 1: the rule called directly. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct { int64_t sand, snow, base; } ctx_t;

static int32_t material(int64_t sand, int64_t snow, int64_t depth, int64_t y, int wet) {
    if (depth < 0) return wet ? 5 : 0;
    if (depth == 0) return y < sand ? 4 : y > snow ? 6 : 3;
    return depth <= 3 ? 2 : 1;
}
__attribute__((noinline)) static int32_t rule(void *c, int64_t depth, int64_t y, int wet) {
    ctx_t *x = c;
    return material(x->sand, x->snow, depth, x->base + y, wet);
}
__attribute__((noinline)) static void scan(int32_t *out, int32_t (*f)(void *, int64_t, int64_t, int), void *c) {
    for (int64_t i = 0; i < 32768; i++) out[i] = f(c, (i & 7) - 1, i >> 10, (i & 16) == 0);
}
__attribute__((noinline)) static void scan_direct(int32_t *out, int64_t sand, int64_t snow, int64_t base) {
    for (int64_t i = 0; i < 32768; i++) out[i] = material(sand, snow, (i & 7) - 1, base + (i >> 10), (i & 16) == 0);
}

int main(void) {
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 1;
    int variant = getenv("VARIANT") ? atoi(getenv("VARIANT")) : 0;
    int64_t acc = 0;
    for (int r = reps; r > 0; r--) {
        int32_t *out = calloc(32768, sizeof(int32_t));
        ctx_t c = { 10, 25, r % 3 };
        if (variant == 0) scan(out, rule, &c); else scan_direct(out, c.sand, c.snow, c.base);
        acc += out[12345 + r % 7];
        free(out);
    }
    printf("%lld\n", (long long)acc);
    return 0;
}
