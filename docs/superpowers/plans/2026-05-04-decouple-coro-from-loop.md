# Decouple Coroutine Runtime from Loop

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the coroutine runtime's dependency on `loop_t`, so that the scheduler can manage timers and deferred callbacks internally. The `loop_t` module remains available for legacy callback-based networking modules (UDS, UDP, DTLS, RUDP, WS, HTTP) until they are individually migrated to coroutines.

**Architecture:** Add a timer heap + MPSC post queue directly into the scheduler. The polling worker (the one that wins the `sched->polling` CAS) processes expired timers after each `platform_poller_wait`. Deferred callbacks are drained in the same spot. `iowait` stops touching `loop_t` entirely — timeouts use the new scheduler timers. `xylem_runtime_start` blocks the main thread on a semaphore instead of `loop_run()`. The `loop_t` is still created for legacy modules but no longer runs on the main thread in the default coroutine path.

**Tech Stack:** C11, minicoro, platform abstractions (poller, sem, socket), intrusive heap, MPSC queue.

---

## Current State Analysis

### Coroutine path dependencies on `loop_t` (what we're removing):

| Caller | What it posts to loop | Why |
|---|---|---|
| `iowait_read/write` | `_iowait_timeout_start_cb` | Create timer for I/O timeout |
| `iowait_read/write` | `_iowait_rd/wr_timeout_stop_cb` | Stop timer after I/O completes |
| `iowait_destroy` | `_iowait_destroy_timer_cb` | Destroy timers (loop-thread owned) |
| `xylem_runtime_sleep` | `_runtime_sleep_post_cb` | Create one-shot timer |
| `xylem_tcp_close` | `_tcp_conn_destroy_cb` | Deferred free of connection |
| `xylem_runtime_start` | `loop_run()` blocks main thread | Main thread parking |
| `xylem_runtime_stop` | `loop_stop()` | Unblock main thread |

### What stays on `loop_t` (legacy callback modules):

UDS, UDP, DTLS, RUDP, WS, HTTP — these use `loop_io_t` and `loop_create_timer` directly. They keep working unchanged. `runtime_get_loop()` remains available for them.

---

## File Map

| File | Action | Responsibility |
|---|---|---|
| `src/runtime/sched-timer.h` | **Create** | Scheduler timer type + API (create/start/stop/destroy) |
| `src/runtime/sched-timer.c` | **Create** | Timer heap implementation, protected by mutex, with process function |
| `src/runtime/scheduler.h` | **Modify** | Add `scheduler_timer_*` and `scheduler_post` public API |
| `src/runtime/scheduler.c` | **Modify** | Embed timer heap + MPSC post queue, process in worker poll path |
| `src/runtime/iowait.h` | **Modify** | Remove `loop_t` parameter from `iowait_create` |
| `src/runtime/iowait.c` | **Modify** | Replace all `loop_post`/`loop_timer` with `scheduler_timer`/`scheduler_post` |
| `src/runtime/xylem-runtime.c` | **Modify** | Replace `loop_run` blocking with semaphore; rewrite sleep to use scheduler timers |
| `src/runtime/runtime.h` | **Modify** | Keep `runtime_get_loop` for legacy; no changes needed |
| `src/net/xylem-tcp.c` | **Modify** | `iowait_create` signature change; replace `loop_post` in close with `scheduler_post` |
| `src/net/xylem-tls.c` | **No change** | Still callback-based, uses loop directly |
| `tests/test-tcp.c` | **Modify** | Safety timers switch to scheduler timers |

---

## Task 1: Add scheduler timer subsystem (`sched-timer.h` / `sched-timer.c`)

**Files:**
- Create: `src/runtime/sched-timer.h`
- Create: `src/runtime/sched-timer.c`

The scheduler needs its own timer heap. Unlike `loop_timer_t` (single-thread, no locking), this one must be safe to call from any worker thread. We use a mutex to protect the heap since timer operations are infrequent (start/stop/fire) and the critical section is tiny (heap insert/remove).

Design:
- `sched_timer_t` — opaque timer handle, embeds a `heap_node_t`
- Timer heap + mutex live in a `sched_timer_mgr_t` struct that the scheduler owns
- `sched_timer_mgr_process(mgr, now_ms)` — called by the polling worker to fire expired timers. Returns the next timeout in ms (for poll wait).
- Timer callbacks receive `(sched_timer_t* timer, void* ud)` — no loop pointer.

