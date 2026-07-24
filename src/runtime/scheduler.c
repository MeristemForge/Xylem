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
 *                                    scheduler_coro_ready() callers
 *
 * When a worker runs out of local work it becomes SEARCHING and does a
 * non-blocking poll + steal round. Exactly one idle worker becomes the
 * poller via CAS on poller_running: it owns the blocking poll that services
 * IO and timers. All other idle workers wait on a per-worker semaphore.
 * Work publication wakes one idle worker only when no worker is already
 * SEARCHING. The idle worker is reserved as SEARCHING before it is signalled,
 * so repeated spawn calls coalesce without repeated OS wakeups. A hand-off
 * into an empty local runnext slot wakes nobody -- the scheduling worker
 * usually consumes that coro itself. If the owner stalls and its FIFO queue
 * is empty, other workers may steal runnext as a last-resort rescue.
 *
 * Coroutine parking flows through scheduler_coro_park. The worker changes
 * RUNNING to WAITING before invoking the commit callback. A successful
 * callback publishes the waiter as its final shared operation, after which
 * a wake source may change WAITING to RUNNABLE immediately.
 */

#include "scheduler.h"

#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"
#include "xylem/xylem-threads.h"

#include "container/heap.h"
#include "container/list.h"
#include "container/mpsc.h"
#include "container/queue.h"
#include "arena.h"
#include "copool.h"
#include "iowait.h"
#include "minicoro/minicoro.h"
#include "platform/platform-cpu.h"
#include "platform/platform-info.h"
#include "platform/platform-sem.h"
#include "platform/platform-socket.h"
#include "runq.h"
#include "sync/spin.h"
#include "wsq.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SCHED_DEQUE_CAP          256
#define SCHED_RUNQ_BATCH_CAP     256

#define SCHED_CREDIT_DEFAULT     128u
/* Prime: avoids sync with power-of-two deque sizes. */
#define SCHED_FAIR_TICK_INTERVAL 61

#define SCHED_CORO_STACK_SIZE   (128 * 1024)
#define SCHED_CORO_POOL_BATCH   (COPOOL_LOCAL_DEFAULT_CAP / 2)

typedef enum {
    WORKER_RUNNING,
    WORKER_SEARCHING,
    WORKER_WAITING,
    WORKER_POLLING,
} _sched_worker_state_t;

typedef enum {
    SCHED_CORO_NEW,
    SCHED_CORO_RUNNABLE,
    SCHED_CORO_RUNNING,
    SCHED_CORO_WAITING,
    SCHED_CORO_DEAD,
} _sched_coro_state_t;

typedef enum {
    SCHED_YIELD_NONE,
    SCHED_YIELD_RUNNABLE,
    SCHED_YIELD_PARK,
} _sched_yield_reason_t;

typedef struct _sched_worker_s {
    thrd_t                          thread;
    wsq_t*                          deque;
    platform_sem_t*                 sem;
    scheduler_t*                    sched;
    uint32_t                        index;
    _sched_yield_reason_t           yield_reason;
    scheduler_coro_park_commit_fn_t park_commit;
    void*                           park_arg;
    _Atomic(_sched_worker_state_t)  state;
    _Atomic(mco_coro*)              runnext;
    copool_local_t*                 coro_pool;

    uint32_t                        sched_tick;
    uint32_t                        credit;
    heap_t                          timers;
    mtx_t                           timer_lock;
    /* Earliest timer deadline, UINT64_MAX if none. */
    _Atomic uint64_t                next_deadline_ms;
    list_t                          registry;        /* coroutines owned for shutdown.   */
    spin_t                          registry_lock;   /* protects registry.               */
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
    _Atomic bool          poller_running;
    spin_t                worker_state_lock;
    _Atomic int32_t       num_idle;
    _Atomic int32_t       num_searching;
    bool                  poller_ready;
    bool                  joined;
    _Atomic int64_t       alive;

    _Atomic uint32_t      timer_rr;
    _Atomic uint32_t      wake_rr;      /* round-robin start for wake_worker scan. */
    _Atomic uint32_t      spawn_rr;     /* round-robin for non-worker spawn owner. */
    mco_desc              coro_desc;
    arena_t*              coro_arena;
    copool_shared_t*      coro_pool;
};

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
    heap_node_t             heap_node;
    scheduler_t*            sched;
    scheduler_timer_fn_t    cb;
    void*                   ud;
    scheduler_timer_ud_fn_t ud_ref;
    scheduler_timer_ud_fn_t ud_unref;
    uint64_t                timeout;
    uint64_t                repeat;
    _timer_state_t          state;
    bool                    stop_pending;
    bool                    reset_pending;
    bool                    spawn;
    _Atomic int32_t         refcnt;
    uint32_t                owner;
};

typedef struct _sched_coro_ctx_s {
    void (*fn)(void*);
    void*                        arg;
    runq_node_t                  runq_node;
    list_node_t                  registry_node;
    mco_coro*                    co;
    _Atomic(_sched_coro_state_t) state;
    uint32_t                     registry_owner;
} _sched_coro_ctx_t;

typedef struct _sched_timer_fire_s {
    scheduler_timer_t*      timer;
    scheduler_timer_fn_t    cb;
    void*                   ud;
    scheduler_timer_ud_fn_t ud_ref;
    scheduler_timer_ud_fn_t ud_unref;
    bool                    spawn;
} _sched_timer_fire_t;

