/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *
 *  Xylem sync-primitive microbenchmark.
 *
 *  Benchmarks each coroutine sync primitive (mutex, cond, waitgroup, sem,
 *  channel) and prints a JSON result on stdout. Because xylem's sync
 *  primitives are *context-adaptive* (a blocking op parks a coroutine or
 *  blocks an OS thread, and the two interoperate), the same worker code runs
 *  in three concurrency modes:
 *
 *    --mode coro    workers are coroutines (xylem_spawn)            [default]
 *    --mode thread  workers are plain OS threads
 *    --mode mixed   half coroutines, half OS threads, sharing one primitive
 *
 *  Go has only goroutines (coro). Rust offers coro (Tokio) and thread (std),
 *  but cannot mix the two on one primitive -- that is the case xylem covers
 *  uniquely.
 *
 *  Usage:
 *    sync-xylem <primitive> [--mode coro|thread|mixed]
 *                           [--workers W] [--tasks T] [--iters N] [--permits K]
 *
 *  Workload model (identical across languages and modes):
 *    mutex      : T workers each do N lock/inc/unlock  -> ops = T*N
 *    cond       : 1 producer + 1 consumer, N hand-offs -> ops = N
 *    waitgroup  : N rounds, each spawns T workers that done() -> ops = T*N
 *    sem        : T workers each do N wait/post, K permits -> ops = T*N
 *    channel    : T senders each send N msgs, 1 receiver -> ops = T*N
 */

#include "xylem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

/* ----------------------------- OS-thread shim ----------------------------- */
/* Used by the thread / mixed modes. Workers call the same xylem primitives;
 * only the execution vehicle (coroutine vs OS thread) changes. */
#ifdef _WIN32
#include <windows.h>
typedef HANDLE bench_thread_t;
typedef struct { void (*fn)(void*); void* arg; } thunk_t;
static DWORD WINAPI _thread_tramp(LPVOID p) {
    thunk_t* t = (thunk_t*)p;
    void (*fn)(void*) = t->fn; void* a = t->arg; free(t);
    fn(a); return 0;
}
static bench_thread_t thread_spawn(void (*fn)(void*), void* arg) {
    thunk_t* t = (thunk_t*)malloc(sizeof(*t)); t->fn = fn; t->arg = arg;
    return CreateThread(NULL, 0, _thread_tramp, t, 0, NULL);
}
static void thread_join(bench_thread_t h) {
    WaitForSingleObject(h, INFINITE); CloseHandle(h);
}
#else
#include <pthread.h>
typedef pthread_t bench_thread_t;
typedef struct { void (*fn)(void*); void* arg; } thunk_t;
static void* _thread_tramp(void* p) {
    thunk_t* t = (thunk_t*)p;
    void (*fn)(void*) = t->fn; void* a = t->arg; free(t);
    fn(a); return NULL;
}
static bench_thread_t thread_spawn(void (*fn)(void*), void* arg) {
    thunk_t* t = (thunk_t*)malloc(sizeof(*t)); t->fn = fn; t->arg = arg;
    pthread_t th; pthread_create(&th, NULL, _thread_tramp, t); return th;
}
static void thread_join(bench_thread_t th) { pthread_join(th, NULL); }
#endif

typedef enum { M_CORO, M_THREAD, M_MIXED } mode_t;
typedef enum { P_MUTEX, P_COND, P_WAITGROUP, P_SEM, P_CHANNEL } prim_t;

static struct {
    prim_t      prim;
    const char* name;
    mode_t      mode;
    const char* mode_name;
    int         workers;
    long        tasks;
    long        iters;
    long        permits;

    xylem_mutex_t*     mtx;
    xylem_cond_t*      cond;
    xylem_sem_t*       sem;
    xylem_channel_t*   ch;
    xylem_waitgroup_t* wg;        /* joins the worker coroutines */
    xylem_waitgroup_t* recv_wg;   /* channel: joins the receiver  */

    volatile uint64_t counter;    /* mutex: contended accumulator */
    int               cond_turn;  /* cond: 0 = producer, 1 = consumer */

    uint64_t total_ops;
    uint64_t elapsed_ns;
} G;

static int g_channel_payload;
#define CHANNEL_MSG ((void*)&g_channel_payload)

