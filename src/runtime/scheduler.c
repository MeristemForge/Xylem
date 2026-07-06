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
 * IO and timers. All other idle workers park on a per-worker semaphore;
 * scheduler_schedule wakes at most one parked worker per push. A
 * hand-off into an empty local
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

#define SCHED_CORO_POOL_CAP_MUL  64
#define SCHED_CREDIT_DEFAULT     128u
/* Prime: avoids sync with power-of-two deque sizes. */
#define SCHED_FAIR_TICK_INTERVAL 61

#define SCHED_CORO_STACK_SIZE \
    (sizeof(void*) > 4 ? 1024 * 1024 : 128 * 1024)

/**
 * Per-worker coroutine-slot cache. Each worker keeps up to
 * SCHED_CORO_POOL_CAP free slots it can pop/push with no lock; only when
 * the local cache underflows or overflows does it exchange a batch of
 * half of SCHED_CORO_POOL_CAP slots with the shared pool under a single
 * lock. Mirrors the per-P gFree/stackcache design in the Go runtime: the
 * hot spawn/death path stays lock-free and the shared-pool lock is amortized.
 */
#define SCHED_CORO_POOL_CAP 64

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
    _Atomic bool         stealing;
    _Atomic(mco_coro*)   runnext;

    uint32_t             sched_tick;
    uint32_t             credit;
    heap_t               timers;
    mtx_t                timer_lock;
    _Atomic uint64_t     next_deadline_ms; /* earliest timer deadline, MAX if none. */
    void**               coro_pool;       /* per-worker free coroutine slots. */
    int32_t              coro_pool_count; /* slots currently held locally.    */
    list_t               registry;         /* coroutines owned for shutdown.   */
    spin_t               registry_lock;    /* protects registry.               */
} _sched_worker_t;

struct scheduler_s {
    _sched_worker_t*      workers;
    int32_t               worker_count;
    runq_t*               runq;
    platform_poller_sq_t  poller;
    platform_poller_sqe_t wakeup_sqe;
    platform_sock_t       wakeup_rd;
    platform_sock_t       wakeup_wr;
    iowait_slab_t*        iowait_slab;
    scheduler_idle_fn_t   idle_cb;
    void*                 idle_ud;
    _Atomic bool          running;
    _Atomic bool          poller_waiting;
    _Atomic bool          poller_running;
    _Atomic int32_t       num_stealing;
    bool                  poller_ready;
    bool                  joined;
    _Atomic int64_t       alive;

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

typedef enum _timer_state_e {
    TIMER_IDLE = 0,
    TIMER_QUEUED,
    TIMER_FIRING,
} _timer_state_t;

/**
 * Scheduler timer.
 *
 * heap_node must remain embedded by value (the per-worker timer heap
 * recovers the timer via heap_entry). Fields are owned by the timer's
 * worker; see the _sched_timer_* helpers for access/locking rules.
 */
struct scheduler_timer_s {
    heap_node_t              heap_node;
    scheduler_t*             sched;
    scheduler_timer_fn_t     cb;
    void*                    ud;
    scheduler_timer_ud_fn_t  ud_ref;
    scheduler_timer_ud_fn_t  ud_unref;
    uint64_t                 timeout;
    uint64_t                 repeat;
    _timer_state_t           state;
    bool                     stop_pending;
    bool                     reset_pending;
    bool                     spawn;
    _Atomic int32_t          refcnt;
    uint32_t                 owner;
};

typedef struct _sched_coro_ctx_s {
    void (*fn)(void*);
    void*                  arg;
    void (*cleanup)(void*); /* called if coroutine never runs; NULL-safe */
    queue_node_t           runq_node;
    list_node_t            registry_node;
    mco_coro*              co;
    _Atomic _park_state_t  park_state;
    uint32_t               registry_owner; /* worker that owns the registry entry. */
} _sched_coro_ctx_t;

typedef struct _sched_timer_fire_s {
    scheduler_timer_t*       timer;
    scheduler_timer_fn_t     cb;
    void*                    ud;
    scheduler_timer_ud_fn_t  ud_ref;
    scheduler_timer_ud_fn_t  ud_unref;
    bool                     spawn;
} _sched_timer_fire_t;

static thread_local _sched_worker_t* _tls_worker;

static void _sched_coro_entry_cb(mco_coro* co) {
    _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)mco_get_user_data(co);
    void (*fn)(void*)   = ctx->fn;
    void* arg           = ctx->arg;