static thread_local _sched_worker_t* _tls_worker;

static void _sched_coro_transition(
    mco_coro*           co,
    _sched_coro_state_t from,
    _sched_coro_state_t to) {
    _sched_coro_ctx_t*  ctx      = (_sched_coro_ctx_t*)mco_get_user_data(co);
    _sched_coro_state_t expected = from;

    if (!atomic_compare_exchange_strong(&ctx->state, &expected, to)) {
        xylem_loge(
            "<sched> invalid coro transition co=%p expected=%d actual=%d next=%d",
            (void*)co,
            (int)from,
            (int)expected,
            (int)to);
        abort();
    }
}

static void _sched_coro_entry_cb(mco_coro* co) {
    _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)mco_get_user_data(co);
    void (*fn)(void*) = ctx->fn;
    void* arg = ctx->arg;

    fn(arg);
}

static _sched_worker_t* _sched_current_worker(scheduler_t* sched) {
    if (!_tls_worker || _tls_worker->sched != sched) {
        return NULL;
    }
    return _tls_worker;
}

static int _sched_coro_take_from_arena(
    scheduler_t*  sched,
    copool_slot_t* slots,
    int            count) {
    void* ptrs[SCHED_CORO_POOL_BATCH];
    if (count > SCHED_CORO_POOL_BATCH) {
        count = SCHED_CORO_POOL_BATCH;
    }

    int take_count = arena_alloc(sched->coro_arena, ptrs, count);
    for (int i = 0; i < take_count; i++) {
        slots[i].ptr   = ptrs[i];
        slots[i].state = COPOOL_SLOT_FRESH;
    }
    return take_count;
}

static void _sched_coro_put_to_arena(
    scheduler_t*        sched,
    const copool_slot_t* slots,
    int                  count) {
    while (count > 0) {
        void* ptrs[SCHED_CORO_POOL_BATCH];
        int put_count = count;
        if (put_count > SCHED_CORO_POOL_BATCH) {
            put_count = SCHED_CORO_POOL_BATCH;
        }
        for (int i = 0; i < put_count; i++) {
            ptrs[i] = slots[i].ptr;
        }
        arena_free(sched->coro_arena, ptrs, put_count);
        slots += put_count;
        count -= put_count;
    }
}

static void _sched_coro_put_to_shared(
    scheduler_t*        sched,
    const copool_slot_t* slots,
    int                  count) {
    if (count <= 0) {
        return;
    }
    int put_count = copool_shared_put(
        sched->coro_pool,
        slots,
        count);
    if (put_count < count) {
        _sched_coro_put_to_arena(
            sched,
            &slots[put_count],
            count - put_count);
    }
}

static void* _sched_coro_alloc_cb(
    size_t             size,
    void*              allocator_data,
    mco_storage_state* storage_state) {
    (void)size;
    scheduler_t*     sched  = (scheduler_t*)allocator_data;
    _sched_worker_t* worker = _sched_current_worker(sched);
    copool_slot_t     slot;

    if (worker && copool_local_take(worker->coro_pool, &slot, 1) == 1) {
        *storage_state =
            slot.state == COPOOL_SLOT_FRESH ? MCO_STORAGE_FRESH
                                            : MCO_STORAGE_REUSABLE;
        return slot.ptr;
    }

    copool_slot_t slots[SCHED_CORO_POOL_BATCH];
    int count = copool_shared_take(
        sched->coro_pool,
        slots,
        worker ? SCHED_CORO_POOL_BATCH : 1);
    if (count == 0) {
        count = _sched_coro_take_from_arena(
            sched,
            slots,
            SCHED_CORO_POOL_BATCH);
    }
    if (count == 0) {
        return NULL;
    }

    slot = slots[0];
    *storage_state =
        slot.state == COPOOL_SLOT_FRESH ? MCO_STORAGE_FRESH
                                        : MCO_STORAGE_REUSABLE;

    if (count == 1) {
        return slot.ptr;
    }
    if (!worker) {
        _sched_coro_put_to_shared(sched, &slots[1], count - 1);
        return slot.ptr;
    }

    int put_count =
        copool_local_put(worker->coro_pool, &slots[1], count - 1);
    _sched_coro_put_to_shared(
        sched,
        &slots[1 + put_count],
        count - 1 - put_count);
    return slot.ptr;
}