- [ ] **Step 1: Create `src/runtime/sched-timer.h`**

```c
/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *  ... (standard license header)
 */

_Pragma("once")

#include <stdint.h>

typedef struct sched_timer_s     sched_timer_t;
typedef struct sched_timer_mgr_s sched_timer_mgr_t;

/**
 * @brief Timer expiry callback.
 *
 * Called on a worker thread when the timer fires.
 *
 * @param timer  The timer that fired.
 * @param ud     User data from sched_timer_start.
 */
typedef void (*sched_timer_fn_t)(sched_timer_t* timer, void* ud);

/**
 * @brief Create a timer manager.
 *
 * @return Manager handle, or NULL on failure.
 */
extern sched_timer_mgr_t* sched_timer_mgr_create(void);

/**
 * @brief Destroy a timer manager.
 *
 * All timers must be stopped/destroyed before calling this.
 *
 * @param mgr  Manager handle.
 */
extern void sched_timer_mgr_destroy(sched_timer_mgr_t* mgr);

/**
 * @brief Process expired timers.
 *
 * Fires callbacks for all timers whose deadline <= now_ms.
 * Must be called periodically (e.g. after each poll wait).
 *
 * @param mgr     Manager handle.
 * @param now_ms  Current time in milliseconds.
 *
 * @return Next timeout in ms until the earliest timer, or -1 if none.
 */
extern int sched_timer_mgr_process(sched_timer_mgr_t* mgr, uint64_t now_ms);

/**
 * @brief Get the timeout until the next timer fires.
 *
 * @param mgr     Manager handle.
 * @param now_ms  Current time in milliseconds.
 *
 * @return Timeout in ms, or -1 if no timers are active.
 */
extern int sched_timer_mgr_next_timeout(sched_timer_mgr_t* mgr, uint64_t now_ms);

/**
 * @brief Create a timer.
 *
 * @param mgr  Manager that owns this timer.
 *
 * @return Timer handle, or NULL on failure.
 */
extern sched_timer_t* sched_timer_create(sched_timer_mgr_t* mgr);

/**
 * @brief Destroy a timer. Stops it first if active.
 *
 * @param timer  Timer handle.
 */
extern void sched_timer_destroy(sched_timer_t* timer);

/**
 * @brief Start or restart a timer.
 *
 * Thread-safe.
 *
 * @param timer       Timer handle.
 * @param cb          Callback to invoke on expiry.
 * @param ud          User data for callback.
 * @param timeout_ms  Delay in milliseconds.
 * @param repeat_ms   Repeat interval, 0 for one-shot.
 */
extern void sched_timer_start(
    sched_timer_t*  timer,
    sched_timer_fn_t cb,
    void*           ud,
    uint64_t        timeout_ms,
    uint64_t        repeat_ms);

/**
 * @brief Stop a running timer.
 *
 * Thread-safe. No-op if already stopped.
 *
 * @param timer  Timer handle.
 */
extern void sched_timer_stop(sched_timer_t* timer);
```

- [ ] **Step 2: Create `src/runtime/sched-timer.c`**