    ctx->cleanup = NULL; /* fn owns arg lifecycle; drain must not free it. */
    fn(arg);
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

/* Allocate one coroutine slot (mco_coro + stack), with guard page. */
static void* _sched_coro_alloc(_sched_coro_pool_t* pool, size_t size) {
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

static void _sched_coro_shrink(_sched_coro_pool_t* pool, void* ptr, size_t size) {
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
static void _sched_coro_free(_sched_coro_pool_t* pool, void* ptr) {
    if (SCHED_STACK_EXTERNAL) {
        free(ptr);
        return;
    }

    size_t page_size = _sched_vmem_page_size();
    size_t total     = (pool->slot_size + page_size - 1) & ~(page_size - 1);
    platform_vmem_release(ptr, total);
}

/* Pop one slot from the shared pool; NULL when empty. Takes the pool lock. */
static void* _sched_coro_pool_pop(_sched_coro_pool_t* pool) {
    void* ptr = NULL;
    spin_lock(&pool->lock);
    if (pool->count > 0) {
        ptr = pool->slots[--pool->count];
    }
    spin_unlock(&pool->lock);
    return ptr;
}

/* Push one slot into the shared pool; release it when the pool is full. */
static void _sched_coro_pool_push(_sched_coro_pool_t* pool, void* ptr) {
    spin_lock(&pool->lock);
    if (pool->count < pool->cap) {
        pool->slots[pool->count++] = ptr;
        spin_unlock(&pool->lock);
        return;
    }
    spin_unlock(&pool->lock);
    _sched_coro_free(pool, ptr);
}

/**
 * The current worker's local slot cache, or NULL when the caller is not a
 * worker of this scheduler (e.g. the initial spawn on the main thread).
 */
static void* _sched_coro_pool_alloc_cb(size_t size, void* allocator_data) {
    _sched_coro_pool_t* pool = (_sched_coro_pool_t*)allocator_data;
    _sched_worker_t*    w    = _tls_worker;
    if (!w || &w->sched->coro_pool != pool || !w->coro_pool) {
        void* ptr = _sched_coro_pool_pop(pool);
        return ptr ? ptr : _sched_coro_alloc(pool, size);
    }

    /* Fast path: local cache hit, no lock. */
    if (w->coro_pool_count > 0) {
        return w->coro_pool[--w->coro_pool_count];
    }

    /* Local cache empty: refill a batch from the shared pool under one lock. */
    spin_lock(&pool->lock);
    int32_t n = (pool->count < SCHED_CORO_POOL_CAP / 2)
                    ? pool->count
                    : SCHED_CORO_POOL_CAP / 2;
    for (int32_t i = 0; i < n; i++) {
        w->coro_pool[i] = pool->slots[--pool->count];
    }
    spin_unlock(&pool->lock);

    if (n > 0) {
        w->coro_pool_count = n - 1;
        return w->coro_pool[n - 1];
    }

    /* Shared pool empty too: allocate a fresh slot. */
    return _sched_coro_alloc(pool, size);
}

static void _sched_coro_pool_dealloc_cb(
    void* ptr, size_t size, void* allocator_data) {
    _sched_coro_pool_t* pool = (_sched_coro_pool_t*)allocator_data;
    _sched_worker_t*    w    = _tls_worker;

    /* Drop the physical stack pages regardless of where the slot lands. */
    _sched_coro_shrink(pool, ptr, size);

    if (!w || &w->sched->coro_pool != pool || !w->coro_pool) {
        _sched_coro_pool_push(pool, ptr);
        return;
    }

    /* Fast path: room in the local cache, no lock. */
    if (w->coro_pool_count < SCHED_CORO_POOL_CAP) {
        w->coro_pool[w->coro_pool_count++] = ptr;
        return;
    }

    /**
     * Local pool full: drain half to the shared pool under one lock,
     * free whatever the pool refuses, then store the new slot.
     */
    int32_t keep = SCHED_CORO_POOL_CAP / 2;
    spin_lock(&pool->lock);
    while (w->coro_pool_count > keep && pool->count < pool->cap) {
        pool->slots[pool->count++] = w->coro_pool[--w->coro_pool_count];
    }
    spin_unlock(&pool->lock);
    while (w->coro_pool_count > keep) {
        _sched_coro_free(pool, w->coro_pool[--w->coro_pool_count]);
    }
    w->coro_pool[w->coro_pool_count++] = ptr;
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

static void _sched_wakeup_flush(scheduler_t* sched) {
    if (!(sched->wakeup_rd != PLATFORM_SO_ERROR_INVALID_SOCKET
     && sched->wakeup_wr != PLATFORM_SO_ERROR_INVALID_SOCKET)) {
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
    if ((sched->wakeup_rd != PLATFORM_SO_ERROR_INVALID_SOCKET
     && sched->wakeup_wr != PLATFORM_SO_ERROR_INVALID_SOCKET)) {
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

static bool _sched_worker_try_steal(scheduler_t* sched, _sched_worker_t* w) {
    if (atomic_load(&w->stealing)) {
        return true;
    }

    int32_t n = atomic_load(&sched->num_stealing);
    for (;;) {
        if (2 * n >= sched->worker_count) {
            return false;
        }
        if (atomic_compare_exchange_weak(&sched->num_stealing, &n, n + 1)) {
            atomic_store(&w->stealing, true);
            return true;
        }
    }
}

static void _sched_wake_worker(scheduler_t* sched) {
    uint32_t n = (uint32_t)sched->worker_count;
    uint32_t start = atomic_load(&sched->wake_rr);
    for (uint32_t j = 0; j < n; j++) {
        uint32_t i = (start + j) % n;
        bool expected = true;
        if (atomic_compare_exchange_strong(&sched->workers[i].parked, &expected, false)) {
            platform_sem_post(sched->workers[i].sem);
            atomic_store(&sched->wake_rr, (i + 1) % n);
            return;
        }
    }
    /**
     * No parked worker -- poller may be sleeping while work is pending,
     * wake it so it re-checks.
     */
    if (atomic_load(&sched->poller_waiting)) {
        _sched_poller_wake(sched);
    }
}

static void _sched_worker_finish_steal(
    scheduler_t* sched, _sched_worker_t* w, bool found_work) {
    if (!atomic_exchange(&w->stealing, false)) {
        return;
    }

    int32_t prev = atomic_fetch_sub(&sched->num_stealing, 1);
    if (found_work && prev == 1) {
        _sched_wake_worker(sched);
    }
}

static mco_coro* _sched_worker_fetch(scheduler_t* sched, _sched_worker_t* w) {
    mco_coro* co = atomic_exchange(&w->runnext, NULL);
    if (co) {
        return co;
    }

    co = (mco_coro*)wsq_pop(w->deque);
    if (co) {
        return co;
    }

    int32_t deque_cap = wsq_remaining(w->deque);
    if (deque_cap > SCHED_DEQUE_CAP / 2) {
        deque_cap = SCHED_DEQUE_CAP / 2;
    }
    queue_node_t* runq_nodes[(SCHED_DEQUE_CAP / 2)];
    int32_t runq_n = runq_pop_fair(
        sched->runq, runq_nodes, deque_cap, sched->worker_count);
    if (runq_n <= 0) {
        return NULL;
    }

    for (int32_t i = 1; i < runq_n; i++) {
        wsq_push(w->deque, queue_entry(runq_nodes[i], _sched_coro_ctx_t, runq_node)->co);
    }
    return queue_entry(runq_nodes[0], _sched_coro_ctx_t, runq_node)->co;
}

static mco_coro* _sched_worker_steal(scheduler_t* sched, _sched_worker_t* w) {
    mco_coro* co;

    if (sched->worker_count <= 1) {
        return NULL;
    }
    if (!_sched_worker_try_steal(sched, w)) {
        return NULL;
    }
    int32_t deque_cap = wsq_remaining(w->deque);
    if (deque_cap > SCHED_DEQUE_CAP / 2) {
        deque_cap = SCHED_DEQUE_CAP / 2;
    }

    uint32_t worker_count = (uint32_t)sched->worker_count;
    uint32_t first = (uint32_t)(w->index + 1) % worker_count;

    /* first skips self, so worker_count-1 covers all other workers */
    for (uint32_t i = 0; i < worker_count - 1; i++) {
        uint32_t target = (first + i) % worker_count;
        void* stolen[(SCHED_DEQUE_CAP / 2)];
        int32_t stolen_n = wsq_steal_half(
            sched->workers[target].deque, stolen, deque_cap);
        if (stolen_n > 0) {
            for (int32_t j = 1; j < stolen_n; j++) {
                wsq_push(w->deque, stolen[j]);
            }
            co = (mco_coro*)stolen[0];
            _sched_worker_finish_steal(sched, w, true);
            return co;
        }
    }

    for (uint32_t i = 0; i < worker_count - 1; i++) {
        uint32_t target = (first + i) % worker_count;
        co = atomic_exchange(&sched->workers[target].runnext, NULL);
        if (co) {
            _sched_worker_finish_steal(sched, w, true);
            return co;
        }
    }

    _sched_worker_finish_steal(sched, w, false);
    return NULL;
}

/**
 * Republish the earliest timer deadline so the lock-free guards in
 * _sched_timer_process / timeout helpers can skip the lock when
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

static void _sched_timer_complete(_sched_timer_fire_t* fire) {
    scheduler_timer_t* timer = fire->timer;
    _sched_worker_t*   owner = &timer->sched->workers[timer->owner];
    bool               armed = false;
    uint64_t           now   = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    mtx_lock(&owner->timer_lock);
    if (timer->state == TIMER_FIRING) {
        if (timer->stop_pending) {
            timer->stop_pending  = false;
            timer->reset_pending = false;
            timer->state         = TIMER_IDLE;
        } else if (timer->reset_pending) {
            timer->timeout       = now + timer->timeout;
            timer->reset_pending = false;
            timer->state         = TIMER_QUEUED;
            heap_insert(&owner->timers, &timer->heap_node);
            armed = true;
        } else if (timer->repeat > 0) {
            timer->timeout = now + timer->repeat;
            timer->state   = TIMER_QUEUED;
            heap_insert(&owner->timers, &timer->heap_node);
            armed = true;
        } else {
            timer->state = TIMER_IDLE;
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

static int _sched_spawn(
    scheduler_t* sched, void (*fn)(void*), void* arg,
    void (*cleanup)(void*)) {
    if (!fn) {
        return -1;
    }

    _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)calloc(1, sizeof(_sched_coro_ctx_t));
    if (!ctx) {
        return -1;
    }

    ctx->fn      = fn;
    ctx->arg     = arg;
    ctx->cleanup = cleanup;

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

int scheduler_spawn(scheduler_t* sched, void (*fn)(void*), void* arg) {
    return _sched_spawn(sched, fn, arg, NULL);
}

static void _sched_timer_launch_cb(void* arg) {
    _sched_timer_fire_t* fire = (_sched_timer_fire_t*)arg;
    fire->cb(fire->timer, fire->ud);
    _sched_timer_complete(fire);
    free(fire);
}

static void _sched_timer_launch_cleanup(void* arg) {
    _sched_timer_fire_t* fire = (_sched_timer_fire_t*)arg;
    if (!fire) {
        return;
    }
    _sched_timer_complete(fire);
    free(fire);
}

static int _sched_timer_launch(
    scheduler_t* sched, _sched_timer_fire_t* fire) {
    _sched_timer_fire_t* f =
        (_sched_timer_fire_t*)calloc(1, sizeof(*f));
    if (!f) {
        xylem_loge("<sched> timer spawn alloc failed");
        return -1;
    }
    *f = *fire;
    if (_sched_spawn(sched,
                     _sched_timer_launch_cb,
                     f,
                     _sched_timer_launch_cleanup) != 0) {
        xylem_loge("<sched> timer spawn failed");
        free(f);
        return -1;
    }
    return 0;
}

static int _sched_timer_process(_sched_worker_t* w) {
    uint64_t now_ms = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
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
                t->state = TIMER_FIRING;
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
                fire.timer    = t;
                fire.cb       = t->cb;
                fire.ud       = t->ud;
                fire.ud_ref   = t->ud_ref;
                fire.ud_unref = t->ud_unref;
                fire.spawn    = t->spawn;

                if (fire.ud_ref) {
                    fire.ud_ref(fire.ud);
                }
            }
        }
        mtx_unlock(&w->timer_lock);

        if (!fire.timer) {
            break;
        }

        if (fire.spawn) {
            if (_sched_timer_launch(w->sched, &fire) != 0) {
                _sched_timer_complete(&fire);
            }
        } else {
            fire.cb(fire.timer, fire.ud);
            _sched_timer_complete(&fire);
        }
    }

    mtx_lock(&w->timer_lock);

    int timeout;
    heap_node_t* root = heap_peek(&w->timers);
    if (!root) {
        timeout = -1;
    } else {
        scheduler_timer_t* t = heap_entry(root, scheduler_timer_t, heap_node);
        if (t->timeout <= now_ms) {
            timeout = 0;
        } else {
            uint64_t diff = t->timeout - now_ms;
            timeout = (diff > INT32_MAX) ? INT32_MAX : (int)diff;
        }
    }

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
static int _sched_timer_poll_timeout(scheduler_t* sched) {
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

static int _sched_timer_worker_timeout(_sched_worker_t* w) {
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

/**
 * Transition a coroutine that a waker wants to run. Returns true if the
 * caller should enqueue it now, false if it must not (either the park
 * callback is still parking and will requeue on return, or another wake
 * already won). A coroutine not in a park handshake (PARK_IDLE) is a
 * normal schedule and is always enqueued.
 */
static bool _sched_try_wake(mco_coro* co) {
    _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)mco_get_user_data(co);
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

static mco_coro* _sched_io_process(
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
        _sched_wakeup_flush(sched);
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
        _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)mco_get_user_data(co);
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

/* Requeue on the parking worker after a declined or woken park. */
static void _sched_worker_enqueue(_sched_worker_t* w, mco_coro* co) {
    if (wsq_push(w->deque, co) != 0) {
        _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)mco_get_user_data(co);
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
        _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)mco_get_user_data(co);
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
    if (!atomic_load(&w->sched->running)) {
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

    _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)mco_get_user_data(co);
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
    if (!atomic_load(&w->sched->running)) {
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
    _sched_worker_enqueue(w, co);
}

static inline void _sched_worker_run(_sched_worker_t* w, mco_coro* co) {
    w->credit = SCHED_CREDIT_DEFAULT;
    mco_resume(co);
    _sched_coro_handle_yield(w, co);
}

static mco_coro* _sched_worker_poll(
    scheduler_t*           sched,
    _sched_worker_t*       w,
    platform_poller_cqe_t* cqes) {
    while (atomic_load(&sched->running)) {
        /* Must set before re-check so producers see it and pipe-wake. */
        atomic_store(&sched->poller_waiting, true);

        mco_coro* co = _sched_worker_fetch(sched, w);
        if (!co) {
            co = _sched_worker_steal(sched, w);
        }
        if (co) {
            atomic_store(&sched->poller_waiting, false);
            atomic_store(&sched->poller_running, false);
            return co;
        }
        int poll_ms = _sched_timer_poll_timeout(sched);
        int n = platform_poller_wait(&sched->poller, cqes, poll_ms);
        atomic_store(&sched->poller_waiting, false);
        if (!atomic_load(&sched->running)) {
            break;
        }
        co = n > 0 ? _sched_io_process(sched, w, cqes, n) : NULL;

        for (int32_t i = 0; i < sched->worker_count; i++) {
            _sched_timer_process(&sched->workers[i]);
        }

        if (!co) {
            co = _sched_worker_fetch(sched, w);
        }
        if (!co) {
            co = _sched_worker_steal(sched, w);
        }
        if (co) {
            atomic_store(&sched->poller_running, false);
            return co;
        }
    }

    atomic_store(&sched->poller_running, false);
    return NULL;
}

static mco_coro* _sched_worker_find(
    scheduler_t* sched, _sched_worker_t* w) {
    platform_poller_cqe_t cqes[PLATFORM_POLLER_CQE_NUM];
    mco_coro* co = NULL;

    /* Prevent busy workers from starving the global runq. */
    if (++w->sched_tick % SCHED_FAIR_TICK_INTERVAL == 0) {
        queue_node_t* node = runq_pop(sched->runq);
        co = node ? queue_entry(node, _sched_coro_ctx_t, runq_node)->co : NULL;
    }
    if (!co) {
        co = _sched_worker_fetch(sched, w);
    }
    if (co) {
        return co;
    }

    bool expected = false;
    if (atomic_compare_exchange_strong(&sched->poller_running, &expected, true)) {
        int n = platform_poller_wait(&sched->poller, cqes, 0);
        co = n > 0 ? _sched_io_process(sched, w, cqes, n) : NULL;
        atomic_store(&sched->poller_running, false);
        if (co) {
            return co;
        }
    }

    co = _sched_worker_steal(sched, w);
    if (co) {
        return co;
    }

    expected = false;
    if (atomic_compare_exchange_strong(&sched->poller_running, &expected, true)) {
        return _sched_worker_poll(sched, w, cqes);
    }
    return NULL;
}

static mco_coro* _sched_worker_park(scheduler_t* sched, _sched_worker_t* w) {
    bool expected = false;
    atomic_compare_exchange_strong(&w->parked, &expected, true);

    queue_node_t* node = runq_pop(sched->runq);
    mco_coro* co = node ? queue_entry(node, _sched_coro_ctx_t, runq_node)->co : NULL;
    if (co) {
        bool expected = true;
        atomic_compare_exchange_strong(&w->parked, &expected, false);
        return co;
    }

    int timer_ms = _sched_timer_worker_timeout(w);
    if (timer_ms == 0) {
        expected = true;
        atomic_compare_exchange_strong(&w->parked, &expected, false);
        return NULL;
    }

    if (timer_ms >= 0) {
        platform_sem_timedwait(w->sem, (uint64_t)timer_ms);
    } else {
        platform_sem_wait(w->sem);
    }
    expected = true;
    atomic_compare_exchange_strong(&w->parked, &expected, false);
    return NULL;
}

static int _sched_worker_entry_cb(void* arg) {
    _sched_worker_t* w = (_sched_worker_t*)arg;
    scheduler_t* sched = w->sched;
    _tls_worker = w;

    while (atomic_load(&sched->running)) {
        _sched_timer_process(w);

        mco_coro* co = _sched_worker_find(sched, w);
        if (co) {
            _sched_worker_run(w, co);
            continue;
        }

        co = _sched_worker_park(sched, w);
        if (co) {
            _sched_worker_run(w, co);
        }
    }

    return 0;
}

static void _sched_cleanup(scheduler_t* sched, int32_t started_count) {
    if (started_count > 0) {
        atomic_store(&sched->running, false);
    }

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
                    t->state = TIMER_IDLE;
                    /* Heap never took a reference; user owns the ref. */
                }
            }
            mtx_destroy(&w->timer_lock);
            if (w->coro_pool) {
                for (int32_t j = 0; j < w->coro_pool_count; j++) {
                    _sched_coro_free(&sched->coro_pool, w->coro_pool[j]);
                }
                free(w->coro_pool);
            }
        }
        free(sched->workers);
    }

    if (sched->runq) {
        runq_destroy(sched->runq);
    }

    if ((sched->wakeup_rd != PLATFORM_SO_ERROR_INVALID_SOCKET
     && sched->wakeup_wr != PLATFORM_SO_ERROR_INVALID_SOCKET)) {
        platform_poller_del(&sched->poller, &sched->wakeup_sqe);
        platform_socket_close(sched->wakeup_rd);
        platform_socket_close(sched->wakeup_wr);
    }

    if (sched->poller_ready) {
        platform_poller_deinit(&sched->poller);
    }

    for (int32_t i = 0; i < sched->coro_pool.count; i++) {
        _sched_coro_free(&sched->coro_pool, sched->coro_pool.slots[i]);
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

    int32_t  worker_count   = (int32_t)platform_info_getcpus();
    uint32_t deque_capacity = SCHED_DEQUE_CAP;
    uint32_t pool_cap       = 0;
    size_t   stack_size     = SCHED_CORO_STACK_SIZE;

    if (worker_count < 1) {
        worker_count = 4;
    }

    if (opts) {
        if (opts->worker_count > 0) {
            worker_count = opts->worker_count;
        }
        if (opts->deque_capacity > 0) {
            deque_capacity = opts->deque_capacity;
        }
        if (opts->coro_pool_capacity > 0) {
            pool_cap = opts->coro_pool_capacity;
        }
        if (opts->coro_stack_size > 0) {
            stack_size = opts->coro_stack_size;
        }
    }
    if (pool_cap == 0) {
        pool_cap = (uint32_t)(worker_count * SCHED_CORO_POOL_CAP_MUL);
    }

    sched->runq = runq_create();
    if (!sched->runq) {
        _sched_cleanup(sched, 0);
        return NULL;
    }

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

    atomic_store(&sched->running, true);
    atomic_store(&sched->num_stealing, 0);
    sched->iowait_slab = iowait_slab_create();
    if (!sched->iowait_slab) {
        _sched_cleanup(sched, 0);
        return NULL;
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

        atomic_store(&w->parked, false);
        atomic_store(&w->stealing, false);
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
        w->coro_pool = (void**)calloc(
            SCHED_CORO_POOL_CAP, sizeof(void*));
        w->coro_pool_count = 0;

        if (!w->deque || !w->sem || !w->coro_pool) {
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
            if (ctx->cleanup) {
                ctx->cleanup(ctx->arg);
            }
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

    bool expected = true;
    if (atomic_compare_exchange_strong(
            &sched->running, &expected, false)) {
        for (int32_t i = 0; i < sched->worker_count; i++) {
            platform_sem_post(sched->workers[i].sem);
            _sched_poller_wake(sched);
        }
    }

    if (_tls_worker && _tls_worker->sched == sched) {
        return;
    }

    for (int32_t i = 0; i < sched->worker_count; i++) {
        thrd_join(sched->workers[i].thread, NULL);
    }
    sched->joined = true;
}

static void _sched_enqueue(scheduler_t* sched, mco_coro* co) {
    _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)mco_get_user_data(co);

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
                    (_sched_coro_ctx_t*)mco_get_user_data(batch[i]);
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
    if (_sched_try_wake(co)) {
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
            if (!_sched_try_wake(cos[i])) {
                continue;
            }
            _sched_worker_enqueue(_tls_worker, cos[i]);
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
        _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)mco_get_user_data(cos[i]);
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
         * Wake one worker for the batch. A stealing worker that finds work
         * expands the wakeup chain, avoiding a batch-sized wakeup herd.
         */
        _sched_wake_worker(sched);
    }
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
    _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)mco_get_user_data(mco_running());
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

void scheduler_timer_start(
    scheduler_timer_t*   timer,
    scheduler_timer_fn_t cb,
    void*                ud,
    uint64_t             timeout_ms,
    uint64_t             repeat_ms) {
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    _sched_worker_t* owner = &timer->sched->workers[timer->owner];
    bool armed = false;

    mtx_lock(&owner->timer_lock);
    timer->cb           = cb;
    timer->ud           = ud;
    timer->repeat       = repeat_ms;

    switch (timer->state) {
    case TIMER_FIRING:
        timer->reset_pending = true;
        timer->timeout       = timeout_ms;
        break;
    case TIMER_QUEUED:
        heap_remove(&owner->timers, &timer->heap_node);
        /* fallthrough */
    case TIMER_IDLE:
        timer->timeout       = now + timeout_ms;
        timer->reset_pending = false;
        timer->state         = TIMER_QUEUED;
        heap_insert(&owner->timers, &timer->heap_node);
        armed = true;
        break;
    }
    _sched_timer_publish_deadline(owner);
    mtx_unlock(&owner->timer_lock);

    if (armed) {
        _sched_timer_wake_owner(owner);
    }
}

bool scheduler_timer_stop(scheduler_timer_t* timer) {
    _sched_worker_t* owner = &timer->sched->workers[timer->owner];

    bool cancelled = false;
    mtx_lock(&owner->timer_lock);
    switch (timer->state) {
    case TIMER_QUEUED:
        heap_remove(&owner->timers, &timer->heap_node);
        timer->state         = TIMER_IDLE;
        timer->stop_pending  = false;
        timer->reset_pending = false;
        cancelled = true;
        break;
    case TIMER_FIRING:
        cancelled = timer->reset_pending;
        timer->stop_pending  = true;
        timer->reset_pending = false;
        break;
    case TIMER_IDLE:
        break;
    }
    _sched_timer_publish_deadline(owner);
    mtx_unlock(&owner->timer_lock);
    return cancelled;
}

bool scheduler_timer_reset(scheduler_timer_t* timer, uint64_t timeout_ms) {
    if (!timer) {
        return false;
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    _sched_worker_t* owner = &timer->sched->workers[timer->owner];
    bool was_queued;
    bool armed = false;

    mtx_lock(&owner->timer_lock);
    was_queued = (timer->state == TIMER_QUEUED);

    if (timer->repeat != 0) {
        timer->repeat = timeout_ms;
    }
    switch (timer->state) {
    case TIMER_FIRING:
        timer->reset_pending = true;
        timer->timeout       = timeout_ms;
        break;
    case TIMER_QUEUED:
        heap_remove(&owner->timers, &timer->heap_node);
        /* fallthrough */
    case TIMER_IDLE:
        timer->timeout       = now + timeout_ms;
        timer->reset_pending = false;
        timer->state         = TIMER_QUEUED;
        heap_insert(&owner->timers, &timer->heap_node);
        armed = true;
        break;
    }
    _sched_timer_publish_deadline(owner);
    mtx_unlock(&owner->timer_lock);

    if (armed) {
        _sched_timer_wake_owner(owner);
    }
    return was_queued;
}

void scheduler_set_idle_cb(
    scheduler_t* sched, scheduler_idle_fn_t cb, void* ud) {
    sched->idle_cb = cb;
    sched->idle_ud = ud;
}