static void _sched_coro_dealloc_cb(
    void*             ptr,
    size_t            size,
    void*             allocator_data,
    mco_storage_state storage_state) {
    (void)size;
    scheduler_t*     sched  = (scheduler_t*)allocator_data;
    _sched_worker_t* worker = _sched_current_worker(sched);
    copool_slot_t     slot = {
        .ptr   = ptr,
        .state = COPOOL_SLOT_REUSABLE,
    };

    if (storage_state == MCO_STORAGE_FRESH) {
        slot.state = COPOOL_SLOT_FRESH;
        _sched_coro_put_to_arena(sched, &slot, 1);
        return;
    }
    if (storage_state != MCO_STORAGE_REUSABLE) {
        xylem_loge("<sched> invalid coroutine storage state=%d", storage_state);
        abort();
    }

    if (!worker) {
        _sched_coro_put_to_shared(sched, &slot, 1);
        return;
    }
    if (copool_local_put(worker->coro_pool, &slot, 1) == 1) {
        return;
    }

    copool_slot_t slots[SCHED_CORO_POOL_BATCH];
    int count = copool_local_take(
        worker->coro_pool,
        slots,
        SCHED_CORO_POOL_BATCH);
    _sched_coro_put_to_shared(sched, slots, count);
    if (copool_local_put(worker->coro_pool, &slot, 1) != 1) {
        xylem_loge("<sched> local coroutine pool put failed");
        abort();
    }
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

static void _sched_worker_signal(
    _sched_worker_t*      w,
    _sched_worker_state_t state) {
    scheduler_t* sched = w->sched;

    switch (state) {
    case WORKER_WAITING:
        platform_sem_post(w->sem);
        break;
    case WORKER_POLLING:
        _sched_poller_wake(sched);
        break;
    case WORKER_RUNNING:
    case WORKER_SEARCHING:
        break;
    }
}

/* Caller must hold w->sched->worker_state_lock. */
static _sched_worker_state_t _sched_worker_transition(
    _sched_worker_t*       w,
    _sched_worker_state_t next_state) {
    scheduler_t*         sched = w->sched;
    _sched_worker_state_t state = atomic_load(&w->state);
    if (state == next_state) {
        return state;
    }

    switch (state) {
    case WORKER_SEARCHING:
        atomic_fetch_sub(&sched->num_searching, 1);
        break;
    case WORKER_WAITING:
    case WORKER_POLLING:
        atomic_fetch_sub(&sched->num_idle, 1);
        break;
    case WORKER_RUNNING:
        break;
    }

    atomic_store(&w->state, next_state);
    switch (next_state) {
    case WORKER_SEARCHING:
        atomic_fetch_add(&sched->num_searching, 1);
        break;
    case WORKER_WAITING:
    case WORKER_POLLING:
        atomic_fetch_add(&sched->num_idle, 1);
        break;
    case WORKER_RUNNING:
        break;
    }
    return state;
}

/* Coalesced wake: no-op while a searcher already exists. */
static void _sched_wake_worker(scheduler_t* sched) {
    if (atomic_load(&sched->num_searching) != 0
     || atomic_load(&sched->num_idle) == 0) {
        return;
    }

    _sched_worker_t*      target        = NULL;
    _sched_worker_state_t waiting_state = WORKER_RUNNING;

    /* Reserve before signalling so concurrent producers coalesce here. */
    spin_lock(&sched->worker_state_lock);
    if (atomic_load(&sched->num_searching) != 0
     || atomic_load(&sched->num_idle) == 0) {
        spin_unlock(&sched->worker_state_lock);
        return;
    }

    uint32_t n     = (uint32_t)sched->worker_count;
    uint32_t start = atomic_load(&sched->wake_rr);
    for (uint32_t j = 0; j < n; j++) {
        uint32_t              i     = (start + j) % n;
        _sched_worker_t*      w     = &sched->workers[i];
        _sched_worker_state_t state = atomic_load(&w->state);
        if (state == WORKER_WAITING || state == WORKER_POLLING) {
            target = w;
            waiting_state = state;
            _sched_worker_transition(w, WORKER_SEARCHING);
            atomic_store(&sched->wake_rr, (i + 1) % n);
            break;
        }
    }
    spin_unlock(&sched->worker_state_lock);

    if (target) {
        _sched_worker_signal(target, waiting_state);
    }
}

static void _sched_poller_handoff(scheduler_t* sched) {
    atomic_store(&sched->poller_running, false);
    _sched_wake_worker(sched);
}

static bool _sched_worker_try_begin_search(_sched_worker_t* w) {
    scheduler_t* sched = w->sched;

    spin_lock(&sched->worker_state_lock);
    _sched_worker_state_t state = atomic_load(&w->state);
    bool                  searching = state == WORKER_SEARCHING;
    if (!searching && atomic_load(&sched->num_searching) == 0) {
        _sched_worker_transition(w, WORKER_SEARCHING);
        searching = true;
    }
    spin_unlock(&sched->worker_state_lock);
    return searching;
}

static void _sched_timer_notify_deadline(_sched_worker_t* owner) {
    scheduler_t*          sched       = owner->sched;
    _sched_worker_state_t owner_state = WORKER_RUNNING;
    bool                  wake_poller = false;

    spin_lock(&sched->worker_state_lock);
    owner_state = atomic_load(&owner->state);
    if (owner_state == WORKER_WAITING || owner_state == WORKER_POLLING) {
        _sched_worker_transition(owner, WORKER_SEARCHING);
    }
    for (int32_t i = 0; i < sched->worker_count; i++) {
        if (atomic_load(&sched->workers[i].state) == WORKER_POLLING) {
            wake_poller = true;
            break;
        }
    }
    spin_unlock(&sched->worker_state_lock);

    _sched_worker_signal(owner, owner_state);
    if (wake_poller && owner_state != WORKER_POLLING) {
        _sched_poller_wake(sched);
    }
}

static mco_coro* _sched_worker_take_runnable(_sched_worker_t* w) {
    scheduler_t* sched = w->sched;
    mco_coro*    co    = atomic_exchange(&w->runnext, NULL);
    if (co) {
        return co;
    }

    co = (mco_coro*)wsq_pop(w->deque);
    if (co) {
        return co;
    }

    int deque_cap = wsq_remaining(w->deque);
    if (deque_cap > SCHED_DEQUE_CAP / 2) {
        deque_cap = SCHED_DEQUE_CAP / 2;
    }
    runq_node_t* runq_nodes[(SCHED_DEQUE_CAP / 2)];
    int runq_n = runq_pop_fair(
        sched->runq, runq_nodes, deque_cap, sched->worker_count);
    if (runq_n <= 0) {
        return NULL;
    }

    for (int i = 1; i < runq_n; i++) {
        wsq_push(w->deque, runq_entry(runq_nodes[i], _sched_coro_ctx_t, runq_node)->co);
    }
    return runq_entry(runq_nodes[0], _sched_coro_ctx_t, runq_node)->co;
}

static mco_coro* _sched_worker_steal_runnable(_sched_worker_t* w) {
    scheduler_t* sched = w->sched;
    mco_coro*    co;

    if (sched->worker_count <= 1) {
        return NULL;
    }
    int deque_cap = wsq_remaining(w->deque);
    if (deque_cap > SCHED_DEQUE_CAP / 2) {
        deque_cap = SCHED_DEQUE_CAP / 2;
    }

    uint32_t worker_count = (uint32_t)sched->worker_count;
    uint32_t first        = (uint32_t)(w->index + 1) % worker_count;

    /* first skips self, so worker_count-1 covers all other workers */
    for (uint32_t i = 0; i < worker_count - 1; i++) {
        uint32_t target = (first + i) % worker_count;
        void*    stolen[(SCHED_DEQUE_CAP / 2)];
        int      stolen_n = wsq_steal_half(
            sched->workers[target].deque, stolen, deque_cap);
        if (stolen_n > 0) {
            for (int j = 1; j < stolen_n; j++) {
                wsq_push(w->deque, stolen[j]);
            }
            co = (mco_coro*)stolen[0];
            return co;
        }
    }

    for (uint32_t i = 0; i < worker_count - 1; i++) {
        uint32_t target = (first + i) % worker_count;
        co = atomic_exchange(&sched->workers[target].runnext, NULL);
        if (co) {
            return co;
        }
    }

    return NULL;
}

/**
 * Republish the earliest timer deadline so the lock-free guards in
 * _sched_timer_process_due / timeout helpers can skip the lock when
 * nothing is due. Caller MUST hold owner->timer_lock; readers use the same
 * default atomic ordering on the worker hot path.
 */
static inline void _sched_timer_publish_next_deadline(_sched_worker_t* owner) {
    heap_node_t* root = heap_peek(&owner->timers);
    uint64_t nd = root
        ? heap_entry(root, scheduler_timer_t, heap_node)->timeout
        : UINT64_MAX;
    atomic_store(&owner->next_deadline_ms, nd);
}

static bool _sched_timer_finish_fire(
    scheduler_timer_t* timer,
    _sched_worker_t*   owner,
    uint64_t           now) {
    if (timer->stop_pending) {
        timer->stop_pending  = false;
        timer->reset_pending = false;
        timer->state         = TIMER_IDLE;
        return false;
    }
    if (timer->reset_pending) {
        timer->timeout       = now + timer->timeout;
        timer->reset_pending = false;
        timer->state         = TIMER_QUEUED;
        heap_insert(&owner->timers, &timer->heap_node);
        return true;
    }
    if (timer->repeat > 0) {
        timer->timeout = now + timer->repeat;
        timer->state   = TIMER_QUEUED;
        heap_insert(&owner->timers, &timer->heap_node);
        return true;
    }
    timer->state = TIMER_IDLE;
    return false;
}

static void _sched_timer_complete(_sched_timer_fire_t* fire) {
    scheduler_timer_t* timer = fire->timer;
    _sched_worker_t*   owner = &timer->sched->workers[timer->owner];
    bool               armed = false;
    uint64_t           now   = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    mtx_lock(&owner->timer_lock);
    switch (timer->state) {
    case TIMER_FIRING:
        armed = _sched_timer_finish_fire(timer, owner, now);
        _sched_timer_publish_next_deadline(owner);
        break;
    case TIMER_IDLE:
    case TIMER_QUEUED:
        break;
    }
    mtx_unlock(&owner->timer_lock);

    if (armed) {
        _sched_timer_notify_deadline(owner);
    }
    if (fire->ud_unref) {
        fire->ud_unref(fire->ud);
    }
    _sched_timer_unref(timer);
}

static void _sched_enqueue_runnable(scheduler_t* sched, mco_coro* co) {
    _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)mco_get_user_data(co);

    if (!_tls_worker || _tls_worker->sched != sched) {
        runq_push(sched->runq, &ctx->runq_node);
        _sched_wake_worker(sched);
        return;
    }

    mco_coro* old = atomic_exchange(&_tls_worker->runnext, co);
    if (old) {
        if (wsq_push(_tls_worker->deque, old) != 0) {
            void* batch[(SCHED_DEQUE_CAP / 2) + 1];
            int count = wsq_pop_half(
                _tls_worker->deque,
                batch,
                SCHED_DEQUE_CAP / 2);
            batch[count++] = old;

            runq_node_t* nodes[(SCHED_DEQUE_CAP / 2) + 1];
            for (int i = 0; i < count; i++) {
                _sched_coro_ctx_t* old_ctx =
                    (_sched_coro_ctx_t*)mco_get_user_data(batch[i]);
                nodes[i] = &old_ctx->runq_node;
            }
            runq_push_batch(sched->runq, nodes, count);
        }
        /* The displaced coroutine is now stealable. */
        _sched_wake_worker(sched);
    }
}