```c
/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *  ... (standard license header)
 */

#include "sched-timer.h"
#include "container/heap.h"
#include "xylem/xylem-utils.h"

#include <stdlib.h>
#include <stdint.h>
#include <threads.h>

struct sched_timer_s {
    heap_node_t       heap_node;
    sched_timer_mgr_t* mgr;
    sched_timer_fn_t  cb;
    void*             ud;
    uint64_t          timeout;
    uint64_t          repeat;
    bool              active;
};

struct sched_timer_mgr_s {
    heap_t  timers;
    mtx_t   lock;
};

static int _timer_cmp(const heap_node_t* a, const heap_node_t* b) {
    const sched_timer_t* ta = heap_entry(a, sched_timer_t, heap_node);
    const sched_timer_t* tb = heap_entry(b, sched_timer_t, heap_node);
    if (ta->timeout < tb->timeout) return -1;
    if (ta->timeout > tb->timeout) return 1;
    return 0;
}

sched_timer_mgr_t* sched_timer_mgr_create(void) {
    sched_timer_mgr_t* mgr = (sched_timer_mgr_t*)calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;
    heap_init(&mgr->timers, _timer_cmp);
    mtx_init(&mgr->lock, mtx_plain);
    return mgr;
}

void sched_timer_mgr_destroy(sched_timer_mgr_t* mgr) {
    if (!mgr) return;
    mtx_destroy(&mgr->lock);
    free(mgr);
}

int sched_timer_mgr_process(sched_timer_mgr_t* mgr, uint64_t now_ms) {
    /* Collect expired timers under lock, fire callbacks outside lock. */
    for (;;) {
        sched_timer_t* timer = NULL;

        mtx_lock(&mgr->lock);
        heap_node_t* root = heap_peek(&mgr->timers);
        if (root) {
            sched_timer_t* t = heap_entry(root, sched_timer_t, heap_node);
            if (t->timeout <= now_ms) {
                heap_dequeue(&mgr->timers);
                if (t->repeat > 0) {
                    t->timeout = now_ms + t->repeat;
                    heap_insert(&mgr->timers, &t->heap_node);
                } else {
                    t->active = false;
                }
                timer = t;
            }
        }
        mtx_unlock(&mgr->lock);

        if (!timer) break;
        timer->cb(timer, timer->ud);
    }

    return sched_timer_mgr_next_timeout(mgr, now_ms);
}

int sched_timer_mgr_next_timeout(sched_timer_mgr_t* mgr, uint64_t now_ms) {
    mtx_lock(&mgr->lock);
    heap_node_t* root = heap_peek(&mgr->timers);
    if (!root) {
        mtx_unlock(&mgr->lock);
        return -1;
    }
    sched_timer_t* t = heap_entry(root, sched_timer_t, heap_node);
    int timeout;
    if (t->timeout <= now_ms) {
        timeout = 0;
    } else {
        uint64_t diff = t->timeout - now_ms;
        timeout = (diff > INT32_MAX) ? INT32_MAX : (int)diff;
    }
    mtx_unlock(&mgr->lock);
    return timeout;
}

sched_timer_t* sched_timer_create(sched_timer_mgr_t* mgr) {
    sched_timer_t* t = (sched_timer_t*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->mgr = mgr;
    return t;
}

void sched_timer_destroy(sched_timer_t* timer) {
    if (!timer) return;
    if (timer->active) {
        sched_timer_stop(timer);
    }
    free(timer);
}

void sched_timer_start(
    sched_timer_t*   timer,
    sched_timer_fn_t cb,
    void*            ud,
    uint64_t         timeout_ms,
    uint64_t         repeat_ms) {
    sched_timer_mgr_t* mgr = timer->mgr;
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    mtx_lock(&mgr->lock);
    if (timer->active) {
        heap_remove(&mgr->timers, &timer->heap_node);
    }
    timer->cb      = cb;
    timer->ud      = ud;
    timer->timeout = now + timeout_ms;
    timer->repeat  = repeat_ms;
    timer->active  = true;
    heap_insert(&mgr->timers, &timer->heap_node);
    mtx_unlock(&mgr->lock);
}

void sched_timer_stop(sched_timer_t* timer) {
    if (!timer->active) return;
    sched_timer_mgr_t* mgr = timer->mgr;
    mtx_lock(&mgr->lock);
    if (timer->active) {
        heap_remove(&mgr->timers, &timer->heap_node);
        timer->active = false;
    }
    mtx_unlock(&mgr->lock);
}
```

- [ ] **Step 3: Add to CMake build**

Add `src/runtime/sched-timer.c` to the xylem library sources in the appropriate CMakeLists.txt.

- [ ] **Step 4: Build and verify compilation**

Run: `cmake --build build`
Expected: Clean compile, no errors.

- [ ] **Step 5: Commit**

```bash
git add src/runtime/sched-timer.h src/runtime/sched-timer.c
git commit -m "feat(runtime): add scheduler timer subsystem (sched-timer)"
```

---

## Task 2: Add post queue and timer manager to scheduler

**Files:**
- Modify: `src/runtime/scheduler.h`
- Modify: `src/runtime/scheduler.c`