static uint64_t now_ns(void) {
    return xylem_utils_getnow(XYLEM_TIME_PRECISION_NSEC);
}

/* ----------------------------- worker launcher ---------------------------- */
/* A collection of OS-thread handles spawned for a run, so they can be joined
 * after the workload completes. */
typedef struct { bench_thread_t* h; int n; } thr_set_t;

/* Does worker #idx run on an OS thread under the active mode? */
static int idx_uses_thread(long idx) {
    switch (G.mode) {
        case M_CORO:   return 0;
        case M_THREAD: return 1;
        case M_MIXED:  return (int)(idx & 1);   /* alternate coro / thread */
    }
    return 0;
}

static void spawn_one(void (*fn)(void*), void* arg, long idx, thr_set_t* ts) {
    if (idx_uses_thread(idx)) ts->h[ts->n++] = thread_spawn(fn, arg);
    else                      xylem_spawn(fn, arg);
}

static void join_set(thr_set_t* ts) {
    for (int i = 0; i < ts->n; i++) thread_join(ts->h[i]);
    ts->n = 0;
}

static thr_set_t thr_alloc(long cap) {
    thr_set_t ts;
    ts.h = (bench_thread_t*)malloc(sizeof(bench_thread_t) * (size_t)(cap > 0 ? cap : 1));
    ts.n = 0;
    return ts;
}

/* ------------------------------------------------------------------ mutex */

static void mutex_worker(void* arg) {
    (void)arg;
    long n = G.iters;
    for (long i = 0; i < n; i++) {
        xylem_mutex_lock(G.mtx);
        G.counter++;
        xylem_mutex_unlock(G.mtx);
    }
    xylem_waitgroup_done(G.wg);
}

static void run_mutex(void) {
    G.mtx = xylem_mutex_create();
    G.wg  = xylem_waitgroup_create();
    G.counter = 0;
    thr_set_t ts = thr_alloc(G.tasks);

    xylem_waitgroup_add(G.wg, (size_t)G.tasks);
    uint64_t t0 = now_ns();
    for (long i = 0; i < G.tasks; i++) spawn_one(mutex_worker, NULL, i, &ts);
    xylem_waitgroup_wait(G.wg);
    uint64_t t1 = now_ns();
    join_set(&ts);

    G.elapsed_ns = t1 - t0;
    G.total_ops  = (uint64_t)G.tasks * (uint64_t)G.iters;

    if (G.counter != G.total_ops) {
        fprintf(stderr, "mutex: counter mismatch %" PRIu64 " != %" PRIu64 "\n",
                G.counter, G.total_ops);
    }

    free(ts.h);
    xylem_waitgroup_destroy(G.wg);
    xylem_mutex_destroy(G.mtx);
}

/* ------------------------------------------------------------------- cond */

static void cond_producer(void* arg) {
    (void)arg;
    long n = G.iters;
    for (long i = 0; i < n; i++) {
        xylem_mutex_lock(G.mtx);
        while (G.cond_turn != 0) xylem_cond_wait(G.cond, G.mtx);
        G.cond_turn = 1;
        xylem_cond_signal(G.cond);
        xylem_mutex_unlock(G.mtx);
    }
    xylem_waitgroup_done(G.wg);
}

static void cond_consumer(void* arg) {
    (void)arg;
    long n = G.iters;
    for (long i = 0; i < n; i++) {
        xylem_mutex_lock(G.mtx);
        while (G.cond_turn != 1) xylem_cond_wait(G.cond, G.mtx);
        G.cond_turn = 0;
        xylem_cond_signal(G.cond);
        xylem_mutex_unlock(G.mtx);
    }
    xylem_waitgroup_done(G.wg);
}

static void run_cond(void) {
    G.mtx  = xylem_mutex_create();
    G.cond = xylem_cond_create();
    G.wg   = xylem_waitgroup_create();
    G.cond_turn = 0;
    thr_set_t ts = thr_alloc(2);

    xylem_waitgroup_add(G.wg, 2);
    uint64_t t0 = now_ns();
    /* idx 0 = producer, idx 1 = consumer; in mixed mode producer is a
     * coroutine and consumer an OS thread, so they hand off across contexts. */
    spawn_one(cond_consumer, NULL, 1, &ts);
    spawn_one(cond_producer, NULL, 0, &ts);
    xylem_waitgroup_wait(G.wg);
    uint64_t t1 = now_ns();
    join_set(&ts);

    G.elapsed_ns = t1 - t0;
    G.total_ops  = (uint64_t)G.iters;

    free(ts.h);
    xylem_waitgroup_destroy(G.wg);
    xylem_cond_destroy(G.cond);
    xylem_mutex_destroy(G.mtx);
}

