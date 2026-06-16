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
 * Scheduling model
 *
 * N worker threads cooperate through a three-tier runnable pool:
 *   1. per-worker `runnext` slot   - single LIFO hand-off, cache-hot
 *   2. per-worker work-stealing    - owner pushes the tail and pops the head
 *      FIFO queue (wsq)              (arrival order); other workers steal from
 *                                    the head. FIFO bounds per-coroutine wait.
 *   3. global runq (mutex)         - overflow from full queues, and
 *                                    injection point for cross-thread
 *                                    scheduler_schedule() callers
 *
 * When a worker runs out of local work it does a non-blocking
 * poll + steal round. Exactly one idle worker becomes the "driver"
 * via CAS on poller_running: it owns the blocking poll that services
 * IO, timers and deferred posts. All other idle workers park on a
 * per-worker semaphore; scheduler_schedule and scheduler_post wake
 * at most one parked worker per push. A hand-off into an empty local
 * runnext slot wakes nobody -- the scheduling worker usually consumes that
 * coro itself. If the owner stalls and its FIFO queue is empty, other workers
 * may steal runnext as a last-resort rescue; this compensates for Xylem not
 * having Tokio's whole-core handoff path that moves LIFO back to the queue.
 *
 * Coroutine parking flows through scheduler_park: the park callback
 * runs *after* mco_yield returns, so a wakeup source can never
 * observe the coroutine pointer before the yield has suspended it.
 * iowait.c relies on this to avoid schedule-before-yield races.
 */

#include "scheduler.h"

#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "iowait.h"
#include "wsq.h"
#include "runq.h"
#include "container/heap.h"
#include "container/list.h"
#include "container/mpsc.h"
#include "container/queue.h"
#include "platform/platform-sem.h"
#include "platform/platform-socket.h"
#include "platform/platform-vmem.h"
#include "platform/platform-info.h"
#include "platform/platform-cpu.h"
#include "sync/spin.h"
#include "xylem/xylem-threads.h"

#include "minicoro/minicoro.h"

#ifdef MCO_USE_FIBERS
#define STACK_EXTERNAL 1
#else
#define STACK_EXTERNAL 0
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#define SCHED_DEQUE_CAP          256
#define SCHED_TIMER_TICK_MS      1
#define SCHED_CORO_POOL_CAP_MUL  64
#define SCHED_CREDIT_DEFAULT     256

#define SCHED_CORO_STACK_SIZE (128 * 1024)

/**
 * Per-worker coroutine-slot cache. Each worker keeps up to
 * SCHED_CORO_CACHE_CAP free slots it can pop/push with no lock; only when
 * the local cache underflows or overflows does it exchange a batch of
 * SCHED_CORO_CACHE_BATCH slots with the shared pool under a single lock.
 * Mirrors the per-P gFree/stackcache design in the Go runtime: the hot
 * spawn/death path stays lock-free and the shared-pool lock is amortized.
 */
#define SCHED_CORO_CACHE_CAP   64
#define SCHED_CORO_CACHE_BATCH 32

typedef struct _sched_coro_pool_s {
    spin_t    lock;
    void**    slots;
    int32_t   count;
    int32_t   cap;
    size_t    slot_size;
    size_t    stack_size;
} _sched_coro_pool_t;

typedef struct _sched_worker_s {
    thrd_t               thread;
    wsq_t*               deque;
    platform_sem_t*      sem;
    scheduler_t*         sched;
    uint32_t             index;
    scheduler_park_fn_t  park_fn;
    void*                park_arg;
    _Atomic bool         parked;
    _Atomic bool         searching;
    _Atomic(mco_coro*)   runnext;
    uint32_t             rng;        /* per-worker xorshift state for steal order */
    uint32_t             sched_tick;
    uint32_t             credit;
    heap_t               timers;
    mtx_t                timer_lock;
    _Atomic uint64_t     next_deadline_ms; /* earliest timer deadline, MAX if none. */
    void**               coro_cache;       /* per-worker free coroutine slots. */
    int32_t              coro_cache_count; /* slots currently held locally.    */
} _sched_worker_t;

struct scheduler_s {
    _sched_worker_t*      workers;
    int32_t               worker_count;
    runq_t*               runq;
    mpsc_t                posts;
    platform_poller_sq_t  poller;
    platform_poller_sqe_t wakeup_sqe;
    platform_sock_t       wakeup_rd;
    platform_sock_t       wakeup_wr;
    iowait_slab_t*        iowait_slab;
    scheduler_idle_fn_t   idle_cb;
    void*                 idle_ud;
    _Atomic bool          post_draining;
    _Atomic bool          running;
    _Atomic bool          poller_waiting;
    _Atomic bool          poller_running;
    _Atomic int32_t       searching;
    bool                  joined;
    _Atomic int64_t       alive;
    _Atomic uint64_t      last_maintenance_ms;
    _Atomic uint32_t      timer_rr;
    list_t                registry;
    spin_t                registry_lock;
    _sched_coro_pool_t          coro_pool;
};


/**
 * Park handshake state, per coroutine. Closes the window where a waker
 * could resume a coroutine while its park callback is still running on
 * the parking worker (and thus still dereferencing the object it parked
 * on -- the classic park_cb-touches-freed-channel race).
 *
 *   IDLE     - running, or sitting in a normal run queue.
 *   ARMING   - between mco_yield and the end of the park callback.
 *   PARKED   - park callback returned true; suspended awaiting a wake.
 *   CLAIMED  - a waker has claimed the coroutine; it is (or will be)
 *              requeued exactly once.
 *
 * At most one waker reaches the claim path per park, because the sync
 * primitive (or iowait) hands off a single one-shot waiter. A waker that
 * observes ARMING only marks CLAIMED and does NOT enqueue: the park
 * callback, on return, sees CLAIMED and requeues the coroutine itself,
 * so resume can never overlap the tail of the callback.
 */
typedef enum _park_state_e {
    PARK_IDLE    = 0,
    PARK_ARMING  = 1,
    PARK_PARKED  = 2,
    PARK_CLAIMED = 3,
} _park_state_t;

typedef struct _sched_coro_ctx_s {
    void (*fn)(void*);
    void*                  arg;
    queue_node_t           runq_node;
    list_node_t            registry_node;
    mco_coro*              co;
    _Atomic _park_state_t  park_state;
} _sched_coro_ctx_t;

typedef struct _sched_post_s {
    mpsc_node_t          node;
    scheduler_post_fn_t  cb;
    void*                ud;
} _sched_post_t;

static thread_local _sched_worker_t* _tls_worker;

static inline _sched_worker_t* _sched_timer_owner(scheduler_timer_t* t) {
    return &t->sched->workers[t->owner];
}

static bool _sched_claim_for_wake(mco_coro* co);
static void _sched_enqueue(scheduler_t* sched, mco_coro* co);

static void _sched_coro_entry(mco_coro* co) {
    _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)mco_get_user_data(co);
    ctx->fn(ctx->arg);
}

static size_t _sched_vmem_page_size(void) {
    static size_t cached;
    if (!cached) {
        cached = platform_vmem_page_size();
    }
    return cached;
}

