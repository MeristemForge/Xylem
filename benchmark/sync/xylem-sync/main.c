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

/**
 * Xylem sync-primitive microbenchmark
 *
 * Benchmarks each coroutine sync primitive (mutex, cond, waitgroup, sem,
 * channel) and prints a JSON result on stdout. Because xylem's sync
 * primitives are context-adaptive (a blocking op parks a coroutine or blocks
 * an OS thread, and the two interoperate), the same worker code runs in three
 * concurrency modes:
 *
 *   --mode coro    workers are coroutines (xylem_spawn)            [default]
 *   --mode thread  workers are plain OS threads
 *   --mode mixed   half coroutines, half OS threads, sharing one primitive
 *
 * Go has only goroutines (coro). Rust offers coro (Tokio) and thread (std),
 * but cannot mix the two on one primitive -- that is the case xylem covers
 * uniquely.
 *
 * Usage:
 *   sync-xylem <primitive> [--mode coro|thread|mixed]
 *                          [--workers W] [--tasks T] [--iters N] [--permits K]
 *
 * Workload model (identical across languages and modes):
 *   mutex      : T workers each do N lock/inc/unlock  -> ops = T*N
 *   cond       : 1 producer + 1 consumer, N hand-offs -> ops = N
 *   waitgroup  : N rounds over a pre-spawned pool of T workers -> ops = T*N
 *   sem        : T workers each do N wait/post, K permits -> ops = T*N
 *   channel    : T senders each send N msgs, 1 receiver -> ops = T*N
 */

#include "xylem.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The thread / mixed modes drive the same xylem primitives from OS threads;
 * only the execution vehicle (coroutine vs OS thread) changes. The thrd_*
 * handles come from xylem's public C11 <threads.h> wrapper (pulled in by
 * xylem.h), the same one the unit tests use, so there is no per-OS thread
 * code here.
 */
typedef thrd_t _bench_thread_t;

typedef struct {
    void (*fn)(void*);
    void* arg;
} _thunk_t;

typedef enum {
    M_CORO,
    M_THREAD,
    M_MIXED,
} _runmode_t;

typedef enum {
    P_MUTEX,
    P_COND,
    P_WAITGROUP,
    P_SEM,
    P_CHANNEL,
    P_HANDOFF,
} _prim_t;

/* A set of OS-thread handles spawned for a run, joined after the workload. */
typedef struct {
    _bench_thread_t* h;
    int              n;
} _thr_set_t;

typedef struct {
    xylem_waitgroup_t** gate;
    xylem_waitgroup_t** fin;
    int64_t             rounds;
} _wg_pool_ctx_t;

typedef struct {
    xylem_sem_t* fwd; /* A -> B */
    xylem_sem_t* bwd; /* B -> A */
    int64_t      n;
} _ho_ctx_t;

#define CHANNEL_MSG ((void*)&_channel_payload)

static int _channel_payload;

static struct {
    _prim_t     prim;
    const char* name;
    _runmode_t  mode;
    const char* mode_name;
    int         workers;
    int64_t     tasks;
    int64_t     iters;
    int64_t     permits;
    bool        chan_c2t;   /* channel mixed: false = thread->coro, true = coro->thread */
    bool        ho_a_thr;   /* handoff: party A on an OS thread? */
    bool        ho_b_thr;   /* handoff: party B on an OS thread? */
    bool        ho_dir_set; /* handoff: --ho-dir given (overrides --mode)? */

    xylem_mutex_t*     mtx;
    xylem_cond_t*      cond;
    xylem_sem_t*       sem;
    xylem_channel_t*   ch;
    xylem_waitgroup_t* wg;      /* joins the worker coroutines */
    xylem_waitgroup_t* recv_wg; /* channel: joins the receiver */

    volatile uint64_t counter;   /* mutex: contended accumulator */
    int               cond_turn; /* cond: 0 = producer, 1 = consumer */

    uint64_t total_ops;
    uint64_t elapsed_ns;
} _g;

static void _die_oom(void) {
    fprintf(stderr, "sync-xylem: out of memory\n");
    exit(1);
}

static uint64_t _now_ns(void) {
    return xylem_utils_getnow(XYLEM_TIME_PRECISION_NSEC);
}

/* Adapt a void(*)(void*) worker to the int(*)(void*) thrd_start_t signature. */
static int _thread_tramp(void* p) {
    _thunk_t* t        = (_thunk_t*)p;
    void (*fn)(void*)  = t->fn;
    void* a            = t->arg;
    free(t);
    fn(a);
    return 0;
}