static void _sched_coro_start(scheduler_t* sched, mco_coro* co) {
    _sched_coro_transition(co, SCHED_CORO_NEW, SCHED_CORO_RUNNABLE);
    _sched_enqueue_runnable(sched, co);
}

int scheduler_coro_spawn(scheduler_t* sched, void (*fn)(void*), void* arg) {
    if (!fn || !atomic_load(&sched->running)) {
        return -1;
    }

    _sched_coro_ctx_t* ctx =
        (_sched_coro_ctx_t*)calloc(1, sizeof(_sched_coro_ctx_t));
    if (!ctx) {
        return -1;
    }

    ctx->fn  = fn;
    ctx->arg = arg;

    mco_desc desc = sched->coro_desc;
    desc.user_data = ctx;

    mco_coro* co = NULL;
    if (mco_create(&co, &desc) != MCO_SUCCESS) {
        free(ctx);
        return -1;
    }

    ctx->co = co;
    atomic_init(&ctx->state, SCHED_CORO_NEW);

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
    _sched_coro_start(sched, co);
    return 0;
}

static void _sched_timer_launch_cb(void* arg) {
    _sched_timer_fire_t* fire = (_sched_timer_fire_t*)arg;
    fire->cb(fire->timer, fire->ud);
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
    if (scheduler_coro_spawn(sched, _sched_timer_launch_cb, f) != 0) {
        xylem_loge("<sched> timer spawn failed");
        free(f);
        return -1;
    }
    return 0;
}