static size_t _sched_coro_metadata_size(size_t coro_size, size_t stack_size) {
    size_t page_size = _sched_vmem_page_size();
    size_t meta = coro_size - stack_size;
    return (meta + page_size - 1) & ~(page_size - 1);
}

/* Reserve, commit, and guard-page one coroutine slot (mco_coro + stack). */
static void* _sched_coro_slot_reserve(_sched_coro_pool_t* pool, size_t size) {
    if (STACK_EXTERNAL) {
        return calloc(1, size);
    }

    size_t page_size = _sched_vmem_page_size();
    size_t total     = (size + page_size - 1) & ~(page_size - 1);

    char* base = (char*)platform_vmem_reserve(total);
    if (!base) {
        return NULL;
    }

    platform_vmem_commit(base, total);

    size_t meta_size = _sched_coro_metadata_size(total, pool->stack_size);
    platform_vmem_protect(
        base + meta_size, page_size, PLATFORM_VMEM_PROT_NONE);

    return base;
}

static void _sched_coro_stack_reset(_sched_coro_pool_t* pool, void* ptr, size_t size) {
    if (STACK_EXTERNAL) {
        return;
    }

    size_t page_size = _sched_vmem_page_size();
    size_t total     = (size + page_size - 1) & ~(page_size - 1);
    size_t meta_size = _sched_coro_metadata_size(total, pool->stack_size);

    size_t stack_start = meta_size + page_size;
    if (total > stack_start) {
        platform_vmem_decommit((char*)ptr + stack_start, total - stack_start);
    }
}

/* Return one coroutine slot's address space to the OS. */
static void _sched_coro_slot_release(_sched_coro_pool_t* pool, void* ptr) {
    if (STACK_EXTERNAL) {
        free(ptr);
        return;
    }

    size_t page_size = _sched_vmem_page_size();
    size_t total     = (pool->slot_size + page_size - 1) & ~(page_size - 1);
    platform_vmem_release(ptr, total);
}

/* Pop one slot from the shared pool; NULL when empty. Takes the pool lock. */
static void* _sched_coro_pool_global_pop(_sched_coro_pool_t* pool) {
    void* ptr = NULL;
    spin_lock(&pool->lock);
    if (pool->count > 0) {
        ptr = pool->slots[--pool->count];
    }
    spin_unlock(&pool->lock);
    return ptr;
}

/* Push one slot into the shared pool; release it when the pool is full. */
static void _sched_coro_pool_global_push(_sched_coro_pool_t* pool, void* ptr) {
    spin_lock(&pool->lock);
    if (pool->count < pool->cap) {
        pool->slots[pool->count++] = ptr;
        spin_unlock(&pool->lock);
        return;
    }
    spin_unlock(&pool->lock);
    _sched_coro_slot_release(pool, ptr);
}

/**
 * The current worker's local slot cache, or NULL when the caller is not a
 * worker of this scheduler (e.g. the initial spawn on the main thread).
 */
static _sched_worker_t* _sched_coro_pool_local(_sched_coro_pool_t* pool) {
    _sched_worker_t* w = _tls_worker;
    if (w && &w->sched->coro_pool == pool && w->coro_cache) {
        return w;
    }
    return NULL;
}

static void* _sched_coro_pool_alloc(size_t size, void* allocator_data) {
    _sched_coro_pool_t*    pool = (_sched_coro_pool_t*)allocator_data;
    _sched_worker_t* w    = _sched_coro_pool_local(pool);

    if (!w) {
        /* Non-worker caller: go straight to the shared pool. */
        void* ptr = _sched_coro_pool_global_pop(pool);
        return ptr ? ptr : _sched_coro_slot_reserve(pool, size);
    }

    /* Fast path: local cache hit, no lock. */
    if (w->coro_cache_count > 0) {
        return w->coro_cache[--w->coro_cache_count];
    }

    /* Local cache empty: refill a batch from the shared pool under one lock. */
    spin_lock(&pool->lock);
    int32_t n = (pool->count < SCHED_CORO_CACHE_BATCH)
                    ? pool->count
                    : SCHED_CORO_CACHE_BATCH;
    for (int32_t i = 0; i < n; i++) {
        w->coro_cache[i] = pool->slots[--pool->count];
    }
    spin_unlock(&pool->lock);

    if (n > 0) {
        w->coro_cache_count = n - 1;
        return w->coro_cache[n - 1];
    }

    /* Shared pool empty too: reserve a fresh slot. */
    return _sched_coro_slot_reserve(pool, size);
}

static void _sched_coro_pool_dealloc(
    void* ptr, size_t size, void* allocator_data) {
    _sched_coro_pool_t*    pool = (_sched_coro_pool_t*)allocator_data;
    _sched_worker_t* w    = _sched_coro_pool_local(pool);

    /* Drop the physical stack pages regardless of where the slot lands. */
    _sched_coro_stack_reset(pool, ptr, size);

    if (!w) {
        _sched_coro_pool_global_push(pool, ptr);
        return;
    }

    /* Fast path: room in the local cache, no lock. */
    if (w->coro_cache_count < SCHED_CORO_CACHE_CAP) {
        w->coro_cache[w->coro_cache_count++] = ptr;
        return;
    }

    /**
     * Local cache full: spill a batch down to the shared pool under one
     * lock, release whatever the pool refuses, then store the new slot.
     */
    int32_t keep = SCHED_CORO_CACHE_CAP - SCHED_CORO_CACHE_BATCH;
    spin_lock(&pool->lock);
    while (w->coro_cache_count > keep && pool->count < pool->cap) {
        pool->slots[pool->count++] = w->coro_cache[--w->coro_cache_count];
    }
    spin_unlock(&pool->lock);
    while (w->coro_cache_count > keep) {
        _sched_coro_slot_release(pool, w->coro_cache[--w->coro_cache_count]);
    }
    w->coro_cache[w->coro_cache_count++] = ptr;
}


static void _sched_timer_ref(scheduler_timer_t* timer) {
    atomic_fetch_add_explicit(&timer->refcnt, 1, memory_order_relaxed);
}

static void _sched_timer_unref(scheduler_timer_t* timer) {
    if (atomic_fetch_sub_explicit(
            &timer->refcnt, 1, memory_order_acq_rel) == 1) {
        free(timer);
    }
}

static int _sched_timer_cmp(
    const heap_node_t* a, const heap_node_t* b) {
    const scheduler_timer_t* ta = heap_entry(a, scheduler_timer_t, heap_node);
    const scheduler_timer_t* tb = heap_entry(b, scheduler_timer_t, heap_node);
    if (ta->timeout < tb->timeout) {
        return -1;
    }
    if (ta->timeout > tb->timeout) {
        return 1;
    }
    return 0;
}

static void _sched_wake_poller(scheduler_t* sched) {
    if (sched->wakeup_wr) {
        char c = 1;
        platform_socket_send(sched->wakeup_wr, &c, 1);
    }
}

static void _sched_wake_worker(scheduler_t* sched);

static bool _sched_try_start_search(scheduler_t* sched, _sched_worker_t* w) {
    if (atomic_load_explicit(&w->searching, memory_order_acquire)) {
        return true;
    }

    int32_t n = atomic_load_explicit(&sched->searching, memory_order_acquire);
    for (;;) {
        if (2 * n >= sched->worker_count) {
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(
                &sched->searching, &n, n + 1,
                memory_order_acq_rel, memory_order_acquire)) {
            atomic_store_explicit(&w->searching, true, memory_order_release);
            return true;
        }
    }
}