static _bench_thread_t _thread_spawn(void (*fn)(void*), void* arg) {
    _thunk_t* t = (_thunk_t*)malloc(sizeof(*t));
    if (!t) {
        _die_oom();
    }
    t->fn  = fn;
    t->arg = arg;
    _bench_thread_t th;
    thrd_create(&th, _thread_tramp, t);
    return th;
}

static void _thread_join(_bench_thread_t th) {
    thrd_join(th, NULL);
}

/* Does worker #idx run on an OS thread under the active mode? */
static bool _idx_uses_thread(int64_t idx) {
    switch (_g.mode) {
        case M_CORO:
            return false;
        case M_THREAD:
            return true;
        case M_MIXED:
            return (idx & 1) != 0; /* alternate coro / thread */
    }
    return false;
}

static void _spawn_one(void (*fn)(void*), void* arg, int64_t idx,
                       _thr_set_t* ts) {
    if (_idx_uses_thread(idx)) {
        ts->h[ts->n++] = _thread_spawn(fn, arg);
    } else {
        xylem_spawn(fn, arg);
    }
}

/* Spawn with an explicit execution vehicle, bypassing the idx-parity rule.
 * The channel mixed mode uses this to pin a clean cross-context direction
 * (all senders one context, the receiver the other). */
static void _spawn_forced(void (*fn)(void*), void* arg, bool use_thread,
                          _thr_set_t* ts) {
    if (use_thread) {
        ts->h[ts->n++] = _thread_spawn(fn, arg);
    } else {
        xylem_spawn(fn, arg);
    }
}

static void _join_set(_thr_set_t* ts) {
    for (int i = 0; i < ts->n; i++) {
        _thread_join(ts->h[i]);
    }
    ts->n = 0;
}

static _thr_set_t _thr_alloc(int64_t cap) {
    _thr_set_t ts;
    size_t     count = (size_t)(cap > 0 ? cap : 1);
    ts.h = (_bench_thread_t*)malloc(sizeof(_bench_thread_t) * count);
    if (!ts.h) {
        _die_oom();
    }
    ts.n = 0;
    return ts;
}

/* mutex */

static void _mutex_worker(void* arg) {
    (void)arg;
    int64_t n = _g.iters;
    for (int64_t i = 0; i < n; i++) {
        xylem_mutex_lock(_g.mtx);
        _g.counter++;
        xylem_mutex_unlock(_g.mtx);
    }
    xylem_waitgroup_done(_g.wg);
}

static void _run_mutex(void) {
    _g.mtx     = xylem_mutex_create();
    _g.wg      = xylem_waitgroup_create();
    _g.counter = 0;
    _thr_set_t ts = _thr_alloc(_g.tasks);

    xylem_waitgroup_add(_g.wg, (size_t)_g.tasks);
    uint64_t t0 = _now_ns();
    for (int64_t i = 0; i < _g.tasks; i++) {
        _spawn_one(_mutex_worker, NULL, i, &ts);
    }
    xylem_waitgroup_wait(_g.wg);
    uint64_t t1 = _now_ns();
    _join_set(&ts);

    _g.elapsed_ns = t1 - t0;
    _g.total_ops  = (uint64_t)_g.tasks * (uint64_t)_g.iters;

    if (_g.counter != _g.total_ops) {
        fprintf(stderr, "mutex: counter mismatch %" PRIu64 " != %" PRIu64 "\n",
                _g.counter, _g.total_ops);
    }

    free(ts.h);
    xylem_waitgroup_destroy(_g.wg);
    xylem_mutex_destroy(_g.mtx);
}

/* cond */

static void _cond_producer(void* arg) {
    (void)arg;
    int64_t n = _g.iters;
    for (int64_t i = 0; i < n; i++) {
        xylem_mutex_lock(_g.mtx);
        while (_g.cond_turn != 0) {
            xylem_cond_wait(_g.cond, _g.mtx);
        }
        _g.cond_turn = 1;
        xylem_cond_signal(_g.cond);
        xylem_mutex_unlock(_g.mtx);
    }
    xylem_waitgroup_done(_g.wg);
}

