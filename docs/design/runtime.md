# Runtime Design

The runtime is the engine behind Xylem's networking stack. It lets code be
written in a straight-line, blocking style while multiplexing thousands of
connections over a small pool of OS threads. This document describes the
scheduler, coroutine model, I/O parking, timers, and the blocking-task pool.

Sources: `src/runtime/` — `runtime.c`, `scheduler.{h,c}`, `iowait.{h,c}`,
`wsq.{h,c}`, `runq.{h,c}`, `dynpool.{h,c}`, and bundled `minicoro/`.

Public API: `include/xylem.h` (`xylem_run`, `xylem_spawn`, `xylem_sleep`,
`xylem_await`, `xylem_shutdown`).

## 1. Goals and model

- **Synchronous-style networking.** No user-visible callbacks on the I/O path.
  `xylem_tcp_read()` reads like a blocking read but suspends the coroutine
  instead of the thread.
- **Scale on a fixed thread pool.** N worker threads (default: CPU count) run
  many coroutines; blocking syscalls are pushed to a separate elastic pool so
  they never starve the workers.
- **Thread-safe wakeups.** Low-level runtime primitives can make a parked
  coroutine runnable from any thread. Public protocol operations remain
  coroutine-only unless their own API documents a context-adaptive contract.

The concurrency unit is a **stackful coroutine** (minicoro). A coroutine that
cannot make progress *parks* (yields to its worker); a wake source later
*schedules* it back onto a worker.

## 2. Component overview

```
            xylem_run / xylem_spawn / xylem_sleep / xylem_await
                                  |
                                  v
   +-----------------------------------------------------------+
   |                        scheduler                          |
   |                                                           |
   |   worker[0]      worker[1]      ...      worker[N-1]       |
   |   runnext        runnext                 runnext          |
   |   wsq      <---- steal ---->  wsq        wsq              |
   |   timers         timers                  timers           |
   |        \            |            /                        |
   |         \           v           /                         |
   |          +------- global runq -------+   (overflow +      |
   |                                          cross-thread)    |
   |                                                           |
   |   one worker == poll driver -> platform poller            |
   +----------------------+-----------------+------------------+
                          |                 |
                          v                 v
                       iowait            dynpool
                  (park on fd)      (blocking tasks)
```

| Component | File | Role |
|-----------|------|------|
| Runtime facade | `runtime.c` | Global singletons; maps `xylem_*` onto scheduler + dynpool. |
| Scheduler | `scheduler.c` | Workers, runnable pool, poll driver, timers, coroutine pool. |
| Work-stealing queue | `wsq.c` | Per-worker lock-free FIFO run queue. |
| Global run queue | `runq.c` | Mutex-protected MPMC overflow / injection queue. |
| I/O wait | `iowait.c` | Per-fd, per-direction coroutine parking on the poller. |
| Blocking pool | `dynpool.c` | Elastic thread pool for blocking work. |
| Coroutines | `minicoro/` | Stackful coroutine primitive (bundled). |

## 3. Boot and shutdown (`runtime.c`)

`runtime_run()` (wrapped by `xylem_run()`):

1. Resolve worker count: `opts->workers` if > 0, else `platform_info_getcpus()`,
   floored at 4 if the query fails.
2. `platform_socket_startup()` (WSAStartup on Windows, ignore `SIGPIPE` on Unix).
3. `scheduler_create()` — builds workers, poller, wakeup socketpair, iowait
   slab, and coroutine pool.
4. `dynpool_create()` — the blocking-task pool.
5. Register an **idle callback** that calls `runtime_shutdown()` when the alive
   coroutine count hits zero, then spawn `main_fn` as the root coroutine.
6. Block the calling thread on a stop semaphore.

`runtime_shutdown()` is a thread-safe one-shot: a CAS on `g_shutdown` ensures
the stop semaphore is posted exactly once. It is called either explicitly by
the user or automatically by the idle callback when the last coroutine exits.