static void _sched_stop_search(
    scheduler_t* sched, _sched_worker_t* w, bool found_work) {
    if (!atomic_exchange_explicit(
            &w->searching, false, memory_order_acq_rel)) {
        return;
    }

    int32_t prev = atomic_fetch_sub_explicit(
        &sched->searching, 1, memory_order_acq_rel);
    if (found_work && prev == 1) {
        _sched_wake_worker(sched);
    }
}

static void _sched_wake_worker(scheduler_t* sched) {
    /**
     * Order the work the caller just published (its runq push) before the
     * parked/poller reads below. This is the producer half of the Dekker
     * handshake in the worker park path: the parking worker publishes
     * parked=true and re-pops the runq behind a matching seq_cst fence, so
     * a producer that observes parked==false here is guaranteed the worker's
     * re-pop will observe this push -- neither side can miss the other.
     */
    atomic_thread_fence(memory_order_seq_cst);

    int32_t expected_searching = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &sched->searching, &expected_searching, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        if (atomic_load_explicit(
                &sched->poller_waiting, memory_order_seq_cst)) {
            _sched_wake_poller(sched);
        }
        return;
    }

    for (int32_t i = 0; i < sched->worker_count; i++) {
        bool expected = true;
        if (atomic_compare_exchange_strong_explicit(
                &sched->workers[i].parked, &expected, false,
                memory_order_acq_rel, memory_order_acquire)) {
            atomic_store_explicit(
                &sched->workers[i].searching, true, memory_order_release);
            platform_sem_post(sched->workers[i].sem);
            return;
        }
    }
    atomic_fetch_sub_explicit(&sched->searching, 1, memory_order_acq_rel);
    if (atomic_load_explicit(
            &sched->poller_waiting, memory_order_seq_cst)) {
        _sched_wake_poller(sched);
    }
}

/**
 * Wake at most one parked worker for a freshly published batch. A woken worker
 * is marked searching; while any worker is searching, further notifications do
 * not wake more workers. When the last searching worker finds work, it wakes
 * one more parked worker, producing Tokio-style controlled expansion instead
 * of a batch-sized wakeup herd.
 */
static void _sched_wake_workers(scheduler_t* sched, int32_t count) {
    if (count <= 0) {
        return;
    }
    _sched_wake_worker(sched);
}

static inline uint32_t _sched_rng_next(_sched_worker_t* w) {
    /* xorshift32; seed is kept non-zero at worker init. */
    uint32_t x = w->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    w->rng = x;
    return x;
}

static inline int32_t _sched_worker_grab_cap(wsq_t* dq) {
    int32_t rem = wsq_remaining(dq);
    int32_t half = SCHED_DEQUE_CAP / 2;
    return rem < half ? rem : half;
}

static inline _sched_coro_ctx_t* _sched_coro_ctx(mco_coro* co) {
    return (_sched_coro_ctx_t*)mco_get_user_data(co);
}

static inline mco_coro* _sched_runq_node_coro(queue_node_t* node) {
    return queue_entry(node, _sched_coro_ctx_t, runq_node)->co;
}

static mco_coro* _sched_pop_global_one(scheduler_t* sched) {
    queue_node_t* node = runq_pop(sched->runq);
    return node ? _sched_runq_node_coro(node) : NULL;
}

static mco_coro* _sched_worker_pop_global(
    scheduler_t* sched, _sched_worker_t* w) {
    int32_t cap = _sched_worker_grab_cap(w->deque);
    queue_node_t* nodes[(SCHED_DEQUE_CAP / 2)];
    int32_t n = runq_pop_fair(sched->runq, nodes, cap, sched->worker_count);
    if (n <= 0) {
        return NULL;
    }

    for (int32_t i = 1; i < n; i++) {
        wsq_push(w->deque, _sched_runq_node_coro(nodes[i]));
    }
    return _sched_runq_node_coro(nodes[0]);
}

static mco_coro* _sched_worker_pop_coro(scheduler_t* sched, _sched_worker_t* w) {
    mco_coro* co = atomic_exchange(&w->runnext, NULL);
    if (co) {
        return co;
    }

    co = (mco_coro*)wsq_pop(w->deque);
    if (co) {
        return co;
    }

    return _sched_worker_pop_global(sched, w);
}

static mco_coro* _sched_worker_steal_coro(scheduler_t* sched, _sched_worker_t* w) {
    if (sched->worker_count <= 1) {
        return NULL;
    }
    if (!_sched_try_start_search(sched, w)) {
        return NULL;
    }
    int32_t cap = _sched_worker_grab_cap(w->deque);

    uint32_t n     = (uint32_t)sched->worker_count;
    uint32_t start = _sched_rng_next(w) % n;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (start + i) % n;
        if (idx != w->index) {
            void* batch[(SCHED_DEQUE_CAP / 2)];
            int32_t cnt = wsq_steal_half(
                sched->workers[idx].deque, batch, cap);
            if (cnt > 0) {
                for (int32_t j = 1; j < cnt; j++) {
                    wsq_push(w->deque, batch[j]);
                }
                return (mco_coro*)batch[0];
            }
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (start + i) % n;
        if (idx != w->index) {
            mco_coro* co = atomic_exchange_explicit(
                &sched->workers[idx].runnext, NULL,
                memory_order_acq_rel);
            if (co) {
                return co;
            }
        }
    }

    return NULL;
}

static mco_coro* _sched_worker_pop_or_steal(
    scheduler_t* sched, _sched_worker_t* w) {
    mco_coro* co = _sched_worker_pop_coro(sched, w);
    return co ? co : _sched_worker_steal_coro(sched, w);
}

static mco_coro* _sched_worker_pop_fair(
    scheduler_t* sched, _sched_worker_t* w) {
    /* Prime: avoids sync with power-of-two deque sizes. */
    if (++w->sched_tick % 61 != 0) {
        return NULL;
    }

    mco_coro* co = _sched_pop_global_one(sched);
    return co ? co : _sched_worker_steal_coro(sched, w);
}

static int _sched_timeout_locked(_sched_worker_t* w, uint64_t now) {
    heap_node_t* root = heap_peek(&w->timers);
    if (!root) {
        return -1;
    }
    scheduler_timer_t* t = heap_entry(root, scheduler_timer_t, heap_node);
    if (t->timeout <= now) {
        return 0;
    }
    uint64_t diff = t->timeout - now;
    return (diff > INT32_MAX) ? INT32_MAX : (int)diff;
}

/**
 * Republish the earliest timer deadline so the lock-free guards in
 * _sched_process_timers / _sched_timer_next_timeout can skip the lock when
 * nothing is due. Caller MUST hold w->timer_lock. Release-ordered to pair
 * with the acquire loads on the worker's hot path.
 */
static inline void _sched_publish_deadline(_sched_worker_t* w) {
    heap_node_t* root = heap_peek(&w->timers);
    uint64_t nd = root
        ? heap_entry(root, scheduler_timer_t, heap_node)->timeout
        : UINT64_MAX;
    atomic_store_explicit(&w->next_deadline_ms, nd, memory_order_release);
}

