# scheduler_park Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `scheduler_park` primitive to eliminate the "schedule-before-yield" race in all sync primitives and runtime APIs.

**Architecture:** `scheduler_park(sched, fn, arg)` stores a callback in the current worker's TLS, then yields. The worker loop detects the callback after `mco_resume` returns, invokes it with the now-suspended coroutine. If the callback returns `false`, the coroutine is immediately re-scheduled. This is modeled after Go's `gopark`. The current worker loop already "parks" coroutines on yield (never re-schedules them automatically), so `scheduler_park` only adds callback invocation — zero behavioral change for existing non-park yields.

**Tech Stack:** C11 atomics, minicoro, existing scheduler/runq/wsdeque.

---

## File Structure

| File | Responsibility |
|------|---------------|
| `src/runtime/scheduler.h` | Add `scheduler_park_fn_t` typedef and `scheduler_park` declaration |
| `src/runtime/scheduler.c` | Implement `scheduler_park` (TLS slot + worker loop handling) |
| `src/sync/xylem-waitgroup.c` | Refactor `wait` to use `scheduler_park` |
| `src/sync/xylem-mutex.c` | Refactor `lock` to use `scheduler_park` |
| `src/sync/xylem-channel.c` | Refactor `recv` to use `scheduler_park` |
| `src/runtime/xylem-runtime.c` | Refactor `sleep` and `submit` to use `scheduler_park` |
| `src/net/addr.c` | Refactor `xylem_addr_resolve` to use `scheduler_park` |

Note: `src/runtime/iowait.c` has the same pattern but uses a CAS-based state machine that partially mitigates the race. It should be refactored as follow-up work.

---

### Task 1: Add `scheduler_park` to the scheduler

**Files:**
- Modify: `src/runtime/scheduler.h`
- Modify: `src/runtime/scheduler.c`

- [ ] **Step 1: Add the typedef and declaration to scheduler.h**

Add after the existing `scheduler_poll_fn_t` typedef, and add `#include <stdbool.h>` at the top:

```c
#include <stdbool.h>

/**
 * @brief Park callback invoked after the coroutine is suspended.
 *
 * Called on the same worker thread, immediately after the coroutine yields.
 * The coroutine is guaranteed to be in MCO_SUSPENDED state.
 *
 * @param co   The now-suspended coroutine.
 * @param arg  Opaque argument from scheduler_park().
 *
 * @return true to confirm park (coroutine stays suspended until
 *         explicitly scheduled), false to cancel (coroutine is
 *         immediately re-scheduled on this worker).
 */
typedef bool (*scheduler_park_fn_t)(mco_coro* co, void* arg);

/**
 * @brief Suspend the calling coroutine and invoke a callback.
 *
 * The callback runs after the coroutine has yielded, on the same
 * worker thread. The callback should publish the coroutine pointer
 * to the appropriate shared state for later wakeup via
 * scheduler_schedule().
 *
 * Must be called from a scheduler worker thread (i.e., from within
 * a running coroutine).
 *
 * @param sched  Scheduler handle.
 * @param fn     Callback invoked with the suspended coroutine.
 * @param arg    Opaque argument passed to fn.
 */
extern void scheduler_park(
    scheduler_t* sched, scheduler_park_fn_t fn, void* arg);
```

- [ ] **Step 2: Add TLS park state to the worker struct**

In `src/runtime/scheduler.c`, add two fields to `_sched_worker_t`:

```c
typedef struct _sched_worker_s {
    thrd_t               thread;
    wsdeque_t*           deque;
    platform_sem_t*      sem;
    scheduler_t*         sched;
    uint32_t             index;
    _Atomic bool         is_polling;
    scheduler_park_fn_t  park_fn;
    void*                park_arg;
} _sched_worker_t;
```

- [ ] **Step 3: Implement `scheduler_park`**

Add to `src/runtime/scheduler.c`:

```c
void scheduler_park(
    scheduler_t* sched, scheduler_park_fn_t fn, void* arg) {
    (void)sched;
    _tls_worker->park_fn  = fn;
    _tls_worker->park_arg = arg;
    mco_yield(mco_running());
}
```

- [ ] **Step 4: Handle park callback in the worker loop**

In `_sched_worker_entry`, replace the coroutine execution block:

```c
if (co) {
    mco_resume(co);
    if (mco_status(co) == MCO_DEAD) {
        _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
        free(ctx);
        mco_destroy(co);
    }
    continue;
}
```

With:

