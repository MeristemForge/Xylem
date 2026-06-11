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
 *    waitgroup  : N rounds over a pre-spawned pool of T workers -> ops = T*N
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
 * only the execution vehicle (coroutine vs OS thread) changes. Threads come
 * from xylem's public C11 <threads.h> wrapper, the same one the unit tests
 * use, so there is no per-OS thread code here. */
#include "xylem/xylem-threads.h"

typedef thrd_t bench_thread_t;
typedef struct { void (*fn)(void*); void* arg; } thunk_t;

/* Adapt a void(*)(void*) worker to the int(*)(void*) thrd_start_t signature. */
static int _thread_tramp(void* p) {
    thunk_t* t = (thunk_t*)p;
    void (*fn)(void*) = t->fn; void* a = t->arg; free(t);
    fn(a); return 0;
}
static bench_thread_t thread_spawn(void (*fn)(void*), void* arg) {
    thunk_t* t = (thunk_t*)malloc(sizeof(*t)); t->fn = fn; t->arg = arg;
    thrd_t th; thrd_create(&th, _thread_tramp, t); return th;
}
static void thread_join(bench_thread_t th) { thrd_join(th, NULL); }

typedef enum { M_CORO, M_THREAD, M_MIXED } mode_t;
typedef enum { P_MUTEX, P_COND, P_WAITGROUP, P_SEM, P_CHANNEL, P_HANDOFF } prim_t;