static int _sched_timer_next_timeout(_sched_worker_t* w) {
    uint64_t nd = atomic_load_explicit(
        &w->next_deadline_ms, memory_order_acquire);
    if (nd == UINT64_MAX) {
        return -1;
    }
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    if (nd <= now) {
        return 0;
    }
    uint64_t diff = nd - now;
    return (diff > INT32_MAX) ? INT32_MAX : (int)diff;
}

static void _sched_timer_spawn_entry(void* arg) {
    scheduler_timer_t* t = (scheduler_timer_t*)arg;
    t->cb(t, t->ud);
    if (t->ud_unref) {
        t->ud_unref(t->ud);
    }
    _sched_timer_unref(t);
}

static int _sched_process_timers(_sched_worker_t* w, uint64_t now_ms) {
    /**
     * Lock-free fast path: skip the lock entirely when no timer is due.
     * next_deadline_ms is republished under the lock on every heap mutation,
     * so an acquire load here pairs with those release stores; a concurrent
     * arm that lands just after this check is caught next iteration (within
     * one tick) or by the wake the arm itself issues.
     */
    if (now_ms < atomic_load_explicit(
            &w->next_deadline_ms, memory_order_acquire)) {
        return -1;
    }

    for (;;) {
        scheduler_timer_t* timer = NULL;

        mtx_lock(&w->timer_lock);
        heap_node_t* root = heap_peek(&w->timers);
        if (root) {
            scheduler_timer_t* t = heap_entry(root, scheduler_timer_t, heap_node);
            if (t->timeout <= now_ms) {
                heap_dequeue(&w->timers);
                if (t->repeat > 0) {
                    t->timeout = now_ms + t->repeat;
                    heap_insert(&w->timers, &t->heap_node);
                } else {
                    t->active = false;
                }
                _sched_timer_ref(t);
                /**
                 * Pin ud while still holding timer_lock, atomically with
                 * pulling this fire off the heap. A concurrent teardown
                 * that frees ud only after dropping its own reference
                 * therefore cannot win the race: either it takes the lock
                 * first and we never dispatch (ud already gone, timer not
                 * found here), or we ref ud here and its free is held off
                 * until the matching ud_unref below. Released after the
                 * callback returns (inline) or in the spawn entry.
                 */
                if (t->ud_ref) {
                    t->ud_ref(t->ud);
                }
                timer = t;
            }
        }
        mtx_unlock(&w->timer_lock);

        if (!timer) {
            break;
        }

        if (timer->spawn) {
            if (scheduler_spawn(
                    w->sched, _sched_timer_spawn_entry, timer) != 0) {
                xylem_loge("<sched> timer spawn failed");
                if (timer->ud_unref) {
                    timer->ud_unref(timer->ud);
                }
                _sched_timer_unref(timer);
            }
        } else {
            timer->cb(timer, timer->ud);
            if (timer->ud_unref) {
                timer->ud_unref(timer->ud);
            }
            _sched_timer_unref(timer);
        }
    }

    mtx_lock(&w->timer_lock);
    int timeout = _sched_timeout_locked(w, now_ms);
    _sched_publish_deadline(w);
    mtx_unlock(&w->timer_lock);
    return timeout;
}

/**
 * Poll timeout (ms) derived from the earliest timer deadline across ALL
 * workers, so the idle poll driver wakes in time to service timers owned by
 * a worker that is stuck running a long coroutine (timer stealing). Returns
 * -1 when no timer is armed anywhere. Each deadline is the lock-free
 * republished hint, so this is a cheap scan of one atomic per worker.
 */
static int _sched_timeout_all(scheduler_t* sched) {
    uint64_t best = UINT64_MAX;
    for (int32_t i = 0; i < sched->worker_count; i++) {
        uint64_t nd = atomic_load_explicit(
            &sched->workers[i].next_deadline_ms, memory_order_acquire);
        if (nd < best) {
            best = nd;
        }
    }
    if (best == UINT64_MAX) {
        return -1;
    }
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    if (best <= now) {
        return 0;
    }
    uint64_t diff = best - now;
    return (diff > INT32_MAX) ? INT32_MAX : (int)diff;
}

/**
 * Fire due timers for every worker (timer stealing). Run by the idle poll
 * driver so timers owned by a worker busy in a long coroutine still fire on
 * time. Each worker's heap is processed under its own timer_lock inside
 * _sched_process_timers; the fast-path deadline guard there makes a
 * not-yet-due worker a single atomic load with no lock. Concurrent firing by
 * the owner itself is safe: both contend the same timer_lock and heap_dequeue
 * hands each due timer to exactly one of them.
 */
static void _sched_process_timers_all(scheduler_t* sched, uint64_t now_ms) {
    for (int32_t i = 0; i < sched->worker_count; i++) {
        _sched_process_timers(&sched->workers[i], now_ms);
    }
}

static void _sched_process_posts(scheduler_t* sched) {
    mpsc_node_t* node;
    while ((node = mpsc_pop(&sched->posts)) != NULL) {
        _sched_post_t* req = mpsc_entry(node, _sched_post_t, node);
        req->cb(req->ud);
        free(req);
    }
}

static void _sched_try_process_posts(scheduler_t* sched) {
    bool expected = false;
    if (atomic_compare_exchange_strong(
            &sched->post_draining, &expected, true)) {
        _sched_process_posts(sched);
        atomic_store(&sched->post_draining, false);
    }
}

static bool _sched_try_acquire_poller(scheduler_t* sched) {
    bool expected = false;
    return atomic_compare_exchange_strong_explicit(
        &sched->poller_running, &expected, true,
        memory_order_acq_rel, memory_order_acquire);
}

static void _sched_release_poller(scheduler_t* sched) {
    atomic_store_explicit(
        &sched->poller_running, false, memory_order_release);
}

static void _sched_set_poller_waiting(scheduler_t* sched, bool waiting) {
    atomic_store_explicit(
        &sched->poller_waiting,
        waiting,
        waiting ? memory_order_seq_cst : memory_order_relaxed);
}

