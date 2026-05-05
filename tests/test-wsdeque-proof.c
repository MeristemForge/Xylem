/*
 * Deterministic proof that wsdeque_pop line 93 has a bug.
 *
 * We directly manipulate atomic top to simulate a stealer winning
 * the CAS race, then verify that pop leaves the deque in a broken state.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Inline the struct to directly poke top */
typedef struct mco_coro mco_coro;

typedef struct {
    _Atomic int64_t bottom;
    _Atomic int64_t top;
    mco_coro**      buffer;
    int64_t         mask;
} wsdeque_exposed_t;

int main(void) {
    /* Allocate deque with 16 slots */
    wsdeque_exposed_t dq;
    int64_t cap = 16;
    dq.buffer = (mco_coro**)calloc((size_t)cap, sizeof(mco_coro*));
    dq.mask = cap - 1;
    atomic_store(&dq.bottom, 0);
    atomic_store(&dq.top, 0);

    mco_coro* sentinel = (mco_coro*)0xCAFEBABE;

    /* Push 6 elements (top=0, bottom=6) then steal 5 (top=5, bottom=6) */
    for (int i = 0; i < 6; i++) {
        int64_t b = atomic_load(&dq.bottom);
        dq.buffer[b & dq.mask] = sentinel;
        atomic_store(&dq.bottom, b + 1);
    }
    for (int i = 0; i < 5; i++) {
        int64_t t = atomic_load(&dq.top);
        atomic_store(&dq.top, t + 1);
    }

    /* State: top=5, bottom=6, one element at buffer[5] */
    printf("Before pop: top=%lld bottom=%lld\n",
           (long long)atomic_load(&dq.top),
           (long long)atomic_load(&dq.bottom));

    /* === Simulate wsdeque_pop with a stealer winning === */

    /* Owner pop: b = bottom - 1 = 5 */
    int64_t b = atomic_load_explicit(&dq.bottom, memory_order_relaxed) - 1;
    atomic_store_explicit(&dq.bottom, b, memory_order_seq_cst);

    int64_t t = atomic_load_explicit(&dq.top, memory_order_acquire);
    /* t=5, b=5 */

    printf("Pop: b=%lld t=%lld (t==b, last element path)\n",
           (long long)b, (long long)t);

    /* Read the element */
    mco_coro* co = dq.buffer[b & dq.mask];

    /* t == b: last element, need CAS */
    /* SIMULATE: stealer wins first, advancing top to 6 */
    printf("Simulating stealer winning CAS(top, 5→6)...\n");
    atomic_store(&dq.top, 6); /* stealer won */

    /* Now owner's CAS fails: atomic_compare_exchange_strong(&top, &t, t+1)
     * On failure, t is updated to current value of top = 6 */
    int64_t expected = t; /* 5 */
    int64_t desired = t + 1; /* 6 */
    _Bool cas_ok = atomic_compare_exchange_strong_explicit(
        &dq.top, &expected, desired,
        memory_order_seq_cst, memory_order_relaxed);

    printf("Owner CAS result: %s, t(expected) now = %lld\n",
           cas_ok ? "SUCCESS" : "FAILED", (long long)expected);

    if (!cas_ok) {
        co = NULL; /* correct: owner didn't get the element */
    }

    /* LINE 93 BUG: bottom = t + 1 (using modified t!) */
    t = expected; /* this is what the code does implicitly */
    int64_t buggy_bottom = t + 1;
    int64_t correct_bottom = b + 1;

    printf("\n=== RESULTS ===\n");
    printf("Buggy   bottom = t + 1 = %lld + 1 = %lld\n",
           (long long)t, (long long)buggy_bottom);
    printf("Correct bottom = b + 1 = %lld + 1 = %lld\n",
           (long long)b, (long long)correct_bottom);
    printf("Current top = %lld\n",
           (long long)atomic_load(&dq.top));

    /* Apply buggy value */
    atomic_store(&dq.bottom, buggy_bottom);
    int64_t final_top = atomic_load(&dq.top);
    int64_t final_bottom = atomic_load(&dq.bottom);

    printf("\nWith buggy fix: top=%lld bottom=%lld → size=%lld\n",
           (long long)final_top, (long long)final_bottom,
           (long long)(final_bottom - final_top));

    if (final_bottom > final_top) {
        printf("BUG CONFIRMED: deque thinks it has %lld phantom element(s)!\n",
               (long long)(final_bottom - final_top));
        printf("buffer[%lld] = %p (stale data)\n",
               (long long)(final_top & dq.mask),
               (void*)dq.buffer[final_top & dq.mask]);
        free(dq.buffer);
        return 1;
    }

    /* Apply correct value */
    atomic_store(&dq.bottom, correct_bottom);
    final_top = atomic_load(&dq.top);
    final_bottom = atomic_load(&dq.bottom);
    printf("\nWith correct fix: top=%lld bottom=%lld → size=%lld ✓\n",
           (long long)final_top, (long long)final_bottom,
           (long long)(final_bottom - final_top));

    free(dq.buffer);
    return 0;
}
