# Go-Style Scheduler Coroutine State Design

Date: 2026-07-17

## Summary

Xylem currently uses a coroutine-local park handshake (`PARK_IDLE`,
`PARK_PARKING`, `PARK_PARKED`, and `PARK_WOKEN`) to prevent a wakeup from
resuming a coroutine while its park callback is still accessing coroutine
state. This makes scheduling and waiter synchronization overlap: the
scheduler accepts early wakeups, defers some enqueue operations, and relies on
each park user to participate in the same handshake.

This design replaces that handshake with a complete scheduler-owned coroutine
state machine and a strict park commit contract. The scheduler owns only
coroutine lifecycle transitions. Each synchronization primitive owns its
waiter publication and single-winner wake protocol.

The design follows the responsibility split used by the Go runtime:

- Scheduler state describes whether a coroutine is runnable, running, or
  waiting.
- A park commit callback publishes the suspended coroutine to an external
  wait source.
- A wake source changes `WAITING` to `RUNNABLE` and enqueues the coroutine.
- Wait protocols use locks or tagged atomic states according to the resource
  they synchronize.

## Goals

- Replace the park-only state with a complete scheduler coroutine state.
- Make every lifecycle transition explicit and validated.
- Allow a coroutine to be readied immediately after its waiter is committed.
- Remove `_sched_try_wake()` and the deferred `PARK_WOKEN` requeue path.
- Separate cooperative yield from park.
- Give each waiter implementation one clear final publication point.
- Preserve existing queue locality, work stealing, and wake-chain behavior.
- Add deterministic tests for the races around park commit.

## Non-Goals

- Redesign the worker state machine or poller ownership protocol.
- Change the local WSQ, global runq, or `runnext` scheduling policy.
- Make all wait protocols use one generic tagged state.
- Match the complete Go channel implementation. Xylem channels remain
  unbounded MPSC channels with a single receiver.
- Change minicoro's internal `mco_status` model.

## Scheduler Coroutine State

The scheduler context owns the following state:

```c
typedef enum {
    SCHED_CORO_NEW,
    SCHED_CORO_RUNNABLE,
    SCHED_CORO_RUNNING,
    SCHED_CORO_WAITING,
    SCHED_CORO_DEAD,
} _sched_coro_state_t;
```

The context replaces `park_state` with the full state:

```c
typedef struct _sched_coro_ctx_s {
    void (*fn)(void*);
    void*                        arg;
    runq_node_t                  runq_node;
    list_node_t                  registry_node;
    mco_coro*                    co;
    _Atomic(_sched_coro_state_t) state;
    uint32_t                     registry_owner;
} _sched_coro_ctx_t;
```

`mco_status` remains minicoro's execution and stack status. Scheduler state is
the source of truth for queue ownership and scheduler lifecycle.

### Valid Transitions

```text
NEW      -> RUNNABLE    start
RUNNABLE -> RUNNING     execute
RUNNING  -> RUNNABLE    cooperative yield
RUNNING  -> WAITING     begin park commit
WAITING  -> RUNNABLE    ready or declined park
RUNNING  -> DEAD        coroutine exit
```

All transitions use a strong compare-and-exchange with the default C11 memory
order (`seq_cst`):

```c
static void _sched_coro_transition(
    mco_coro* co,
    _sched_coro_state_t from,
    _sched_coro_state_t to) {
    _sched_coro_ctx_t* ctx =
        (_sched_coro_ctx_t*)mco_get_user_data(co);

    _sched_coro_state_t expected = from;
    if (!atomic_compare_exchange_strong(
            &ctx->state,
            &expected,
            to)) {
        xylem_loge(
            "<sched> invalid coro transition co=%p expected=%d "
            "actual=%d next=%d",
            (void*)co,
            (int)from,
            (int)expected,
            (int)to);
        abort();
    }
}
```

Initialization to `NEW` may use `atomic_init`. No lifecycle transition uses a
plain store.

Invalid transitions are scheduler or waiter protocol bugs and abort instead
of being silently ignored. Examples include duplicate ready, a duplicate runq
entry, execution of a waiting coroutine, and a yield without a scheduler
yield reason.