static mco_coro* _sched_process_io(
    scheduler_t* sched,
    _sched_worker_t* w,
    platform_poller_cqe_t* cqes,
    int n) {
    mco_coro* batch_coros[PLATFORM_POLLER_CQE_NUM * 2];
    runnable_batch_t batch = {
        .coros = batch_coros,
        .cap = (int32_t)(sizeof(batch_coros) / sizeof(batch_coros[0])),
        .n   = 0,
    };

    bool woken = false;

    for (int i = 0; i < n; i++) {
        if (cqes[i].ud == NULL) {
            woken = true;
            continue;
        }
        iowait_on_event(sched, (int)cqes[i].op, cqes[i].ud, &batch);
    }

    if (woken) {
        char buf[64];
        while (platform_socket_recv(sched->wakeup_rd, buf, sizeof(buf)) > 0) {
        }
        if (PLATFORM_POLLER_TRIGGER_MODE != PLATFORM_POLLER_TRIGGER_ET) {
            sched->wakeup_sqe.op = PLATFORM_POLLER_RD_OP;
            platform_poller_mod(&sched->poller, &sched->wakeup_sqe);
        }
    }

    if (batch.n <= 0) {
        return NULL;
    }
    /*
     * Tokio-style IO dispatch: run one task now, then keep the rest local to
     * the driver worker so the hot path avoids global runq injection. Other
     * workers can steal from this deque; only overflow goes to the global runq.
     */
    mco_coro* first = NULL;
    queue_node_t* spill[PLATFORM_POLLER_CQE_NUM * 2];
    int32_t spill_n = 0;
    int32_t local_n = 0;
    int32_t local_cap = SCHED_DEQUE_CAP / 2;
    bool local_queued = false;

    for (int32_t i = 0; i < batch.n; i++) {
        mco_coro* co = batch.coros[i];
        if (!_sched_claim_for_wake(co)) {
            continue;
        }
        if (!first) {
            first = co;
            continue;
        }
        if (local_n < local_cap && wsq_push(w->deque, co) == 0) {
            local_n++;
            local_queued = true;
        } else {
            _sched_coro_ctx_t* ctx = _sched_coro_ctx(co);
            spill[spill_n++] = &ctx->runq_node;
        }
    }

    if (spill_n > 0) {
        runq_push_batch(sched->runq, spill, spill_n);
    }
    if (local_queued || spill_n > 0) {
        _sched_wake_worker(sched);
    }
    return first;
}

static void _sched_process_idle_work(scheduler_t* sched) {
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    _sched_process_timers_all(sched, now);
    _sched_try_process_posts(sched);
}

/* Requeue on the parking worker after a declined or claimed park. */
static void _sched_enqueue_local(_sched_worker_t* w, mco_coro* co) {
    if (wsq_push(w->deque, co) != 0) {
        _sched_coro_ctx_t* ctx = _sched_coro_ctx(co);
        runq_push(w->sched->runq, &ctx->runq_node);
        _sched_wake_worker(w->sched);
    }
}

static void _sched_handle_yield(_sched_worker_t* w, mco_coro* co) {
    if (mco_status(co) == MCO_DEAD) {
        _sched_coro_ctx_t* ctx = _sched_coro_ctx(co);

        spin_lock(&w->sched->registry_lock);
        list_remove(&w->sched->registry, &ctx->registry_node);
        spin_unlock(&w->sched->registry_lock);

        free(ctx);
        mco_destroy(co);

        scheduler_t* sched = w->sched;
        int64_t prev = atomic_fetch_sub(&sched->alive, 1);
        if (prev == 1 && sched->idle_cb) {
            sched->idle_cb(sched->idle_ud);
        }
        return;
    }
    if (!w->park_fn) {
        xylem_loge("<sched> yield without park co=%p", (void*)co);
        scheduler_schedule(w->sched, co);
        return;
    }
    scheduler_park_fn_t fn = w->park_fn;
    void* arg = w->park_arg;
    w->park_fn  = NULL;
    w->park_arg = NULL;

    _sched_coro_ctx_t* ctx = _sched_coro_ctx(co);
    atomic_store_explicit(&ctx->park_state, PARK_ARMING, memory_order_release);

    if (fn(co, arg)) {
        /**
         * Commit the park with a CAS, not a plain store: a waker may have
         * raced in during the callback and set CLAIMED. A blind store
         * would clobber that and strand the coroutine forever -- the waker
         * deliberately did not enqueue, expecting us to (lost wakeup). The
         * CAS parks only if we are still ARMING, i.e. untouched by a waker.
         */
        _park_state_t expected = PARK_ARMING;
        if (atomic_compare_exchange_strong_explicit(
                &ctx->park_state, &expected, PARK_PARKED,
                memory_order_acq_rel, memory_order_acquire)) {
            return; /* cleanly parked; a waker will requeue us */
        }
        /**
         * CAS failed, so expected == PARK_CLAIMED: a waker claimed us
         * mid-callback and deliberately did not enqueue, so the requeue
         * is ours.
         */
    }

    /* Declined, or claimed during arming: requeue on this worker. */
    atomic_store_explicit(&ctx->park_state, PARK_IDLE, memory_order_relaxed);
    /**
     * Enqueue raw, without _sched_claim_for_wake: we are not a waker. The
     * coro is not on any wait structure (either the park callback declined
     * to park, or the lone waker that set CLAIMED already removed it and
     * left the requeue to us), so no other waker can reference it -- there
     * is nothing to dedup against. Routing this through the claim would be
     * wrong anyway: park_state is now IDLE, which the claim treats as a
     * normal always-enqueue, so it would add no protection.
     */
    _sched_enqueue_local(w, co);
}

static bool _sched_credit_park_cb(mco_coro* co, void* arg) {
    (void)co;
    (void)arg;
    return false;
}

static inline void _sched_run_coro(_sched_worker_t* w, mco_coro* co) {
    w->credit = SCHED_CREDIT_DEFAULT;
    mco_resume(co);
    _sched_handle_yield(w, co);
}

static void _sched_drain(_sched_worker_t* w, scheduler_t* sched) {
    for (;;) {
        mco_coro* co = _sched_worker_pop_coro(sched, w);
        if (!co) {
            co = _sched_worker_steal_coro(sched, w);
        }
        if (!co) {
            break;
        }
        _sched_run_coro(w, co);
    }
}

static void _sched_maintenance(scheduler_t* sched, _sched_worker_t* w) {
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    _sched_process_timers(w, now);

    uint64_t last = atomic_load_explicit(
        &sched->last_maintenance_ms, memory_order_relaxed);
    if (now - last < SCHED_TIMER_TICK_MS) {
        return;
    }
    if (!atomic_compare_exchange_strong_explicit(
            &sched->last_maintenance_ms,
            &last,
            now,
            memory_order_acq_rel,
            memory_order_relaxed)) {
        return;
    }

    _sched_try_process_posts(sched);
}

static mco_coro* _sched_worker_drive_poller(
    scheduler_t*           sched,
    _sched_worker_t*       w,
    platform_poller_cqe_t* cqes) {
    while (atomic_load(&sched->running)) {
        /* Must set before re-check so producers see it and pipe-wake. */
        _sched_set_poller_waiting(sched, true);

        mco_coro* co = _sched_worker_pop_or_steal(sched, w);
        if (co) {
            _sched_set_poller_waiting(sched, false);
            _sched_release_poller(sched);
            return co;
        }

        int poll_ms = _sched_timeout_all(sched);
        _sched_stop_search(sched, w, false);
        int n = platform_poller_wait(&sched->poller, cqes, poll_ms);
        _sched_set_poller_waiting(sched, false);
        co = n > 0 ? _sched_process_io(sched, w, cqes, n) : NULL;

        _sched_process_idle_work(sched);

        if (!co) {
            co = _sched_worker_pop_or_steal(sched, w);
        }
        if (co) {
            _sched_stop_search(sched, w, true);
            _sched_release_poller(sched);
            return co;
        }
    }

    _sched_release_poller(sched);
    return NULL;
}