```c
if (co) {
    mco_resume(co);
    if (mco_status(co) == MCO_DEAD) {
        _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
        free(ctx);
        mco_destroy(co);
    } else if (w->park_fn) {
        scheduler_park_fn_t fn = w->park_fn;
        void* arg = w->park_arg;
        w->park_fn  = NULL;
        w->park_arg = NULL;
        if (!fn(co, arg)) {
            wsdeque_push(w->deque, co);
        }
    }
    continue;
}
```

Key: when `park_fn` is NULL (normal yield without park), do nothing — the coroutine stays suspended until explicitly scheduled. This matches current behavior exactly.

- [ ] **Step 5: Apply the same change to the drain loop**

Replace the drain loop:

```c
for (;;) {
    mco_coro* co = _sched_try_get_coro(sched, w);
    if (!co) {
        break;
    }
    mco_resume(co);
    if (mco_status(co) == MCO_DEAD) {
        _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
        free(ctx);
        mco_destroy(co);
    }
}
```

With:

```c
for (;;) {
    mco_coro* co = _sched_try_get_coro(sched, w);
    if (!co) {
        break;
    }
    mco_resume(co);
    if (mco_status(co) == MCO_DEAD) {
        _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
        free(ctx);
        mco_destroy(co);
    } else if (w->park_fn) {
        scheduler_park_fn_t fn = w->park_fn;
        void* arg = w->park_arg;
        w->park_fn  = NULL;
        w->park_arg = NULL;
        if (!fn(co, arg)) {
            wsdeque_push(w->deque, co);
        }
    }
}
```

- [ ] **Step 6: Build and run existing test-runtime**

Run (WSL): `cd out-wsl && ninja test-runtime && ./tests/test-runtime`

Expected: All 10 tests pass unchanged. `scheduler_park` is additive.

- [ ] **Step 7: Commit**

```bash
git add src/runtime/scheduler.h src/runtime/scheduler.c
git commit -m "feat(scheduler): add scheduler_park for race-free coroutine suspension"
```

---

### Task 2: Refactor xylem_waitgroup to use scheduler_park

**Files:**
- Modify: `src/sync/xylem-waitgroup.c`
- Test: `tests/test-waitgroup.c`

- [ ] **Step 1: Rewrite xylem_waitgroup_wait**

Replace the entire file content after the includes with:

```c
#include "xylem/sync/xylem-waitgroup.h"

#include "runtime/runtime.h"
#include "runtime/scheduler.h"

#include <stdatomic.h>
#include <stdlib.h>

struct xylem_waitgroup_s {
    atomic_size_t      cnt;
    _Atomic(mco_coro*) wait_coro;
};

xylem_waitgroup_t* xylem_waitgroup_create(void) {
    xylem_waitgroup_t* wg =
        (xylem_waitgroup_t*)calloc(1, sizeof(xylem_waitgroup_t));
    if (!wg) {
        return NULL;
    }
    atomic_init(&wg->cnt, 0);
    atomic_init(&wg->wait_coro, NULL);
    return wg;
}

void xylem_waitgroup_destroy(xylem_waitgroup_t* wg) {
    if (!wg) {
        return;
    }
    free(wg);
}

void xylem_waitgroup_add(xylem_waitgroup_t* wg, size_t delta) {
    atomic_fetch_add(&wg->cnt, delta);
}

void xylem_waitgroup_done(xylem_waitgroup_t* wg) {
    size_t prev = atomic_fetch_sub(&wg->cnt, 1);
    if (prev == 1) {
        mco_coro* co = atomic_exchange(&wg->wait_coro, NULL);
        if (co) {
            scheduler_schedule(runtime_get_scheduler(), co);
        }
    }
}

static bool _wg_park_cb(mco_coro* co, void* arg) {
    xylem_waitgroup_t* wg = (xylem_waitgroup_t*)arg;
    atomic_store(&wg->wait_coro, co);
    if (atomic_load(&wg->cnt) == 0) {
        atomic_store(&wg->wait_coro, NULL);
        return false;
    }
    return true;
}

void xylem_waitgroup_wait(xylem_waitgroup_t* wg) {
    if (atomic_load(&wg->cnt) == 0) {
        return;
    }
    scheduler_park(runtime_get_scheduler(), _wg_park_cb, wg);
}
```

- [ ] **Step 2: Build and run test-waitgroup**

Run: `cd out-wsl && ninja test-waitgroup && ./tests/test-waitgroup`

Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add src/sync/xylem-waitgroup.c
git commit -m "fix(waitgroup): use scheduler_park to eliminate schedule-before-yield race"
```

---

### Task 3: Refactor xylem_mutex to use scheduler_park

**Files:**
- Modify: `src/sync/xylem-mutex.c`
- Test: `tests/test-mutex.c`

- [ ] **Step 1: Rewrite xylem_mutex_lock**

Replace the full file:

```c
#include "xylem/sync/xylem-mutex.h"