## API Naming

The scheduler coroutine lifecycle API is:

```text
scheduler_coro_spawn         create a coroutine
_sched_coro_start            NEW -> RUNNABLE
scheduler_coro_ready         WAITING -> RUNNABLE
scheduler_coro_ready_batch   batch WAITING -> RUNNABLE
_sched_coro_execute          RUNNABLE -> RUNNING
scheduler_coro_yield         RUNNING -> RUNNABLE
scheduler_coro_park          RUNNING -> WAITING
_sched_coro_exit             RUNNING -> DEAD
```

`ready` is used instead of `resume`: readying only publishes a coroutine to a
run queue. Actual execution remains the responsibility of `mco_resume()` in
`_sched_coro_execute()`.

`scheduler_schedule()` and `scheduler_schedule_batch()` are replaced by
`scheduler_coro_ready()` and `scheduler_coro_ready_batch()` at wake sites.
New coroutines do not pass through `ready`; `scheduler_coro_spawn()` calls
`_sched_coro_start()` because their source state is `NEW`.

## Yield and Park Separation

Worker-local yield bookkeeping becomes explicit:

```c
typedef enum {
    SCHED_YIELD_NONE,
    SCHED_YIELD_RUNNABLE,
    SCHED_YIELD_PARK,
} _sched_yield_reason_t;
```

Each worker stores:

```c
_sched_yield_reason_t            yield_reason;
scheduler_coro_park_commit_fn_t  park_commit;
void*                            park_arg;
```

The park callback type is renamed:

```c
typedef bool (*scheduler_coro_park_commit_fn_t)(
    mco_coro* co,
    void* arg);
```

`scheduler_coro_yield()` sets `SCHED_YIELD_RUNNABLE` and yields directly. It
does not use a callback that returns false.

`scheduler_coro_park()` validates the current worker and scheduler, stores the
commit callback and argument, sets `SCHED_YIELD_PARK`, and yields.

## Park Commit Contract

After `mco_resume()` returns for a park request, the worker:

1. Copies the commit callback and argument to local variables.
2. Clears all worker-local park bookkeeping.
3. Transitions the coroutine from `RUNNING` to `WAITING`.
4. Invokes the commit callback.

If the callback returns true, it has committed the waiter. The worker returns
immediately without reading the coroutine, its scheduler context, or the
callback argument. Another worker may already have changed the coroutine to
`RUNNABLE` or `RUNNING`.

If the callback returns false, it must not have published the coroutine to a
wake source. The worker transitions `WAITING` to `RUNNABLE` and enqueues it on
the current worker.

```c
static void _sched_coro_commit_park(
    _sched_worker_t* w,
    mco_coro* co) {
    scheduler_coro_park_commit_fn_t commit = w->park_commit;
    void* arg = w->park_arg;

    w->yield_reason = SCHED_YIELD_NONE;
    w->park_commit  = NULL;
    w->park_arg     = NULL;

    _sched_coro_transition(
        co,
        SCHED_CORO_RUNNING,
        SCHED_CORO_WAITING);

    if (commit(co, arg)) {
        return;
    }

    _sched_coro_transition(
        co,
        SCHED_CORO_WAITING,
        SCHED_CORO_RUNNABLE);
    _sched_worker_enqueue(w, co);
}
```

The final externally visible operation in every successful commit callback
must make the waiter claimable. After that operation, the callback may only
return true.

Tagged waiter states rely on coroutine and waiter pointers being naturally
aligned and therefore greater than the highest low-valued sentinel. The
implementation must document this invariant and validate alignment where the
concrete allocation type is available.

## Worker Execution

`_sched_coro_execute()` validates `RUNNABLE -> RUNNING`, resets worker-local
yield bookkeeping and credit, and calls `mco_resume()`.

After `mco_resume()` returns:

- `MCO_DEAD` transitions `RUNNING -> DEAD` and destroys the coroutine.
- `SCHED_YIELD_RUNNABLE` transitions `RUNNING -> RUNNABLE` and enqueues it.
- `SCHED_YIELD_PARK` runs the park commit flow.
- `SCHED_YIELD_NONE` aborts because the coroutine bypassed the scheduler API.

