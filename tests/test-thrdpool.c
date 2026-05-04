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

#include "xylem.h"
#include "thrdpool.h"
#include "assert.h"

#include <stdatomic.h>
#include <string.h>

#include <threads.h>

static void _increment(void* arg) {
    atomic_int* counter = (atomic_int*)arg;
    atomic_fetch_add(counter, 1);
}

/* Test: create and immediately destroy an empty pool. */
static void test_create_destroy(void) {
    thrdpool_t* pool = thrdpool_create(4);
    ASSERT(pool != NULL);
    thrdpool_destroy(pool);
}

/* Test: post jobs and verify all are executed. */
static void test_post_jobs(void) {
    enum { N = 100 };
    atomic_int counter = 0;

    thrdpool_t* pool = thrdpool_create(4);
    ASSERT(pool != NULL);

    for (int i = 0; i < N; i++) {
        thrdpool_submit(pool, _increment, (void*)&counter);
    }

    thrdpool_destroy(pool);
    ASSERT(atomic_load(&counter) == N);
}

/* Test: single thread pool works correctly. */
static void test_single_thread(void) {
    enum { N = 50 };
    atomic_int counter = 0;

    thrdpool_t* pool = thrdpool_create(1);
    ASSERT(pool != NULL);

    for (int i = 0; i < N; i++) {
        thrdpool_submit(pool, _increment, (void*)&counter);
    }

    thrdpool_destroy(pool);
    ASSERT(atomic_load(&counter) == N);
}

typedef struct {
    int*        arr;
    int         idx;
    int         val;
    atomic_int* done;
} write_arg_t;

static void _write_value(void* arg) {
    write_arg_t* wa = (write_arg_t*)arg;
    wa->arr[wa->idx] = wa->val;
    atomic_fetch_add(wa->done, 1);
}

/* Test: verify job arguments are passed correctly. */
static void test_job_args(void) {
    enum { N = 20 };
    int        arr[N];
    write_arg_t args[N];
    atomic_int done = 0;

    memset(arr, 0, sizeof(arr));

    thrdpool_t* pool = thrdpool_create(4);
    ASSERT(pool != NULL);

    for (int i = 0; i < N; i++) {
        args[i].arr  = arr;
        args[i].idx  = i;
        args[i].val  = i * 10;
        args[i].done = &done;
        thrdpool_submit(pool, _write_value, &args[i]);
    }

    while (atomic_load(&done) < N) {
        thrd_yield();
    }

    for (int i = 0; i < N; i++) {
        ASSERT(arr[i] == i * 10);
    }

    thrdpool_destroy(pool);
}

/* Test: many threads with many jobs. */
static void test_many_threads(void) {
    enum { N = 200 };
    atomic_int counter = 0;

    thrdpool_t* pool = thrdpool_create(16);
    ASSERT(pool != NULL);

    for (int i = 0; i < N; i++) {
        thrdpool_submit(pool, _increment, (void*)&counter);
    }

    thrdpool_destroy(pool);
    ASSERT(atomic_load(&counter) == N);
}

int main(void) {
    test_create_destroy();
    test_post_jobs();
    test_single_thread();
    test_job_args();
    test_many_threads();
    return 0;
}