#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "container/queue.h"

#include <stdatomic.h>
#include <stdlib.h>

typedef struct {
    queue_node_t node;
    mco_coro*    co;
} _mutex_waiter_t;

struct xylem_mutex_s {
    _Atomic uint32_t state;
    atomic_flag      guard;
    queue_t          waiters;
};

static void _spin_lock(atomic_flag* flag) {
    while (atomic_flag_test_and_set_explicit(flag, memory_order_acquire)) {
    }
}

static void _spin_unlock(atomic_flag* flag) {
    atomic_flag_clear_explicit(flag, memory_order_release);
}

xylem_mutex_t* xylem_mutex_create(void) {
    xylem_mutex_t* mtx =
        (xylem_mutex_t*)calloc(1, sizeof(xylem_mutex_t));
    if (!mtx) {
        return NULL;
    }
    atomic_init(&mtx->state, 0);
    atomic_flag_clear(&mtx->guard);
    queue_init(&mtx->waiters);
    return mtx;
}

void xylem_mutex_destroy(xylem_mutex_t* mtx) {
    if (!mtx) {
        return;
    }
    free(mtx);
}

typedef struct {
    xylem_mutex_t*  mtx;
    _mutex_waiter_t waiter;
} _mutex_park_ctx_t;

static bool _mutex_park_cb(mco_coro* co, void* arg) {
    _mutex_park_ctx_t* ctx = (_mutex_park_ctx_t*)arg;
    ctx->waiter.co = co;

    _spin_lock(&ctx->mtx->guard);

    uint32_t expected = 0;
    if (atomic_compare_exchange_strong(&ctx->mtx->state, &expected, 1)) {
        _spin_unlock(&ctx->mtx->guard);
        return false;
    }

    queue_enqueue(&ctx->mtx->waiters, &ctx->waiter.node);
    _spin_unlock(&ctx->mtx->guard);
    return true;
}

void xylem_mutex_lock(xylem_mutex_t* mtx) {
    uint32_t expected = 0;
    if (atomic_compare_exchange_strong(&mtx->state, &expected, 1)) {
        return;
    }

    _mutex_park_ctx_t ctx;
    ctx.mtx = mtx;
    scheduler_park(runtime_get_scheduler(), _mutex_park_cb, &ctx);
}

void xylem_mutex_unlock(xylem_mutex_t* mtx) {
    _spin_lock(&mtx->guard);
    queue_node_t* node = queue_dequeue(&mtx->waiters);
    _spin_unlock(&mtx->guard);

    if (node) {
        _mutex_waiter_t* w = queue_entry(node, _mutex_waiter_t, node);
        scheduler_schedule(runtime_get_scheduler(), w->co);
    } else {
        atomic_store(&mtx->state, 0);
    }
}
```

- [ ] **Step 2: Build and run test-mutex**

Run: `cd out-wsl && ninja test-mutex && ./tests/test-mutex`

Expected: PASS (both ping_pong and concurrent).

- [ ] **Step 3: Commit**

```bash
git add src/sync/xylem-mutex.c
git commit -m "fix(mutex): use scheduler_park to eliminate schedule-before-yield race"
```

---

### Task 4: Refactor xylem_channel to use scheduler_park

**Files:**
- Modify: `src/sync/xylem-channel.c`
- Test: `tests/test-channel.c`

- [ ] **Step 1: Rewrite xylem_channel_recv**

Replace the full file:

```c
#include "xylem/sync/xylem-channel.h"

#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "container/mpsc.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct _channel_msg_s {
    mpsc_node_t node;
    void*       payload;
} _channel_msg_t;

struct xylem_channel_s {
    mpsc_t             queue;
    _Atomic(mco_coro*) wait_coro;
    _Atomic bool       closed;
};

xylem_channel_t* xylem_channel_create(void) {
    xylem_channel_t* ch =
        (xylem_channel_t*)calloc(1, sizeof(xylem_channel_t));
    if (!ch) {
        return NULL;
    }
    mpsc_init(&ch->queue);
    atomic_init(&ch->wait_coro, NULL);
    atomic_init(&ch->closed, false);
    return ch;
}