Embed a `sched_timer_mgr_t*` and an MPSC post queue into the scheduler. The polling worker processes timers and posts after each `platform_poller_wait`. Add `scheduler_post()` as the cross-thread callback mechanism (replaces `loop_post` for coroutine code). Add `scheduler_timer_create/start/stop/destroy` as convenience wrappers.

- [ ] **Step 1: Add new declarations to `src/runtime/scheduler.h`**

Add after the existing `scheduler_set_poller` declaration:

```c
#include "sched-timer.h"

typedef void (*scheduler_post_fn_t)(void* ud);

/**
 * @brief Post a deferred callback to the scheduler.
 *
 * Thread-safe. The callback will be invoked on a worker thread
 * during the next poll/timer processing pass.
 *
 * @param sched  Scheduler handle.
 * @param cb     Callback function.
 * @param ud     User data.
 *
 * @return 0 on success, -1 on failure.
 */
extern int scheduler_post(scheduler_t* sched, scheduler_post_fn_t cb, void* ud);

/**
 * @brief Get the scheduler's timer manager.
 *
 * @param sched  Scheduler handle.
 *
 * @return Timer manager handle.
 */
extern sched_timer_mgr_t* scheduler_get_timer_mgr(scheduler_t* sched);
```

- [ ] **Step 2: Add post queue node type and timer manager to `scheduler_s` in `scheduler.c`**

Add to the struct and includes:

```c
#include "sched-timer.h"
#include "container/mpsc.h"

/* Add inside struct scheduler_s: */
    sched_timer_mgr_t*   timer_mgr;
    mpsc_t               posts;
```

Add the post node type:

```c
typedef struct {
    mpsc_node_t          node;
    scheduler_post_fn_t  cb;
    void*                ud;
} _sched_post_t;
```

- [ ] **Step 3: Initialize timer_mgr and posts in `scheduler_create`**

After `runq_create`:

```c
    sched->timer_mgr = sched_timer_mgr_create();
    if (!sched->timer_mgr) {
        _sched_teardown(sched, 0);
        return NULL;
    }
    mpsc_init(&sched->posts);
```

- [ ] **Step 4: Destroy timer_mgr in `_sched_teardown`**

```c
    if (sched->timer_mgr) {
        sched_timer_mgr_destroy(sched->timer_mgr);
    }
```

- [ ] **Step 5: Add `_sched_process_timers_and_posts` helper**

```c
static void _sched_process_timers_and_posts(scheduler_t* sched) {
    /* Drain post queue. */
    mpsc_node_t* node;
    while ((node = mpsc_pop(&sched->posts)) != NULL) {
        _sched_post_t* req = mpsc_entry(node, _sched_post_t, node);
        req->cb(req->ud);
        free(req);
    }

    /* Process expired timers. */
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    sched_timer_mgr_process(sched->timer_mgr, now);
}
```

- [ ] **Step 6: Call from the polling worker path in `_sched_worker_entry`**

In the polling branch, after `_sched_process_poll_events`, add:

```c
                if (n > 0) {
                    _sched_process_poll_events(sched, cqes, n);
                }
                _sched_process_timers_and_posts(sched);
                continue;
```

Also adjust the poll timeout to respect the timer deadline:

```c
            if (atomic_compare_exchange_strong(
                    &sched->polling, &expected, true)) {
                atomic_store(&w->is_polling, true);
                uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
                int timer_timeout = sched_timer_mgr_next_timeout(
                    sched->timer_mgr, now);
                int poll_ms = SCHED_POLL_TIMEOUT_MS;
                if (timer_timeout >= 0 && timer_timeout < poll_ms) {
                    poll_ms = timer_timeout;
                }
                int n = platform_poller_wait(
                    sched->poller, cqes, poll_ms);
                /* ... rest unchanged ... */
```

- [ ] **Step 7: Implement `scheduler_post` and `scheduler_get_timer_mgr`**

```c
int scheduler_post(scheduler_t* sched, scheduler_post_fn_t cb, void* ud) {
    _sched_post_t* req = (_sched_post_t*)calloc(1, sizeof(*req));
    if (!req) return -1;
    req->cb = cb;
    req->ud = ud;
    mpsc_push(&sched->posts, &req->node);

    /* Wake a worker if all are sleeping. */
    _sched_notify_worker(sched);
    return 0;
}

sched_timer_mgr_t* scheduler_get_timer_mgr(scheduler_t* sched) {
    return sched->timer_mgr;
}
```