static void _cond_consumer(void* arg) {
    (void)arg;
    int64_t n = _g.iters;
    for (int64_t i = 0; i < n; i++) {
        xylem_mutex_lock(_g.mtx);
        while (_g.cond_turn != 1) {
            xylem_cond_wait(_g.cond, _g.mtx);
        }
        _g.cond_turn = 0;
        xylem_cond_signal(_g.cond);
        xylem_mutex_unlock(_g.mtx);
    }
    xylem_waitgroup_done(_g.wg);
}

static void _run_cond(void) {
    _g.mtx       = xylem_mutex_create();
    _g.cond      = xylem_cond_create();
    _g.wg        = xylem_waitgroup_create();
    _g.cond_turn = 0;
    _thr_set_t ts = _thr_alloc(2);

    xylem_waitgroup_add(_g.wg, 2);
    uint64_t t0 = _now_ns();
    /* idx 0 = producer, idx 1 = consumer; in mixed mode producer is a
     * coroutine and consumer an OS thread, so they hand off across contexts. */
    _spawn_one(_cond_consumer, NULL, 1, &ts);
    _spawn_one(_cond_producer, NULL, 0, &ts);
    xylem_waitgroup_wait(_g.wg);
    uint64_t t1 = _now_ns();
    _join_set(&ts);

    _g.elapsed_ns = t1 - t0;
    _g.total_ops  = (uint64_t)_g.iters;

    free(ts.h);
    xylem_waitgroup_destroy(_g.wg);
    xylem_cond_destroy(_g.cond);
    xylem_mutex_destroy(_g.mtx);
}

/*
 * Isolated waitgroup benchmark: measures ONLY the waitgroup primitive
 * (add/done/wait + the park/wake handoff), never task creation. A fixed
 * pool of T workers is spawned once, outside the timed region, and loops
 * over the rounds. Each round uses a fresh pair of single-use waitgroups
 * -- gate[r] (main opens it to release the pool) and fin[r] (the pool
 * signals completion, main joins) -- so there is no reuse-contract hazard
 * and no allocation inside the measured loop. The timed loop is purely:
 *   main:    fin.Add(T); gate.Done();         fin.Wait()
 *   worker:               gate.Wait(); fin.Done()
 */
static void _wg_pool_worker(void* arg) {
    _wg_pool_ctx_t* c = (_wg_pool_ctx_t*)arg;
    for (int64_t r = 0; r < c->rounds; r++) {
        xylem_waitgroup_wait(c->gate[r]); /* wait for round r to open */
        xylem_waitgroup_done(c->fin[r]);  /* signal this worker is done */
    }
}

static void _run_waitgroup(void) {
    int64_t    rounds = _g.iters;
    int64_t    tasks  = _g.tasks;
    _thr_set_t ts     = _thr_alloc(tasks);

    /* Pre-allocate the per-round gate + fin waitgroups and pre-arm the
     * gates (closed). Each instance is used exactly once, so reuse is never
     * an issue. All of this -- and the worker spawn below -- is OUTSIDE the
     * timed region, so task-creation cost never enters the measurement. */
    xylem_waitgroup_t** gate =
        (xylem_waitgroup_t**)malloc(sizeof(*gate) * (size_t)rounds);
    xylem_waitgroup_t** fin =
        (xylem_waitgroup_t**)malloc(sizeof(*fin) * (size_t)rounds);
    if (!gate || !fin) {
        _die_oom();
    }
    for (int64_t r = 0; r < rounds; r++) {
        gate[r] = xylem_waitgroup_create();
        fin[r]  = xylem_waitgroup_create();
        xylem_waitgroup_add(gate[r], 1); /* gate starts closed */
    }

    _wg_pool_ctx_t ctx = {gate, fin, rounds};
    ts.n = 0;
    for (int64_t t = 0; t < tasks; t++) {
        _spawn_one(_wg_pool_worker, &ctx, t, &ts);
    }

    uint64_t t0 = _now_ns();
    for (int64_t r = 0; r < rounds; r++) {
        xylem_waitgroup_add(fin[r], (size_t)tasks); /* expect T workers   */
        xylem_waitgroup_done(gate[r]);              /* open: release pool */
        xylem_waitgroup_wait(fin[r]);               /* join the pool      */
    }
    uint64_t t1 = _now_ns();

    _join_set(&ts); /* workers have finished the last round; reap threads */

    for (int64_t r = 0; r < rounds; r++) {
        xylem_waitgroup_destroy(gate[r]);
        xylem_waitgroup_destroy(fin[r]);
    }
    free(gate);
    free(fin);
    free(ts.h);

    _g.elapsed_ns = t1 - t0;
    _g.total_ops  = (uint64_t)rounds * (uint64_t)tasks;
}