static mco_coro* _sched_worker_find_coro(
    scheduler_t*           sched,
    _sched_worker_t*       w,
    platform_poller_cqe_t* cqes) {

    mco_coro* co = _sched_worker_pop_coro(sched, w);
    if (co) {
        return co;
    }

    if (_sched_try_acquire_poller(sched)) {
        int n = platform_poller_wait(&sched->poller, cqes, 0);
        co = n > 0 ? _sched_process_io(sched, w, cqes, n) : NULL;
        _sched_release_poller(sched);
        if (co) {
            return co;
        }
    }

    co = _sched_worker_steal_coro(sched, w);
    if (co) {
        return co;
    }

    if (!_sched_try_acquire_poller(sched)) {
        return NULL;
    }
    return _sched_worker_drive_poller(sched, w, cqes);
}

static mco_coro* _sched_worker_park(
    scheduler_t* sched, _sched_worker_t* w) {
    /*
     * Lost-wakeup safety is a Dekker handshake with _sched_wake_worker:
     *
     *   worker:    store parked=true; seq_cst fence; re-pop runq
     *   producer:  push to runq;      seq_cst fence; load parked
     */
    atomic_store(&w->parked, true);
    atomic_thread_fence(memory_order_seq_cst);

    mco_coro* co = _sched_pop_global_one(sched);
    if (co) {
        atomic_store(&w->parked, false);
        return co;
    }

    int timer_ms = _sched_timer_next_timeout(w);
    _sched_stop_search(sched, w, false);
    if (timer_ms >= 0) {
        platform_sem_timedwait(w->sem, (uint64_t)timer_ms);
    } else {
        platform_sem_wait(w->sem);
    }
    atomic_store(&w->parked, false);
    return NULL;
}

static int _sched_worker_entry(void* arg) {
    _sched_worker_t* w = (_sched_worker_t*)arg;
    scheduler_t* sched = w->sched;
    _tls_worker = w;

    platform_poller_cqe_t cqes[PLATFORM_POLLER_CQE_NUM];

    while (atomic_load(&sched->running)) {
        _sched_maintenance(sched, w);

        mco_coro* co = _sched_worker_pop_fair(sched, w);
        if (!co) {
            co = _sched_worker_find_coro(sched, w, cqes);
        }
        if (co) {
            _sched_stop_search(sched, w, true);
            _sched_run_coro(w, co);
            continue;
        }

        co = _sched_worker_park(sched, w);
        if (co) {
            _sched_stop_search(sched, w, true);
            _sched_run_coro(w, co);
        }
    }

    _sched_drain(w, sched);
    return 0;
}

static void _sched_cleanup(scheduler_t* sched, int32_t nstarted) {
    atomic_store(&sched->running, false);

    if (sched->workers) {
        if (!sched->joined) {
            for (int32_t i = 0; i < nstarted; i++) {
                platform_sem_post(sched->workers[i].sem);
                _sched_wake_poller(sched);
            }
            for (int32_t i = 0; i < nstarted; i++) {
                thrd_join(sched->workers[i].thread, NULL);
            }
            sched->joined = true;
        }

        /* iowait timers call scheduler_timer_stop which takes timer_lock. */
        iowait_slab_destroy(sched->iowait_slab);
        sched->iowait_slab = NULL;

        for (int32_t i = 0; i < sched->worker_count; i++) {
            _sched_worker_t* w = &sched->workers[i];
            if (w->deque) {
                wsq_destroy(w->deque);
            }
            if (w->sem) {
                platform_sem_destroy(w->sem);
            }
            {
                heap_node_t* n;
                while ((n = heap_peek(&w->timers)) != NULL) {
                    scheduler_timer_t* t = heap_entry(n, scheduler_timer_t, heap_node);
                    heap_dequeue(&w->timers);
                    t->active = false;
                    _sched_timer_unref(t);
                }
            }
            mtx_destroy(&w->timer_lock);
            if (w->coro_cache) {
                for (int32_t j = 0; j < w->coro_cache_count; j++) {
                    _sched_coro_slot_release(&sched->coro_pool, w->coro_cache[j]);
                }
                free(w->coro_cache);
            }
        }
        free(sched->workers);
    }

    if (sched->runq) {
        runq_destroy(sched->runq);
    }

    _sched_process_posts(sched);

    if (sched->wakeup_rd) {
        platform_poller_del(&sched->poller, &sched->wakeup_sqe);
        platform_socket_close(sched->wakeup_rd);
        platform_socket_close(sched->wakeup_wr);
    }

    platform_poller_deinit(&sched->poller);
    if (sched->iowait_slab) {
        iowait_slab_destroy(sched->iowait_slab);
    }

    for (int32_t i = 0; i < sched->coro_pool.count; i++) {
        _sched_coro_slot_release(&sched->coro_pool, sched->coro_pool.slots[i]);
    }
    free(sched->coro_pool.slots);

    free(sched);
}

scheduler_t* scheduler_create(scheduler_opts_t* opts) {
    scheduler_t* sched = (scheduler_t*)calloc(1, sizeof(scheduler_t));
    if (!sched) {
        return NULL;
    }

    int32_t worker_count = (int32_t)platform_info_getcpus();
    if (worker_count < 1) {
        worker_count = 4;
    }

    uint32_t deque_capacity = SCHED_DEQUE_CAP;

    if (opts) {
        if (opts->worker_count > 0) {
            worker_count = opts->worker_count;
        }
        if (opts->deque_capacity > 0) {
            deque_capacity = opts->deque_capacity;
        }
    }

    sched->runq = runq_create();
    if (!sched->runq) {
        _sched_cleanup(sched, 0);
        return NULL;
    }

    mpsc_init(&sched->posts);
    list_init(&sched->registry);
    spin_init(&sched->registry_lock);

    platform_poller_init(&sched->poller);
    {
        platform_sock_t pair[2];
        if (platform_socket_socketpair(0, SOCK_STREAM, 0, pair) == 0) {
            sched->wakeup_rd = pair[0];
            sched->wakeup_wr = pair[1];
            platform_socket_enable_nonblocking(sched->wakeup_rd, true);
            platform_socket_enable_nonblocking(sched->wakeup_wr, true);

            memset(&sched->wakeup_sqe, 0, sizeof(sched->wakeup_sqe));
            sched->wakeup_sqe.fd = (platform_poller_fd_t)sched->wakeup_rd;
            sched->wakeup_sqe.op = PLATFORM_POLLER_RD_OP;
            sched->wakeup_sqe.ud = NULL;
            platform_poller_add(&sched->poller, &sched->wakeup_sqe);
        }
    }

    atomic_store(&sched->running, true);
    atomic_store(&sched->searching, 0);

    sched->iowait_slab = iowait_slab_create();
    if (!sched->iowait_slab) {
        _sched_cleanup(sched, 0);
        return NULL;
    }

    {
        uint32_t pool_cap = (uint32_t)(worker_count * SCHED_CORO_POOL_CAP_MUL);
        if (opts && opts->coro_pool_capacity > 0) {
            pool_cap = opts->coro_pool_capacity;
        }
        size_t stack_size = SCHED_CORO_STACK_SIZE;
        if (opts && opts->coro_stack_size > 0) {
            stack_size = opts->coro_stack_size;
        }
        mco_desc tmp = mco_desc_init(_sched_coro_entry, stack_size);
        sched->coro_pool.slot_size  = tmp.coro_size;
        sched->coro_pool.stack_size = stack_size;
        sched->coro_pool.cap        = (int32_t)pool_cap;
        sched->coro_pool.count      = 0;
        spin_init(&sched->coro_pool.lock);
        sched->coro_pool.slots = (void**)calloc(
            pool_cap, sizeof(void*));
        if (!sched->coro_pool.slots) {
            _sched_cleanup(sched, 0);
            return NULL;
        }
    }

    sched->worker_count = worker_count;
    sched->workers = (_sched_worker_t*)calloc(
        (size_t)worker_count, sizeof(_sched_worker_t));
    if (!sched->workers) {
        _sched_cleanup(sched, 0);
        return NULL;
    }

    for (int32_t i = 0; i < worker_count; i++) {
        _sched_worker_t* w = &sched->workers[i];
        w->deque = wsq_create(deque_capacity);
        w->sem = platform_sem_create(0);
        w->sched = sched;
        w->index = (uint32_t)i;
        w->rng = 2654435761u * (uint32_t)(i + 1); /* non-zero xorshift seed */
        atomic_store(&w->searching, false);
        heap_init(&w->timers, _sched_timer_cmp);
        mtx_init(&w->timer_lock, mtx_plain);
        atomic_init(&w->next_deadline_ms, UINT64_MAX);
        w->coro_cache = (void**)calloc(
            SCHED_CORO_CACHE_CAP, sizeof(void*));
        w->coro_cache_count = 0;

        if (!w->deque || !w->sem || !w->coro_cache) {
            _sched_cleanup(sched, 0);
            return NULL;
        }
    }

    for (int32_t i = 0; i < worker_count; i++) {
        if (thrd_create(&sched->workers[i].thread,
                        _sched_worker_entry,
                        &sched->workers[i]) != thrd_success) {
            _sched_cleanup(sched, i);
            return NULL;
        }
    }

    return sched;
}