static int _sched_timer_process_due(_sched_worker_t* w) {
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

    int          timeout;
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

    _sched_timer_publish_next_deadline(w);
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

static int _sched_timer_wait_timeout(_sched_worker_t* w) {
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

static mco_coro* _sched_worker_dispatch_poller_runnables(
    _sched_worker_t*       w,
    platform_poller_cqe_t* cqes,
    int                    n) {
    scheduler_t* sched   = w->sched;
    mco_coro*    run_now = NULL;
    mco_coro*    runnables[IOWAIT_EVENT_RUNNABLE_CAP];
    runq_node_t* runq_nodes[
        PLATFORM_POLLER_CQE_NUM * IOWAIT_EVENT_RUNNABLE_CAP];
    int32_t runq_count       = 0;
    int32_t local_count      = 0;
    bool    has_wakeup_event = false;

    for (int i = 0; i < n; i++) {
        if (cqes[i].ud == NULL) {
            has_wakeup_event = true;
            continue;
        }

        size_t runnable_count = iowait_process_event(
            sched,
            (int)cqes[i].op,
            cqes[i].ud,
            runnables);
        for (size_t j = 0; j < runnable_count; j++) {
            mco_coro* co = runnables[j];
            _sched_coro_transition(
                co,
                SCHED_CORO_WAITING,
                SCHED_CORO_RUNNABLE);
            if (!run_now) {
                run_now = co;
                continue;
            }
            if (wsq_push(w->deque, co) == 0) {
                local_count++;
                continue;
            }
            _sched_coro_ctx_t* ctx =
                (_sched_coro_ctx_t*)mco_get_user_data(co);
            runq_nodes[runq_count++] = &ctx->runq_node;
        }
    }

    if (has_wakeup_event) {
        _sched_wakeup_flush(sched);
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

static void _sched_worker_enqueue_runnable(_sched_worker_t* w, mco_coro* co) {
    if (wsq_push(w->deque, co) != 0) {
        _sched_coro_ctx_t* ctx = (_sched_coro_ctx_t*)mco_get_user_data(co);
        runq_push(w->sched->runq, &ctx->runq_node);
        _sched_wake_worker(w->sched);
    }
}

static void _sched_coro_exit(_sched_worker_t* w, mco_coro* co) {
    _sched_coro_ctx_t* ctx   = (_sched_coro_ctx_t*)mco_get_user_data(co);
    _sched_worker_t*   owner = &w->sched->workers[ctx->registry_owner];

    _sched_coro_transition(co, SCHED_CORO_RUNNING, SCHED_CORO_DEAD);

    spin_lock(&owner->registry_lock);
    list_remove(&owner->registry, &ctx->registry_node);
    spin_unlock(&owner->registry_lock);

    free(ctx);
    mco_destroy(co);

    scheduler_t* sched = w->sched;
    int64_t      prev  = atomic_fetch_sub(&sched->alive, 1);
    if (prev == 1 && sched->idle_cb) {
        sched->idle_cb(sched->idle_ud);
    }
}

static void _sched_coro_commit_park(_sched_worker_t* w, mco_coro* co) {
    scheduler_coro_park_commit_fn_t commit = w->park_commit;
    void*                           arg    = w->park_arg;

    w->yield_reason = SCHED_YIELD_NONE;
    w->park_commit  = NULL;
    w->park_arg     = NULL;

    if (!commit) {
        xylem_loge("<sched> park without commit co=%p", (void*)co);
        abort();
    }

    _sched_coro_transition(co, SCHED_CORO_RUNNING, SCHED_CORO_WAITING);
    if (commit(co, arg)) {
        return;
    }

    _sched_coro_transition(co, SCHED_CORO_WAITING, SCHED_CORO_RUNNABLE);
    _sched_worker_enqueue_runnable(w, co);
}

static void _sched_coro_abort_on_error(mco_result result) {
    if (result != MCO_SUCCESS) {
        abort();
    }
}

static inline void _sched_worker_execute_runnable(_sched_worker_t* w, mco_coro* co) {
    w->yield_reason = SCHED_YIELD_NONE;
    w->park_commit  = NULL;
    w->park_arg     = NULL;
    w->credit       = SCHED_CREDIT_DEFAULT;

    _sched_coro_transition(co, SCHED_CORO_RUNNABLE, SCHED_CORO_RUNNING);
    _sched_coro_abort_on_error(mco_resume(co));

    if (mco_status(co) == MCO_DEAD) {
        _sched_coro_exit(w, co);
        return;
    }
    if (!atomic_load(&w->sched->running)) {
        w->yield_reason = SCHED_YIELD_NONE;
        w->park_commit  = NULL;
        w->park_arg     = NULL;
        return;
    }

    switch (w->yield_reason) {
    case SCHED_YIELD_RUNNABLE:
        w->yield_reason = SCHED_YIELD_NONE;
        _sched_coro_transition(
            co,
            SCHED_CORO_RUNNING,
            SCHED_CORO_RUNNABLE);
        _sched_worker_enqueue_runnable(w, co);
        return;
    case SCHED_YIELD_PARK:
        _sched_coro_commit_park(w, co);
        return;
    case SCHED_YIELD_NONE:
        xylem_loge("<sched> yield without reason co=%p", (void*)co);
        abort();
    }
}

static mco_coro* _sched_worker_poll_runnable(
    _sched_worker_t*       w,
    platform_poller_cqe_t* cqes) {
    scheduler_t* sched = w->sched;
    mco_coro*    co    = NULL;

    while (atomic_load(&sched->running)) {
        /* Publishing the idle state before the recheck closes lost wakeups. */
        spin_lock(&sched->worker_state_lock);
        _sched_worker_transition(w, WORKER_POLLING);
        spin_unlock(&sched->worker_state_lock);

        co = _sched_worker_take_runnable(w);
        if (!co && _sched_worker_try_begin_search(w)) {
            co = _sched_worker_steal_runnable(w);
            if (!co) {
                spin_lock(&sched->worker_state_lock);
                _sched_worker_transition(w, WORKER_POLLING);
                spin_unlock(&sched->worker_state_lock);
            }
        }
        if (!co) {
            co = _sched_worker_take_runnable(w);
        }
        if (co) {
            break;
        }
        int poll_ms = _sched_timer_poll_timeout(sched);
        int n = platform_poller_wait(&sched->poller, cqes, poll_ms);
        if (!atomic_load(&sched->running)) {
            break;
        }
        co = n > 0
            ? _sched_worker_dispatch_poller_runnables(w, cqes, n)
            : NULL;

        for (int32_t i = 0; i < sched->worker_count; i++) {
            _sched_timer_process_due(&sched->workers[i]);
        }

        if (!co) {
            co = _sched_worker_take_runnable(w);
        }
        if (!co && _sched_worker_try_begin_search(w)) {
            co = _sched_worker_steal_runnable(w);
            if (!co) {
                spin_lock(&sched->worker_state_lock);
                _sched_worker_transition(w, WORKER_POLLING);
                spin_unlock(&sched->worker_state_lock);
            }
        }
        if (!co) {
            co = _sched_worker_take_runnable(w);
        }
        if (co) {
            break;
        }
    }

    if (!co) {
        atomic_store(&sched->poller_running, false);
        return NULL;
    }
    spin_lock(&sched->worker_state_lock);
    _sched_worker_transition(w, WORKER_RUNNING);
    spin_unlock(&sched->worker_state_lock);
    _sched_poller_handoff(sched);
    return co;
}

static void _sched_worker_wait(_sched_worker_t* w) {
    int timer_ms = _sched_timer_wait_timeout(w);
    if (timer_ms > 0) {
        platform_sem_timedwait(w->sem, (uint64_t)timer_ms);
    }
    if (timer_ms < 0) {
        platform_sem_wait(w->sem);
    }
    spin_lock(&w->sched->worker_state_lock);
    if (atomic_load(&w->state) == WORKER_WAITING) {
        _sched_worker_transition(w, WORKER_RUNNING);
    }
    spin_unlock(&w->sched->worker_state_lock);
}

static mco_coro* _sched_worker_find_runnable(_sched_worker_t* w) {
    scheduler_t*          sched = w->sched;
    platform_poller_cqe_t cqes[PLATFORM_POLLER_CQE_NUM];
    mco_coro*             co = NULL;

    while (atomic_load(&sched->running)) {
        _sched_timer_process_due(w);

        co = NULL;
        /* Prevent busy workers from starving the global runq. */
        if (++w->sched_tick % SCHED_FAIR_TICK_INTERVAL == 0) {
            runq_node_t* node = runq_pop(sched->runq);
            co = node ? runq_entry(node, _sched_coro_ctx_t, runq_node)->co
                      : NULL;
        }
        if (!co) {
            co = _sched_worker_take_runnable(w);
        }
        if (co) {
            break;
        }

        bool expected = false;
        if (atomic_compare_exchange_strong(
                &sched->poller_running,
                &expected,
                true)) {
            int n = platform_poller_wait(&sched->poller, cqes, 0);
            co = n > 0
                ? _sched_worker_dispatch_poller_runnables(w, cqes, n)
                : NULL;
            if (co) {
                _sched_poller_handoff(sched);
                break;
            }
            atomic_store(&sched->poller_running, false);
        }

        if (_sched_worker_try_begin_search(w)) {
            co = _sched_worker_steal_runnable(w);
            if (co) {
                break;
            }
        }

        expected = false;
        if (atomic_compare_exchange_strong(
                &sched->poller_running,
                &expected,
                true)) {
            return _sched_worker_poll_runnable(w, cqes);
        }

        /* Publish WAITING before the final work and poller rechecks. */
        spin_lock(&sched->worker_state_lock);
        _sched_worker_transition(w, WORKER_WAITING);
        spin_unlock(&sched->worker_state_lock);

        co = _sched_worker_take_runnable(w);
        if (!co && _sched_worker_try_begin_search(w)) {
            co = _sched_worker_steal_runnable(w);
            if (!co) {
                spin_lock(&sched->worker_state_lock);
                _sched_worker_transition(w, WORKER_WAITING);
                spin_unlock(&sched->worker_state_lock);
            }
        }
        if (!co) {
            co = _sched_worker_take_runnable(w);
        }
        if (co) {
            break;
        }

        expected = false;
        if (atomic_compare_exchange_strong(
                &sched->poller_running,
                &expected,
                true)) {
            return _sched_worker_poll_runnable(w, cqes);
        }

        _sched_worker_wait(w);
    }
    if (!co) {
        return NULL;
    }
    spin_lock(&sched->worker_state_lock);
    _sched_worker_state_t state =
        _sched_worker_transition(w, WORKER_RUNNING);
    spin_unlock(&sched->worker_state_lock);
    if (state != WORKER_RUNNING) {
        _sched_wake_worker(sched);
    }
    return co;
}

static int _sched_worker_entry_cb(void* arg) {
    _sched_worker_t* w     = (_sched_worker_t*)arg;
    scheduler_t*     sched = w->sched;
    _tls_worker = w;

    while (atomic_load(&sched->running)) {
        mco_coro* co = _sched_worker_find_runnable(w);
        if (co) {
            _sched_worker_execute_runnable(w, co);
        }
    }

    return 0;
}

static void _sched_cleanup(scheduler_t* sched, int32_t started_count) {
    if (started_count > 0) {
        atomic_store(&sched->running, false);
    }

    if (sched->workers && !sched->joined) {
        _sched_poller_wake(sched);
        for (int32_t i = 0; i < started_count; i++) {
            platform_sem_post(sched->workers[i].sem);
        }
        for (int32_t i = 0; i < started_count; i++) {
            thrd_join(sched->workers[i].thread, NULL);
        }
        sched->joined = true;
    }

    /* iowait timers call scheduler_timer_stop which takes timer_lock. */
    iowait_slab_destroy(sched->iowait_slab);
    sched->iowait_slab = NULL;

    if (sched->workers) {
        for (int32_t i = 0; i < sched->worker_count; i++) {
            _sched_worker_t* w = &sched->workers[i];
            if (w->deque) {
                wsq_destroy(w->deque);
            }
            if (w->sem) {
                platform_sem_destroy(w->sem);
            }
            copool_local_destroy(w->coro_pool);
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

    copool_shared_destroy(sched->coro_pool);
    arena_destroy(sched->coro_arena);
    free(sched);
}

scheduler_t* scheduler_create(scheduler_opts_t* opts) {
    scheduler_t* sched = (scheduler_t*)calloc(1, sizeof(scheduler_t));
    if (!sched) {
        return NULL;
    }

    sched->wakeup_rd = PLATFORM_SO_ERROR_INVALID_SOCKET;
    sched->wakeup_wr = PLATFORM_SO_ERROR_INVALID_SOCKET;

    int32_t           worker_count = (int32_t)platform_info_getcpus();
    size_t            stack_size   = SCHED_CORO_STACK_SIZE;

    if (worker_count < 1) {
        worker_count = 4;
    }

    if (opts) {
        if (opts->worker_count > 0) {
            worker_count = opts->worker_count;
        }
        if (opts->coro_stack_size > 0) {
            stack_size = opts->coro_stack_size;
        }
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
    atomic_store(&sched->num_idle, 0);
    atomic_store(&sched->num_searching, 0);
    spin_init(&sched->worker_state_lock);
    sched->iowait_slab = iowait_slab_create();
    if (!sched->iowait_slab) {
        _sched_cleanup(sched, 0);
        return NULL;
    }

    sched->coro_desc                =
        mco_desc_init(_sched_coro_entry_cb, stack_size);
    sched->coro_desc.alloc_cb       = _sched_coro_alloc_cb;
    sched->coro_desc.dealloc_cb     = _sched_coro_dealloc_cb;
    sched->coro_desc.allocator_data = sched;
    sched->coro_arena = arena_create(sched->coro_desc.coro_size);
    sched->coro_pool  = copool_shared_create();
    if (!sched->coro_arena || !sched->coro_pool) {
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

        atomic_store(&w->state, WORKER_RUNNING);
        heap_init(&w->timers, _sched_timer_compare_cb);
        list_init(&w->registry);
        spin_init(&w->registry_lock);
        if (mtx_init(&w->timer_lock, mtx_plain) != thrd_success) {
            _sched_cleanup(sched, 0);
            return NULL;
        }
        sched->worker_count = i + 1;

        w->deque     = wsq_create(SCHED_DEQUE_CAP);
        w->sem       = platform_sem_create(0);
        w->coro_pool = copool_local_create(0);
        atomic_init(&w->next_deadline_ms, UINT64_MAX);

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
        _sched_poller_wake(sched);
        for (int32_t i = 0; i < sched->worker_count; i++) {
            platform_sem_post(sched->workers[i].sem);
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

void scheduler_coro_ready(scheduler_t* sched, mco_coro* co) {
    if (!atomic_load(&sched->running)) {
        return;
    }
    _sched_coro_transition(co, SCHED_CORO_WAITING, SCHED_CORO_RUNNABLE);
    _sched_enqueue_runnable(sched, co);
}

void scheduler_coro_ready_batch(
    scheduler_t* sched,
    mco_coro**   coros,
    int          count) {
    if (count <= 0 || !atomic_load(&sched->running)) {
        return;
    }

    if (_tls_worker && _tls_worker->sched == sched && sched->worker_count == 1) {
        for (int i = 0; i < count; i++) {
            _sched_coro_transition(
                coros[i],
                SCHED_CORO_WAITING,
                SCHED_CORO_RUNNABLE);
            _sched_worker_enqueue_runnable(_tls_worker, coros[i]);
        }
        return;
    }

    runq_node_t* nodes[SCHED_RUNQ_BATCH_CAP];
    int          node_count = 0;

    for (int i = 0; i < count; i++) {
        _sched_coro_transition(
            coros[i],
            SCHED_CORO_WAITING,
            SCHED_CORO_RUNNABLE);
        _sched_coro_ctx_t* ctx =
            (_sched_coro_ctx_t*)mco_get_user_data(coros[i]);
        nodes[node_count++] = &ctx->runq_node;
        if (node_count == SCHED_RUNQ_BATCH_CAP) {
            runq_push_batch(sched->runq, nodes, node_count);
            node_count = 0;
        }
    }

    if (node_count > 0) {
        runq_push_batch(sched->runq, nodes, node_count);
    }

    _sched_wake_worker(sched);
}

void scheduler_coro_park(
    scheduler_t*                    sched,
    scheduler_coro_park_commit_fn_t commit,
    void*                           arg) {
    if (!_tls_worker || _tls_worker->sched != sched || !mco_running()) {
        xylem_loge(
            "<sched> park outside coroutine worker=%p co=%p",
            (void*)_tls_worker,
            (void*)mco_running());
        abort();
    }
    if (!commit) {
        xylem_loge("<sched> park without commit");
        abort();
    }

    _tls_worker->yield_reason = SCHED_YIELD_PARK;
    _tls_worker->park_commit  = commit;
    _tls_worker->park_arg     = arg;
    _sched_coro_abort_on_error(mco_yield(mco_running()));
}

bool scheduler_coro_consume_credit(uint32_t cost) {
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

void scheduler_coro_yield(void) {
    if (!_tls_worker || !mco_running()) {
        return;
    }
    _tls_worker->yield_reason = SCHED_YIELD_RUNNABLE;
    _sched_coro_abort_on_error(mco_yield(mco_running()));
}

platform_poller_sq_t* scheduler_get_poller(scheduler_t* sched) {
    return &sched->poller;
}

iowait_slab_t* scheduler_get_iowait_slab(scheduler_t* sched) {
    return sched->iowait_slab;
}

scheduler_timer_t* scheduler_timer_create(scheduler_t* sched) {
    if (!atomic_load(&sched->running)) {
        return NULL;
    }
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
    atomic_init(&t->refcnt, 1);
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
    if (!atomic_load(&timer->sched->running)) {
        return;
    }
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    _sched_worker_t* owner = &timer->sched->workers[timer->owner];
    bool             armed = false;

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
    _sched_timer_publish_next_deadline(owner);
    mtx_unlock(&owner->timer_lock);

    if (armed) {
        _sched_timer_notify_deadline(owner);
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
    _sched_timer_publish_next_deadline(owner);
    mtx_unlock(&owner->timer_lock);
    return cancelled;
}

bool scheduler_timer_reset(scheduler_timer_t* timer, uint64_t timeout_ms) {
    if (!timer || !atomic_load(&timer->sched->running)) {
        return false;
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    _sched_worker_t* owner     = &timer->sched->workers[timer->owner];
    bool             cancelled = false;
    bool             armed     = false;

    mtx_lock(&owner->timer_lock);
    cancelled = (timer->state == TIMER_QUEUED)
                || (timer->state == TIMER_FIRING && timer->reset_pending);

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
    _sched_timer_publish_next_deadline(owner);
    mtx_unlock(&owner->timer_lock);

    if (armed) {
        _sched_timer_notify_deadline(owner);
    }
    return cancelled;
}

void scheduler_set_idle_cb(
    scheduler_t*        sched,
    scheduler_idle_fn_t cb,
    void*               ud) {
    sched->idle_cb = cb;
    sched->idle_ud = ud;
}