**Teardown order matters** and is fixed to avoid use-after-free:

```
scheduler_stop()     // stop + join workers, but keep memory allocated
dynpool_destroy()    // join blocking threads; late scheduler_schedule() calls
                     // from finishing tasks still see a valid scheduler
scheduler_destroy()  // now free scheduler memory
```

`scheduler_stop()` joins workers without freeing, so a dynpool thread that
finishes a blocking task and calls `scheduler_schedule()` to resume its
coroutine can still touch the scheduler safely. Only after the dynpool is fully
drained does `scheduler_destroy()` release memory.

## 4. The runnable pool: three tiers

A coroutine that is ready to run lives in exactly one of three places, ordered
from cheapest to most contended:

1. **`runnext` (per-worker, single slot).** A LIFO hand-off used when a worker
   schedules a coroutine onto itself. Cache-hot; checked first on pop. Pushing
   a new coroutine here evicts the previous occupant down to the queue. Only
   the owning worker pops its `runnext`, so a stalled owner (blocked syscall,
   long CPU loop) would normally strand the slot — steals touch only the queue.
   As a last resort, after the local queue and every victim queue come up
   empty, an idle worker may steal a `runnext` entry that has aged past
   `SCHED_RUNNEXT_STEAL_MS`; the race with the owner's own pop is settled by an
   atomic exchange, so a coroutine is never run twice.
2. **Work-stealing queue (`wsq`, per-worker).** The owner pushes at the tail
   and pops from the head (FIFO, arrival order); other workers steal from the
   head too. Lock-free, single-producer / multi-consumer. FIFO bounds how long
   any one coroutine waits. Default capacity 256 (power of 2).
3. **Global run queue (`runq`).** A mutex-protected MPMC queue. Two roles:
   overflow when a deque is full, and the **injection point** for any
   cross-thread `scheduler_schedule()` caller.

### `scheduler_schedule(sched, co)`

- **From a worker of this scheduler:** push to `runnext`; if `runnext` was
  occupied, push the evicted coroutine to the local deque. If the deque is
  full, spill half of it plus the evictee to the global runq as a batch.
- **From any other thread** (a different scheduler's worker, a dynpool thread,
  application thread): push straight to the global runq and wake one worker.

After scheduling, `_sched_wake_worker()` wakes at most one parked worker via
its semaphore; if none are parked but the poll driver is blocked, it pokes the
wakeup fd instead.

### `scheduler_schedule_batch(sched, cos, n)`

Pushes a whole array to the global runq under one lock acquisition and performs
**one** wake, amortizing lock + signal cost. Used by the I/O path when a single
poll pass makes many coroutines runnable.

### Park-state handshake (no resume mid-callback)

`scheduler_park()` runs the park callback *after* `mco_yield()` (see §7), but
the callback itself still touches the object it parks on — a channel's waiter
list, a sem's queue. A waker that fires while the callback is mid-flight must
not resume the coroutine yet, or the resumed coroutine could race the tail of
its own park callback (the classic "park_cb touches a freed channel" bug). A
per-coroutine `park_state` closes that window:

| State | Meaning |
|-------|---------|
| `IDLE` | running, or sitting in a normal run queue |
| `PARKING` | between `mco_yield()` and the end of the park callback |
| `PARKED` | callback returned true; suspended, awaiting a wake |
| `WOKEN` | a waker has marked it; it is (or will be) requeued exactly once |

Shutdown does not run park cleanup. If shutdown happens before or after a park
record is published, the coroutine is simply stranded until process exit;
`xylem_run()` return is the runtime lifetime boundary.

The parking worker (`_sched_handle_yield`) and the waker (`scheduler_schedule`
-> `_sched_try_wake`) cooperate through a CAS on `park_state`:

- **Parking worker:** store `PARKING`, run the callback. If the callback wants to
  park, CAS `PARKING -> PARKED`. A clean CAS means it is parked and some waker
  will requeue it later. If the CAS instead finds `WOKEN`, a waker raced in
  during the callback and deliberately did *not* enqueue, so the requeue is the
  worker's — done now via `_sched_requeue_local`.
- **Waker:** inspect `park_state` and act on its kind:
  - `IDLE` — not in a park handshake; a normal schedule, always enqueue.
  - `PARKED` — CAS to `WOKEN` and enqueue (the callback already returned; the
    waker owns the requeue).
  - `PARKING` — CAS to `WOKEN` but do **not** enqueue (the callback, on
    return, sees `WOKEN` and requeues itself).
  - `WOKEN` — another waker already claimed it; nothing to do.

Only one waker reaches the wake path per park, because the sync primitive (or
`iowait`) hands off a single one-shot waiter. The net guarantee: the coroutine
is requeued exactly once, and a resume never overlaps the tail of its park
callback. The delicate case is a waker arriving mid-callback:

```
 Parking worker (_sched_handle_yield)      Waker (scheduler_schedule)
        |                                          |
   park_state = PARKING                             |
   run park callback (still touching               |
   the parked-on object) ...                       |
        |                                  claim: sees PARKING
        |                                  CAS PARKING -> WOKEN
        |                                  return false -> does NOT enqueue
   callback returns true                           |
   CAS PARKING -> PARKED  FAILS (WOKEN) <--------+
        |
   park_state = IDLE
   _sched_requeue_local(co)   (the worker requeues, exactly once)
        v
```

## 5. Worker loop (`_sched_worker_entry`)

Each worker thread loops while the scheduler is running:

1. **Maintenance.** Fire due timers.
2. **Occasional global pull.** Every 61st tick (61 is prime, so it never
   resonates with the power-of-two deque sizes), pull one coroutine from the
   global runq and do a non-blocking poll, so global work and I/O can't starve
   behind a hot local deque.
3. **Find work** via `_sched_worker_find_coro()`:
   - Pop local (`runnext` → deque → fair share of the global runq).
   - If nobody owns the poll driver, do a **non-blocking** poll and take any
     coroutine it produces.
   - Try to **steal** from other workers' deques (half at a time).
   - Otherwise try to become the **poll driver** (CAS on `poller_running`).
     The driver does the blocking poll; non-drivers fall through and park.
4. **Run** the coroutine: `mco_resume()`, then handle its yield.
5. If no work was found and this worker isn't the driver, **park** on its
   semaphore — with a timeout equal to the next local timer, so timers still
   fire on an otherwise idle scheduler.

On shutdown the loop exits without draining queued or parked coroutines.
`scheduler_destroy()` destroys any remaining registered coroutines without
resuming user cleanup paths.

### The poll driver

Exactly one worker at a time owns the blocking poll, selected by a CAS on
`poller_running`. The driver:

- Publishes `poller_waiting = true` (seq-cst) *before* re-checking for work, so
  a concurrent producer either sees the flag (and pokes the wakeup fd) or the
  driver sees the new work — no lost wakeup.
- Computes the poll timeout from the nearest timer deadline across **all**
  workers (`_sched_timer_poll_timeout`), not just its own, so it wakes in time to
  service a timer owned by a worker that is busy in a long coroutine.
- After waking, greedily drains additional ready events with zero-timeout
  polls, fires due timers for **every** worker (timer stealing,
  repeated `_sched_timer_process` calls, see §8), then runs the first ready
  coroutine and schedules the rest.

This "one driver, many parkers" design means there is no dedicated I/O thread:
whichever worker happens to go idle takes over polling, and it relinquishes the
role the moment it finds a runnable coroutine.

The "no lost wakeup" handshake is the delicate part. The risk is: a producer
makes a coroutine runnable just as the driver is about to block in the poller,
and the driver sleeps without noticing. The seq-cst `poller_waiting` flag closes
that window — the producer and the driver are guaranteed to see *at least one*
of each other's stores:

```
   Producer thread                         Poll driver (idle worker)
   (scheduler_schedule)                     |
        |                                    | poller_waiting = true   (seq-cst)
        | push work to runq                  |  (store BEFORE re-check)
        | store seq-cst                       |
        |                                    | re-check: pop local / steal?
        |                                    |
        |  -- both stores are seq-cst, so the threads cannot BOTH miss --
        |                                    |
        | load poller_waiting:               | == case A: re-check found work ==
        |   == true ==                       | poller_waiting = false
        |     poke wakeup fd ------------.    | run it (never blocks)
        |   == false ==                  |    |
        |     a worker is already        |    | == case B: re-check empty ==
        |     awake; no poke needed      `--> | poller_wait(timeout = next timer)
        |                                     |   <- wakes on fd poke or I/O
        v                                     v
```

If the driver's re-check runs first and finds the new work (case A), it never
blocks. If the producer's push lands first, the driver's re-check sees it. If
they interleave, seq-cst ordering guarantees the producer observes
`poller_waiting == true` and pokes the wakeup fd, so the blocking poll returns
at once. No interleaving leaves the driver asleep with work pending.

## 6. Coroutines (minicoro + pooling)

Coroutines are stackful, provided by the bundled `minicoro` library. Each has a
fixed 128 KiB stack.

`scheduler_spawn()` allocates a `_sched_coro_ctx_t` (entry fn, arg, intrusive runq and
registry nodes), creates the coroutine, links it into the scheduler's
**registry** (so leaked-but-alive coroutines can be destroyed at teardown),
bumps the `alive` counter, and routes it through `scheduler_schedule()`. When a
coroutine returns, `_sched_handle_yield()` sees `MCO_DEAD`, unlinks it from the
registry, frees it, decrements `alive`, and — if it was the last one — fires the
idle callback that shuts the runtime down.

### Stack allocation and the coroutine pool

To avoid hammering the allocator on every spawn, the scheduler keeps a
**coroutine pool** (default capacity `worker_count * 64`) of recycled stacks:

- **Stack-based platforms.** `_sched_coro_pool_alloc()` allocates read/write
  virtual memory and marks one page `PROT_NONE` as a **guard page** below the
  stack to turn overflow into an immediate fault. On free, reusable stack pages
  are reset so the OS may reclaim their backing while the mapping remains in
  the pool.
- **Fiber-based platforms** (`MCO_USE_FIBERS`, e.g. Windows fibers): fall back
  to `calloc`/`free`.

The pool is guarded by a spinlock and bounded by its capacity; overflow and
scheduler teardown deallocate complete regions.

## 7. I/O parking (`iowait.c`)

`iowait` bridges a coroutine and the platform poller. One handle wraps one fd
and tracks **two independent directions** (read, write), each holding at most
one parked coroutine.

### Park / wake handshake

The subtlety is avoiding a "wake before yield" race. `scheduler_park()` invokes
the park callback **after** `mco_yield()` has actually suspended the coroutine,
so a wake source can never observe the coroutine pointer before it is safely
parked. The steps:

1. `iowait_read()` calls `scheduler_park()` with `_iowait_park_cb` and `&w->rd`.
2. After the yield, `_iowait_park_cb` publishes the coroutine into
   `rd.waiter` with a CAS. A slot already holding a parked coroutine means an
   illegal second reader — the process aborts with a diagnostic.
3. It **re-checks** `closed`, `deadline_error`, and `deadline_expired`,
   because close or a deadline reset could have raced in between publish and
   the check. If any condition is true, it wakes itself immediately.
4. On resume, the return value is decided from the current state by re-reading
   `closed`, `deadline_error`, and `deadline`: `IOWAIT_READY`, `IOWAIT_TIMEOUT`,
   `IOWAIT_ERROR`, or `IOWAIT_CLOSED`.

The fd is already registered with the kernel poller before parking — it was
armed at `iowait_create` (and, on LT+ONESHOT, re-armed in `iowait_on_event`
after each event). Park never touches the poller.

The same steps as a time-ordered diagram (top to bottom is time; `|` is each
actor's timeline):

```
 Coroutine      scheduler_park     _iowait_park_cb     poller / poll driver
     |                 |                  |                     |
     |  iowait_read()  |                  |                     |
     |---------------->|                  |                     |
     |                 | mco_yield()      |                     |
     |  (suspended) ...| .........        |                     |
     |                 |                  |                     |
     |                 | cb runs AFTER yield -> no waker can see |
     |                 | the coroutine before it is parked      |
     |                 |----------------->|                     |
     |                 |                  | publish waiter CAS  |
     |                 |                  | (abort if prev)     |
     |                 |                  | re-check closed /   |
     |                 |                  | deadline (raced in?)|
     |                 |                  |                      |
     | == case A: closed/deadline already passed ==             |
     |<----------------------------------| self-wake: schedule  |
     |                 |                  | co immediately       |
     |                 |                  |                      |
     | == case B: still waiting ==        |                      |
     |                 |                  |   fd becomes ready    |
     |                 |                  |        CQE (gen,idx)  |
     |                 |                  |   tryref + gen check  |
     |<--------------------------------------- exchange(&rd.waiter, |
     |                 |                  |   READY) -> schedule(co)
     |                                                           |
     | resume; result = READY / TIMEOUT / CLOSED / ERROR         |
     v                                                           v
