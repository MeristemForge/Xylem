#include "xylem.h"
#include "assert.h"

#define THREAD_COUNT 10
#define STRESS_THREADS 20
#define OPERATIONS_PER_THREAD 1000

static atomic_int worker_completed = 0;
static atomic_int worker_started = 0;
static atomic_int stress_counter = 0;

static int worker_thread(void* arg) {
    xylem_waitgroup_t* wg = (xylem_waitgroup_t*)arg;
    atomic_fetch_add(&worker_started, 1);
    struct timespec ts = {.tv_nsec = 1000000};
    thrd_sleep(&ts, NULL);
    atomic_fetch_add(&worker_completed, 1);
    xylem_waitgroup_done(wg);
    return 0;
}

static int early_done_thread(void* arg) {
    xylem_waitgroup_t* wg = (xylem_waitgroup_t*)arg;
    xylem_waitgroup_done(wg);
    struct timespec ts = {.tv_nsec = 5000000};
    thrd_sleep(&ts, NULL);
    return 0;
}

static int worker_thread_completion(void* arg) {
    xylem_waitgroup_t* wg = (xylem_waitgroup_t*)arg;

    int          iterations = 10 + (rand() % 50);
    volatile int dummy = 0;
    for (int i = 0; i < iterations; i++) {
        dummy += i;
    }
    xylem_waitgroup_done(wg);
    return 0;
}

static void test_create_destroy(void) {
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);
    xylem_waitgroup_destroy(wg);
}

static void test_basic_operations(void) {
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);
    xylem_waitgroup_add(wg, 0);
    xylem_waitgroup_done(wg);
    xylem_waitgroup_add(wg, 3);
    xylem_waitgroup_done(wg);
    xylem_waitgroup_done(wg);
    xylem_waitgroup_done(wg);
    xylem_waitgroup_wait(wg);
    xylem_waitgroup_destroy(wg);
}

static void test_multiple_threads(void) {
    thrd_t       threads[THREAD_COUNT];
    worker_completed = 0;
    worker_started = 0;

    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);
    xylem_waitgroup_add(wg, THREAD_COUNT);

    for (int i = 0; i < THREAD_COUNT; i++) {
        int result = thrd_create(&threads[i], worker_thread, wg);
        ASSERT(result == thrd_success);
    }
    while (atomic_load(&worker_started) < THREAD_COUNT) {
        thrd_yield();
    }
    xylem_waitgroup_wait(wg);
    ASSERT(atomic_load(&worker_completed) == THREAD_COUNT);
    xylem_waitgroup_wait(wg);

    for (int i = 0; i < THREAD_COUNT; i++) {
        thrd_join(threads[i], NULL);
    }
    xylem_waitgroup_destroy(wg);
}

static void test_early_done(void) {
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);
    thrd_t thread;
    xylem_waitgroup_add(wg, 1);
    int result = thrd_create(&thread, early_done_thread, wg);
    ASSERT(result == thrd_success);
    xylem_waitgroup_wait(wg);
    thrd_join(thread, NULL);
    xylem_waitgroup_destroy(wg);
}

static void test_repeated_done(void) {
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);
    xylem_waitgroup_done(wg);
    xylem_waitgroup_done(wg);
    xylem_waitgroup_done(wg);
    xylem_waitgroup_wait(wg);
    xylem_waitgroup_destroy(wg);
}

static void test_large_delta(void) {
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);
    const size_t LARGE_COUNT = 1000;
    xylem_waitgroup_add(wg, LARGE_COUNT);
    for (size_t i = 0; i < LARGE_COUNT; i++) {
        xylem_waitgroup_done(wg);
    }
    xylem_waitgroup_wait(wg);
    xylem_waitgroup_destroy(wg);
}

static void test_concurrent_stress(void) {
    srand((unsigned int)time(NULL));
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);
    thrd_t threads[STRESS_THREADS];

    xylem_waitgroup_add(wg, STRESS_THREADS);

    for (int i = 0; i < STRESS_THREADS; i++) {
        int result = thrd_create(&threads[i], worker_thread_completion, wg);
        ASSERT(result == thrd_success);
    }
    xylem_waitgroup_wait(wg);

    for (int i = 0; i < STRESS_THREADS; i++) {
        thrd_join(threads[i], NULL);
    }
    xylem_waitgroup_destroy(wg);
}


int main(void) {
    test_create_destroy();
    test_basic_operations();
    test_repeated_done();
    test_large_delta();
    test_multiple_threads();
    test_early_done();
    test_concurrent_stress();
    return 0;
}