static struct {
    prim_t      prim;
    const char* name;
    mode_t      mode;
    const char* mode_name;
    int         workers;
    long        tasks;
    long        iters;
    long        permits;
    int         chan_c2t;   /* channel mixed: 0 = thread->coro, 1 = coro->thread */
    int         ho_a_thr;   /* handoff: party A on an OS thread? */
    int         ho_b_thr;   /* handoff: party B on an OS thread? */

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

/* Spawn with an explicit execution vehicle, bypassing the idx-parity rule.
 * The channel mixed mode uses this to pin a clean cross-context direction
 * (all senders one context, the receiver the other). */
static void spawn_forced(void (*fn)(void*), void* arg, int use_thread,
                         thr_set_t* ts) {
    if (use_thread) ts->h[ts->n++] = thread_spawn(fn, arg);
    else            xylem_spawn(fn, arg);
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
/*
 * Isolated waitgroup benchmark: measures ONLY the waitgroup primitive
 * (add/done/wait + the park/wake handoff), never task creation. A fixed
 * pool of T workers is spawned once, outside the timed region, and loops
 * over the rounds. Each round uses a fresh pair of single-use waitgroups
 * -- `gate[r]` (main opens it to release the pool) and `fin[r]` (the pool
 * signals completion, main joins) -- so there is no reuse-contract hazard
 * and no allocation inside the measured loop. The timed loop is purely:
 *   main:    fin.Add(T); gate.Done();         fin.Wait()
 *   worker:               gate.Wait(); fin.Done()
 */
typedef struct {
    xylem_waitgroup_t** gate;
    xylem_waitgroup_t** fin;
    long                rounds;
} wg_pool_ctx_t;

static void wg_pool_worker(void* arg) {
    wg_pool_ctx_t* c = (wg_pool_ctx_t*)arg;
    for (long r = 0; r < c->rounds; r++) {
        xylem_waitgroup_wait(c->gate[r]);   /* wait for round r to open */
        xylem_waitgroup_done(c->fin[r]);    /* signal this worker is done */
    }
}

static void run_waitgroup(void) {
    long      rounds = G.iters;
    long      T      = G.tasks;
    thr_set_t ts     = thr_alloc(T);

    /* Pre-allocate the per-round gate + fin waitgroups and pre-arm the
     * gates (closed). Each instance is used exactly once, so reuse is never
     * an issue. All of this -- and the worker spawn below -- is OUTSIDE the
     * timed region, so task-creation cost never enters the measurement. */
    xylem_waitgroup_t** gate =
        (xylem_waitgroup_t**)malloc(sizeof(*gate) * (size_t)rounds);
    xylem_waitgroup_t** fin =
        (xylem_waitgroup_t**)malloc(sizeof(*fin) * (size_t)rounds);
    for (long r = 0; r < rounds; r++) {
        gate[r] = xylem_waitgroup_create();
        fin[r]  = xylem_waitgroup_create();
        xylem_waitgroup_add(gate[r], 1); /* gate starts closed */
    }

    wg_pool_ctx_t ctx = { gate, fin, rounds };
    ts.n = 0;
    for (long t = 0; t < T; t++) spawn_one(wg_pool_worker, &ctx, t, &ts);

    uint64_t t0 = now_ns();
    for (long r = 0; r < rounds; r++) {
        xylem_waitgroup_add(fin[r], (size_t)T); /* expect T workers   */
        xylem_waitgroup_done(gate[r]);          /* open: release pool */
        xylem_waitgroup_wait(fin[r]);           /* join the pool      */
    }
    uint64_t t1 = now_ns();

    join_set(&ts); /* workers have finished the last round; reap threads */

    for (long r = 0; r < rounds; r++) {
        xylem_waitgroup_destroy(gate[r]);
        xylem_waitgroup_destroy(fin[r]);
    }
    free(gate);
    free(fin);
    free(ts.h);

    G.elapsed_ns = t1 - t0;
    G.total_ops  = (uint64_t)rounds * (uint64_t)T;
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
     * coroutine, so this is valid even in thread/mixed mode. Unbounded
     * (cap 0): send never reports full, an MPSC matching the Go/Rust
     * channels this benchmark compares against. */
    G.ch      = xylem_channel_create(0);
    G.wg      = xylem_waitgroup_create();
    G.recv_wg = xylem_waitgroup_create();
    G.counter = 0;
    thr_set_t ts = thr_alloc(G.tasks + 1);

    xylem_waitgroup_add(G.recv_wg, 1);
    xylem_waitgroup_add(G.wg, (size_t)G.tasks);

    uint64_t t0 = now_ns();
    if (G.mode == M_MIXED) {
        /* Pure single-direction cross-context test. The channel is
         * unbounded, so only the receiver ever blocks and the wake is
         * always sender-context -> receiver-context. Pin every sender to
         * one context and the receiver to the other so the handoff is
         * exclusively thread->coro (t2c) or coro->thread (c2t). */
        int recv_thread = G.chan_c2t ? 1 : 0; /* c2t: receiver is a thread */
        int send_thread = G.chan_c2t ? 0 : 1; /* opposite context          */
        spawn_forced(chan_receiver, NULL, recv_thread, &ts);
        for (long i = 0; i < G.tasks; i++)
            spawn_forced(chan_sender, NULL, send_thread, &ts);
    } else {
        spawn_one(chan_receiver, NULL, G.tasks, &ts); /* receiver idx = tasks */
        for (long i = 0; i < G.tasks; i++) spawn_one(chan_sender, NULL, i, &ts);
    }
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

/* --------------------------------------------------------------- handoff */
/*
 * Cross-context wake-latency probe. Two parties ping-pong through a pair
 * of binary semaphores (no mutex, no predicate -- the lightest blocking
 * handoff there is), so each round-trip forces exactly one wake in each
 * direction and the ns/op is the bare A<->B wake latency. The two parties'
 * execution vehicles are pinned independently (--ho-dir), so the same code
 * measures every context pair:
 *
 *   cc : coroutine  <-> coroutine   (pure userspace reschedule)
 *   ct : coroutine  <-> OS thread   (the cross-context case; the costly
 *                                    thread->coro wake is on the hot path)
 *   tt : OS thread  <-> OS thread   (futex both ways)
 *
 * Comparing the three rows shows which end-to-end handoff is expensive and
 * by how much -- the decision table for "which side should block where".
 */
typedef struct {
    xylem_sem_t* fwd; /* A -> B */
    xylem_sem_t* bwd; /* B -> A */
    long         n;
} ho_ctx_t;

static void ho_party_a(void* arg) {
    ho_ctx_t* c = (ho_ctx_t*)arg;
    for (long i = 0; i < c->n; i++) {
        xylem_sem_post(c->fwd); /* wake B */
        xylem_sem_wait(c->bwd); /* wait for B */
    }
    xylem_waitgroup_done(G.wg);
}

static void ho_party_b(void* arg) {
    ho_ctx_t* c = (ho_ctx_t*)arg;
    for (long i = 0; i < c->n; i++) {
        xylem_sem_wait(c->fwd); /* wait for A */
        xylem_sem_post(c->bwd); /* wake A */
    }
    xylem_waitgroup_done(G.wg);
}

static void run_handoff(void) {
    xylem_sem_t* fwd = xylem_sem_create(0);
    xylem_sem_t* bwd = xylem_sem_create(0);
    G.wg             = xylem_waitgroup_create();
    xylem_waitgroup_add(G.wg, 2);

    ho_ctx_t  ctx = { fwd, bwd, G.iters };
    thr_set_t ts  = thr_alloc(2);

    uint64_t t0 = now_ns();
    spawn_forced(ho_party_b, &ctx, G.ho_b_thr, &ts);
    spawn_forced(ho_party_a, &ctx, G.ho_a_thr, &ts);
    xylem_waitgroup_wait(G.wg);
    uint64_t t1 = now_ns();
    join_set(&ts);

    G.elapsed_ns = t1 - t0;
    G.total_ops  = (uint64_t)G.iters;

    free(ts.h);
    xylem_waitgroup_destroy(G.wg);
    xylem_sem_destroy(fwd);
    xylem_sem_destroy(bwd);
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
        case P_HANDOFF:   run_handoff();   break;
    }
    print_result();
}

static int parse_prim(const char* s) {
    if (!strcmp(s, "mutex"))     { G.prim = P_MUTEX;     G.name = "mutex";     return 0; }
    if (!strcmp(s, "cond"))      { G.prim = P_COND;      G.name = "cond";      return 0; }
    if (!strcmp(s, "waitgroup")) { G.prim = P_WAITGROUP; G.name = "waitgroup"; return 0; }
    if (!strcmp(s, "sem"))       { G.prim = P_SEM;       G.name = "sem";       return 0; }
    if (!strcmp(s, "channel"))   { G.prim = P_CHANNEL;   G.name = "channel";   return 0; }
    if (!strcmp(s, "handoff"))   { G.prim = P_HANDOFF;   G.name = "handoff";   return 0; }
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
        "usage: %s <mutex|cond|waitgroup|sem|channel|handoff> "
        "[--mode coro|thread|mixed] "
        "[--workers W] [--tasks T] [--iters N] [--permits K] "
        "[--chan-dir t2c|c2t] [--ho-dir cc|ct|tt]\n", prog);
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
        } else if (!strcmp(argv[i], "--chan-dir") && i + 1 < argc) {
            const char* d = argv[++i];
            if (!strcmp(d, "t2c"))      G.chan_c2t = 0;
            else if (!strcmp(d, "c2t")) G.chan_c2t = 1;
            else { usage(argv[0]); return 2; }
        } else if (!strcmp(argv[i], "--ho-dir") && i + 1 < argc) {
            const char* d = argv[++i];
            if (!strcmp(d, "cc"))      { G.ho_a_thr = 0; G.ho_b_thr = 0; }
            else if (!strcmp(d, "ct")) { G.ho_a_thr = 0; G.ho_b_thr = 1; }
            else if (!strcmp(d, "tt")) { G.ho_a_thr = 1; G.ho_b_thr = 1; }
            else { usage(argv[0]); return 2; }
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (G.tasks < 1)   G.tasks = 1;
    if (G.iters < 1)   G.iters = 1;
    if (G.permits < 1) G.permits = 1;

    /* Make the channel mixed direction visible in the JSON `mode` field so
     * the two directions land in distinct result rows. */
    if (G.prim == P_CHANNEL && G.mode == M_MIXED) {
        G.mode_name = G.chan_c2t ? "mixed-c2t" : "mixed-t2c";
    }
    /* handoff ignores --mode; report its context pair as the `mode`. */
    if (G.prim == P_HANDOFF) {
        G.mode_name = G.ho_a_thr ? "tt" : (G.ho_b_thr ? "ct" : "cc");
    }

    xylem_opts_t rt = {0};
    rt.workers = G.workers;
    xylem_run(bench_main, NULL, &rt);
    return 0;
}