```

### Three wake sources, one winner

A parked coroutine can be woken by an **I/O event**, a **deadline timer**, or
**`iowait_close()`**. Each tries to claim the parked coroutine from the
per-direction slot; only the claimant that observes the coroutine pointer
actually reschedules it. I/O readiness is cached as `READY` in the slot for
the next parker to consume without blocking. Deadline timeout is deliberately
not latched as an unconditional result: the resumed coroutine reports timeout
only if the deadline is still expired when it runs. If a deadline timer fires
and wakes the coroutine, but the deadline is cleared or reset before it
resumes, that wake is treated as spurious and the wait retries.

```
   I/O event      deadline timer     iowait_close          rd.waiter (atomic)
       |                |                  |          EMPTY | READY | PARKED(co)
       |                |                  |                     |
       |  -- the three sources may fire concurrently --          |
       |  CAS to READY, return old co ------------------------->|
       |                | CAS to EMPTY, return old co --------->|
       |                |                  | CAS to EMPTY, ---->|
       |                |                  | return old co       |
       |                |                  |                     |
       |  exactly ONE CAS returns co; the others get NULL/READY  |
       |                                                         |
       |  winner: schedule(co)  ----------------------------->  Coroutine
       |                                                         |
       |  on resume, co re-reads current state to report         |
       |  READY / TIMEOUT / ERROR / CLOSED, or retries           |
       v                                                         v