- [ ] **Step 8: Build and verify**

Run: `cmake --build build`
Expected: Clean compile.

- [ ] **Step 9: Commit**

```bash
git add src/runtime/scheduler.h src/runtime/scheduler.c
git commit -m "feat(runtime): embed timer manager and post queue in scheduler"
```

---

## Task 3: Rewrite `iowait` to use scheduler timers instead of loop

**Files:**
- Modify: `src/runtime/iowait.h`
- Modify: `src/runtime/iowait.c`

Remove all `loop_t` dependencies from iowait. Timeouts use `sched_timer_t` directly (thread-safe, no need to post to loop). The `iowait_create` signature changes: `loop_t* loop` parameter is removed.

- [ ] **Step 1: Update `iowait.h` — remove `loop.h` include, change `iowait_create` signature**

Before:
```c
#include "runtime/loop.h"
...
extern iowait_t* iowait_create(loop_t* loop, platform_sock_t fd);
```

After:
```c
#include "platform/platform-socket.h"
...
/**
 * @brief Create an IO wait handle bound to a file descriptor.
 *
 * @param fd  Non-blocking socket descriptor.
 *
 * @return IO wait handle, or NULL on failure.
 */
extern iowait_t* iowait_create(platform_sock_t fd);
```

- [ ] **Step 2: Rewrite `iowait.c` internals**

Replace the `iowait_s` struct — remove `loop`, `loop_timer_t*`, add `sched_timer_t*`:

```c
#include "sched-timer.h"
#include "runtime.h"
/* Remove: #include "runtime/loop.h" if present as separate include */

struct iowait_s {
    platform_poller_sq_t* poller;
    platform_poller_sqe_t sqe;
    sched_timer_t*        rd_timer;
    sched_timer_t*        wr_timer;
    platform_sock_t       fd;
    mco_coro*             rd_coro;
    mco_coro*             wr_coro;
    _Atomic int           rd_state;
    _Atomic int           wr_state;
    bool                  rd_timed_out;
    bool                  wr_timed_out;
    bool                  registered;
    bool                  closed;
};
```

- [ ] **Step 3: Rewrite timeout callbacks — they are now `sched_timer_fn_t`**

Before (loop timer callback signature):
```c
static void _iowait_rd_timeout_cb(loop_t* loop, loop_timer_t* timer, void* ud) {
```

After:
```c
static void _iowait_rd_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    iowait_t* w = (iowait_t*)ud;
    mco_coro* co = _iowait_wake(&w->rd_state, &w->rd_coro);
    if (co) {
        w->rd_timed_out = true;
        scheduler_schedule(runtime_get_scheduler(), co);
    }
}

static void _iowait_wr_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    iowait_t* w = (iowait_t*)ud;
    mco_coro* co = _iowait_wake(&w->wr_state, &w->wr_coro);
    if (co) {
        w->wr_timed_out = true;
        scheduler_schedule(runtime_get_scheduler(), co);
    }
}
```

- [ ] **Step 4: Rewrite `iowait_read` — direct timer start, no loop_post**

Before:
```c
    if (timeout_ms > 0) {
        _iowait_timeout_ctx_t* ctx = ...;
        loop_post(w->loop, _iowait_timeout_start_cb, ctx);
    }
    ...
    if (timeout_ms > 0) {
        loop_post(w->loop, _iowait_rd_timeout_stop_cb, w);
    }
```

After:
```c
    if (timeout_ms > 0) {
        if (!w->rd_timer) {
            w->rd_timer = sched_timer_create(
                scheduler_get_timer_mgr(runtime_get_scheduler()));
        }
        sched_timer_start(w->rd_timer, _iowait_rd_timeout_cb,
                          w, timeout_ms, 0);
    }
    ...
    if (timeout_ms > 0 && w->rd_timer) {
        sched_timer_stop(w->rd_timer);
    }
```

Same pattern for `iowait_write` with `wr_timer` / `_iowait_wr_timeout_cb`.