void xylem_channel_destroy(xylem_channel_t* ch) {
    if (!ch) {
        return;
    }

    atomic_store(&ch->closed, true);

    mco_coro* co = atomic_exchange(&ch->wait_coro, NULL);
    if (co) {
        scheduler_schedule(runtime_get_scheduler(), co);
    }

    mpsc_node_t* node;
    while ((node = mpsc_pop(&ch->queue)) != NULL) {
        _channel_msg_t* msg = mpsc_entry(node, _channel_msg_t, node);
        free(msg);
    }
    free(ch);
}

int xylem_channel_send(xylem_channel_t* ch, void* msg) {
    if (!ch || !msg) {
        return -1;
    }

    _channel_msg_t* m = (_channel_msg_t*)calloc(1, sizeof(_channel_msg_t));
    if (!m) {
        return -1;
    }

    m->payload = msg;
    mpsc_push(&ch->queue, &m->node);

    mco_coro* co = atomic_exchange(&ch->wait_coro, NULL);
    if (co) {
        scheduler_schedule(runtime_get_scheduler(), co);
    }
    return 0;
}

static bool _channel_park_cb(mco_coro* co, void* arg) {
    xylem_channel_t* ch = (xylem_channel_t*)arg;

    atomic_store(&ch->wait_coro, co);

    if (atomic_load(&ch->closed)) {
        atomic_store(&ch->wait_coro, NULL);
        return false;
    }

    mpsc_node_t* node = mpsc_pop(&ch->queue);
    if (node) {
        atomic_store(&ch->wait_coro, NULL);
        mpsc_push(&ch->queue, node);
        return false;
    }

    return true;
}

void* xylem_channel_recv(xylem_channel_t* ch) {
    if (!ch) {
        return NULL;
    }

    for (;;) {
        if (atomic_load(&ch->closed)) {
            return NULL;
        }

        mpsc_node_t* node = mpsc_pop(&ch->queue);
        if (node) {
            _channel_msg_t* m = mpsc_entry(node, _channel_msg_t, node);
            void* payload = m->payload;
            free(m);
            return payload;
        }

        scheduler_park(runtime_get_scheduler(), _channel_park_cb, ch);
    }
}
```

- [ ] **Step 2: Build and run test-channel**

Run: `cd out-wsl && ninja test-channel && ./tests/test-channel`

Expected: PASS (20 rounds).

- [ ] **Step 3: Commit**

```bash
git add src/sync/xylem-channel.c
git commit -m "fix(channel): use scheduler_park to eliminate schedule-before-yield race"
```

---

### Task 5: Refactor xylem_runtime_sleep and xylem_runtime_submit

**Files:**
- Modify: `src/runtime/xylem-runtime.c`
- Test: `tests/test-runtime.c` (existing tests)

- [ ] **Step 1: Refactor xylem_runtime_sleep**

Replace `xylem_runtime_sleep` and its helper `_runtime_sleep_timeout_cb`/`_runtime_sleep_post_cb`:

```c
static void _runtime_sleep_timeout_cb(
    loop_t* loop,
    loop_timer_t* timer,
    void* ud) {
    (void)loop;
    mco_coro* co = (mco_coro*)ud;
    loop_destroy_timer(timer);
    scheduler_schedule(g_sched, co);
}

static void _runtime_sleep_post_cb(
    loop_t* loop,
    loop_post_t* req,
    void* ud) {
    (void)loop;
    (void)req;
    _sleep_ctx_t* ctx = (_sleep_ctx_t*)ud;
    loop_timer_t* timer = loop_create_timer(g_loop);
    loop_start_timer(
        timer, _runtime_sleep_timeout_cb, ctx->co, ctx->timeout_ms, 0);
    free(ctx);
}

static bool _sleep_park_cb(mco_coro* co, void* arg) {
    uint64_t ms = *(uint64_t*)arg;
    _sleep_ctx_t* ctx = (_sleep_ctx_t*)malloc(sizeof(_sleep_ctx_t));
    if (!ctx) {
        return false;
    }
    ctx->co = co;
    ctx->timeout_ms = ms;
    loop_post(g_loop, _runtime_sleep_post_cb, ctx);
    return true;
}

void xylem_runtime_sleep(uint64_t ms) {
    scheduler_park(g_sched, _sleep_park_cb, &ms);
}
```

- [ ] **Step 2: Refactor xylem_runtime_submit**

Replace `xylem_runtime_submit`:

```c
typedef struct {
    _submit_ctx_t* ctx;
    bool           ok;
} _submit_park_arg_t;

static bool _submit_park_cb(mco_coro* co, void* arg) {
    _submit_park_arg_t* pa = (_submit_park_arg_t*)arg;
    pa->ctx->co = co;
    if (dynpool_submit(g_dynpool, _runtime_submit_worker, pa->ctx) != 0) {
        pa->ok = false;
        return false;
    }
    pa->ok = true;
    return true;
}