```

### Generation-tagged slab — rejecting stale events

iowait handles are allocated from a per-scheduler **paged slab** with a
free-list. The poller's user-data is a `(generation, slab-index)` pair packed
into a `uintptr_t` (16 generation bits, the rest index; layout validated by a
`_Static_assert` and works on 32- and 64-bit). When a handle is retired its
generation is bumped and the slot returns to the free list. If a completion
event arrives for a recycled slot, `_iowait_tryref()` compares the event's tag
against the slot's current generation and **rejects the mismatch**, so a stale
CQE can never wake the wrong coroutine. Index 0 is reserved as the NULL
sentinel used by the scheduler's wakeup fd.

### I/O event handling and poller re-arm

Both trigger modes register the fd with read+write interest:

- **ET (Linux/macOS):** `iowait_create` calls `platform_poller_add(RW_OP)` once.
  The kernel reports every ready transition; the fd stays registered for the
  lifetime of the handle. No re-arm is ever needed.

- **LT + ONESHOT (Windows/wepoll):** `iowait_create` likewise calls
  `platform_poller_add(RW_OP)`. After each event the kernel auto-disables the
  fd — `iowait_on_event` syncs `interest` to `NO_OP` and calls
  `platform_poller_mod(RW_OP)` again under `arm_lock` with a closed re-check.
  ET mode skips this entirely.

Because the fd is always registered (ET) or re-registered before the next
park (LT), the park callback never touches the poller — it only publishes the
parked coroutine into the direction's slot and re-checks close/deadline.

`iowait_close()` drops the poller subscription **synchronously** under the arm
lock before waking parked coroutines, so the caller can close the underlying fd
right afterward without racing a deferred `EPOLL_CTL_DEL` against a recycled fd
number. It is idempotent and may race with parked waits or deadline setters.

Deadline state is per direction. `set_deadline`, `clear_deadline`, and close's
deadline stop path are serialized by that direction's `deadline_lock`, which
protects lazy timer creation, timer reset/stop, and the arm/ref accounting. The
lock does not protect socket I/O, stream/TLS state, or object lifetime.

`iowait_destroy()` is different from close: it is the final owner-side release
and must not race with any other iowait API call. The handle itself is retired
only when all internal refs (in-flight poller callbacks and armed deadline
timers) are dropped, but a parked waiter does **not** hold an iowait ref. The
owning connection must therefore keep the handle alive until every parked reader
and writer has returned, then call destroy exactly as the last release. The
internal stream, listener, and datagram owners call their close operation first
so deadline timers and poller state are detached before the fd is closed; their
destroy operation performs that close automatically when no concurrent operation
needs a separate close/wait/destroy sequence.

## 8. Timers

Each worker owns a binary **min-heap** of timers keyed by absolute expiry
(`xylem_utils_getnow(MSEC)`), guarded by a per-worker mutex. A timer is created
against the scheduler (`scheduler_timer_create`), assigned to an owner worker, and
armed with `scheduler_timer_start(cb, ud, timeout_ms, repeat_ms)`.

- Due timers are popped in `_sched_timer_process()`, which either runs the
  callback **inline on the firing thread** or, if `spawn` is set, normally runs
  it in a fresh coroutine. If spawning that coroutine fails, the fire is
  completed without invoking the callback, and the normal completion/ud_unref
  path still runs.
- Periodic timers (`repeat > 0`) are reinserted only after the fired callback
  completes. A timer in `FIRING` state records stop/reset/start requests as
  pending flags (`stop_pending`, `reset_pending`) and applies them in the
  completion path (`_sched_timer_complete`), so callbacks for the same timer
  never overlap. The priority order is:

  1. **stop** — always wins. Neither `reset` nor `start` clear `stop_pending`.
     Once a stop has been requested, the timer goes idle and any concurrent
     re-arm request is silently dropped.
  2. **reset** — honoured only when `stop_pending` is false. For periodic
     timers, `reset` also updates the repeat interval.
  3. **repeat** — the original periodic interval, used when neither stop nor
     reset was requested.

- Operations on the same internal scheduler timer handle are serialized by the
  owner worker's `timer_lock`; `start`, `reset`, and `stop` are thread-safe.
  An already dispatched callback cannot be withdrawn by `stop()`, but `stop()`
  does prevent a repeat requeue or a deferred reset from the same firing round.
- Timers are reference counted so `scheduler_timer_destroy()` is safe to call
  concurrently with an in-flight fire. `scheduler_timer_stop()` returns true
  when it removes a queued timer or cancels a deferred reset from a callback
  already in flight. `scheduler_timer_reset()` returns true when it removes a
  queued timer or overwrites an earlier deferred reset from the same in-flight
  callback; the first reset requested by a currently firing callback returns
  false because no older pending fire was cancelled. This lets callers (e.g.
  the iowait deadline path) know whether to release a reference the older
  queued/deferred arm would otherwise drop. The iowait deadline callback
  re-checks the absolute deadline before waking, so a callback that was already
  dispatched before a clear/reset can become a harmless spurious wake.

The public `xylem_timer_*` wrapper exposes those same lifetime rules with an
opaque, cancel-consumed handle. `xylem_timer_after()` is one-shot;
`xylem_timer_every()` repeats only after the previous callback returns, and
`xylem_timer_reset()` preserves the callback/user data while re-arming the next
fire. Resetting a timer from inside its own callback schedules a deferred
re-arm; resetting it again before the callback returns overwrites that deferred
re-arm and reports cancellation.

`xylem_ticker` is the pull-based counterpart to `xylem_timer_every()`. It uses
an inline scheduler timer callback that never runs user code: the callback only
records the tick timestamp and posts a semaphore if no previous tick is pending.
The receiver drains ticks with `xylem_ticker_recv()`, which is
context-adaptive; when the receiver falls behind, ticks coalesce to one buffered
timestamp instead of queuing unbounded work. `xylem_ticker_close()` is the
concurrent shutdown boundary: it stops the timer and wakes a blocked receiver,
which returns 0. `xylem_ticker_destroy()` consumes the handle and calls close as
a cleanup fallback, but callers that run a receiver in another context should
close first, wait for the receiver to exit, then destroy the ticker.

`xylem_sleep(ms)` is built directly on this: it creates a one-shot timer whose
callback reschedules the sleeping coroutine, then parks.

### Timer stealing — who fires whose timers, and where they run

A worker normally drains its own heap on the local `_sched_timer_process` path.
But a worker stuck running a long coroutine never reaches that path, which would
delay its timers. To bound that, the **idle poll driver also fires due timers
for every worker** by calling `_sched_timer_process()` after each poll wake. This
is why the driver derives its poll timeout from the nearest deadline across all
workers (§5) — it must wake in time to cover a busy worker's timers.

Each worker's heap is still processed under **its own** `timer_lock`, and a
fast-path acquire-load of the republished `next_deadline_ms` makes a
not-yet-due worker a single atomic with no lock taken. Concurrent firing by the
owner and the driver is safe: both contend the same lock, and `heap_dequeue`
hands each due timer to exactly one of them.

The key consequence is **where a stolen timer runs**: ownership only locates the
heap, never routes execution back to the owner. The timer fires on **whichever
thread pulled it off the heap** — in the steal case that is the idle poll
driver, not the owner:

- **Inline timer (`spawn == false`):** `timer->cb` runs synchronously on the
  driver thread.
- **Spawn timer (`spawn == true`):** `scheduler_spawn()` builds a coroutine and
  routes it through `_sched_enqueue`, which keys off the *running* worker
  (`_tls_worker` = the driver), so the coroutine lands in the driver's own
  `runnext`/deque and is then free to be work-stolen by any worker, including
  the original owner.

## 9. Blocking-task pool (`dynpool.c`)

Some work genuinely blocks (DNS via `getaddrinfo`, file I/O, third-party calls).
Running it on a worker would stall every coroutine pinned to that worker, so it
is offloaded to the **dynamic thread pool**.

`xylem_await(fn, arg)` → `runtime_submit()`:

1. Allocate a context capturing `fn`, `arg`, the scheduler, and (filled in at
   park time) the calling coroutine.
2. `scheduler_park()` the coroutine; in the park callback, `dynpool_submit()`
   hands the task to the pool. If submission fails, the park callback returns
   `false` so the coroutine is rescheduled immediately and `runtime_submit()`
   reports `-1`.
3. A pool thread runs `fn(arg)`, then calls `scheduler_schedule()` to resume the
   coroutine — a cross-thread wakeup that lands on the global runq.
   If shutdown destroys the pool before a queued task runs, process-exit
   teardown owns the remaining submit context.

The cross-thread handoff (top to bottom is time):

```
 Coroutine        runtime_submit /        dynpool            worker that
 (on worker A)    park callback           thread             picks it up
     |                 |                    |                     |
     | xylem_await()   |                    |                     |
     |---------------->|                    |                     |
     |                 | scheduler_park():  |                     |
     | (suspended) ... | publish co into    |                     |
     |                 | ctx, then          |                     |
     |                 | dynpool_submit() ->|                     |
     |                 |                    | run fn(arg)         |
     |   meanwhile worker A runs OTHER coroutines                 |
     |                 |                    | (blocking work)     |
     |                 |                    |                     |
     |                 |                    | done -> schedule(co)|
     |                 |                    | push to global runq |
     |                 |                    | wake a worker ----->|
     |                 |                    |                     | pop co
     |<----------------------------------------------------------| resume co
     v                 (may resume on a DIFFERENT worker than A)  v
