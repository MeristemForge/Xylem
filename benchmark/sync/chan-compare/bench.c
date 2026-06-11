/** Channel implementation A/B benchmark.
 *
 * Compares the production lock-free MPSC channel (xylem_channel_*) against
 * a semaphore-based variant (chsem_*) built on xylem_sem + the same MPSC
 * queue. Same workload for both: T sender workers push N messages each into
 * one channel, a single receiver drains until close. Senders/receiver run as
 * coroutines, OS threads, or a mix, to exercise both wake paths:
 *   - coro   : scheduler park/resume (orig: atomic slot; sem: spin+sched)
 *   - thread : orig blocks on per-thread sem (tsem); sem barges on a futex
 *   - mixed  : alternate
 *
 * Not a correctness proof -- it relies on close() happening only after all
 * sends complete (so no message is in a transient mid-push state at close).
 */

#include "xylem.h"
#include "xylem/sync/xylem-channel.h"
#include "xylem/sync/xylem-sem.h"
#include "xylem/sync/xylem-waitgroup.h"
#include "xylem/xylem-utils.h"
#include "xylem/xylem-threads.h"

#include "container/mpsc.h" /* private header; build with -I <root>/src */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------- sem-based channel ---------------------------- */

typedef struct {
    mpsc_node_t node;
    void*       payload;
} chsem_msg_t;

typedef struct {
    mpsc_t         queue;
    xylem_sem_t*   items;  /* counts available messages (+1 on close) */
    _Atomic int    closed;
    _Atomic size_t count;
    size_t         cap;
} chsem_t;

static chsem_t* chsem_create(size_t cap) {
    chsem_t* c = (chsem_t*)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    mpsc_init(&c->queue);
    c->items = xylem_sem_create(0);
    atomic_init(&c->closed, 0);
    atomic_init(&c->count, 0);
    c->cap = cap;
    return c;
}

static void chsem_destroy(chsem_t* c) {
    if (!c) {
        return;
    }
    mpsc_node_t* n;
    while ((n = mpsc_pop(&c->queue)) != NULL) {
        free(mpsc_entry(n, chsem_msg_t, node));
    }
    xylem_sem_destroy(c->items);
    free(c);
}

static int chsem_send(chsem_t* c, void* msg) {
    size_t prev = atomic_fetch_add_explicit(&c->count, 1, memory_order_acq_rel);
    if (c->cap != 0 && prev >= c->cap) {
        atomic_fetch_sub_explicit(&c->count, 1, memory_order_relaxed);
        return XYLEM_CHANNEL_FULL;
    }
    chsem_msg_t* m = (chsem_msg_t*)calloc(1, sizeof(*m));
    if (!m) {
        atomic_fetch_sub_explicit(&c->count, 1, memory_order_relaxed);
        return -1;
    }
    m->payload = msg;
    mpsc_push(&c->queue, &m->node);
    xylem_sem_post(c->items);
    return 0;
}

static void chsem_close(chsem_t* c) {
    atomic_store_explicit(&c->closed, 1, memory_order_release);
    xylem_sem_post(c->items); /* wake a parked receiver with no message */
}

static void* chsem_recv(chsem_t* c) {
    xylem_sem_wait(c->items);
    for (;;) {
        mpsc_node_t* n = mpsc_pop(&c->queue);
        if (n) {
            chsem_msg_t* m = mpsc_entry(n, chsem_msg_t, node);
            void*        p = m->payload;
            free(m);
            atomic_fetch_sub_explicit(&c->count, 1, memory_order_relaxed);
            return p;
        }
        /* Token but no node yet: either the close token (drained), or a
         * producer mid-push. Drain done only when closed AND empty. */
        if (atomic_load_explicit(&c->closed, memory_order_acquire)
            && atomic_load_explicit(&c->count, memory_order_acquire) == 0) {
            return NULL;
        }
        /* transient mid-push: spin until the node links */
    }
}

/* ------------------------------- vtable ----------------------------------- */

typedef struct {
    const char* name;
    void* (*create)(size_t);
    void  (*destroy)(void*);
    void  (*close)(void*);
    int   (*send)(void*, void*);
    void* (*recv)(void*);
} chan_vt_t;