When a parked coroutine resumes, the ready and execute paths have already
performed `WAITING -> RUNNABLE -> RUNNING`. `scheduler_coro_park()` does not
write scheduler state after `mco_yield()` returns.

## Ready Operations

`scheduler_coro_ready()` performs exactly one state transition before queue
publication:

```text
WAITING -> RUNNABLE -> enqueue
```

The function aborts if the coroutine is not waiting. Wait protocols must
perform single-winner arbitration before calling it.

`scheduler_coro_ready_batch()` validates and transitions every coroutine
before placing it in the batch runq path. It retains the existing fixed batch
capacity and single worker wake behavior. The poller path may still select one
ready coroutine to execute immediately, but it must first perform
`WAITING -> RUNNABLE`; `_sched_coro_execute()` then performs
`RUNNABLE -> RUNNING`.

## I/O Wait Protocol

I/O readiness uses the Go-style poll semaphore protocol because a poller event
may be an edge that must be retained when no coroutine pointer is committed.

```c
enum {
    IOWAIT_WAITER_NONE  = 0,
    IOWAIT_WAITER_WAIT  = 1,
    IOWAIT_WAITER_READY = 2,
};
```

```text
NONE   no waiter or pending readiness
WAIT   waiter slot reserved; coroutine pointer not committed
READY  readiness arrived and has not been consumed
> 2    committed mco_coro* pointer
```

The operation-level helpers are:

```c
_iowait_wait()
_iowait_wait_commit_cb()
_iowait_unblock(_iowait_dir_t* d, bool io_ready)
```

`_iowait_wait()` consumes an existing `READY`, reserves `NONE -> WAIT`,
rechecks close and deadline state, and calls `scheduler_coro_park()`.

`_iowait_wait_commit_cb()` performs `WAIT -> co`. The CAS is its final shared
operation.

`_iowait_unblock()` follows Go's `netpollunblock` semantics:

```text
io_ready = true:
    NONE/WAIT/co -> READY
    READY        -> READY

io_ready = false (close, timeout, internal error):
    NONE         -> NONE
    WAIT/co      -> NONE
    READY        -> READY
```

Close, timeout, and internal-error fields must be published before calling
`_iowait_unblock(..., false)`. A coroutine that resumes after the unblock must
observe the winning result.

When the old value is a coroutine pointer, `_iowait_unblock()` returns it and
the caller invokes `scheduler_coro_ready()` or adds it to a ready batch.

The result enum remains separate from waiter synchronization:

```text
IOWAIT_READY
IOWAIT_CLOSED
IOWAIT_TIMEOUT
IOWAIT_ERROR
```

Pending readiness is consumed before reporting an error. Among error results,
the priority is `CLOSED`, then `TIMEOUT`, then `ERROR`. If a deadline was reset
before the coroutine executes, the wait loop retries.

## Lock-Protected Synchronization Primitives

Mutex, semaphore, waitgroup, and condition variable wait queues already use a
guard. They do not need tagged waiter states.

For a successful commit, releasing the guard is the final waiter publication.
All waiter fields, references, timer setup, and condition rechecks occur before
that unlock.

Callback names become:

```c
_mutex_wait_commit_cb()
_sem_wait_commit_cb()
_wg_wait_commit_cb()
_cond_wait_commit_cb()
```

### Mutex

The callback sets `w->co`, locks the mutex guard, rechecks `locked`, and either
declines the park or enqueues the waiter. Guard release is the commit point.

### Semaphore

The callback rechecks for a banked token under the guard. A timed waiter takes
all semaphore and timer references and arms its timer before the final guard
release. A timer that fires immediately cannot claim the waiter until it
acquires the same guard.

### Waitgroup

The callback rechecks `count` under the guard. A zero count declines the park;
otherwise guard release commits the queued waiter.

### Condition Variable

The current callback publishes the waiter and then accesses the stack waiter
again to unlock the user mutex. This is unsafe under the strict commit
contract.