void scheduler_destroy(scheduler_t* sched) {
    if (!sched) {
        return;
    }

    scheduler_stop(sched);

    spin_lock(&sched->registry_lock);
    while (!list_empty(&sched->registry)) {
        list_node_t* n = list_head(&sched->registry);
        list_remove(&sched->registry, n);
        spin_unlock(&sched->registry_lock);

        _sched_coro_ctx_t* ctx = list_entry(n, _sched_coro_ctx_t, registry_node);
        mco_destroy(ctx->co);
        free(ctx);

        spin_lock(&sched->registry_lock);
    }
    spin_unlock(&sched->registry_lock);

    _sched_cleanup(sched, sched->worker_count);
}

void scheduler_stop(scheduler_t* sched) {
    if (!sched || sched->joined) {
        return;
    }

    atomic_store(&sched->running, false);
    for (int32_t i = 0; i < sched->worker_count; i++) {
        platform_sem_post(sched->workers[i].sem);
        _sched_wake_poller(sched);
    }
    for (int32_t i = 0; i < sched->worker_count; i++) {
        thrd_join(sched->workers[i].thread, NULL);
    }
    sched->joined = true;
}

static void _sched_enqueue(scheduler_t* sched, mco_coro* co) {
    _sched_coro_ctx_t* ctx = _sched_coro_ctx(co);

    if (!_tls_worker || _tls_worker->sched != sched) {
        runq_push(sched->runq, &ctx->runq_node);
        _sched_wake_worker(sched);
        return;
    }

    /**
     * Publish co into this worker's LIFO slot. The owner checks it first for
     * locality; stealers may take it only after regular deque stealing fails.
     */
    mco_coro* old = atomic_exchange(&_tls_worker->runnext, co);
    if (old) {
        if (wsq_push(_tls_worker->deque, old) != 0) {
            void* batch[(SCHED_DEQUE_CAP / 2) + 1];
            int32_t n = wsq_pop_half(
                _tls_worker->deque, batch, (SCHED_DEQUE_CAP / 2));
            batch[n++] = old;

            queue_node_t* nodes[(SCHED_DEQUE_CAP / 2) + 1];
            for (int32_t i = 0; i < n; i++) {
                _sched_coro_ctx_t* c =
                    _sched_coro_ctx(batch[i]);
                nodes[i] = &c->runq_node;
            }
            runq_push_batch(sched->runq, nodes, n);
        }
        /**
         * The displaced `old` is now stealable (queue/global), so wake a
         * parked worker to pick it up.
         */
        _sched_wake_worker(sched);
    }

    /**
     * `co` went into an empty local LIFO slot: this worker normally consumes
     * it on its next schedule. Skipping a sibling wake preserves the cheap
     * hand-off; if the owner stalls, stealers can still take the slot after
     * they fail to steal regular deque work.
     */
}

/**
 * Transition a coroutine that a waker wants to run. Returns true if the
 * caller should enqueue it now, false if it must not (either the park
 * callback is still arming and will requeue on return, or another claim
 * already won). A coroutine not in a park handshake (PARK_IDLE) is a
 * normal schedule and is always enqueued.
 */
static bool _sched_claim_for_wake(mco_coro* co) {
    _sched_coro_ctx_t* ctx = _sched_coro_ctx(co);
    _park_state_t st =
        atomic_load_explicit(&ctx->park_state, memory_order_acquire);
    for (;;) {
        switch (st) {
        case PARK_IDLE:
            /* Not in a park handshake: a normal schedule, always enqueue. */
            return true;
        case PARK_PARKED:
            /* Park callback already returned; claim it, we requeue. */
            if (atomic_compare_exchange_weak_explicit(
                    &ctx->park_state, &st, PARK_CLAIMED,
                    memory_order_acq_rel, memory_order_acquire)) {
                return true;
            }
            break; /* st reloaded by the CAS; re-evaluate */
        case PARK_ARMING:
            /* Park callback still arming; it sees CLAIMED and requeues. */
            if (atomic_compare_exchange_weak_explicit(
                    &ctx->park_state, &st, PARK_CLAIMED,
                    memory_order_acq_rel, memory_order_acquire)) {
                return false;
            }
            break; /* st reloaded (ARMING may have advanced to PARKED) */
        case PARK_CLAIMED:
            /* Another waker already claimed it; nothing for us to do. */
            return false;
        }
    }
}

void scheduler_schedule(scheduler_t* sched, mco_coro* co) {
    if (_sched_claim_for_wake(co)) {
        _sched_enqueue(sched, co);
    }
}

void scheduler_schedule_batch(
    scheduler_t* sched, mco_coro** cos, int32_t n) {
    if (n <= 0) {
        return;
    }

    if (_tls_worker && _tls_worker->sched == sched && sched->worker_count == 1) {
        for (int32_t i = 0; i < n; i++) {
            if (!_sched_claim_for_wake(cos[i])) {
                continue;
            }
            _sched_enqueue_local(_tls_worker, cos[i]);
        }
        return;
    }

    enum { INLINE_CAP = 64 };
    queue_node_t*  inline_nodes[INLINE_CAP];
    queue_node_t** nodes = inline_nodes;
    if (n > INLINE_CAP) {
        nodes = (queue_node_t**)calloc((size_t)n, sizeof(*nodes));
        if (!nodes) {
            for (int32_t i = 0; i < n; i++) {
                scheduler_schedule(sched, cos[i]);
            }
            return;
        }
    }
    int32_t m = 0;
    for (int32_t i = 0; i < n; i++) {
        /**
         * A coro still arming its park must not be enqueued here; its park
         * callback owns the requeue. Drop it from the batch.
         */
        if (!_sched_claim_for_wake(cos[i])) {
            continue;
        }
        _sched_coro_ctx_t* ctx = _sched_coro_ctx(cos[i]);
        nodes[m++] = &ctx->runq_node;
    }
    if (m > 0) {
        runq_push_batch(sched->runq, nodes, m);
    }
    if (nodes != inline_nodes) {
        free(nodes);
    }

    if (m > 0) {
        _sched_wake_workers(sched, m);
    }
}

