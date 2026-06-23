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
#include "xylem/xylem-threads.h"

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

#include "minicoro/minicoro.h"

#ifdef MCO_USE_FIBERS
#define SCHED_STACK_EXTERNAL 1
#else
#define SCHED_STACK_EXTERNAL 0
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SCHED_DEQUE_CAP          256
#define SCHED_TIMER_TICK_MS      1
#define SCHED_CORO_POOL_CAP_MUL  64
#define SCHED_CREDIT_DEFAULT     128
#define SCHED_IO_CREDIT_DEFAULT  32
#define SCHED_IO_BYTES_DEFAULT   (512 * 1024)
#define SCHED_FAIR_TICK_INTERVAL 61

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
    uint32_t             io_credit;
    size_t               io_bytes;
    heap_t               timers;
    mtx_t                timer_lock;
    _Atomic uint64_t     next_deadline_ms; /* earliest timer deadline, MAX if none. */
    void**               coro_cache;       /* per-worker free coroutine slots. */
    int32_t              coro_cache_count; /* slots currently held locally.    */
    list_t               registry;         /* coroutines owned for shutdown.   */
    spin_t               registry_lock;    /* protects registry.               */
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
    _Atomic int           state;
    _Atomic bool          poller_waiting;
    _Atomic bool          poller_running;
    _Atomic int32_t       searching;
    bool                  poller_ready;
    bool                  joined;
    _Atomic int64_t       alive;
    _Atomic uint64_t      last_maintenance_ms;
    _Atomic uint32_t      timer_rr;
    _Atomic uint32_t      wake_rr;       /* round-robin start for wake_worker scan. */
    _Atomic uint32_t      spawn_rr;      /* round-robin for non-worker spawn owner. */
    _sched_coro_pool_t    coro_pool;
};


/**
 * Park handshake state, per coroutine. Closes the window where a waker
 * could resume a coroutine while its park callback is still running on
 * the parking worker (and thus still dereferencing the object it parked
 * on -- the classic park_cb-touches-freed-channel race).
 *
 *   IDLE    - running, or sitting in a normal run queue.
 *   PARKING - between mco_yield and the end of the park callback.
 *   PARKED  - park callback returned true; suspended awaiting a wake.
 *   WOKEN   - a waker has marked the coroutine; it is (or will be)
 *             requeued exactly once.
 *
 * At most one waker reaches the wake path per park, because the sync
 * primitive (or iowait) hands off a single one-shot waiter. A waker that
 * observes PARKING only marks WOKEN and does NOT enqueue: the park
 * callback, on return, sees WOKEN and requeues the coroutine itself,
 * so resume can never overlap the tail of the callback.
 */
typedef enum _park_state_e {
    PARK_IDLE    = 0,
    PARK_PARKING = 1,
    PARK_PARKED  = 2,
    PARK_WOKEN   = 3,
} _park_state_t;

typedef enum _sched_state_e {
    SCHED_STOPPED  = 0,
    SCHED_RUNNING  = 1,
    SCHED_STOPPING = 2,
} _sched_state_t;

typedef struct _sched_coro_ctx_s {
    void (*fn)(void*);
    void*                  arg;
    queue_node_t           runq_node;
    list_node_t            registry_node;
    mco_coro*              co;
    _Atomic _park_state_t  park_state;
    uint32_t               registry_owner; /* worker that owns the registry entry. */
} _sched_coro_ctx_t;

typedef struct _sched_post_s {
    mpsc_node_t          node;
    scheduler_post_fn_t  cb;
    void*                ud;
} _sched_post_t;

typedef struct _sched_timer_fire_s {
    scheduler_timer_t*       timer;
    scheduler_timer_fn_t     cb;
    void*                    ud;
    scheduler_timer_ud_fn_t  ud_unref;
    bool                     spawn;
} _sched_timer_fire_t;

static thread_local _sched_worker_t* _tls_worker;

static inline _sched_worker_t* _sched_timer_worker(
    scheduler_timer_t* timer) {
    return &timer->sched->workers[timer->owner];
}

static inline bool _sched_is_running(scheduler_t* sched) {
    return sched
        && atomic_load(&sched->state) == SCHED_RUNNING;
}

static inline bool _sched_is_stopping(scheduler_t* sched) {
    return !sched || atomic_load(&sched->state) != SCHED_RUNNING;
}