- [ ] **Step 5: Rewrite `iowait_create` — remove loop parameter**

```c
iowait_t* iowait_create(platform_sock_t fd) {
    iowait_t* w = (iowait_t*)calloc(1, sizeof(iowait_t));
    if (!w) return NULL;

    w->poller = runtime_get_poller();
    w->fd     = fd;

    w->sqe.fd      = (platform_poller_fd_t)fd;
    w->sqe.ud      = w;
    w->sqe.oneshot  = 1;
    w->sqe.op      = PLATFORM_POLLER_NO_OP;

    return w;
}
```

- [ ] **Step 6: Rewrite `iowait_destroy` — no loop_post needed**

```c
void iowait_destroy(iowait_t* w) {
    if (!w->closed) {
        iowait_close(w);
    }

    if (w->registered) {
        platform_poller_del(w->poller, &w->sqe);
        w->registered = false;
    }

    if (w->rd_timer) {
        sched_timer_destroy(w->rd_timer);
    }
    if (w->wr_timer) {
        sched_timer_destroy(w->wr_timer);
    }
    free(w);
}
```

- [ ] **Step 7: Remove all `_iowait_timeout_ctx_t`, `_iowait_timeout_start_cb`, `_iowait_rd/wr_timeout_stop_cb` functions**

These are no longer needed — timer start/stop is now direct.

- [ ] **Step 8: Build and verify**

Run: `cmake --build build`
Expected: Compile errors in callers of `iowait_create` (TCP module) — expected, fixed in Task 4.

- [ ] **Step 9: Commit**

```bash
git add src/runtime/iowait.h src/runtime/iowait.c
git commit -m "refactor(runtime): iowait uses scheduler timers, no loop dependency"
```

---

## Task 4: Update TCP module and runtime for new iowait signature

**Files:**
- Modify: `src/net/xylem-tcp.c`
- Modify: `src/runtime/xylem-runtime.c`
- Modify: `src/runtime/runtime.h`

- [ ] **Step 1: Update `xylem-tcp.c` — `iowait_create` calls**

Before:
```c
    tcp->waiter = iowait_create(loop, fd);
```

After:
```c
    tcp->waiter = iowait_create(fd);
```

Two call sites: `_tcp_conn_alloc` and `xylem_tcp_listen`.

- [ ] **Step 2: Update `xylem_tcp_close` — use `scheduler_post` instead of `loop_post`**

Before:
```c
void xylem_tcp_close(xylem_tcp_conn_t* tcp) {
    if (tcp->closed) return;
    tcp->closed = true;
    loop_post(tcp->loop, _tcp_conn_destroy_cb, tcp);
}
```

After:
```c
static void _tcp_conn_destroy_post_cb(void* ud) {
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;
    iowait_close(tcp->waiter);
    iowait_destroy(tcp->waiter);
    free(tcp->read_buf);
    shutdown(tcp->fd, PLATFORM_SHUT_WR);
    platform_socket_close(tcp->fd);
    free(tcp);
}

void xylem_tcp_close(xylem_tcp_conn_t* tcp) {
    if (tcp->closed) return;
    tcp->closed = true;
    scheduler_post(runtime_get_scheduler(), _tcp_conn_destroy_post_cb, tcp);
}
```

Remove the old `_tcp_conn_destroy_cb` (which had `loop_t*` and `loop_post_t*` params).

- [ ] **Step 3: Remove `loop` field from `xylem_tcp_conn_s` if no longer needed**

Check if `tcp->loop` is used anywhere else in xylem-tcp.c. If only used for `loop_post` and `iowait_create`, remove it. The `_tcp_conn_alloc` function no longer needs the `loop` parameter either — but it's still passed from `xylem_tcp_listen` and `xylem_tcp_dial` which receive it from `runtime_get_loop()`. Keep the `loop` field for now since the listener still stores it and legacy modules may need it. Alternatively, if `tcp->loop` is only used in the two places we just changed, remove it.

- [ ] **Step 4: Update `xylem_runtime_sleep` — use scheduler timers**