static void* vt_orig_create(size_t cap) { return xylem_channel_create(cap); }
static void  vt_orig_destroy(void* c)   { xylem_channel_destroy((xylem_channel_t*)c); }
static void  vt_orig_close(void* c)      { xylem_channel_close((xylem_channel_t*)c); }
static int   vt_orig_send(void* c, void* m) { return xylem_channel_send((xylem_channel_t*)c, m); }
static void* vt_orig_recv(void* c)       { return xylem_channel_recv((xylem_channel_t*)c); }

static void* vt_sem_create(size_t cap)   { return chsem_create(cap); }
static void  vt_sem_destroy(void* c)     { chsem_destroy((chsem_t*)c); }
static void  vt_sem_close(void* c)       { chsem_close((chsem_t*)c); }
static int   vt_sem_send(void* c, void* m) { return chsem_send((chsem_t*)c, m); }
static void* vt_sem_recv(void* c)        { return chsem_recv((chsem_t*)c); }

static const chan_vt_t VT_ORIG = {
    "lockfree", vt_orig_create, vt_orig_destroy, vt_orig_close,
    vt_orig_send, vt_orig_recv
};
static const chan_vt_t VT_SEM = {
    "sem", vt_sem_create, vt_sem_destroy, vt_sem_close,
    vt_sem_send, vt_sem_recv
};

/* ------------------------------ OS-thread shim ---------------------------- */

typedef struct { void (*fn)(void*); void* arg; } thunk_t;

