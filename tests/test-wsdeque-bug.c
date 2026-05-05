/*
 * Test to verify wsdeque_pop bug: t+1 should be b+1 on line 93.
 *
 * Strategy: hammer pop+steal on a single-element deque to trigger
 * the CAS failure path. If bottom overflows, a subsequent pop/steal
 * will return a stale (phantom) pointer.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>

#include "runtime/wsdeque.h"

#define ITERATIONS 2000000

static wsdeque_t* g_dq;
static _Atomic int g_running;
static _Atomic int64_t g_phantom_count;

/* Stealer thread: continuously tries to steal. */
static int stealer_fn(void* arg) {
    (void)arg;
    while (atomic_load(&g_running)) {
        mco_coro* co = wsdeque_steal(g_dq);
        if (co != NULL && co != (mco_coro*)0xDEADBEEF) {
            /* Got something that's NOT our sentinel → phantom element! */
            atomic_fetch_add(&g_phantom_count, 1);
        }
    }
    return 0;
}

int main(void) {
    g_dq = wsdeque_create(4); /* 16 slots */
    if (!g_dq) {
        fprintf(stderr, "wsdeque_create failed\n");
        return 1;
    }

    atomic_store(&g_running, 1);
    atomic_store(&g_phantom_count, 0);

    /* Start stealer threads */
    thrd_t stealers[3];
    for (int i = 0; i < 3; i++) {
        thrd_create(&stealers[i], stealer_fn, NULL);
    }

    mco_coro* sentinel = (mco_coro*)0xDEADBEEF;
    int64_t pop_got = 0;
    int64_t steal_got = 0;
    int64_t push_count = 0;

    for (int i = 0; i < ITERATIONS; i++) {
        /* Push one element */
        if (wsdeque_push(g_dq, sentinel) == 0) {
            push_count++;
        }
        /* Immediately try to pop (races with stealers on the last element) */
        mco_coro* co = wsdeque_pop(g_dq);
        if (co == sentinel) {
            pop_got++;
        }
    }

    atomic_store(&g_running, 0);
    for (int i = 0; i < 3; i++) {
        thrd_join(stealers[i], NULL);
    }

    int64_t phantoms = atomic_load(&g_phantom_count);
    printf("push=%lld pop_got=%lld phantoms_from_steal=%lld\n",
           (long long)push_count, (long long)pop_got, (long long)phantoms);

    /* The real check: after all operations, deque should be empty.
     * If the bug exists, bottom > top (phantom elements). */
    mco_coro* stale = wsdeque_pop(g_dq);
    if (stale != NULL) {
        printf("BUG CONFIRMED: pop after drain returned %p (expected NULL)\n",
               (void*)stale);
        wsdeque_destroy(g_dq);
        return 1;
    }

    stale = wsdeque_steal(g_dq);
    if (stale != NULL) {
        printf("BUG CONFIRMED: steal after drain returned %p (expected NULL)\n",
               (void*)stale);
        wsdeque_destroy(g_dq);
        return 1;
    }

    printf("OK: deque is empty after drain (no phantom elements detected)\n");
    wsdeque_destroy(g_dq);
    return 0;
}