Before:
```c
static void _runtime_sleep_post_cb(loop_t* loop, loop_post_t* req, void* ud) {
    (void)loop; (void)req;
    _sleep_ctx_t* ctx = (_sleep_ctx_t*)ud;
    loop_timer_t* timer = loop_create_timer(g_loop);
    loop_start_timer(timer, _runtime_sleep_timeout_cb, ctx, ctx->timeout_ms, 0);
}

void xylem_runtime_sleep(uint64_t ms) {
    _sleep_ctx_t* ctx = ...;
    ctx->co = mco_running();
    ctx->timeout_ms = ms;
    loop_post(g_loop, _runtime_sleep_post_cb, ctx);
    mco_yield(mco_running());
}
```

After:
```c
static void _runtime_sleep_timeout_cb(sched_timer_t* timer, void* ud) {
    mco_coro* co = (mco_coro*)ud;
    sched_timer_destroy(timer);
    scheduler_schedule(g_sched, co);
}

void xylem_runtime_sleep(uint64_t ms) {
    sched_timer_mgr_t* mgr = scheduler_get_timer_mgr(g_sched);
    sched_timer_t* timer = sched_timer_create(mgr);
    if (!timer) return;
    sched_timer_start(timer, _runtime_sleep_timeout_cb,
                      mco_running(), ms, 0);
    mco_yield(mco_running());
}
```

Remove `_sleep_ctx_t`, `_runtime_sleep_post_cb`, old `_runtime_sleep_timeout_cb`.

- [ ] **Step 5: Update `xylem_runtime_start` — block main thread with semaphore instead of `loop_run`**

Before:
```c
void xylem_runtime_start(...) {
    g_loop = loop_create();
    platform_poller_init(&g_poller);
    g_sched = scheduler_create(&sched_opts);
    scheduler_set_poller(g_sched, &g_poller, iowait_on_event);
    g_dynpool = dynpool_create(NULL);
    scheduler_spawn(g_sched, main_fn, arg);
    loop_run(g_loop);          /* <-- blocks here */
    scheduler_destroy(g_sched);
    ...
}
```

After:
```c
#include "platform/platform-sem.h"

static platform_sem_t* g_stop_sem;

void xylem_runtime_start(...) {
    g_loop = loop_create();    /* keep for legacy modules */
    platform_poller_init(&g_poller);
    g_sched = scheduler_create(&sched_opts);
    scheduler_set_poller(g_sched, &g_poller, iowait_on_event);
    g_dynpool = dynpool_create(NULL);
    g_stop_sem = platform_sem_create(0);

    scheduler_spawn(g_sched, main_fn, arg);

    /* Block main thread until xylem_runtime_stop() is called. */
    platform_sem_wait(g_stop_sem);

    scheduler_shutdown(g_sched);
    scheduler_destroy(g_sched);
    dynpool_destroy(g_dynpool);
    platform_poller_deinit(&g_poller);
    loop_destroy(g_loop);
    platform_sem_destroy(g_stop_sem);
    g_stop_sem = NULL;
}
```

- [ ] **Step 6: Update `xylem_runtime_stop`**

Before:
```c
void xylem_runtime_stop(void) {
    scheduler_shutdown(g_sched);
    loop_stop(g_loop);
}
```

After:
```c
void xylem_runtime_stop(void) {
    if (g_stop_sem) {
        platform_sem_post(g_stop_sem);
    }
}
```

Note: `scheduler_shutdown` is now called in `xylem_runtime_start` after the sem_wait returns, ensuring proper ordering.

- [ ] **Step 7: Build and verify**

Run: `cmake --build build`
Expected: Clean compile.

- [ ] **Step 8: Run TCP tests**

Run: `ctest --test-dir build -R test-tcp -V`
Expected: All TCP tests pass.

- [ ] **Step 9: Commit**

```bash
git add src/net/xylem-tcp.c src/runtime/xylem-runtime.c src/runtime/runtime.h
git commit -m "refactor(runtime): TCP and runtime use scheduler timers, main thread blocks on semaphore"
```

---

## Task 5: Update test safety timers

**Files:**
- Modify: `tests/test-tcp.c`
- Modify: `tests/test-waitgroup.c`
- Modify: `tests/test-mutex.c`
- Modify: `tests/test-channel.c`

Tests that use the coroutine runtime (not the callback-based modules) have safety timers that currently do `loop_post` + `loop_create_timer`. These need to switch to scheduler timers.