/* -------------------------------------------------------------- waitgroup */

static void wg_worker(void* arg) {
    xylem_waitgroup_done((xylem_waitgroup_t*)arg);
}

static void run_waitgroup(void) {
    long rounds = G.iters;
    thr_set_t ts = thr_alloc(G.tasks);

    uint64_t t0 = now_ns();
    for (long r = 0; r < rounds; r++) {
        xylem_waitgroup_t* w = xylem_waitgroup_create();
        xylem_waitgroup_add(w, (size_t)G.tasks);
        ts.n = 0;
        for (long t = 0; t < G.tasks; t++) spawn_one(wg_worker, w, t, &ts);
        xylem_waitgroup_wait(w);
        join_set(&ts);
        xylem_waitgroup_destroy(w);
    }
    uint64_t t1 = now_ns();

    G.elapsed_ns = t1 - t0;
    G.total_ops  = (uint64_t)rounds * (uint64_t)G.tasks;
    free(ts.h);
}

/* -------------------------------------------------------------------- sem */

static void sem_worker(void* arg) {
    (void)arg;
    long n = G.iters;
    for (long i = 0; i < n; i++) {
        xylem_sem_wait(G.sem);
        xylem_sem_post(G.sem);
    }
    xylem_waitgroup_done(G.wg);
}

static void run_sem(void) {
    G.sem = xylem_sem_create((unsigned int)G.permits);
    G.wg  = xylem_waitgroup_create();
    thr_set_t ts = thr_alloc(G.tasks);

    xylem_waitgroup_add(G.wg, (size_t)G.tasks);
    uint64_t t0 = now_ns();
    for (long i = 0; i < G.tasks; i++) spawn_one(sem_worker, NULL, i, &ts);
    xylem_waitgroup_wait(G.wg);
    uint64_t t1 = now_ns();
    join_set(&ts);

    G.elapsed_ns = t1 - t0;
    G.total_ops  = (uint64_t)G.tasks * (uint64_t)G.iters;

    free(ts.h);
    xylem_waitgroup_destroy(G.wg);
    xylem_sem_destroy(G.sem);
}

/* ---------------------------------------------------------------- channel */

static void chan_sender(void* arg) {
    (void)arg;
    long n = G.iters;
    for (long i = 0; i < n; i++) {
        while (xylem_channel_send(G.ch, CHANNEL_MSG) != 0) {
            xylem_sleep(0);
        }
    }
    xylem_waitgroup_done(G.wg);
}

static void chan_receiver(void* arg) {
    (void)arg;
    uint64_t got = 0;
    for (;;) {
        void* msg = xylem_channel_recv(G.ch);
        if (!msg) break;
        got++;
    }
    G.counter = got;
    xylem_waitgroup_done(G.recv_wg);
}

static void run_channel(void) {
    /* create() is coroutine-only -- run_channel executes in the root
     * coroutine, so this is valid even in thread/mixed mode. */
    G.ch      = xylem_channel_create();
    G.wg      = xylem_waitgroup_create();
    G.recv_wg = xylem_waitgroup_create();
    G.counter = 0;
    thr_set_t ts = thr_alloc(G.tasks + 1);

    xylem_waitgroup_add(G.recv_wg, 1);
    xylem_waitgroup_add(G.wg, (size_t)G.tasks);

    uint64_t t0 = now_ns();
    spawn_one(chan_receiver, NULL, G.tasks, &ts);   /* receiver idx = tasks */
    for (long i = 0; i < G.tasks; i++) spawn_one(chan_sender, NULL, i, &ts);
    xylem_waitgroup_wait(G.wg);          /* all senders done */
    xylem_channel_close(G.ch);           /* let the receiver finish */
    xylem_waitgroup_wait(G.recv_wg);     /* receiver drained */
    uint64_t t1 = now_ns();
    join_set(&ts);

    G.elapsed_ns = t1 - t0;
    G.total_ops  = (uint64_t)G.tasks * (uint64_t)G.iters;

    if (G.counter != G.total_ops) {
        fprintf(stderr, "channel: recv mismatch %" PRIu64 " != %" PRIu64 "\n",
                G.counter, G.total_ops);
    }

    free(ts.h);
    xylem_waitgroup_destroy(G.recv_wg);
    xylem_waitgroup_destroy(G.wg);
    xylem_channel_destroy(G.ch);
}