static void _sched_coro_entry_cb(mco_coro* co) {
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
    if (SCHED_STACK_EXTERNAL) {
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
    if (SCHED_STACK_EXTERNAL) {
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
    if (SCHED_STACK_EXTERNAL) {
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

static void* _sched_coro_pool_alloc_cb(size_t size, void* allocator_data) {
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

static void _sched_coro_pool_dealloc_cb(
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
     * Local cache full: move a batch down to the shared pool under one
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
    atomic_fetch_add(&timer->refcnt, 1);
}

static void _sched_timer_unref(scheduler_timer_t* timer) {
    if (atomic_fetch_sub(&timer->refcnt, 1) == 1) {
        free(timer);
    }
}

static int _sched_timer_compare_cb(
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

static bool _sched_wakeup_is_valid(scheduler_t* sched) {
    return sched->wakeup_rd != PLATFORM_SO_ERROR_INVALID_SOCKET
           && sched->wakeup_wr != PLATFORM_SO_ERROR_INVALID_SOCKET;
}

static void _sched_wakeup_drain(scheduler_t* sched) {
    if (!_sched_wakeup_is_valid(sched)) {
        return;
    }

    char buf[64];
    while (platform_socket_recv(sched->wakeup_rd, buf, sizeof(buf)) > 0) {
    }

    if (PLATFORM_POLLER_TRIGGER_MODE != PLATFORM_POLLER_TRIGGER_ET) {
        sched->wakeup_sqe.op = PLATFORM_POLLER_RD_OP;
        platform_poller_mod(&sched->poller, &sched->wakeup_sqe);
    }
}

static void _sched_poller_wake(scheduler_t* sched) {
    if (_sched_wakeup_is_valid(sched)) {
        char c = 1;
        platform_socket_send(sched->wakeup_wr, &c, 1);
    }
}

static void _sched_timer_wake_owner(_sched_worker_t* owner) {
    if (atomic_load(&owner->parked)) {
        platform_sem_post(owner->sem);
    }
    if (atomic_load(&owner->sched->poller_waiting)) {
        _sched_poller_wake(owner->sched);
    }
}

static void _sched_wake_worker(scheduler_t* sched);

static bool _sched_worker_mark_parked(_sched_worker_t* w) {
    bool expected = false;
    if (atomic_compare_exchange_strong(&w->parked, &expected, true)) {
        return true;
    }
    return false;
}

static bool _sched_worker_clear_parked(_sched_worker_t* w) {
    bool expected = true;
    if (atomic_compare_exchange_strong(&w->parked, &expected, false)) {
        return true;
    }
    return false;
}

static bool _sched_search_try_start(scheduler_t* sched, _sched_worker_t* w) {
    if (atomic_load(&w->searching)) {
        return true;
    }

    int32_t n = atomic_load(&sched->searching);
    for (;;) {
        if (2 * n >= sched->worker_count) {
            return false;
        }
        if (atomic_compare_exchange_weak(&sched->searching, &n, n + 1)) {
            atomic_store(&w->searching, true);
            return true;
        }
    }
}

static void _sched_search_stop(
    scheduler_t* sched, _sched_worker_t* w, bool found_work) {
    if (!atomic_exchange(&w->searching, false)) {
        return;
    }

    int32_t prev = atomic_fetch_sub(&sched->searching, 1);
    if (found_work && prev == 1) {
        _sched_wake_worker(sched);
    }
}

static void _sched_wake_worker(scheduler_t* sched) {
    int32_t expected_searching = 0;
    if (!atomic_compare_exchange_strong(&sched->searching, &expected_searching, 1)) {
        if (atomic_load(&sched->poller_waiting)) {
            _sched_poller_wake(sched);
        }
        return;
    }

    /**
     * Round-robin scan so repeated wakeups don't all land on worker[0].
     * The CAS on ->searching serialises concurrent callers, so the hint
     * is touched by at most one thread at a time; relaxed is sufficient.
     */
    uint32_t start = atomic_load(&sched->wake_rr);
    for (int32_t j = 0; j < sched->worker_count; j++) {
        int32_t i = (int32_t)(((uint32_t)start + (uint32_t)j)
                              % (uint32_t)sched->worker_count);
        if (_sched_worker_clear_parked(&sched->workers[i])) {
            atomic_store(&sched->workers[i].searching, true);
            platform_sem_post(sched->workers[i].sem);
            atomic_store(&sched->wake_rr,
                         (uint32_t)((i + 1) % sched->worker_count));
            return;
        }
    }
    atomic_fetch_sub(&sched->searching, 1);
    if (atomic_load(&sched->poller_waiting)) {
        _sched_poller_wake(sched);
    }
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

static inline int32_t _sched_deque_get_batch_cap(wsq_t* deque) {
    int32_t remaining = wsq_remaining(deque);
    int32_t half = SCHED_DEQUE_CAP / 2;
    return remaining < half ? remaining : half;
}

static inline _sched_coro_ctx_t* _sched_coro_get_ctx(mco_coro* co) {
    return (_sched_coro_ctx_t*)mco_get_user_data(co);
}

static inline mco_coro* _sched_runq_node_to_coro(queue_node_t* node) {
    return queue_entry(node, _sched_coro_ctx_t, runq_node)->co;
}

static mco_coro* _sched_runq_pop_one(scheduler_t* sched) {
    queue_node_t* node = runq_pop(sched->runq);
    return node ? _sched_runq_node_to_coro(node) : NULL;
}

static mco_coro* _sched_worker_pop_runq(
    scheduler_t* sched, _sched_worker_t* w) {
    int32_t deque_cap = _sched_deque_get_batch_cap(w->deque);
    queue_node_t* runq_nodes[(SCHED_DEQUE_CAP / 2)];
    int32_t runq_n = runq_pop_fair(
        sched->runq, runq_nodes, deque_cap, sched->worker_count);
    if (runq_n <= 0) {
        return NULL;
    }

    for (int32_t i = 1; i < runq_n; i++) {
        wsq_push(w->deque, _sched_runq_node_to_coro(runq_nodes[i]));
    }
    return _sched_runq_node_to_coro(runq_nodes[0]);
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

    return _sched_worker_pop_runq(sched, w);
}

static mco_coro* _sched_worker_steal_coro(scheduler_t* sched, _sched_worker_t* w) {
    if (sched->worker_count <= 1) {
        return NULL;
    }
    if (!_sched_search_try_start(sched, w)) {
        return NULL;
    }
    int32_t deque_cap = _sched_deque_get_batch_cap(w->deque);

    uint32_t worker_count = (uint32_t)sched->worker_count;
    uint32_t start = _sched_rng_next(w) % worker_count;

    for (uint32_t i = 0; i < worker_count; i++) {
        uint32_t victim = (start + i) % worker_count;
        if (victim != w->index) {
            void* stolen[(SCHED_DEQUE_CAP / 2)];
            int32_t stolen_n = wsq_steal_half(
                sched->workers[victim].deque, stolen, deque_cap);
            if (stolen_n > 0) {
                for (int32_t j = 1; j < stolen_n; j++) {
                    wsq_push(w->deque, stolen[j]);
                }
                return (mco_coro*)stolen[0];
            }
        }
    }

    for (uint32_t i = 0; i < worker_count; i++) {
        uint32_t victim = (start + i) % worker_count;
        if (victim != w->index) {
            mco_coro* co = atomic_exchange(&sched->workers[victim].runnext, NULL);
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

static int _sched_timer_timeout_locked(_sched_worker_t* w, uint64_t now) {
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
 * _sched_timer_process / _sched_timer_next_timeout can skip the lock when
 * nothing is due. Caller MUST hold w->timer_lock; readers use the same
 * default atomic ordering on the worker hot path.
 */
static inline void _sched_timer_publish_deadline(_sched_worker_t* w) {
    heap_node_t* root = heap_peek(&w->timers);
    uint64_t nd = root
        ? heap_entry(root, scheduler_timer_t, heap_node)->timeout
        : UINT64_MAX;
    atomic_store(&w->next_deadline_ms, nd);
}

static int _sched_timer_next_timeout(_sched_worker_t* w) {
    uint64_t nd = atomic_load(&w->next_deadline_ms);
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

static void _sched_timer_fire_done(_sched_timer_fire_t* fire) {
    scheduler_timer_t* timer = fire->timer;
    _sched_worker_t*   owner = _sched_timer_worker(timer);
    bool               armed = false;
    uint64_t           now   = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    mtx_lock(&owner->timer_lock);
    if (timer->state == SCHED_TIMER_FIRING) {
        if (timer->stop_pending) {
            timer->stop_pending  = false;
            timer->reset_pending = false;
            timer->state         = SCHED_TIMER_IDLE;
        } else if (timer->reset_pending) {
            timer->timeout       = now + timer->reset_timeout;
            timer->repeat        = timer->reset_repeat;
            timer->reset_pending = false;
            timer->state         = SCHED_TIMER_QUEUED;
            heap_insert(&owner->timers, &timer->heap_node);
            armed = true;
        } else if (timer->repeat > 0 && _sched_is_running(timer->sched)) {
            timer->timeout = now + timer->repeat;
            timer->state   = SCHED_TIMER_QUEUED;
            heap_insert(&owner->timers, &timer->heap_node);
            armed = true;
        } else {
            timer->state = SCHED_TIMER_IDLE;
        }
        _sched_timer_publish_deadline(owner);
    }
    mtx_unlock(&owner->timer_lock);

    if (armed) {
        _sched_timer_wake_owner(owner);
    }
    if (fire->ud_unref) {
        fire->ud_unref(fire->ud);
    }
    _sched_timer_unref(timer);
}

static void _sched_timer_spawn_entry_cb(void* arg) {
    _sched_timer_fire_t* fire = (_sched_timer_fire_t*)arg;
    fire->cb(fire->timer, fire->ud);
    _sched_timer_fire_done(fire);
    free(fire);
}

static int _sched_timer_fire_spawn(
    scheduler_t* sched, _sched_timer_fire_t* fire) {
    _sched_timer_fire_t* heap_fire =
        (_sched_timer_fire_t*)calloc(1, sizeof(*heap_fire));
    if (!heap_fire) {
        xylem_loge("<sched> timer spawn alloc failed");
        return -1;
    }
    *heap_fire = *fire;
    if (scheduler_spawn(sched, _sched_timer_spawn_entry_cb, heap_fire) != 0) {
        xylem_loge("<sched> timer spawn failed");
        free(heap_fire);
        return -1;
    }
    return 0;
}

static int _sched_timer_process(_sched_worker_t* w, uint64_t now_ms) {
    /**
     * Lock-free fast path: skip the lock entirely when no timer is due.
     * next_deadline_ms is republished under the lock on every heap mutation;
     * a concurrent arm that lands just after this check is caught next
     * iteration (within one tick) or by the wake the arm itself issues.
     */
    if (now_ms < atomic_load(&w->next_deadline_ms)) {
        return -1;
    }

    for (;;) {
        _sched_timer_fire_t fire = {0};

        mtx_lock(&w->timer_lock);
        heap_node_t* root = heap_peek(&w->timers);
        if (root) {
            scheduler_timer_t* t = heap_entry(root, scheduler_timer_t, heap_node);
            if (t->timeout <= now_ms) {
                heap_dequeue(&w->timers);
                t->state = SCHED_TIMER_FIRING;
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
                fire.timer    = t;
                fire.cb       = t->cb;
                fire.ud       = t->ud;
                fire.ud_unref = t->ud_unref;
                fire.spawn    = t->spawn;
            }
        }
        mtx_unlock(&w->timer_lock);

        if (!fire.timer) {
            break;
        }

        if (fire.spawn) {
            if (_sched_timer_fire_spawn(w->sched, &fire) != 0) {
                _sched_timer_fire_done(&fire);
            }
        } else {
            fire.cb(fire.timer, fire.ud);
            _sched_timer_fire_done(&fire);
        }
    }

    mtx_lock(&w->timer_lock);
    int timeout = _sched_timer_timeout_locked(w, now_ms);
    _sched_timer_publish_deadline(w);
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
static int _sched_timer_timeout_all(scheduler_t* sched) {
    uint64_t best = UINT64_MAX;
    for (int32_t i = 0; i < sched->worker_count; i++) {
        uint64_t nd = atomic_load(&sched->workers[i].next_deadline_ms);
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
 * _sched_timer_process; the fast-path deadline guard there makes a
 * not-yet-due worker a single atomic load with no lock. Concurrent firing by
 * the owner itself is safe: both contend the same timer_lock and heap_dequeue
 * hands each due timer to exactly one of them.
 */
static void _sched_timer_process_all(scheduler_t* sched, uint64_t now_ms) {
    for (int32_t i = 0; i < sched->worker_count; i++) {
        _sched_timer_process(&sched->workers[i], now_ms);
    }
}

static void _sched_posts_drain(scheduler_t* sched) {
    mpsc_node_t* node;
    while ((node = mpsc_pop(&sched->posts)) != NULL) {
        _sched_post_t* req = mpsc_entry(node, _sched_post_t, node);
        req->cb(req->ud);
        free(req);
    }
}

static void _sched_posts_try_drain(scheduler_t* sched) {
    bool expected = false;
    if (atomic_compare_exchange_strong(&sched->post_draining, &expected, true)) {
        _sched_posts_drain(sched);
        atomic_store(&sched->post_draining, false);
    }
}

static bool _sched_poller_try_acquire(scheduler_t* sched) {
    bool expected = false;
    return atomic_compare_exchange_strong(&sched->poller_running, &expected, true);
}

static void _sched_poller_release(scheduler_t* sched) {
    atomic_store(&sched->poller_running, false);
}

static void _sched_poller_set_waiting(scheduler_t* sched, bool waiting) {
    atomic_store(&sched->poller_waiting, waiting);
}

/**
 * Transition a coroutine that a waker wants to run. Returns true if the
 * caller should enqueue it now, false if it must not (either the park
 * callback is still parking and will requeue on return, or another wake
 * already won). A coroutine not in a park handshake (PARK_IDLE) is a
 * normal schedule and is always enqueued.
 */
static bool _sched_try_wake(mco_coro* co) {
    _sched_coro_ctx_t* ctx = _sched_coro_get_ctx(co);
    _park_state_t state =
        atomic_load(&ctx->park_state);
    for (;;) {
        switch (state) {
        case PARK_IDLE:
            return true;
        case PARK_PARKED:
            if (atomic_compare_exchange_weak(&ctx->park_state, &state, PARK_WOKEN)) {
                return true;
            }
            break;
        case PARK_PARKING:
            if (atomic_compare_exchange_weak(&ctx->park_state, &state, PARK_WOKEN)) {
                return false;
            }
            break;
        case PARK_WOKEN:
            return false;
        }
    }
}

static mco_coro* _sched_io_process_events(
    scheduler_t* sched,
    _sched_worker_t* w,
    platform_poller_cqe_t* cqes,
    int n) {
    mco_coro* ready_coros[PLATFORM_POLLER_CQE_NUM * 2];
    runnable_batch_t ready = {
        .coros = ready_coros,
        .cap = (int32_t)(sizeof(ready_coros) / sizeof(ready_coros[0])),
        .n   = 0,
    };

    bool has_wakeup_event = false;

    for (int i = 0; i < n; i++) {
        if (cqes[i].ud == NULL) {
            has_wakeup_event = true;
            continue;
        }
        iowait_on_event(sched, (int)cqes[i].op, cqes[i].ud, &ready);
    }

    if (has_wakeup_event) {
        _sched_wakeup_drain(sched);
    }

    int32_t ready_count = ready.n;
    if (ready_count <= 0) {
        return NULL;
    }

    mco_coro* run_now = NULL;
    queue_node_t* runq_nodes[PLATFORM_POLLER_CQE_NUM * 2];
    int32_t runq_count = 0;
    int32_t local_count = 0;

    for (int32_t i = 0; i < ready_count; i++) {
        mco_coro* co = ready.coros[i];
        if (!_sched_try_wake(co)) {
            continue;
        }
        if (!run_now) {
            run_now = co;
            continue;
        }
        if (wsq_push(w->deque, co) == 0) {
            local_count++;
            continue;
        }
        _sched_coro_ctx_t* ctx = _sched_coro_get_ctx(co);
        runq_nodes[runq_count++] = &ctx->runq_node;
    }

    if (runq_count > 0) {
        runq_push_batch(sched->runq, runq_nodes, runq_count);
        if (sched->worker_count > 1) {
            _sched_wake_worker(sched);
        }
        return run_now;
    }

    if (local_count > 0 && sched->worker_count > 1) {
        _sched_wake_worker(sched);
    }
    return run_now;
}

static mco_coro* _sched_worker_find_fair_coro(
    scheduler_t*           sched,
    _sched_worker_t*       w,
    platform_poller_cqe_t* cqes) {
    /* Prime: avoids sync with power-of-two deque sizes. */
    if (++w->sched_tick % SCHED_FAIR_TICK_INTERVAL != 0) {
        return NULL;
    }

    mco_coro* co = _sched_runq_pop_one(sched);
    if (co) {
        return co;
    }

    if (_sched_poller_try_acquire(sched)) {
        int n = platform_poller_wait(&sched->poller, cqes, 0);
        co = n > 0 ? _sched_io_process_events(sched, w, cqes, n) : NULL;
        _sched_poller_release(sched);
        if (co) {
            return co;
        }
    }

    return _sched_worker_steal_coro(sched, w);
}

/* Requeue on the parking worker after a declined or woken park. */
static void _sched_enqueue_local(_sched_worker_t* w, mco_coro* co) {
    if (wsq_push(w->deque, co) != 0) {
        _sched_coro_ctx_t* ctx = _sched_coro_get_ctx(co);
        runq_push(w->sched->runq, &ctx->runq_node);
        _sched_wake_worker(w->sched);
    }
}

static bool _sched_credit_park_cb(mco_coro* co, void* arg) {
    (void)co;
    (void)arg;
    return false;
}

static void _sched_coro_handle_yield(_sched_worker_t* w, mco_coro* co) {
    if (mco_status(co) == MCO_DEAD) {
        _sched_coro_ctx_t* ctx = _sched_coro_get_ctx(co);
        _sched_worker_t*   owner = &w->sched->workers[ctx->registry_owner];

        spin_lock(&owner->registry_lock);
        list_remove(&owner->registry, &ctx->registry_node);
        spin_unlock(&owner->registry_lock);

        free(ctx);
        mco_destroy(co);

        scheduler_t* sched = w->sched;
        int64_t prev = atomic_fetch_sub(&sched->alive, 1);
        if (prev == 1 && sched->idle_cb) {
            sched->idle_cb(sched->idle_ud);
        }
        return;
    }
    if (_sched_is_stopping(w->sched)) {
        w->park_fn  = NULL;
        w->park_arg = NULL;
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

    _sched_coro_ctx_t* ctx = _sched_coro_get_ctx(co);
    atomic_store(&ctx->park_state, PARK_PARKING);

    if (fn(co, arg)) {
        /**
         * Commit the park with a CAS, not a plain store: a waker may have
         * raced in during the callback and set WOKEN. A blind store
         * would clobber that and strand the coroutine forever -- the waker
         * deliberately did not enqueue, expecting us to (lost wakeup). The
         * CAS parks only if we are still PARKING, i.e. untouched by a waker.
         */
        _park_state_t expected = PARK_PARKING;
        if (atomic_compare_exchange_strong(&ctx->park_state, &expected, PARK_PARKED)) {
            return; /* cleanly parked; a waker will requeue us */
        }
        /**
         * CAS failed, so expected == PARK_WOKEN: a waker woke us
         * mid-callback and deliberately did not enqueue, so the requeue
         * is ours.
         */
    }

    /* Declined, or woken during parking: requeue the coroutine ourselves. */
    atomic_store(&ctx->park_state, PARK_IDLE);
    if (_sched_is_stopping(w->sched)) {
        return;
    }
    /**
     * Enqueue raw, without _sched_try_wake: we are not a waker. The
     * coro is not on any wait structure (either the park callback declined
     * to park, or the lone waker that set WOKEN already removed it and
     * left the requeue to us), so no other waker can reference it -- there
     * is nothing to dedup against. Routing this through _sched_try_wake
     * would be wrong: park_state is now IDLE, which treated as a
     * normal always-enqueue, so it would add no protection.
     */
    _sched_enqueue_local(w, co);
}

static inline void _sched_run_coro(_sched_worker_t* w, mco_coro* co) {
    w->credit = SCHED_CREDIT_DEFAULT;
    w->io_credit = SCHED_IO_CREDIT_DEFAULT;
    w->io_bytes = SCHED_IO_BYTES_DEFAULT;
    mco_resume(co);
    _sched_coro_handle_yield(w, co);
}

static bool _sched_try_run_coro(_sched_worker_t* w, mco_coro* co) {
    if (_sched_is_stopping(w->sched)) {
        return false;
    }
    _sched_run_coro(w, co);
    return true;
}

static void _sched_maintenance(scheduler_t* sched, _sched_worker_t* w) {
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    _sched_timer_process(w, now);

    uint64_t last = atomic_load(&sched->last_maintenance_ms);
    if (now - last < SCHED_TIMER_TICK_MS) {
        return;
    }
    if (!atomic_compare_exchange_strong(&sched->last_maintenance_ms, &last, now)) {
        return;
    }

    _sched_posts_try_drain(sched);
}

static mco_coro* _sched_worker_drive_poller(
    scheduler_t*           sched,
    _sched_worker_t*       w,
    platform_poller_cqe_t* cqes) {
    while (_sched_is_running(sched)) {
        /* Must set before re-check so producers see it and pipe-wake. */
        _sched_poller_set_waiting(sched, true);

        mco_coro* co = _sched_worker_pop_or_steal(sched, w);
        if (co) {
            _sched_poller_set_waiting(sched, false);
            _sched_poller_release(sched);
            return co;
        }

        int poll_ms = _sched_timer_timeout_all(sched);
        _sched_search_stop(sched, w, false);
        int n = platform_poller_wait(&sched->poller, cqes, poll_ms);
        _sched_poller_set_waiting(sched, false);
        if (!_sched_is_running(sched)) {
            break;
        }
        co = n > 0 ? _sched_io_process_events(sched, w, cqes, n) : NULL;

        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        _sched_timer_process_all(sched, now);
        _sched_posts_try_drain(sched);

        if (!co) {
            co = _sched_worker_pop_or_steal(sched, w);
        }
        if (co) {
            _sched_search_stop(sched, w, true);
            _sched_poller_release(sched);
            return co;
        }
    }

    _sched_poller_release(sched);
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

    if (_sched_poller_try_acquire(sched)) {
        int n = platform_poller_wait(&sched->poller, cqes, 0);
        co = n > 0 ? _sched_io_process_events(sched, w, cqes, n) : NULL;
        _sched_poller_release(sched);
        if (co) {
            return co;
        }
    }

    co = _sched_worker_steal_coro(sched, w);
    if (co) {
        return co;
    }

    if (!_sched_poller_try_acquire(sched)) {
        return NULL;
    }
    return _sched_worker_drive_poller(sched, w, cqes);
}

static mco_coro* _sched_worker_park(
    scheduler_t* sched, _sched_worker_t* w) {
    _sched_search_stop(sched, w, false);
    _sched_worker_mark_parked(w);

    mco_coro* co = _sched_runq_pop_one(sched);
    if (co) {
        _sched_worker_clear_parked(w);
        return co;
    }

    int timer_ms = _sched_timer_next_timeout(w);
    if (timer_ms >= 0) {
        platform_sem_timedwait(w->sem, (uint64_t)timer_ms);
    } else {
        platform_sem_wait(w->sem);
    }
    _sched_worker_clear_parked(w);
    return NULL;
}

static int _sched_worker_entry_cb(void* arg) {
    _sched_worker_t* w = (_sched_worker_t*)arg;
    scheduler_t* sched = w->sched;
    _tls_worker = w;

    platform_poller_cqe_t cqes[PLATFORM_POLLER_CQE_NUM];

    while (_sched_is_running(sched)) {
        _sched_maintenance(sched, w);

        mco_coro* co = _sched_worker_find_fair_coro(sched, w, cqes);
        if (!co) {
            co = _sched_worker_find_coro(sched, w, cqes);
        }
        if (co) {
            _sched_search_stop(sched, w, true);
            if (!_sched_try_run_coro(w, co)) {
                break;
            }
            continue;
        }

        co = _sched_worker_park(sched, w);
        if (co) {
            _sched_search_stop(sched, w, true);
            if (!_sched_try_run_coro(w, co)) {
                break;
            }
        }
    }

    return 0;
}

static void _sched_cleanup(scheduler_t* sched, int32_t started_count) {
    atomic_store(&sched->state, SCHED_STOPPING);

    if (sched->workers) {
        if (!sched->joined) {
            for (int32_t i = 0; i < started_count; i++) {
                platform_sem_post(sched->workers[i].sem);
                _sched_poller_wake(sched);
            }
            for (int32_t i = 0; i < started_count; i++) {
                thrd_join(sched->workers[i].thread, NULL);
            }
            sched->joined = true;
        }

        _sched_posts_drain(sched);

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
                heap_node_t* node;
                while ((node = heap_peek(&w->timers)) != NULL) {
                    scheduler_timer_t* t =
                        heap_entry(node, scheduler_timer_t, heap_node);
                    heap_dequeue(&w->timers);
                    t->state = SCHED_TIMER_IDLE;
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

    if (_sched_wakeup_is_valid(sched)) {
        platform_poller_del(&sched->poller, &sched->wakeup_sqe);
        platform_socket_close(sched->wakeup_rd);
        platform_socket_close(sched->wakeup_wr);
    }

    if (sched->poller_ready) {
        platform_poller_deinit(&sched->poller);
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

    sched->wakeup_rd = PLATFORM_SO_ERROR_INVALID_SOCKET;
    sched->wakeup_wr = PLATFORM_SO_ERROR_INVALID_SOCKET;

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
    atomic_store(&sched->wake_rr, 0);
    atomic_store(&sched->spawn_rr, 0);

    if (platform_poller_init(&sched->poller) != 0) {
        _sched_cleanup(sched, 0);
        return NULL;
    }
    sched->poller_ready = true;
    {
        platform_sock_t pair[2];
        if (platform_socket_socketpair(0, SOCK_STREAM, 0, pair) != 0) {
            _sched_cleanup(sched, 0);
            return NULL;
        }

        platform_socket_enable_nonblocking(pair[0], true);
        platform_socket_enable_nonblocking(pair[1], true);

        memset(&sched->wakeup_sqe, 0, sizeof(sched->wakeup_sqe));
        sched->wakeup_sqe.fd = (platform_poller_fd_t)pair[0];
        sched->wakeup_sqe.op = PLATFORM_POLLER_RD_OP;
        sched->wakeup_sqe.ud = NULL;
        if (platform_poller_add(&sched->poller, &sched->wakeup_sqe) != 0) {
            platform_socket_close(pair[0]);
            platform_socket_close(pair[1]);
            _sched_cleanup(sched, 0);
            return NULL;
        }
        sched->wakeup_rd = pair[0];
        sched->wakeup_wr = pair[1];
    }

    atomic_store(&sched->state, SCHED_RUNNING);
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
        mco_desc desc_probe = mco_desc_init(_sched_coro_entry_cb, stack_size);
        sched->coro_pool.slot_size  = desc_probe.coro_size;
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

    sched->workers = (_sched_worker_t*)calloc(
        (size_t)worker_count, sizeof(_sched_worker_t));
    if (!sched->workers) {
        _sched_cleanup(sched, 0);
        return NULL;
    }
    sched->worker_count = 0;

    for (int32_t i = 0; i < worker_count; i++) {
        _sched_worker_t* w = &sched->workers[i];
        w->sched = sched;
        w->index = (uint32_t)i;
        w->rng = 2654435761u * (uint32_t)(i + 1); /* non-zero xorshift seed */
        atomic_store(&w->parked, false);
        atomic_store(&w->searching, false);
        heap_init(&w->timers, _sched_timer_compare_cb);
        list_init(&w->registry);
        spin_init(&w->registry_lock);
        if (mtx_init(&w->timer_lock, mtx_plain) != thrd_success) {
            _sched_cleanup(sched, 0);
            return NULL;
        }
        sched->worker_count = i + 1;

        w->deque = wsq_create(deque_capacity);
        w->sem = platform_sem_create(0);
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
                        _sched_worker_entry_cb,
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
    if (_tls_worker && _tls_worker->sched == sched) {
        xylem_loge("<sched> destroy from worker sched=%p", (void*)sched);
        abort();
    }

    scheduler_stop(sched);

    /* Drain per-worker registries of any coroutines stranded in queues. */
    for (int32_t i = 0; i < sched->worker_count; i++) {
        _sched_worker_t* w = &sched->workers[i];
        spin_lock(&w->registry_lock);
        while (!list_empty(&w->registry)) {
            list_node_t* node = list_head(&w->registry);
            list_remove(&w->registry, node);
            spin_unlock(&w->registry_lock);

            _sched_coro_ctx_t* ctx =
                list_entry(node, _sched_coro_ctx_t, registry_node);
            mco_destroy(ctx->co);
            free(ctx);

            spin_lock(&w->registry_lock);
        }
        spin_unlock(&w->registry_lock);
    }

    _sched_cleanup(sched, sched->worker_count);
}

void scheduler_stop(scheduler_t* sched) {
    if (!sched || sched->joined) {
        return;
    }

    int expected = SCHED_RUNNING;
    if (atomic_compare_exchange_strong(
            &sched->state, &expected, SCHED_STOPPING)) {
        for (int32_t i = 0; i < sched->worker_count; i++) {
            platform_sem_post(sched->workers[i].sem);
            _sched_poller_wake(sched);
        }
    } else if (expected == SCHED_STOPPED) {
        return;
    }

    if (_tls_worker && _tls_worker->sched == sched) {
        return;
    }

    for (int32_t i = 0; i < sched->worker_count; i++) {
        thrd_join(sched->workers[i].thread, NULL);
    }
    sched->joined = true;
    atomic_store(&sched->state, SCHED_STOPPED);
}

static void _sched_enqueue(scheduler_t* sched, mco_coro* co) {
    _sched_coro_ctx_t* ctx = _sched_coro_get_ctx(co);

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

            queue_node_t* runq_nodes[(SCHED_DEQUE_CAP / 2) + 1];
            for (int32_t i = 0; i < n; i++) {
                _sched_coro_ctx_t* c =
                    _sched_coro_get_ctx(batch[i]);
                runq_nodes[i] = &c->runq_node;
            }
            runq_push_batch(sched->runq, runq_nodes, n);
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

void scheduler_schedule(scheduler_t* sched, mco_coro* co) {
    if (!_sched_is_running(sched)) {
        return;
    }
    if (_sched_try_wake(co)) {
        _sched_enqueue(sched, co);
    }
}

void scheduler_schedule_batch(
    scheduler_t* sched, mco_coro** cos, int32_t n) {
    if (n <= 0 || !_sched_is_running(sched)) {
        return;
    }

    if (_tls_worker && _tls_worker->sched == sched && sched->worker_count == 1) {
        for (int32_t i = 0; i < n; i++) {
            if (!_sched_try_wake(cos[i])) {
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
    int32_t claimed_n = 0;
    for (int32_t i = 0; i < n; i++) {
        /**
         * A coro still parking must not be enqueued here; its park
         * callback owns the requeue. Drop it from the batch.
         */
        if (!_sched_try_wake(cos[i])) {
            continue;
        }
        _sched_coro_ctx_t* ctx = _sched_coro_get_ctx(cos[i]);
        nodes[claimed_n++] = &ctx->runq_node;
    }
    if (claimed_n > 0) {
        runq_push_batch(sched->runq, nodes, claimed_n);
    }
    if (nodes != inline_nodes) {
        free(nodes);
    }

    if (claimed_n > 0) {
        /**
         * Wake one worker for the batch. A searching worker that finds work
         * expands the wakeup chain, avoiding a batch-sized wakeup herd.
         */
        _sched_wake_worker(sched);
    }
}

int scheduler_spawn(scheduler_t* sched, void (*fn)(void*), void* arg) {
    if (!fn || !_sched_is_running(sched)) {
        return -1;
    }

    _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)calloc(1, sizeof(_sched_coro_ctx_t));
    if (!ctx) {
        return -1;
    }

    ctx->fn = fn;
    ctx->arg = arg;

    mco_desc desc = mco_desc_init(
        _sched_coro_entry_cb, sched->coro_pool.stack_size);
    desc.alloc_cb       = _sched_coro_pool_alloc_cb;
    desc.dealloc_cb     = _sched_coro_pool_dealloc_cb;
    desc.allocator_data = &sched->coro_pool;
    desc.user_data      = ctx;

    mco_coro* co = NULL;
    if (mco_create(&co, &desc) != MCO_SUCCESS) {
        free(ctx);
        return -1;
    }

    if (!_sched_is_running(sched)) {
        mco_destroy(co);
        free(ctx);
        return -1;
    }

    ctx->co = co;

    {
        uint32_t owner_idx;
        if (_tls_worker && _tls_worker->sched == sched) {
            owner_idx = _tls_worker->index;
        } else {
            owner_idx = atomic_fetch_add(&sched->spawn_rr, 1)
                        % (uint32_t)sched->worker_count;
        }
        ctx->registry_owner = owner_idx;
        _sched_worker_t* owner = &sched->workers[owner_idx];
        spin_lock(&owner->registry_lock);
        list_insert_tail(&owner->registry, &ctx->registry_node);
        spin_unlock(&owner->registry_lock);
    }

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
    _sched_coro_ctx_t* ctx = _sched_coro_get_ctx(mco_running());
    atomic_store(&ctx->park_state, PARK_IDLE);
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

bool scheduler_consume_io_credit(size_t bytes) {
    if (!_tls_worker || !mco_running()) {
        return false;
    }
    bool exhausted = false;

    if (_tls_worker->io_credit > 1) {
        _tls_worker->io_credit--;
    } else {
        _tls_worker->io_credit = 0;
        exhausted = true;
    }

    if (_tls_worker->io_bytes > bytes) {
        _tls_worker->io_bytes -= bytes;
    } else {
        _tls_worker->io_bytes = 0;
        exhausted = true;
    }

    return exhausted;
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
    if (!_sched_is_running(sched)) {
        return -1;
    }

    _sched_post_t* req = (_sched_post_t*)calloc(1, sizeof(*req));
    if (!req) {
        return -1;
    }
    req->cb = cb;
    req->ud = ud;
    if (!_sched_is_running(sched)) {
        free(req);
        return -1;
    }
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
    if (_tls_worker && _tls_worker->sched == sched) {
        t->owner = _tls_worker->index;
    } else {
        uint32_t rr = atomic_fetch_add(&sched->timer_rr, 1);
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

int scheduler_timer_start(
    scheduler_timer_t*   timer,
    scheduler_timer_fn_t cb,
    void*                ud,
    uint64_t             timeout_ms,
    uint64_t             repeat_ms) {
    if (!timer || !_sched_is_running(timer->sched)) {
        return -1;
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    _sched_worker_t* owner = _sched_timer_worker(timer);
    bool armed = false;
    mtx_lock(&owner->timer_lock);
    if (timer->state == SCHED_TIMER_QUEUED) {
        xylem_loge("<sched> double start on queued timer=%p", (void*)timer);
        abort();
    }
    timer->cb            = cb;
    timer->ud            = ud;
    timer->repeat        = repeat_ms;
    timer->stop_pending  = false;
    if (timer->state == SCHED_TIMER_FIRING) {
        timer->reset_pending = true;
        timer->reset_timeout = timeout_ms;
        timer->reset_repeat  = repeat_ms;
    } else {
        timer->timeout       = now + timeout_ms;
        timer->reset_pending = false;
        timer->state         = SCHED_TIMER_QUEUED;
        heap_insert(&owner->timers, &timer->heap_node);
        armed = true;
    }
    _sched_timer_publish_deadline(owner);
    mtx_unlock(&owner->timer_lock);

    if (armed) {
        _sched_timer_wake_owner(owner);
    }
    return 0;
}

bool scheduler_timer_stop(scheduler_timer_t* timer) {
    _sched_worker_t* owner = _sched_timer_worker(timer);

    bool cancelled = false;
    mtx_lock(&owner->timer_lock);
    if (timer->state == SCHED_TIMER_QUEUED) {
        heap_remove(&owner->timers, &timer->heap_node);
        timer->state         = SCHED_TIMER_IDLE;
        timer->stop_pending  = false;
        timer->reset_pending = false;
        cancelled = true;
    } else if (timer->state == SCHED_TIMER_FIRING) {
        timer->stop_pending  = true;
        timer->reset_pending = false;
    }
    _sched_timer_publish_deadline(owner);
    mtx_unlock(&owner->timer_lock);
    return cancelled;
}

bool scheduler_timer_reset(scheduler_timer_t* timer, uint64_t timeout_ms) {
    if (!timer || !_sched_is_running(timer->sched)) {
        return false;
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    _sched_worker_t* owner = _sched_timer_worker(timer);
    bool was_active;
    bool armed = false;
    mtx_lock(&owner->timer_lock);
    was_active = (timer->state == SCHED_TIMER_QUEUED);
    if (timer->state == SCHED_TIMER_QUEUED) {
        heap_remove(&owner->timers, &timer->heap_node);
        timer->timeout = now + timeout_ms;
        if (timer->repeat != 0) {
            timer->repeat = timeout_ms;
        }
        timer->stop_pending  = false;
        timer->reset_pending = false;
        heap_insert(&owner->timers, &timer->heap_node);
        armed = true;
    } else if (timer->state == SCHED_TIMER_FIRING) {
        timer->stop_pending   = false;
        timer->reset_pending  = true;
        timer->reset_timeout  = timeout_ms;
        timer->reset_repeat   = timer->repeat != 0 ? timeout_ms : 0;
    } else {
        timer->timeout = now + timeout_ms;
        if (timer->repeat != 0) {
            timer->repeat = timeout_ms;
        }
        timer->stop_pending  = false;
        timer->reset_pending = false;
        timer->state         = SCHED_TIMER_QUEUED;
        heap_insert(&owner->timers, &timer->heap_node);
        armed = true;
    }
    _sched_timer_publish_deadline(owner);
    mtx_unlock(&owner->timer_lock);

    if (armed) {
        _sched_timer_wake_owner(owner);
    }
    return was_active;
}

void scheduler_set_idle_cb(
    scheduler_t* sched, scheduler_idle_fn_t cb, void* ud) {
    sched->idle_cb = cb;
    sched->idle_ud = ud;
}