/* sem */

static void _sem_worker(void* arg) {
    (void)arg;
    int64_t n = _g.iters;
    for (int64_t i = 0; i < n; i++) {
        xylem_sem_wait(_g.sem);
        xylem_sem_post(_g.sem);
    }
    xylem_waitgroup_done(_g.wg);
}

static void _run_sem(void) {
    _g.sem = xylem_sem_create((unsigned int)_g.permits);
    _g.wg  = xylem_waitgroup_create();
    _thr_set_t ts = _thr_alloc(_g.tasks);

    xylem_waitgroup_add(_g.wg, (size_t)_g.tasks);
    uint64_t t0 = _now_ns();
    for (int64_t i = 0; i < _g.tasks; i++) {
        _spawn_one(_sem_worker, NULL, i, &ts);
    }
    xylem_waitgroup_wait(_g.wg);
    uint64_t t1 = _now_ns();
    _join_set(&ts);

    _g.elapsed_ns = t1 - t0;
    _g.total_ops  = (uint64_t)_g.tasks * (uint64_t)_g.iters;

    free(ts.h);
    xylem_waitgroup_destroy(_g.wg);
    xylem_sem_destroy(_g.sem);
}

/* channel */

static void _chan_sender(void* arg) {
    (void)arg;
    int64_t n = _g.iters;
    for (int64_t i = 0; i < n; i++) {
        while (xylem_channel_send(_g.ch, CHANNEL_MSG) != 0) {
            xylem_sleep(0);
        }
    }
    xylem_waitgroup_done(_g.wg);
}

static void _chan_receiver(void* arg) {
    (void)arg;
    uint64_t got = 0;
    for (;;) {
        void* msg = xylem_channel_recv(_g.ch);
        if (!msg) {
            break;
        }
        got++;
    }
    _g.counter = got;
    xylem_waitgroup_done(_g.recv_wg);
}

static void _run_channel(void) {
    /* create() is coroutine-only -- run_channel executes in the root
     * coroutine, so this is valid even in thread/mixed mode. Unbounded
     * (cap 0): send never reports full, an MPSC matching the Go/Rust
     * channels this benchmark compares against. */
    _g.ch      = xylem_channel_create(0);
    _g.wg      = xylem_waitgroup_create();
    _g.recv_wg = xylem_waitgroup_create();
    _g.counter = 0;
    _thr_set_t ts = _thr_alloc(_g.tasks + 1);

    xylem_waitgroup_add(_g.recv_wg, 1);
    xylem_waitgroup_add(_g.wg, (size_t)_g.tasks);

    uint64_t t0 = _now_ns();
    if (_g.mode == M_MIXED) {
        /* Pure single-direction cross-context test. The channel is
         * unbounded, so only the receiver ever blocks and the wake is
         * always sender-context -> receiver-context. Pin every sender to
         * one context and the receiver to the other so the handoff is
         * exclusively thread->coro (t2c) or coro->thread (c2t). */
        bool recv_thread = _g.chan_c2t;  /* c2t: receiver is a thread */
        bool send_thread = !_g.chan_c2t; /* opposite context */
        _spawn_forced(_chan_receiver, NULL, recv_thread, &ts);
        for (int64_t i = 0; i < _g.tasks; i++) {
            _spawn_forced(_chan_sender, NULL, send_thread, &ts);
        }
    } else {
        /* receiver idx = tasks */
        _spawn_one(_chan_receiver, NULL, _g.tasks, &ts);
        for (int64_t i = 0; i < _g.tasks; i++) {
            _spawn_one(_chan_sender, NULL, i, &ts);
        }
    }
    xylem_waitgroup_wait(_g.wg);      /* all senders done */
    xylem_channel_close(_g.ch);       /* let the receiver finish */
    xylem_waitgroup_wait(_g.recv_wg); /* receiver drained */
    uint64_t t1 = _now_ns();
    _join_set(&ts);

    _g.elapsed_ns = t1 - t0;
    _g.total_ops  = (uint64_t)_g.tasks * (uint64_t)_g.iters;

    if (_g.counter != _g.total_ops) {
        fprintf(stderr, "channel: recv mismatch %" PRIu64 " != %" PRIu64 "\n",
                _g.counter, _g.total_ops);
    }

    free(ts.h);
    xylem_waitgroup_destroy(_g.recv_wg);
    xylem_waitgroup_destroy(_g.wg);
    xylem_channel_destroy(_g.ch);
}

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
static void _ho_party_a(void* arg) {
    _ho_ctx_t* c = (_ho_ctx_t*)arg;
    for (int64_t i = 0; i < c->n; i++) {
        xylem_sem_post(c->fwd); /* wake B */
        xylem_sem_wait(c->bwd); /* wait for B */
    }
    xylem_waitgroup_done(_g.wg);
}