/* ----------------------------------------------------------------- output */

static void print_result(void) {
    double sec  = G.elapsed_ns / 1e9;
    double ops  = (sec > 0.0) ? (double)G.total_ops / sec : 0.0;
    double nspo = (G.total_ops) ? (double)G.elapsed_ns / (double)G.total_ops
                                : 0.0;

    printf("{\n");
    printf("  \"primitive\": \"%s\",\n", G.name);
    printf("  \"lang\": \"xylem\",\n");
    printf("  \"mode\": \"%s\",\n", G.mode_name);
    printf("  \"workers\": %d,\n", G.workers);
    printf("  \"tasks\": %ld,\n", G.tasks);
    printf("  \"iters\": %ld,\n", G.iters);
    if (G.prim == P_SEM) printf("  \"permits\": %ld,\n", G.permits);
    printf("  \"total_ops\": %" PRIu64 ",\n", G.total_ops);
    printf("  \"duration_sec\": %.6f,\n", sec);
    printf("  \"ops_per_sec\": %.0f,\n", ops);
    printf("  \"ns_per_op\": %.2f\n", nspo);
    printf("}\n");
}

/* ----------------------------------------------------------------- driver */

static void bench_main(void* arg) {
    (void)arg;
    switch (G.prim) {
        case P_MUTEX:     run_mutex();     break;
        case P_COND:      run_cond();      break;
        case P_WAITGROUP: run_waitgroup(); break;
        case P_SEM:       run_sem();       break;
        case P_CHANNEL:   run_channel();   break;
    }
    print_result();
}

static int parse_prim(const char* s) {
    if (!strcmp(s, "mutex"))     { G.prim = P_MUTEX;     G.name = "mutex";     return 0; }
    if (!strcmp(s, "cond"))      { G.prim = P_COND;      G.name = "cond";      return 0; }
    if (!strcmp(s, "waitgroup")) { G.prim = P_WAITGROUP; G.name = "waitgroup"; return 0; }
    if (!strcmp(s, "sem"))       { G.prim = P_SEM;       G.name = "sem";       return 0; }
    if (!strcmp(s, "channel"))   { G.prim = P_CHANNEL;   G.name = "channel";   return 0; }
    return -1;
}

static int parse_mode(const char* s) {
    if (!strcmp(s, "coro"))   { G.mode = M_CORO;   G.mode_name = "coro";   return 0; }
    if (!strcmp(s, "thread")) { G.mode = M_THREAD; G.mode_name = "thread"; return 0; }
    if (!strcmp(s, "mixed"))  { G.mode = M_MIXED;  G.mode_name = "mixed";  return 0; }
    return -1;
}

static void usage(const char* prog) {
    fprintf(stderr,
        "usage: %s <mutex|cond|waitgroup|sem|channel> "
        "[--mode coro|thread|mixed] "
        "[--workers W] [--tasks T] [--iters N] [--permits K]\n", prog);
}

int main(int argc, char** argv) {
    G.mode = M_CORO; G.mode_name = "coro";
    G.workers = 0;
    G.tasks   = 8;
    G.iters   = 100000;
    G.permits = 4;

    if (argc < 2 || parse_prim(argv[1]) != 0) {
        usage(argv[0]);
        return 2;
    }

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            if (parse_mode(argv[++i]) != 0) { usage(argv[0]); return 2; }
        } else if (!strcmp(argv[i], "--workers") && i + 1 < argc) {
            G.workers = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--tasks") && i + 1 < argc) {
            G.tasks = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
            G.iters = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--permits") && i + 1 < argc) {
            G.permits = atol(argv[++i]);
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (G.tasks < 1)   G.tasks = 1;
    if (G.iters < 1)   G.iters = 1;
    if (G.permits < 1) G.permits = 1;

    xylem_opts_t rt = {0};
    rt.workers = G.workers;
    xylem_run(bench_main, NULL, &rt);
    return 0;
}
