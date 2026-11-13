/* The C of hash64.march. VARIANT 0: the same 32-bit-lane hash (as
 * Strata.Seed.cell). VARIANT 1: the same splitmix-style mix in one 63-bit
 * lane. VARIANT 2: what C would write: the mix on a full 64-bit lane. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static inline uint64_t low32(uint64_t v) { return v & 4294967295ull; }
static inline uint64_t high32(uint64_t v) { return (v >> 32) & 4294967295ull; }
static inline uint64_t h32(uint64_t x0) {
    uint64_t x1 = x0 ^ (x0 >> 16), x2 = low32(x1 * 73244475ull);
    uint64_t x3 = x2 ^ (x2 >> 16), x4 = low32(x3 * 73244475ull);
    return x4 ^ (x4 >> 16);
}
static inline uint64_t mix(uint64_t seed, uint64_t salt) {
    uint64_t a = h32(low32(seed) ^ h32(low32(salt + 2654435769ull)));
    uint64_t b = h32(high32(seed) ^ h32(high32(salt) + 1013904223ull) ^ (a + 1640531527ull));
    return (b & 1073741823ull) * 4294967296ull + a;
}
static inline uint64_t cell(uint64_t seed, uint64_t salt, uint64_t x, uint64_t y, uint64_t z) {
    uint64_t s = mix(seed, salt);
    uint64_t a0 = h32(low32(s) ^ h32(low32(x + 2246822519ull)));
    uint64_t a1 = h32(a0 ^ h32(low32(y + 3266489917ull)));
    uint64_t a2 = h32(a1 ^ h32(low32(z + 668265263ull)));
    uint64_t b = h32(high32(s) ^ a2 ^ 374761393ull);
    return (b & 1073741823ull) * 4294967296ull + a2;
}
#define M63 4611686018427387903ull
static inline uint64_t m63(uint64_t v) {
    uint64_t m = v & M63;
    uint64_t b = ((m ^ (m >> 30)) * 2685821657736338717ull) & M63;
    uint64_t d = ((b ^ (b >> 27)) * 1181783497276652981ull) & M63;
    return d ^ (d >> 31);
}
static inline uint64_t m64(uint64_t m) {
    m = (m ^ (m >> 30)) * 0xbf58476d1ce4e5b9ull;
    m = (m ^ (m >> 27)) * 0x94d049bb133111ebull;
    return m ^ (m >> 31);
}

__attribute__((noinline)) static uint64_t go(int variant, uint64_t i, uint64_t n) {
    uint64_t acc = 0;
    if (variant == 0) for (; i < n; i++) acc ^= cell(7, 21, i, 0, n - i);
    else if (variant == 1) for (; i < n; i++) acc ^= m63(m63(m63(m63(7 ^ 21) ^ i) ^ 0) ^ (n - i));
    else for (; i < n; i++) acc ^= m64(m64(m64(m64(7 ^ 21) ^ i) ^ 0) ^ (n - i));
    return acc;
}

int main(void) {
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 1;
    int variant = getenv("VARIANT") ? atoi(getenv("VARIANT")) : 0;
    uint64_t acc = 0;
    for (int r = reps; r > 0; r--) acc ^= go(variant, (uint64_t)(r % 3), 4096);
    printf("%llu\n", (unsigned long long)acc);
    return 0;
}