static void _ho_party_b(void* arg) {
    _ho_ctx_t* c = (_ho_ctx_t*)arg;
    for (int64_t i = 0; i < c->n; i++) {
        xylem_sem_wait(c->fwd); /* wait for A */
        xylem_sem_post(c->bwd); /* wake A */
    }
    xylem_waitgroup_done(_g.wg);
}

static void _run_handoff(void) {
    xylem_sem_t* fwd = xylem_sem_create(0);
    xylem_sem_t* bwd = xylem_sem_create(0);
    _g.wg            = xylem_waitgroup_create();
    xylem_waitgroup_add(_g.wg, 2);

    _ho_ctx_t  ctx = {fwd, bwd, _g.iters};
    _thr_set_t ts  = _thr_alloc(2);

    uint64_t t0 = _now_ns();
    _spawn_forced(_ho_party_b, &ctx, _g.ho_b_thr, &ts);
    _spawn_forced(_ho_party_a, &ctx, _g.ho_a_thr, &ts);
    xylem_waitgroup_wait(_g.wg);
    uint64_t t1 = _now_ns();
    _join_set(&ts);

    _g.elapsed_ns = t1 - t0;
    _g.total_ops  = (uint64_t)_g.iters;

    free(ts.h);
    xylem_waitgroup_destroy(_g.wg);
    xylem_sem_destroy(fwd);
    xylem_sem_destroy(bwd);
}

/* output */

static void _print_result(void) {
    double sec  = (double)_g.elapsed_ns / 1e9;
    double ops  = (sec > 0.0) ? (double)_g.total_ops / sec : 0.0;
    double nspo = (_g.total_ops) ? (double)_g.elapsed_ns / (double)_g.total_ops
                                 : 0.0;

    printf("{\n");
    printf("  \"primitive\": \"%s\",\n", _g.name);
    printf("  \"lang\": \"xylem\",\n");
    printf("  \"mode\": \"%s\",\n", _g.mode_name);
    printf("  \"workers\": %d,\n", _g.workers);
    printf("  \"tasks\": %" PRId64 ",\n", _g.tasks);
    printf("  \"iters\": %" PRId64 ",\n", _g.iters);
    if (_g.prim == P_SEM) {
        printf("  \"permits\": %" PRId64 ",\n", _g.permits);
    }
    printf("  \"total_ops\": %" PRIu64 ",\n", _g.total_ops);
    printf("  \"duration_sec\": %.6f,\n", sec);
    printf("  \"ops_per_sec\": %.0f,\n", ops);
    printf("  \"ns_per_op\": %.2f\n", nspo);
    printf("}\n");
}

/* driver */

static void _bench_main(void* arg) {
    (void)arg;
    switch (_g.prim) {
        case P_MUTEX:
            _run_mutex();
            break;
        case P_COND:
            _run_cond();
            break;
        case P_WAITGROUP:
            _run_waitgroup();
            break;
        case P_SEM:
            _run_sem();
            break;
        case P_CHANNEL:
            _run_channel();
            break;
        case P_HANDOFF:
            _run_handoff();
            break;
    }
    _print_result();
}

static int _parse_prim(const char* s) {
    if (!strcmp(s, "mutex")) {
        _g.prim = P_MUTEX;
        _g.name = "mutex";
        return 0;
    }
    if (!strcmp(s, "cond")) {
        _g.prim = P_COND;
        _g.name = "cond";
        return 0;
    }
    if (!strcmp(s, "waitgroup")) {
        _g.prim = P_WAITGROUP;
        _g.name = "waitgroup";
        return 0;
    }
    if (!strcmp(s, "sem")) {
        _g.prim = P_SEM;
        _g.name = "sem";
        return 0;
    }
    if (!strcmp(s, "channel")) {
        _g.prim = P_CHANNEL;
        _g.name = "channel";
        return 0;
    }
    if (!strcmp(s, "handoff")) {
        _g.prim = P_HANDOFF;
        _g.name = "handoff";
        return 0;
    }
    return -1;
}