int scheduler_spawn(scheduler_t* sched, void (*fn)(void*), void* arg) {
    _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)calloc(1, sizeof(_sched_coro_ctx_t));
    if (!ctx) {
        return -1;
    }

    ctx->fn = fn;
    ctx->arg = arg;

    mco_desc desc = mco_desc_init(
        _sched_coro_entry, sched->coro_pool.stack_size);
    desc.alloc_cb       = _sched_coro_pool_alloc;
    desc.dealloc_cb     = _sched_coro_pool_dealloc;
    desc.allocator_data = &sched->coro_pool;
    desc.user_data      = ctx;

    mco_coro* co = NULL;
    if (mco_create(&co, &desc) != MCO_SUCCESS) {
        free(ctx);
        return -1;
    }

    ctx->co = co;

    spin_lock(&sched->registry_lock);
    list_insert_tail(&sched->registry, &ctx->registry_node);
    spin_unlock(&sched->registry_lock);

    atomic_fetch_add(&sched->alive, 1);
    scheduler_schedule(sched, co);
    return 0;
}

void scheduler_park(
    scheduler_t* sched, scheduler_park_fn_t fn, void* arg) {
    (void)sched;
    if (!_tls_worker || !mco_running()) {
        xylem_loge(
            "<sched> park outside coroutine worker=%p co=%p",
            (void*)_tls_worker, (void*)mco_running());
        abort();
    }

    _tls_worker->park_fn  = fn;
    _tls_worker->park_arg = arg;
    mco_yield(mco_running());

    /* Resumed: clear park bookkeeping so the coro runs as PARK_IDLE. */
    _sched_coro_ctx_t* ctx = _sched_coro_ctx(mco_running());
    atomic_store_explicit(&ctx->park_state, PARK_IDLE, memory_order_relaxed);
}

bool scheduler_consume_credit(uint32_t cost) {
    if (cost == 0 || !_tls_worker || !mco_running()) {
        return false;
    }
    if (_tls_worker->credit > cost) {
        _tls_worker->credit -= cost;
        return false;
    }
    _tls_worker->credit = 0;
    return true;
}

void scheduler_yield_credit(void) {
    if (!_tls_worker || !mco_running()) {
        return;
    }
    scheduler_park(_tls_worker->sched, _sched_credit_park_cb, NULL);
}

platform_poller_sq_t* scheduler_get_poller(scheduler_t* sched) {
    return &sched->poller;
}

iowait_slab_t* scheduler_get_iowait_slab(scheduler_t* sched) {
    return sched->iowait_slab;
}

int scheduler_post(
    scheduler_t* sched, scheduler_post_fn_t cb, void* ud) {
    _sched_post_t* req = (_sched_post_t*)calloc(1, sizeof(*req));
    if (!req) {
        return -1;
    }
    req->cb = cb;
    req->ud = ud;
    mpsc_push(&sched->posts, &req->node);
    _sched_wake_worker(sched);
    return 0;
}

scheduler_timer_t* scheduler_timer_create(scheduler_t* sched) {
    scheduler_timer_t* t = (scheduler_timer_t*)calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->sched = sched;
    if (_tls_worker) {
        t->owner = _tls_worker->index;
    } else {
        uint32_t rr = atomic_fetch_add_explicit(
            &sched->timer_rr, 1, memory_order_relaxed);
        t->owner = rr % (uint32_t)sched->worker_count;
    }
    _sched_timer_ref(t);
    return t;
}

void scheduler_timer_set_spawn(scheduler_timer_t* timer, bool spawn) {
    timer->spawn = spawn;
}

void scheduler_timer_set_ud_guard(
    scheduler_timer_t*      timer,
    scheduler_timer_ud_fn_t ref,
    scheduler_timer_ud_fn_t unref) {
    timer->ud_ref   = ref;
    timer->ud_unref = unref;
}

void scheduler_timer_destroy(scheduler_timer_t* timer) {
    if (!timer) {
        return;
    }
    scheduler_timer_stop(timer);
    _sched_timer_unref(timer);
}

void scheduler_timer_start(
    scheduler_timer_t*   timer,
    scheduler_timer_fn_t cb,
    void*            ud,
    uint64_t         timeout_ms,
    uint64_t         repeat_ms) {
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    _sched_worker_t* ow = _sched_timer_owner(timer);
    mtx_lock(&ow->timer_lock);
    if (timer->active) {
        xylem_loge("<sched> double start on active timer=%p", (void*)timer);
        abort();
    }
    timer->cb      = cb;
    timer->ud      = ud;
    timer->timeout = now + timeout_ms;
    timer->repeat  = repeat_ms;
    timer->active  = true;
    heap_insert(&ow->timers, &timer->heap_node);
    _sched_publish_deadline(ow);
    mtx_unlock(&ow->timer_lock);

    if (atomic_load(&ow->parked)) {
        platform_sem_post(ow->sem);
    }
    if (atomic_load_explicit(
            &timer->sched->poller_waiting, memory_order_seq_cst)) {
        _sched_wake_poller(timer->sched);
    }
}

bool scheduler_timer_stop(scheduler_timer_t* timer) {
    _sched_worker_t* ow = _sched_timer_owner(timer);

    bool cancelled = false;
    mtx_lock(&ow->timer_lock);
    if (timer->active) {
        heap_remove(&ow->timers, &timer->heap_node);
        timer->active = false;
        cancelled = true;
    }
    _sched_publish_deadline(ow);
    mtx_unlock(&ow->timer_lock);
    return cancelled;
}

bool scheduler_timer_reset(scheduler_timer_t* timer, uint64_t timeout_ms) {
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    _sched_worker_t* ow = _sched_timer_owner(timer);
    bool was_active;
    mtx_lock(&ow->timer_lock);
    was_active = timer->active;
    if (was_active) {
        heap_remove(&ow->timers, &timer->heap_node);
    }
    timer->timeout = now + timeout_ms;
    if (timer->repeat != 0) {
        timer->repeat = timeout_ms;
    }
    timer->active = true;
    heap_insert(&ow->timers, &timer->heap_node);
    _sched_publish_deadline(ow);
    mtx_unlock(&ow->timer_lock);

    if (atomic_load(&ow->parked)) {
        platform_sem_post(ow->sem);
    }
    if (atomic_load_explicit(
            &timer->sched->poller_waiting, memory_order_seq_cst)) {
        _sched_wake_poller(timer->sched);
    }
    return was_active;
}

void scheduler_set_idle_cb(
    scheduler_t* sched, scheduler_idle_fn_t cb, void* ud) {
    sched->idle_cb = cb;
    sched->idle_ud = ud;
}