Note: Tests for UDS, UDP, RUDP, DTLS, TLS use the loop directly for their callback-based modules — those tests stay unchanged.

- [ ] **Step 1: Update `test-tcp.c` safety timer**

Before:
```c
static void _safety_timeout_cb(loop_t* loop, loop_timer_t* timer, void* ud) {
    (void)loop; (void)ud;
    loop_stop_timer(timer);
    loop_destroy_timer(timer);
    xylem_runtime_stop();
    ASSERT(0 && "test timed out");
}

static void _safety_timer_post_cb(loop_t* loop, loop_post_t* req, void* ud) {
    (void)req; (void)ud;
    loop_timer_t* t = loop_create_timer(loop);
    loop_start_timer(t, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);
}

static void _start_safety_timer(void) {
    loop_post(runtime_get_loop(), _safety_timer_post_cb, NULL);
}
```

After:
```c
static void _safety_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)ud;
    sched_timer_destroy(timer);
    xylem_runtime_stop();
    ASSERT(0 && "test timed out");
}

static void _start_safety_timer(void) {
    sched_timer_mgr_t* mgr = scheduler_get_timer_mgr(runtime_get_scheduler());
    sched_timer_t* t = sched_timer_create(mgr);
    sched_timer_start(t, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);
}
```

Apply the same pattern to `test-waitgroup.c`, `test-mutex.c`, `test-channel.c`.

- [ ] **Step 2: Build and run all coroutine-based tests**

Run: `cmake --build build && ctest --test-dir build -R "test-tcp|test-waitgroup|test-mutex|test-channel" -V`
Expected: All pass.

- [ ] **Step 3: Run the full test suite to verify no regressions in callback-based modules**

Run: `ctest --test-dir build -V`
Expected: All tests pass (callback-based modules still use loop unchanged).

- [ ] **Step 4: Commit**

```bash
git add tests/test-tcp.c tests/test-waitgroup.c tests/test-mutex.c tests/test-channel.c
git commit -m "refactor(tests): coroutine tests use scheduler timers"
```

---

## Task 6: Start loop thread for legacy modules (conditional)

**Files:**
- Modify: `src/runtime/xylem-runtime.c`

The legacy callback-based modules (UDS, UDP, etc.) still need `loop_run()` on a thread. Instead of blocking the main thread with it, spawn a dedicated loop thread that runs only when legacy modules are active.

- [ ] **Step 1: Add a loop runner thread**

```c
#include "c11-threads.h"

static thrd_t g_loop_thread;
static bool   g_loop_running;

static int _loop_thread_entry(void* arg) {
    (void)arg;
    loop_run(g_loop);
    return 0;
}
```

- [ ] **Step 2: Start the loop thread in `xylem_runtime_start`**

After creating the loop:
```c
    g_loop = loop_create();
    g_loop_running = true;
    thrd_create(&g_loop_thread, _loop_thread_entry, NULL);
```

- [ ] **Step 3: Stop the loop thread in `xylem_runtime_start` cleanup**

After `platform_sem_wait(g_stop_sem)`:
```c
    scheduler_shutdown(g_sched);
    if (g_loop_running) {
        loop_stop(g_loop);
        thrd_join(g_loop_thread, NULL);
        g_loop_running = false;
    }
```

- [ ] **Step 4: Build and run full test suite**

Run: `cmake --build build && ctest --test-dir build -V`
Expected: All tests pass — both coroutine and callback-based.

- [ ] **Step 5: Commit**

```bash
git add src/runtime/xylem-runtime.c
git commit -m "refactor(runtime): loop runs on dedicated thread for legacy modules"
```

---

## Summary of Architecture After This Change

```
Before:
  Main thread:  loop_run() [blocks, handles timers + posts + I/O for legacy]
  Workers:      coroutines + netpoll

After:
  Main thread:  sem_wait() [blocks until stop]
  Loop thread:  loop_run() [only for legacy callback modules]
  Workers:      coroutines + netpoll + scheduler timers + scheduler posts
```

When all modules are eventually coroutine-ized, the loop thread and `loop_t` can be deleted entirely — just remove the loop thread creation and `runtime_get_loop()`.
