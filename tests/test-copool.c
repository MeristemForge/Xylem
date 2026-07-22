/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */

#include "assert.h"
#include "xylem/xylem-threads.h"

#include "runtime/copool.h"

#include <stdint.h>

#define TEST_SHARED_THREADS 4
#define TEST_SHARED_SLOTS   64

typedef struct {
    copool_shared_t* pool;
    copool_slot_t*   slots;
    int              count;
} _shared_worker_t;

static int _shared_put_thread(void* arg) {
    _shared_worker_t* worker = (_shared_worker_t*)arg;
    int count = copool_shared_put(
        worker->pool,
        worker->slots,
        worker->count);
    return count == worker->count ? 0 : -1;
}

static void test_local_default_capacity(void) {
    copool_local_t* pool = copool_local_create(0);
    ASSERT(pool != NULL);
    ASSERT(copool_local_capacity(pool) == COPOOL_LOCAL_DEFAULT_CAP);

    int values[COPOOL_LOCAL_DEFAULT_CAP + 1];
    copool_slot_t slots[COPOOL_LOCAL_DEFAULT_CAP + 1];
    for (int i = 0; i < COPOOL_LOCAL_DEFAULT_CAP + 1; i++) {
        slots[i].ptr   = &values[i];
        slots[i].state =
            (i & 1) ? COPOOL_SLOT_REUSABLE : COPOOL_SLOT_FRESH;
    }

    ASSERT(
        copool_local_put(
            pool,
            slots,
            COPOOL_LOCAL_DEFAULT_CAP + 1) == COPOOL_LOCAL_DEFAULT_CAP);

    copool_slot_t acquired[COPOOL_LOCAL_DEFAULT_CAP];
    ASSERT(
        copool_local_take(
            pool,
            acquired,
            COPOOL_LOCAL_DEFAULT_CAP) == COPOOL_LOCAL_DEFAULT_CAP);
    for (int i = 0; i < COPOOL_LOCAL_DEFAULT_CAP; i++) {
        int source = COPOOL_LOCAL_DEFAULT_CAP - 1 - i;
        ASSERT(acquired[i].ptr == slots[source].ptr);
        ASSERT(acquired[i].state == slots[source].state);
    }

    copool_local_destroy(pool);
}

static void test_local_custom_capacity(void) {
    copool_local_t* pool = copool_local_create(3);
    ASSERT(pool != NULL);
    ASSERT(copool_local_capacity(pool) == 3);

    int values[4];
    copool_slot_t slots[4] = {
        {.ptr = &values[0], .state = COPOOL_SLOT_FRESH},
        {.ptr = &values[1], .state = COPOOL_SLOT_REUSABLE},
        {.ptr = &values[2], .state = COPOOL_SLOT_FRESH},
        {.ptr = &values[3], .state = COPOOL_SLOT_REUSABLE},
    };
    ASSERT(copool_local_put(pool, slots, 4) == 3);

    copool_slot_t acquired;
    ASSERT(copool_local_take(pool, &acquired, 1) == 1);
    ASSERT(acquired.ptr == slots[2].ptr);
    ASSERT(acquired.state == COPOOL_SLOT_FRESH);

    copool_local_destroy(pool);
    ASSERT(copool_local_create(-1) == NULL);
}

static void test_shared_unbounded_lifo(void) {
    copool_shared_t* pool = copool_shared_create();
    ASSERT(pool != NULL);

    int values[TEST_SHARED_SLOTS * 2];
    copool_slot_t slots[TEST_SHARED_SLOTS * 2];
    for (int i = 0; i < TEST_SHARED_SLOTS * 2; i++) {
        slots[i].ptr   = &values[i];
        slots[i].state =
            (i & 1) ? COPOOL_SLOT_REUSABLE : COPOOL_SLOT_FRESH;
    }
    ASSERT(
        copool_shared_put(
            pool,
            slots,
            TEST_SHARED_SLOTS * 2) == TEST_SHARED_SLOTS * 2);

    copool_slot_t acquired[TEST_SHARED_SLOTS * 2];
    ASSERT(
        copool_shared_take(
            pool,
            acquired,
            TEST_SHARED_SLOTS * 2) == TEST_SHARED_SLOTS * 2);
    for (int i = 0; i < TEST_SHARED_SLOTS * 2; i++) {
        int source = TEST_SHARED_SLOTS * 2 - 1 - i;
        ASSERT(acquired[i].ptr == slots[source].ptr);
        ASSERT(acquired[i].state == slots[source].state);
    }
    ASSERT(copool_shared_take(pool, acquired, 1) == 0);

    copool_shared_destroy(pool);
}

static void test_shared_concurrent_put(void) {
    copool_shared_t* pool = copool_shared_create();
    ASSERT(pool != NULL);

    int values[TEST_SHARED_THREADS][TEST_SHARED_SLOTS];
    copool_slot_t slots[TEST_SHARED_THREADS][TEST_SHARED_SLOTS];
    _shared_worker_t workers[TEST_SHARED_THREADS];
    thrd_t threads[TEST_SHARED_THREADS];

    for (int i = 0; i < TEST_SHARED_THREADS; i++) {
        for (int j = 0; j < TEST_SHARED_SLOTS; j++) {
            slots[i][j].ptr   = &values[i][j];
            slots[i][j].state =
                (j & 1) ? COPOOL_SLOT_REUSABLE : COPOOL_SLOT_FRESH;
        }
        workers[i].pool  = pool;
        workers[i].slots = slots[i];
        workers[i].count = TEST_SHARED_SLOTS;
        ASSERT(
            thrd_create(
                &threads[i],
                _shared_put_thread,
                &workers[i]) == thrd_success);
    }
    for (int i = 0; i < TEST_SHARED_THREADS; i++) {
        int result = -1;
        ASSERT(thrd_join(threads[i], &result) == thrd_success);
        ASSERT(result == 0);
    }

    copool_slot_t acquired[TEST_SHARED_THREADS * TEST_SHARED_SLOTS];
    int count = copool_shared_take(
        pool,
        acquired,
        TEST_SHARED_THREADS * TEST_SHARED_SLOTS);
    ASSERT(count == TEST_SHARED_THREADS * TEST_SHARED_SLOTS);
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < i; j++) {
            ASSERT(acquired[i].ptr != acquired[j].ptr);
        }
    }

    copool_shared_destroy(pool);
}

static void test_null_arguments(void) {
    copool_slot_t slot = {
        .ptr   = (void*)(uintptr_t)1,
        .state = COPOOL_SLOT_REUSABLE,
    };

    ASSERT(copool_local_capacity(NULL) == 0);
    ASSERT(copool_local_take(NULL, &slot, 1) == 0);
    ASSERT(copool_local_put(NULL, &slot, 1) == 0);
    ASSERT(copool_shared_take(NULL, &slot, 1) == 0);
    ASSERT(copool_shared_put(NULL, &slot, 1) == 0);
    copool_local_destroy(NULL);
    copool_shared_destroy(NULL);
}

int main(void) {
    test_local_default_capacity();
    test_local_custom_capacity();
    test_shared_unbounded_lifo();
    test_shared_concurrent_put();
    test_null_arguments();
    return 0;
}