static int _parse_mode(const char* s) {
    if (!strcmp(s, "coro")) {
        _g.mode      = M_CORO;
        _g.mode_name = "coro";
        return 0;
    }
    if (!strcmp(s, "thread")) {
        _g.mode      = M_THREAD;
        _g.mode_name = "thread";
        return 0;
    }
    if (!strcmp(s, "mixed")) {
        _g.mode      = M_MIXED;
        _g.mode_name = "mixed";
        return 0;
    }
    return -1;
}

static void _usage(const char* prog) {
    fprintf(stderr,
            "usage: %s <mutex|cond|waitgroup|sem|channel|handoff> "
            "[--mode coro|thread|mixed] "
            "[--workers W] [--tasks T] [--iters N] [--permits K] "
            "[--chan-dir t2c|c2t] [--ho-dir cc|ct|tt]\n",
            prog);
}

int main(int argc, char** argv) {
    _g.mode      = M_CORO;
    _g.mode_name = "coro";
    _g.workers   = 0;
    _g.tasks     = 8;
    _g.iters     = 100000;
    _g.permits   = 4;

    if (argc < 2 || _parse_prim(argv[1]) != 0) {
        _usage(argv[0]);
        return 2;
    }

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            if (_parse_mode(argv[++i]) != 0) {
                _usage(argv[0]);
                return 2;
            }
        } else if (!strcmp(argv[i], "--workers") && i + 1 < argc) {
            _g.workers = (int)strtol(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--tasks") && i + 1 < argc) {
            _g.tasks = (int64_t)strtoll(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
            _g.iters = (int64_t)strtoll(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--permits") && i + 1 < argc) {
            _g.permits = (int64_t)strtoll(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--chan-dir") && i + 1 < argc) {
            const char* d = argv[++i];
            if (!strcmp(d, "t2c")) {
                _g.chan_c2t = false;
            } else if (!strcmp(d, "c2t")) {
                _g.chan_c2t = true;
            } else {
                _usage(argv[0]);
                return 2;
            }
        } else if (!strcmp(argv[i], "--ho-dir") && i + 1 < argc) {
            const char* d = argv[++i];
            if (!strcmp(d, "cc")) {
                _g.ho_a_thr = false;
                _g.ho_b_thr = false;
            } else if (!strcmp(d, "ct")) {
                _g.ho_a_thr = false;
                _g.ho_b_thr = true;
            } else if (!strcmp(d, "tt")) {
                _g.ho_a_thr = true;
                _g.ho_b_thr = true;
            } else {
                _usage(argv[0]);
                return 2;
            }
            _g.ho_dir_set = true;
        } else {
            _usage(argv[0]);
            return 2;
        }
    }

    if (_g.tasks < 1) {
        _g.tasks = 1;
    }
    if (_g.iters < 1) {
        _g.iters = 1;
    }
    if (_g.permits < 1) {
        _g.permits = 1;
    }

    /* Make the channel mixed direction visible in the JSON `mode` field so
     * the two directions land in distinct result rows. */
    if (_g.prim == P_CHANNEL && _g.mode == M_MIXED) {
        _g.mode_name = _g.chan_c2t ? "mixed-c2t" : "mixed-t2c";
    }
    /* handoff ignores --mode; report its context pair as the `mode`. */
    if (_g.prim == P_HANDOFF) {
        /* Direction follows --mode (coro=cc, thread=tt, mixed=ct) unless an
         * explicit --ho-dir overrode it, so the driver's mode sweep maps
         * straight onto the context pairs. */
        if (!_g.ho_dir_set) {
            if (_g.mode == M_THREAD) {
                _g.ho_a_thr = true;
                _g.ho_b_thr = true;
            } else if (_g.mode == M_MIXED) {
                _g.ho_a_thr = false;
                _g.ho_b_thr = true;
            } else {
                _g.ho_a_thr = false;
                _g.ho_b_thr = false;
            }
        }
        _g.mode_name = _g.ho_a_thr ? "tt" : (_g.ho_b_thr ? "ct" : "cc");
    }

    xylem_opts_t rt = {0};
    rt.workers      = _g.workers;
    xylem_run(_bench_main, NULL, &rt);
    return 0;
}