The callback instead holds `cond->guard` while it enqueues the waiter and
unlocks the user mutex. Releasing `cond->guard` is the final operation:

```text
set waiter coroutine
lock cond guard
enqueue waiter
unlock user mutex
unlock cond guard    final publication
return true
```

This preserves the condition-variable invariant that the waiter is queued
before the user mutex becomes available to a signaler.

## Channel Wait Protocol

Xylem channels remain lock-free on the MPSC data and waiter paths. Adding a
channel waiter lock would change the progress guarantee and add contention to
every send.

The channel needs only a pre-commit marker:

```c
enum {
    CHANNEL_WAITER_NONE = 0,
    CHANNEL_WAITER_WAIT = 1,
};
```

The waiter field becomes `_Atomic uintptr_t`:

```text
NONE      no waiter
WAIT      coroutine reserved the slot but has not committed its pointer
> WAIT    committed coroutine or OS-thread waiter pointer
```

There is no `READY` marker. Channel wake conditions are durable:

- A sent message remains in the MPSC queue.
- `closed` remains true.
- `timer_fired` remains true.

The coroutine receive loop performs `NONE -> WAIT`, rechecks the durable
conditions, and parks. `_channel_wait_commit_cb()` performs `WAIT -> waiter`
as its final operation.

`_channel_unblock()` atomically exchanges the slot to `NONE`:

```text
old NONE      no action
old WAIT      prevent the pending park; callback returns false
old pointer   return the waiter for wakeup
```

Send publishes the message before unblocking. Close publishes `closed` before
unblocking. Timeout publishes `timer_fired` before unblocking.

OS-thread waiters publish their pointer directly because `thrd_wake` retains
an early signal. Timed thread cancellation continues to use an exact
`pointer -> NONE` compare-and-exchange.

## Runtime Sleep and Blocking Submit

These operations have a single completion source after successful launch and
do not need an additional waiter state.

For `runtime_sleep()`, `scheduler_timer_start()` is the final externally
visible operation in `_runtime_sleep_commit_cb()`. The callback does not access
the coroutine, stack argument, or timer after the call.

For `runtime_submit()`, all context fields are set before `dynpool_submit()`.
A failed submission cleans up and returns false. A successful submission is
the final externally visible operation; the callback only returns true.

## DNS Resolve Wait Protocol

DNS resolution has two racing completion sources: the dynpool worker and the
deadline timer. It retains its atomic single-winner design instead of adding a
mutex.

The waiter becomes a tagged `_Atomic uintptr_t`:

```c
enum {
    ADDR_WAITER_NONE    = 0,
    ADDR_WAITER_WAIT    = 1,
    ADDR_WAITER_DONE    = 2,
    ADDR_WAITER_TIMEOUT = 3,
};
```

```text
WAIT       park is being prepared
DONE       resolver worker won
TIMEOUT    timer won
> TIMEOUT  committed mco_coro* pointer
```

The separate `timed_out` flag is removed. The terminal waiter state identifies
the winner.

The commit callback initializes the job reference, submits the job, arms the
timer and its reference, and finally performs `WAIT -> co`. If the worker or
timer already changed the state to a terminal value, the CAS fails and the
callback returns false.

The worker completes all result writes before changing `WAIT/co -> DONE`. The
timer changes `WAIT/co -> TIMEOUT`. A transition from a committed pointer
returns the coroutine for `scheduler_coro_ready()`. A transition from `WAIT`
does not schedule; the commit callback observes the terminal state and
declines the park.

The caller reads the terminal waiter state before reading the result. Default
`seq_cst` atomics publish worker result writes. Existing references continue to
keep the context alive when timeout allows the resolver job to outlive the
waiting coroutine.

## Mux Stream Waiters

Mux streams already protect receive buffer state, send window state, and
stream state with `s->lock`. The same lock should protect waiter publication.
No tagged state or new lock is required.

The fields become plain pointers under the stream lock:

```c
mco_coro* recv_waiter;
mco_coro* send_waiter;
```