```

Note the coroutine can resume on **any** worker, not necessarily the one it
parked on — the wakeup is a plain cross-thread `scheduler_schedule()` into the
global runq. If `dynpool_submit()` fails, the park callback returns `false`, the
coroutine is rescheduled immediately, and `runtime_submit()` returns `-1`.

The pool spawns threads on demand up to `max_threads` (default 512) and lets
idle threads exit after `idle_timeout` (default 10 s). Task submission holds
the pool mutex while it enqueues the task and updates worker lifecycle state;
the condition variable wakes idle workers when the queue or shutdown state
changes.

## 10. Concurrency invariants

- **A parked coroutine is requeued exactly once, never mid-callback.**
  `scheduler_park()` runs its callback after `mco_yield()`, and the per-coroutine
  `park_state` handshake (`PARKING`/`PARKED`/`WOKEN`, §4) ensures a waker that
  races the still-running callback marks it `WOKEN` without enqueuing — the
  callback then owns the requeue, so a resume never overlaps the callback tail.
- **At most one reader and one writer per `iowait` direction.** Enforced by the
  exchange-on-publish check; violations `abort()`.
- **Stale completion events are rejected**, not tolerated — via generation tags.
- **No lost wakeups against the poll driver.** The driver sets `poller_waiting`
  (seq-cst) before its final work re-check; producers that miss a parked worker
  fall back to poking the wakeup fd.
- **Teardown invalidates runtime-backed objects.**
  Most applications use one runtime near process exit. Sequential `xylem_run()`
  calls are supported for tests or controlled scenarios if the previous run
  leaves no live runtime-backed resources or external threads using them.
  Shutdown joins running blocking tasks and frees core scheduler structures, but
  it does not guarantee full cleanup for coroutines stranded in already-committed
  parks.

## 11. Configuration

| Option | Where | Default | Meaning |
|--------|-------|---------|---------|
| `workers` | `xylem_opts_t` / `runtime_opts_t` | CPU count | Scheduler worker threads. |
| `deque_capacity` | `scheduler_opts_t` | 256 | Per-worker deque capacity (power of 2). |
| `coro_pool_capacity` | `scheduler_opts_t` | `workers * 64` | Recycled coroutine stacks. |
| `max_threads` | `dynpool_opts_t` | 512 | Max blocking-pool threads. |
| `idle_timeout` | `dynpool_opts_t` | 10000 ms | Blocking-pool idle thread lifetime. |
| Coroutine stack | compile-time | 128 KiB | Per-coroutine stack size. |

## 13. Related docs

- System overview and how protocols sit on the runtime:
  [`../architecture.md`](../architecture.md).
- Platform poller / socket / vmem details:
  [`platform.md`](platform.md).
- Per-protocol designs (TCP, UDP, TLS, …): [`../design/`](.).
- Runtime test design: [`../test/runtime.md`](../test/runtime.md) *(planned)*.
