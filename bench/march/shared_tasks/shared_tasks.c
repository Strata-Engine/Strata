/* The C of shared_tasks.march: TASKS threads read one shared struct (VARIANT 0
 * and 2) or each its own copy (VARIANT 1). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

typedef struct { int64_t *arr; int64_t mask, pad; } reg_t;
typedef struct { const reg_t *r; int variant; int64_t reps, sum; } job_t;

__attribute__((noinline)) static int64_t through(const reg_t *r, int64_t n) {
    int64_t acc = 0;
    for (int64_t i = 0; i < n; i++) acc += r->arr[i & r->mask];
    return acc;
}
static void *work(void *p) {
    job_t *j = p;
    if (j->variant == 1) {
        reg_t own = *j->r;
        own.arr = malloc(1024 * sizeof(int64_t));
        memcpy(own.arr, j->r->arr, 1024 * sizeof(int64_t));
        j->sum = through(&own, j->reps * 4096);
    } else j->sum = through(j->r, j->reps * 4096);
    return NULL;
}

int main(void) {
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 1;
    int variant = getenv("VARIANT") ? atoi(getenv("VARIANT")) : 0;
    int tasks = getenv("TASKS") ? atoi(getenv("TASKS")) : 8;
    reg_t r = { malloc(1024 * sizeof(int64_t)), 1023, 0 };
    for (int k = 0; k < 1024; k++) r.arr[k] = 1;
    pthread_t th[64]; job_t jobs[64];
    int64_t acc = 0;
    for (int t = 0; t < tasks; t++) { jobs[t] = (job_t){ &r, variant, reps, 0 }; pthread_create(&th[t], NULL, work, &jobs[t]); }
    for (int t = 0; t < tasks; t++) { pthread_join(th[t], NULL); acc += jobs[t].sum; }
    printf("%lld\n", (long long)acc);
    return 0;
}