`_mux_recv_wait_commit_cb()` locks the stream, rechecks receive data and
terminal state, publishes `recv_waiter`, and releases the lock as its final
operation.

`_mux_send_wait_commit_cb()` rechecks send window and terminal state,
publishes `send_waiter`, and releases the lock as its final operation.

Every state update that satisfies or terminates a wait detaches the associated
waiter under the same lock and calls `scheduler_coro_ready()` after unlocking:

- Receive data detaches `recv_waiter`.
- Send-window updates detach `send_waiter`.
- Remote FIN detaches `recv_waiter`.
- Reset and session close detach both waiters.
- `xylem_mux_close_stream()` detaches both waiters.

This also closes the existing lost-wakeup window between the read/write
condition check and the current atomic waiter publication.

## Failure Handling and Invariants

The following conditions abort:

- A state transition does not observe its required source state.
- A coroutine returns from `mco_resume()` without dying or setting a yield
  reason.
- A park request has no commit callback.
- A callback returns false after an external waker has changed the coroutine
  state from `WAITING`.
- A single-waiter resource detects a second concurrent waiter.

The following conditions do not abort:

- A durable condition is satisfied before a commit callback publishes its
  waiter. The callback returns false.
- An I/O event or channel completion wins while the waiter slot contains its
  `WAIT` marker.
- DNS worker completion or timeout wins before `WAIT -> co`.

The scheduler does not deduplicate invalid wakeups. Each wait protocol must
return a coroutine pointer to at most one caller.

## Deterministic Race Testing

Tests must not rely on sleeps to hit park windows. Test builds add a scheduler
park checkpoint after `RUNNING -> WAITING` and before the commit callback. The
checkpoint is compiled only when `XYLEM_ENABLE_TESTING` is enabled, so
production builds have no branch or state.

Tests coordinate the checkpoint with C11 mutex/condition variables or platform
semaphores.

### Scheduler Tests

Add `tests/test-scheduler.c` and register it with `xylem_add_test(scheduler)`.
Cover:

- Spawn lifecycle through `DEAD`.
- Repeated cooperative yield.
- Declined park.
- Successful park followed by external ready.
- Batch ready with exactly-once execution.

### Wait Protocol Tests

- Mutex unlock, semaphore post, waitgroup done, and condition signal before
  waiter commit.
- Condition waiter destruction immediately after resume under ASAN/TSAN.
- I/O readiness before wait, during `WAIT`, and after pointer commit.
- I/O readiness racing close, timeout, and deadline reset.
- Channel send, close, and timeout before commit, during `WAIT`, and after
  pointer commit.
- Channel send versus timeout with no stale waiter and exactly one result.
- Repeated `runtime_sleep(0)` and no-op `runtime_submit()` completions.
- DNS worker and timeout winning both before and after pointer commit, plus
  dynpool submission failure.
- Mux receive data and send-window updates during the park checkpoint.
- Mux reset and local close waking committed read and write waiters.

### Verification Matrix

Run:

- Normal MSVC test suite.
- ASAN and UBSAN builds under GCC or Clang.
- TSAN under Linux or WSL.
- One-worker and multi-worker scheduler configurations.
- Repeated race-test execution.

## Implementation Order

1. Add scheduler coroutine state, yield reason, transition validation, and API
   renames.
2. Convert scheduler spawn, execute, yield, park, exit, ready, batch ready, and
   poller fast paths.
3. Convert lock-protected synchronization primitives.
4. Convert iowait to `NONE/WAIT/READY/co` and Go-style unblock semantics.
5. Convert channel to `NONE/WAIT/waiter`.
6. Convert sleep and runtime submit callbacks.
7. Convert DNS to tagged terminal states.
8. Convert mux waiters to the existing stream lock.
9. Add deterministic checkpoint tests and extend module race tests.
10. Update scheduler, runtime, sync, channel, iowait, and mux documentation.

This is the logical order inside one cohesive implementation change, not a set
of independently shippable intermediate states. The old
`scheduler_schedule()` API and old park states are removed after every caller
has migrated. No buildable checkpoint may mix the old park handshake with the
new coroutine lifecycle state.