static int _thread_tramp(void* p) {
    thunk_t* t = (thunk_t*)p;
    void (*fn)(void*) = t->fn;
    void* a = t->arg;
    free(t);
    fn(a);
    return 0;
}
static thrd_t thread_spawn(void (*fn)(void*), void* arg) {
    thunk_t* t = (thunk_t*)malloc(sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    thrd_t th;
    thrd_create(&th, _thread_tramp, t);
    return th;
}

/* ------------------------------- workload --------------------------------- */

typedef enum { M_CORO, M_THREAD, M_MIXED } mode_t;

static int g_payload;
#define MSG ((void*)&g_payload)

static struct {
    const chan_vt_t*    vt;
    void*               ch;
    mode_t              mode;
    long                tasks;
    long                iters;
    xylem_waitgroup_t*  send_wg;
    xylem_waitgroup_t*  recv_wg;
    _Atomic uint64_t    received;
} G;

static int idx_uses_thread(long idx) {
    switch (G.mode) {
        case M_CORO:   return 0;
        case M_THREAD: return 1;
        case M_MIXED:  return (int)(idx & 1);
    }
    return 0;
}

static void sender(void* arg) {
    (void)arg;
    for (long i = 0; i < G.iters; i++) {
        while (G.vt->send(G.ch, MSG) != 0) {
            xylem_sleep(0);
        }
    }
    xylem_waitgroup_done(G.send_wg);
}

static void receiver(void* arg) {
    (void)arg;
    uint64_t got = 0;
    for (;;) {
        void* m = G.vt->recv(G.ch);
        if (!m) {
            break;
        }
        got++;
    }
    atomic_store(&G.received, got);
    xylem_waitgroup_done(G.recv_wg);
}

typedef struct { thrd_t* h; int n; } thr_set_t;

static void spawn_one(void (*fn)(void*), void* arg, long idx, thr_set_t* ts) {
    if (idx_uses_thread(idx)) {
        ts->h[ts->n++] = thread_spawn(fn, arg);
    } else {
        xylem_spawn(fn, arg);
    }
}

static uint64_t now_ns(void) {
    return xylem_utils_getnow(XYLEM_TIME_PRECISION_NSEC);
}

/* Returns ns elapsed; fills *ops with the message total. */
static uint64_t run_one(const chan_vt_t* vt, mode_t mode, uint64_t* ops) {
    G.vt       = vt;
    G.mode     = mode;
    G.ch       = vt->create(0); /* unbounded */
    G.send_wg  = xylem_waitgroup_create();
    G.recv_wg  = xylem_waitgroup_create();
    atomic_store(&G.received, 0);

    thr_set_t ts;
    ts.h = (thrd_t*)malloc(sizeof(thrd_t) * (size_t)(G.tasks + 1));
    ts.n = 0;

    xylem_waitgroup_add(G.recv_wg, 1);
    xylem_waitgroup_add(G.send_wg, (size_t)G.tasks);

    uint64_t t0 = now_ns();
    spawn_one(receiver, NULL, G.tasks, &ts); /* receiver idx = tasks */
    for (long i = 0; i < G.tasks; i++) {
        spawn_one(sender, NULL, i, &ts);
    }
    xylem_waitgroup_wait(G.send_wg);
    vt->close(G.ch);
    xylem_waitgroup_wait(G.recv_wg);
    uint64_t t1 = now_ns();

    for (int i = 0; i < ts.n; i++) {
        thrd_join(ts.h[i], NULL);
    }
    free(ts.h);

    uint64_t total = (uint64_t)G.tasks * (uint64_t)G.iters;
    if (atomic_load(&G.received) != total) {
        fprintf(stderr, "  [%s] MISMATCH received %llu != %llu\n",
                vt->name,
                (unsigned long long)atomic_load(&G.received),
                (unsigned long long)total);
    }
    *ops = total;

    xylem_waitgroup_destroy(G.recv_wg);
    xylem_waitgroup_destroy(G.send_wg);
    vt->destroy(G.ch);
    return t1 - t0;
}

/* --------------------------------- driver --------------------------------- */

static const char* mode_name(mode_t m) {
    return m == M_CORO ? "coro" : (m == M_THREAD ? "thread" : "mixed");
}

static struct {
    long   tasks;
    long   iters;
    int    workers;
    int    reps;
    mode_t only_mode;
    int    mode_all;
} OPT = { 4, 200000, 0, 3, M_CORO, 1 };

static void run_matrix(mode_t mode) {
    uint64_t ops = 0;

    /* warm up once (allocator / TLS sem creation), then take best of reps. */
    uint64_t best_orig = (uint64_t)-1, best_sem = (uint64_t)-1;
    (void)run_one(&VT_ORIG, mode, &ops);
    for (int r = 0; r < OPT.reps; r++) {
        uint64_t e = run_one(&VT_ORIG, mode, &ops);
        if (e < best_orig) best_orig = e;
    }
    (void)run_one(&VT_SEM, mode, &ops);
    for (int r = 0; r < OPT.reps; r++) {
        uint64_t e = run_one(&VT_SEM, mode, &ops);
        if (e < best_sem) best_sem = e;
    }

    double ns_orig = (double)best_orig / (double)ops;
    double ns_sem  = (double)best_sem / (double)ops;
    double mops_orig = 1000.0 / ns_orig;
    double mops_sem  = 1000.0 / ns_sem;

    printf("  %-7s | lockfree %7.1f ns/op  %6.2f Mops/s | "
           "sem %7.1f ns/op  %6.2f Mops/s | sem/lockfree %.2fx\n",
           mode_name(mode), ns_orig, mops_orig, ns_sem, mops_sem,
           ns_sem / ns_orig);
}

static void bench_main(void* arg) {
    (void)arg;
    G.tasks = OPT.tasks;
    G.iters = OPT.iters;

    printf("channel A/B: tasks=%ld iters/sender=%ld total_msgs=%lld "
           "reps=best-of-%d workers=%d\n",
           OPT.tasks, OPT.iters,
           (long long)OPT.tasks * OPT.iters, OPT.reps,
           OPT.workers);

    if (OPT.mode_all) {
        run_matrix(M_CORO);
        run_matrix(M_THREAD);
        run_matrix(M_MIXED);
    } else {
        run_matrix(OPT.only_mode);
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tasks") && i + 1 < argc) {
            OPT.tasks = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
            OPT.iters = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--workers") && i + 1 < argc) {
            OPT.workers = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--reps") && i + 1 < argc) {
            OPT.reps = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char* m = argv[++i];
            OPT.mode_all = 0;
            if (!strcmp(m, "coro")) OPT.only_mode = M_CORO;
            else if (!strcmp(m, "thread")) OPT.only_mode = M_THREAD;
            else if (!strcmp(m, "mixed")) OPT.only_mode = M_MIXED;
            else OPT.mode_all = 1;
        }
    }
    if (OPT.tasks < 1) OPT.tasks = 1;
    if (OPT.iters < 1) OPT.iters = 1;
    if (OPT.reps  < 1) OPT.reps = 1;

    xylem_opts_t rt = {0};
    rt.workers = OPT.workers;
    xylem_run(bench_main, NULL, &rt);
    return 0;
}
