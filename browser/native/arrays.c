#include <stdint.h>
#include <stddef.h>

extern void *march_alloc(int64_t size);
extern void *memcpy(void *dst, const void *src, size_t size);

/* Layout shared by March's native-array lowering and Strata's C FFI. */
typedef struct {
    int64_t refs;
    int32_t tag;
    int32_t pad;
    int64_t length;
    int64_t kind;
} Array;

static Array *allocate(int64_t length, int64_t kind, size_t width) {
    if (length < 0 || (uint64_t)length > (SIZE_MAX - sizeof(Array)) / width)
        __builtin_trap();
    Array *a = march_alloc(sizeof(Array) + (size_t)length * width);
    a->refs = 1;
    a->tag = -6;
    a->length = length;
    a->kind = kind;
    return a;
}

#define ARRAY_OPS(name, type, value_type, kind)                                            \
    Array *native_##name##_arr_make(int64_t len, value_type value) {               \
        Array *a = allocate(len, kind, sizeof(type));                          \
        type *data = (type *)(a + 1);                                          \
        for (int64_t i = 0; i < len; i++) data[i] = (type)value;                 \
        return a;                                                             \
    }                                                                         \
    int64_t native_##name##_arr_length(const Array *a) { return a->length; }     \
    value_type native_##name##_arr_get(const Array *a, int64_t i) {                \
        if (i < 0 || i >= a->length) __builtin_trap();                           \
        return ((const type *)(a + 1))[i];                                     \
    }                                                                         \
    Array *native_##name##_arr_set(Array *a, int64_t i, value_type value) {          \
        if (i < 0 || i >= a->length) __builtin_trap();                           \
        if (a->refs != 1) {                                                    \
            Array *copy = allocate(a->length, kind, sizeof(type));              \
            memcpy(copy + 1, a + 1, (size_t)a->length * sizeof(type));          \
            if (a->refs > 1) a->refs--;                                        \
            a = copy;                                                         \
        }                                                                     \
        ((type *)(a + 1))[i] = (type)value;                                     \
        return a;                                                             \
    }

ARRAY_OPS(int, int64_t, int64_t, 0)
ARRAY_OPS(i32, int32_t, int64_t, 3)
ARRAY_OPS(u8, uint8_t, int64_t, 4)
ARRAY_OPS(f32, float, double, 2)

_Noreturn void march_panic_user(void *message) {
    (void)message;
    __builtin_trap();
}

/* Clang lowers March's checked i64 multiplication to a widened multiply.
 * Build the result from 32-bit limbs so this freestanding helper does not
 * itself depend on compiler-rt. */
__uint128_t __multi3(__uint128_t a, __uint128_t b) {
    uint64_t x = (uint64_t)a, y = (uint64_t)b;
    uint64_t x0 = (uint32_t)x, x1 = x >> 32;
    uint64_t y0 = (uint32_t)y, y1 = y >> 32;
    uint64_t lo = x0 * y0;
    uint64_t mid = x1 * y0 + (lo >> 32);
    uint64_t carry = (uint32_t)mid + x0 * y1;
    uint64_t hi = x1 * y1 + (mid >> 32) + (carry >> 32);
    hi += x * (uint64_t)(b >> 64) + y * (uint64_t)(a >> 64);
    return ((__uint128_t)hi << 64) | ((carry << 32) | (uint32_t)lo);
}

/* March's erased Float slots carry a pointer to a boxed binary64 value. */
typedef struct {
    int64_t refs;
    int32_t tag;
    int32_t pad;
    double value;
} FloatBox;

void *march_alloc_float(double value) {
    FloatBox *box = march_alloc(sizeof(*box));
    *box = (FloatBox){ .refs = 1, .tag = -3, .value = value };
    return box;
}

double march_unbox_float(const void *box) {
    return ((const FloatBox *)box)->value;
}