int xylem_runtime_submit(void (*fn)(void*), void* arg) {
    _submit_ctx_t* ctx = (_submit_ctx_t*)malloc(sizeof(_submit_ctx_t));
    if (!ctx) {
        return -1;
    }
    ctx->fn    = fn;
    ctx->arg   = arg;
    ctx->sched = g_sched;

    _submit_park_arg_t pa = { .ctx = ctx, .ok = false };
    scheduler_park(g_sched, _submit_park_cb, &pa);

    if (!pa.ok) {
        free(ctx);
        return -1;
    }
    return 0;
}
```

- [ ] **Step 3: Build and run test-runtime**

Run: `cd out-wsl && ninja test-runtime && ./tests/test-runtime`

Expected: All 10 tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/runtime/xylem-runtime.c
git commit -m "fix(runtime): use scheduler_park in sleep/submit for race-free suspension"
```

---

### Task 6: Refactor addr.c to use scheduler_park

**Files:**
- Modify: `src/net/addr.c`

- [ ] **Step 1: Read the current resolve implementation context**

The current code:
```c
ctx.co = mco_running();
dynpool_submit(runtime_get_dynpool(), _addr_resolve_work, &ctx);
mco_yield(mco_running());
```

- [ ] **Step 2: Refactor to use scheduler_park**

Replace with:

```c
static bool _addr_park_cb(mco_coro* co, void* arg) {
    _addr_resolve_ctx_t* ctx = (_addr_resolve_ctx_t*)arg;
    ctx->co = co;
    dynpool_submit(runtime_get_dynpool(), _addr_resolve_work, ctx);
    return true;
}
```

And in the resolve function, replace:
```c
ctx.co = mco_running();
dynpool_submit(runtime_get_dynpool(), _addr_resolve_work, &ctx);
mco_yield(mco_running());
```

With:
```c
scheduler_park(runtime_get_scheduler(), _addr_park_cb, &ctx);
```

Add `#include "runtime/scheduler.h"` if not already present, and remove `#include "minicoro/minicoro.h"` if no longer needed.

- [ ] **Step 3: Build**

Run: `cd out-wsl && ninja`

Expected: Clean build.

- [ ] **Step 4: Commit**

```bash
git add src/net/addr.c
git commit -m "fix(addr): use scheduler_park in DNS resolve for race-free suspension"
```

---

### Task 7: Full test suite verification

**Files:** None (verification only)

- [ ] **Step 1: Build all targets**

Run: `cd out-wsl && ninja`

Expected: Clean build, no warnings.

- [ ] **Step 2: Run all tests**

```bash
./tests/test-runtime
./tests/test-waitgroup
./tests/test-channel
./tests/test-mutex
```

Expected: All pass.

- [ ] **Step 3: Build with TSAN and run**

```bash
cd /path/to/Xylem
cmake -B out-tsan -DCMAKE_BUILD_TYPE=Debug -DXYLEM_SANITIZER=thread -G Ninja
cmake --build out-tsan
./out-tsan/tests/test-waitgroup
./out-tsan/tests/test-channel
./out-tsan/tests/test-mutex
./out-tsan/tests/test-runtime
```

Expected: No TSAN data-race warnings from sync primitives.

---

## Design Notes

### Why TLS park slot, not per-coroutine state?

Per-coroutine state would require extending minicoro (vendored). A TLS slot is simpler:
- `scheduler_park` sets `_tls_worker->park_fn/park_arg`, then yields.
- The worker loop (same thread, immediately after `mco_resume` returns) reads the slot.
- No atomics needed — single-thread access only.

### Normal yield semantics preserved

The current worker loop does nothing when a coroutine yields (no re-schedule). After this change, that behavior is unchanged: `park_fn == NULL` → do nothing. This is correct because every existing `mco_yield` call is preceded by registering a wakeup via some other mechanism (timer, dynpool, etc.).

### `bool` return: double-check pattern

The callback can return `false` to cancel the park. This is essential for the "double-check after store" pattern:
- Waitgroup: check `cnt == 0` after storing pointer
- Mutex: try CAS(0→1) after spin_lock
- Channel: check queue non-empty or closed after storing pointer

If the condition changed between the caller's check and the park, the callback cancels, and the coroutine resumes immediately with no observable effect.

### Future work: iowait.c

`iowait.c` has the same race pattern but uses a CAS state machine (`IOWAIT_IDLE → IOWAIT_WAITING`) that partially mitigates it. A full fix with `scheduler_park` requires restructuring its state machine. This is tracked as follow-up work.
