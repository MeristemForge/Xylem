# Runtime Design

The runtime is the engine behind Xylem's networking stack. It lets code be
written in a straight-line, blocking style while multiplexing thousands of
connections over a small pool of OS threads. This document describes the
scheduler, coroutine model, I/O parking, timers, and the blocking-task pool.

Sources: `src/runtime/` — `runtime.c`, `scheduler.{h,c}`, `arena.{h,c}`,
`copool.{h,c}`, `iowait.{h,c}`, `wsq.{h,c}`, `runq.{h,c}`,
`dynpool.{h,c}`, and bundled `minicoro/`.

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
| Work-stealing queue | `wsq.c` | Per-worker fixed-capacity SPMC FIFO run queue. |
| Global run queue | `runq.c` | Mutex-protected intrusive singly linked MPMC FIFO. |
| I/O wait | `iowait.c` | Per-fd, per-direction coroutine parking on the poller. |
| Blocking pool | `dynpool.c` | Elastic thread pool for blocking work. |
| Coroutine pool | `copool.c`, `arena.c` | Reusable-slot caches over fresh arena storage. |
| Coroutines | `minicoro/` | Bundled stackful coroutine primitive and backend storage layout. |

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

While `runtime_run()` is fully active, the `runtime_shutdown()` signalling path
is a thread-safe one-shot: a CAS on `g_shutdown` ensures the stop semaphore is
posted exactly once. Calls during initialization or teardown are outside that
contract. Shutdown is requested either explicitly by the user or automatically
by the idle callback when the last coroutine exits.

External runtime users are owned by a live coroutine. That owner remains alive
until every external thread has stopped and been joined, with no external
runtime call still in flight. Consequently `xylem_shutdown()` and automatic
shutdown do not race with external `xylem_spawn()` calls. After scheduler stop
begins, spawn, ready, and timer submission reject or ignore new work; cleanup
operations remain available until scheduler destruction.

**Teardown order matters** and is fixed to avoid use-after-free:

```
scheduler_stop()     // non-worker caller stops + joins; memory stays allocated
dynpool_destroy()    // join blocking threads; late scheduler_coro_ready() calls
                     // from finishing tasks still see a valid scheduler
scheduler_destroy()  // now free scheduler memory
```

On the runtime teardown path, the non-worker `scheduler_stop()` caller joins
workers without freeing scheduler memory. A dynpool thread that finishes a
blocking task afterward can still call `scheduler_coro_ready()` safely; it
observes the stopped scheduler and returns without requeueing the coroutine.
Only after the dynpool is fully drained does `scheduler_destroy()` release
memory.

## 4. The runnable pool: three tiers

A coroutine that is ready to run lives in exactly one of three places, ordered
from cheapest to most contended:

1. **`runnext` (per-worker, single slot).** A LIFO hand-off used when a worker
   schedules a coroutine onto itself. Cache-hot; checked first on pop. Pushing
   a new coroutine here evicts the previous occupant down to the queue. Only
   the owning worker normally pops its `runnext`. As a last resort, after the
   local queue and every victim queue come up empty, a searching worker may
   steal another worker's `runnext`. The race with the owner's own pop is
   settled by an atomic exchange, so a coroutine is never run twice.
2. **Work-stealing queue (`wsq`, per-worker).** The owner pushes at the tail
   and pops from the head (FIFO, arrival order); other workers steal from the
   head too. It is a fixed-capacity, single-producer / multi-consumer ring.
   FIFO bounds how long any one coroutine waits. Default capacity 256 (power
   of 2).
3. **Global run queue (`runq`).** A mutex-protected intrusive singly linked
   MPMC FIFO. Two roles:
   overflow when a deque is full, and the **injection point** for any
   cross-thread `scheduler_coro_ready()` caller.

### Work-stealing queue representation and invariants

Each `wsq` owns a power-of-two array of atomic element pointers. `head` and
`tail` are ever-increasing `uint64_t` sequence counters modulo 2^64; the
physical array index is `counter & (capacity - 1)`, and `tail - head` is the
current logical length. Using 64 bits makes a stale CAS surviving a complete
counter cycle practically unreachable.

Only the owner writes `tail`. A push stores the pointer into its atomic slot
before storing `tail + 1`. The project uses the default sequentially consistent
C11 atomic operations, so a consumer that observes the new tail also observes
the published pointer. Consumers read candidate slots before attempting to
advance `head`. If another consumer wins the CAS, the loser retries with the
head value returned by the failed CAS.

Slots themselves are atomic for a separate reason: after one consumer advances
`head`, the producer may wrap and reuse the released physical slot while a
losing consumer is still reading its stale candidate batch. Atomic slot access
makes that overlap data-race-free; the losing consumer discards the stale read
after its head CAS fails.

`wsq_pop()` claims one oldest element. `wsq_pop_half()` and
`wsq_steal_half()` claim `min(ceil(available / 2), elems_cap)` elements with one
successful head CAS. The owner uses `pop_half` when a full local queue must
spill a batch to the global `runq`; thieves use `steal_half` to distribute
useful work without taking the victim's entire queue.

### `scheduler_coro_ready(sched, co)`

While the scheduler is running, wake sources first perform
`WAITING -> RUNNABLE`, then publish the coroutine:

- **From a worker of this scheduler:** push to `runnext`; if `runnext` was
  occupied, push the evicted coroutine to the local deque. If the deque is
  full, spill half of it plus the evictee to the global runq as a batch.
- **From any other thread** (a different scheduler's worker, a dynpool thread,
  application thread): push straight to the global runq and wake one worker.

The transition uses a strong CAS. A duplicate wake, wake-before-commit, or
attempt to ready a running coroutine is a protocol bug and aborts instead of
silently adding a duplicate runq entry.

After scheduler stop begins, `scheduler_coro_ready()` returns before the state
transition and publication.

After publication, `_sched_wake_worker()` reserves one parked or polling worker
as searching before signalling it. Up to `ceil(worker_count / 2)` workers may
search concurrently; further producers coalesce once that limit is reached.
Parked workers are signalled through their semaphore; a blocked poll owner is
signalled through the poller wakeup fd.

### `scheduler_coro_ready_batch(sched, coros, count)`

Normally transitions every coroutine from `WAITING` to `RUNNABLE`, appends runq
nodes in fixed-size batches, and performs **one** wake. This amortizes lock and
signal cost when one poll pass releases many waiters. A call from the sole
worker of a single-worker scheduler instead queues each coroutine locally.
After scheduler stop begins, the whole operation is a no-op.

### Coroutine lifecycle and park commit

Scheduler state is separate from minicoro's execution status:

| Transition | Operation |
|------------|-----------|
| `NEW -> RUNNABLE` | `_sched_coro_start()` after creation |
| `RUNNABLE -> RUNNING` | `_sched_worker_execute_runnable()` |
| `RUNNING -> RUNNABLE` | `scheduler_coro_yield()` |
| `RUNNING -> WAITING` | park commit begins |
| `WAITING -> RUNNABLE` | wake source or declined commit |
| `RUNNING -> DEAD` | `_sched_coro_exit()` |

`scheduler_coro_park()` records a commit callback and yields. After
`mco_resume()` returns to the worker, `_sched_coro_commit_park()` changes
`RUNNING -> WAITING` and invokes the callback. The callback follows a strict
publication contract:

1. A successful callback's final shared operation publishes the waiter.
2. After publication it may only return `true`; it must not touch the
   coroutine, callback argument, or parked-on object again.
3. A callback returning `false` must not have published the waiter. The worker
   performs `WAITING -> RUNNABLE` and requeues the coroutine locally.
4. Wake sources arbitrate one winner in their own waiter protocol, then call
   `scheduler_coro_ready()`.

This lets a wake source ready and even execute the coroutine immediately after
the commit point without overlapping any callback work. Locks, tagged waiter
states, or durable conditions close each resource's check-versus-publication
window; the scheduler no longer contains a separate park-only handshake.

```
 Parking worker                         Wake source
       |                                     |
 RUNNING -> WAITING                          |
 run commit callback                        |
 prepare waiter fields                      |
 publish waiter (final shared operation) -->| claim waiter
 return true                                | WAITING -> RUNNABLE
       |                                     | enqueue
       |                                     | RUNNABLE -> RUNNING
       v                                     v
```

Shutdown does not run park cleanup. If shutdown happens before or after a park
record is published, the coroutine is stranded until teardown;
`xylem_run()` return remains the runtime lifetime boundary.

### Spawn fairness

`scheduler_coro_spawn()` is a scheduling primitive: it creates a coroutine,
publishes it as runnable, and does not yield the caller. `runtime_spawn()` adds
the runtime fairness policy. Each successful spawn from a runtime coroutine
consumes one cooperative step. When the current step budget is exhausted, the
caller yields after publishing the child and resumes with a fresh step budget.
This bounds runnable accumulation when a single worker repeatedly spawns
without performing another cooperative operation. Calls from plain OS threads
do not own a worker step budget and only enqueue the child.

The API does not guarantee that a child starts after the spawn call returns.
Other workers could already run it, and step exhaustion gives a single worker
the same opportunity. Failed spawns neither consume a step nor yield.

### Cooperative time slice

Each coroutine resume starts a new 1 ms UTC time slice.
`runtime_consume_time()` reports whether that slice has elapsed; it does not
preempt or yield the coroutine by itself. A caller that receives `true` must
call `runtime_yield()`. A clock value earlier than the recorded resume time is
treated as an exhausted slice so a UTC rollback cannot extend that run
indefinitely.

Successful network operations check the time budget and yield when it is
exhausted. Synchronization operations and coroutine spawn use a step budget
instead, avoiding a clock read on their short, fixed-cost paths. Pure
computation must call `runtime_consume_time()` explicitly; the scheduler cannot
preempt a coroutine that reaches no cooperative check point.

## 5. Worker loop (`_sched_worker_entry_cb`)

Each worker thread calls `_sched_worker_find_runnable()`, which keeps searching
or waiting until it returns a coroutine or the scheduler stops:

1. **Maintenance.** Fire due timers.
2. **Occasional global pull.** Every 61st tick (61 is prime, so it never
   resonates with the power-of-two deque sizes), pull one coroutine from the
   global runq and do a non-blocking poll, so global work and I/O can't starve
   behind a hot local deque.
3. **Find work** via `_sched_worker_find_runnable()`:
   - Pop local (`runnext` → deque → fair share of the global runq).
   - If nobody owns the poll driver, do a **non-blocking** poll and take any
     coroutine it produces.
   - Try to **steal** from other workers' deques (half at a time).
   - Otherwise try to become the **poll driver** (CAS on `poller_running`).
     The driver does the blocking poll.
4. If no work was found and this worker isn't the driver, publish
   `WORKER_WAITING`, then recheck all runnable pools. This closes the race with
   producers deciding whether a worker needs to be woken.
5. If the final recheck is still empty, `_sched_worker_wait()` waits on the
   worker semaphore with a timeout equal to the next local timer. After wake or
   timeout, runnable discovery restarts from timer maintenance.

The worker entry executes the returned coroutine through `_sched_worker_execute_runnable()`:
transition `RUNNABLE -> RUNNING`, call `mco_resume()`, then handle exit, yield,
or coroutine park. This is the only coroutine execution point in the worker
loop.

On shutdown the loop exits without draining queued or parked coroutines.
`scheduler_destroy()` destroys any remaining registered coroutines without
resuming user cleanup paths.

### The poll driver

Exactly one worker at a time owns the blocking poll, selected by a CAS on
`poller_running`. The driver:

- Publishes `WORKER_POLLING` under `worker_state_lock` before re-checking for
  work. A producer either races before the recheck and is observed, or reserves
  the polling worker as `WORKER_SEARCHING` and pokes the wakeup fd.
- Computes the poll timeout from the nearest timer deadline across **all**
  workers (`_sched_timer_poll_timeout`), not just its own, so it wakes in time to
  service a timer owned by a worker that is busy in a long coroutine.
- After waking, greedily drains additional ready events with zero-timeout
  polls, fires due timers for **every** worker (timer stealing,
  repeated `_sched_timer_process_due` calls, see §8), then runs the first ready
  coroutine and schedules the rest.

This "one driver, many waiters" design means there is no dedicated I/O thread:
whichever worker happens to go idle takes over polling, and it relinquishes the
role the moment it finds a runnable coroutine.

The no-lost-wakeup boundary is the worker state lock. Before blocking, a worker
publishes `WORKER_WAITING` or `WORKER_POLLING`, then rechecks all runnable pools.
A producer that needs help takes the same lock, reserves one idle worker as
`WORKER_SEARCHING`, updates `num_idle/num_searching`, and only then signals it.
Semaphore waiters receive a post; a polling worker receives a wakeup-fd event.
Because reservation and idle publication are serialized, the worker either
sees the work during its final recheck or the producer sees an idle state and
wakes it. Searchers suppress redundant wakeups after half of the workers are
searching and extend the wake chain after finding work.

## 6. Coroutines (minicoro + pooling)

Coroutines are stackful, provided by the bundled `minicoro` library. Each has a
configured fixed-size stack (`coro_stack_size`, default 128 KiB).

`scheduler_coro_spawn()` allocates a `_sched_coro_ctx_t` (entry fn, arg, intrusive runq and
registry nodes), creates the coroutine, links it into the scheduler's
**registry** (so leaked-but-alive coroutines can be destroyed at teardown),
bumps the `alive` counter, and calls `_sched_coro_start()` for
`NEW -> RUNNABLE`. When a coroutine returns, `_sched_coro_exit()` performs
`RUNNING -> DEAD`, unlinks it from the registry, frees it, decrements `alive`,
and — if it was the last one — fires the idle callback that shuts the runtime
down.

### Coroutine allocation ownership

Coroutine allocation is split across two ownership paths:

```
scheduler -> minicoro
scheduler -> copool
scheduler -> arena -> platform-vmem
```

Minicoro owns descriptor layout, coroutine construction, and backend-specific
storage preparation. Local and shared copools are independent containers that
store slot addresses plus fresh/reusable state without owning slot memory.
Arena owns page-aligned addresses, reservations, and full-slot decommit.
Scheduler connects the paths through the persistent minicoro descriptor's
allocator callbacks.

The public `coro_stack_size` configuration flow is unchanged. Runtime options
copy it into `scheduler_opts_t`; scheduler creation passes the selected value to
`mco_desc_init()` and stores the result as the persistent `sched->coro_desc`.
The descriptor retains the scheduler allocator callbacks and data unchanged.
`runtime.c` injects `MCO_GET_PAGE_SIZE()` and commit/decommit hooks backed by
`platform-vmem`. Windows x64 ASM applies its coroutine-specific `PAGE_GUARD`
directly inside minicoro without exposing stack-layout accessors to Xylem.

Each spawn copies the persistent descriptor, changes only `user_data`, and
calls `mco_create()` directly. Its allocator callback first checks the current
worker's local pool and then the shared pool or arena. The callback returns the
slot together with its explicit `MCO_STORAGE_FRESH` or `MCO_STORAGE_REUSABLE`
state. A fresh slot receives the backend-specific commit and guard layout; a
reusable slot skips VM preparation and restores its saved state. The lower-level
`mco_init()`
path assumes its caller already supplied accessible storage and performs no VM
preparation. The persistent descriptor outlives every coroutine pool. Teardown
destroys registered coroutines, worker-local pools, and the shared pool before
destroying the arena.

Successful `mco_destroy()` returns storage as `MCO_STORAGE_REUSABLE`. A failure
while preparing or initializing a coroutine returns it as `MCO_STORAGE_FRESH`,
so the scheduler bypasses both caches and returns the slot to arena for full
decommit.

On return to copool, the slot enters the worker-local or shared cache unchanged.
Windows ASM therefore retains its committed stack extent, moving guard, and
saved `StackLimit`. Minicoro reads and validates that saved limit before
reinitializing the context, then restores it after context creation. Windows
Fiber and Unix likewise retain their reusable state.

Slot state follows three transitions:

```
fresh arena slot -> local/shared cache or mco_create -> runnable coroutine
used coroutine   -> local/shared cache               -> reusable slot
cache fallback   -> arena full decommit              -> fresh slot
```

Local and shared pools have separate ownership and APIs. Each worker owns one
`copool_local_t`; the scheduler owns one `copool_shared_t` and the arena. Neither
pool knows the slot size or calls virtual-memory operations:

- A worker-local pool is a lock-free fixed array with configurable capacity;
  scheduler workers use the default of 64. Allocation pops locally first. An
  empty local pool takes up to 32 slots from shared, then from arena. A full
  local pool moves 32 slots to shared before retaining the newly freed slot.
- The shared pool is an unbounded `list_t` protected by a spinlock. Every active
  entry has an external metadata node containing the slot and its
  fresh/reusable state. Nodes are allocated on put and freed on take; empty nodes
  are not cached. Put appends at the tail and take removes from the tail,
  prioritizing recently returned reusable slots.
- An external allocator caller takes one shared slot. If shared is empty, the
  scheduler allocates up to 32 fresh arena slots, returns one, and places the
  surplus in shared. A shared-node allocation failure returns unstored slots to
  arena for decommit. Shared locking and arena locking are never nested.

The arena owns fixed-size, page-aligned slots and an external `void*` free-slot
array; no allocator metadata is stored inside a free slot. The aligned slot
size may not exceed 1 MiB. Creating an arena eagerly reserves its first region.
Each region contains at least 64 slots and is at most 64 MiB: reservation starts
at `floor(64 MiB / slot_size)` slots and halves on failure until a 64-slot
attempt. The new region is fully decommitted before any address is published.
An allocation attempts at most one region growth and removes fresh addresses
from the free array without touching their pages, so it may return a partial
batch when growth fails. Local and shared pools may cache those fresh addresses
without touching their pages; minicoro prepares one only when it is selected for
a coroutine.

The arena mutex protects region metadata and the free-slot array, while full
decommit runs before the mutex is acquired. Regions remain reserved through all
slot reuse and are released in full only when scheduler teardown destroys the
arena. Arena-backed slots therefore share region mappings, and VMA usage grows
with region reservations rather than coroutine count.

### Context-backend boundary and overflow detection

For embedded-stack backends, minicoro places the coroutine object, context,
storage, and stack in the arena slot. Windows x64 ASM additionally page-aligns
the embedded stack after metadata and prepares the initial guard/top pages when
a fresh slot becomes reusable. Later reuse preserves the current guard and
committed extent. Unix mappings remain addressable in both states; macOS fresh
slots leave `MADV_FREE_REUSABLE` through `MADV_FREE_REUSE` before initialization.
On the Windows Fiber backend, the slot holds only the coroutine object, context,
and storage; `CreateFiberEx()` and `DeleteFiber()` own the external Fiber stack.

Minicoro retains its delayed range checks on backends that use them. ASAN builds
also use minicoro's sanitizer fiber-switch integration, while arena explicitly
unpoisons allocated slots and poisons slots after successful decommit.
Scheduler resume and yield paths abort on every non-success minicoro result.
This includes `MCO_STACK_OVERFLOW`, because execution cannot safely continue
after the delayed check detects that the stack already crossed its range.
Native Windows `EXCEPTION_STACK_OVERFLOW` remains an SEH exception and is not
converted into a minicoro result. The page-level backend contracts are detailed
in [`platform.md`](platform.md) §5.

#### Operating-system resource limits

The pool does not impose a fixed limit on active coroutines. Allocation can
still fail when a new region cannot be reserved or a slot cannot be committed.
On Windows, reserve uses `MEM_RESERVE` and a successful arena return uses
`MEM_DECOMMIT`; destroying the arena releases each complete region with
`MEM_RELEASE`. Windows ASM cached slots retain committed metadata plus their
current lazy stack extent, while Fiber slots commit their complete arena
metadata block and keep the Fiber-owned stack outside it. Returning or
allocating a cached slot performs no VM operation.

On Linux, each complete arena region is one anonymous writable mapping and
remains subject to the system memory commit policy. Slot decommit uses
`MADV_FREE`, so physical pages may remain resident until memory pressure and
recommitted contents are always treated as unspecified. The per-process VMA
count grows with arena regions rather than active or cached slots; region
reservation can still reach `vm.max_map_count`, address-space, or commit-policy
limits.

## 7. I/O parking (`iowait.c`)

`iowait` bridges a coroutine and the platform poller. One handle wraps one fd
and tracks **two independent directions** (read, write), each holding at most
one parked coroutine.

### Park / wake handshake

The subtlety is avoiding a wake between the final state check and coroutine
publication. Each direction uses `NONE / WAIT / READY / co`:

1. The waiter consumes a retained `READY` before checking errors.
2. Without `READY`, it reads `poll_info` and the direction deadline. Result
   priority is `CLOSED`, `TIMEOUT`, then `ERROR`.
3. It reserves the slot with `NONE -> WAIT`, then repeats the result check.
   If an error appeared, `_iowait_transition_waiter(..., IOWAIT_WAITER_NONE)`
   cancels the reservation while preserving any concurrent `READY`.
4. `scheduler_coro_park()` yields the coroutine and invokes
   `_iowait_wait_commit_cb()`. The callback's final operation is the CAS
   `WAIT -> co`; it returns false if a wake source already consumed `WAIT`.
5. After resume, the waiter consumes `READY` first, otherwise checks
   `poll_info` again. A deadline reset can turn a timeout wake into a harmless
   retry.

The fd is already registered with the kernel poller before parking — it was
armed at `iowait_create` (and, on LT+ONESHOT, re-armed in `iowait_process_event`
after each event). Park never touches the poller.

The same steps as a time-ordered diagram (top to bottom is time; `|` is each
actor's timeline):

```
 Coroutine       rd.waiter     scheduler_coro_park    wake source
     |                |                 |                  |
     | NONE -> WAIT   |                 |                  |
     | re-check poll_info/deadline      |                  |
     |--------------------------------->| mco_yield()      |
     |                |<----------------| WAIT -> co       |
     |                |                 |                  |
     |                |<-----------------------------------|
     |                | co -> READY/NONE; return old co    |
     |<----------------------------------------------------|
     | ready(co), resume, consume READY or poll_info        |
     v                v                 v                  v
```

### Three wake sources, one winner

A parked coroutine can be woken by an **I/O event**, a **deadline timer**, or
**`iowait_close()`**. Each tries to claim the parked coroutine from the
per-direction slot; only the claimant that observes the coroutine pointer
actually readies it. I/O readiness is cached as `READY` in the slot for
the next parker to consume without blocking. Deadline expiry is latched in the
direction-specific bit of the handle's `poll_info` word and cleared whenever
that deadline is reset or disabled. If a deadline timer wakes the coroutine
but the deadline is reset before it resumes, the cleared timeout bit makes
that wake spurious and the wait retries.

```
   I/O event      deadline timer     iowait_close          waiter (atomic)
       |                |                  |        NONE | WAIT | READY | co
       |                |                  |                     |
       |  -- the three sources may fire concurrently --          |
       |  transition to READY, return old co ------------------->|
       |                | publish timeout; transition to NONE -->|
       |                |                  | publish CLOSED; ---->|
       |                |                  | transition to NONE   |
       |                |                  |                     |
       |  only the transition that observes co returns it        |
       |                                                         |
       |  winner: ready(co)  -------------------------------->  Coroutine
       |                                                         |
       |  on resume, co consumes READY or reads poll_info         |
       |  to report TIMEOUT / ERROR / CLOSED, or retries          |
       v                                                         v
```

### Generation-tagged slab — rejecting stale events

iowait handles are allocated from a per-scheduler **paged slab** with a
free-list. The poller's user-data is a `(generation, slab-index)` pair packed
into a `uintptr_t` (16 generation bits, the rest index; layout validated by a
`_Static_assert` and works on 32- and 64-bit). When a handle is retired its
generation is bumped and the slot returns to the free list. If a completion
event arrives for a recycled slot, `_iowait_try_ref()` compares the event's tag
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
  fd — `iowait_process_event` syncs `interest` to `NO_OP` and calls
  `platform_poller_mod(RW_OP)` again under `poll_lock` with a closed re-check.
  ET mode skips this entirely.

Read readiness, write readiness, and LT re-arm are handled under one
`poll_lock` acquisition per completion event. After releasing the lock,
`iowait_process_event()` returns up to two replaced coroutine pointers in a
fixed-capacity output array.

`poll_lock` belongs to one `iowait` handle; unrelated fds never share it. It
gives close and readiness publication a total order: readiness first retains
`READY`, while close first prevents later readiness and LT re-arm. On ET
platforms the event-side critical section contains only the closed check and
waiter transitions. On LT+ONESHOT it also contains `platform_poller_mod()`, and
close contains the matching `platform_poller_del()`.

Short-lived connections contend only when a completion races with close on
the same fd. A waiting poller worker can temporarily delay the rest of its CQE
batch, so coroutine queue placement remains outside the lock. Keeping poller
modification and deletion inside the critical section avoids a re-arm-after-
close race and makes `iowait_close()` a synchronous fd barrier.

The scheduler owns runnable placement.
`_sched_worker_dispatch_poller_runnables()` consumes each event result
immediately: the first runnable becomes `run_now`, subsequent runnables go to
the polling worker's local deque, and local overflow is batched into the global
run queue. `iowait` does not know about these queueing choices and never flushes
scheduler batches itself.

Because the fd is always registered (ET) or re-registered before the next
park (LT), the commit callback never touches the poller. Its only operation is
the final `WAIT -> co` publication CAS.

`iowait_close()` drops the poller subscription **synchronously** under
`poll_lock` before waking parked coroutines, so the caller can close the
underlying fd right afterward without racing a deferred `EPOLL_CTL_DEL` against
a recycled fd number. It is idempotent and may race with parked waits, a second
close, or deadline setters.

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

- Due timers are popped in `_sched_timer_process_due()`, which either runs the
  callback **inline on the firing thread** or, if `spawn` is set, normally runs
  it in a fresh coroutine. If spawning that coroutine fails, the fire is
  completed without invoking the callback, and the normal completion/ud_unref
  path still runs.
- Optional userdata guards pin callback state across dispatch. `ud_ref` runs
  under `timer_lock` and must be a trivial reference acquisition. `ud_unref`
  runs after the lock is released while the timer fire reference is still held;
  it may therefore perform final userdata teardown, including destroying the
  firing timer, but must not re-arm it or retain userdata after release.
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
  owner worker's `timer_lock`; `start`, `reset`, and `stop` are thread-safe while
  the scheduler is running. Timer `create`, `start`, and `reset` must not race
  with `scheduler_stop()` or `scheduler_destroy()`; sequential calls after stop
  are rejected or ignored. An already dispatched callback cannot be withdrawn
  by `stop()`, but `stop()` does prevent a repeat requeue or a deferred reset
  from the same firing round.
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
opaque, single-owner handle. `xylem_timer_after()` is one-shot;
`xylem_timer_every()` repeats only after the previous callback returns, and
`xylem_timer_reset()` preserves the callback/user data while re-arming the next
fire. `xylem_timer_stop()` and `xylem_timer_reset()` do not consume the handle
and may be called from its callback. Resetting a timer from inside its own
callback schedules a deferred re-arm; resetting it again before the callback
returns overwrites that deferred re-arm and reports cancellation. Stop takes
priority over a reset requested from the same callback. The owner must release
the handle exactly once with `xylem_timer_destroy()` after any callback-side
timer operations have finished; destroy stops the timer as a fallback but must
not be called from the callback or race with another operation on the same
handle. Destroy is not a callback completion barrier: a callback dispatched
before stop/destroy may still run afterward, so the caller must keep its
callback and user-data resources alive until that callback returns.
Public timer callbacks use the spawned path. If allocation of the per-fire
context or callback coroutine fails, that fire is dropped without an
asynchronous error: a one-shot is not retried, while a periodic timer continues
with its next interval.

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

`xylem_sleep(ms)` is built directly on this: its park commit arms a one-shot
timer, and the timer callback readies the sleeping coroutine.

### Timer stealing — who fires whose timers, and where they run

A worker normally drains its own heap on the local `_sched_timer_process_due` path.
But a worker stuck running a long coroutine never reaches that path, which would
delay its timers. To bound that, the **idle poll driver also fires due timers
for every worker** by calling `_sched_timer_process_due()` after each poll wake. This
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
- **Spawn timer (`spawn == true`):** `scheduler_coro_spawn()` builds a coroutine and
  routes it through `_sched_enqueue_runnable`, which keys off the *running* worker
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
2. `scheduler_coro_park()` the coroutine; in the commit callback,
   `dynpool_submit()` is the final publication operation. If submission fails,
   the callback returns `false` so the coroutine is rescheduled immediately and
   `runtime_submit()` reports `-1`.
3. A pool thread runs `fn(arg)`, then calls `scheduler_coro_ready()`. While the
   scheduler is running, this resumes the coroutine through a cross-thread
   wakeup that lands on the global runq. After scheduler stop begins, the call
   is a no-op and the parked coroutine remains registered for scheduler
   destruction.
   If shutdown destroys the pool before a queued task runs, process-exit
   teardown owns the remaining submit context.

The cross-thread handoff (top to bottom is time):

```
 Coroutine        runtime_submit /        dynpool            worker that
 (on worker A)    commit callback         thread             picks it up
     |                 |                    |                     |
     | xylem_await()   |                    |                     |
     |---------------->|                    |                     |
     |                 | coro_park():       |                     |
     | (suspended) ... | publish co into    |                     |
     |                 | ctx, then          |                     |
     |                 | dynpool_submit() ->|                     |
     |                 |                    | run fn(arg)         |
     |   meanwhile worker A runs OTHER coroutines                 |
     |                 |                    | (blocking work)     |
     |                 |                    |                     |
     |                 |                    | done -> ready(co)   |
     |                 |                    | push to global runq |
     |                 |                    | wake a worker ----->|
     |                 |                    |                     | pop co
     |<----------------------------------------------------------| resume co
     v                 (may resume on a DIFFERENT worker than A)  v
```

While the scheduler is running, the coroutine can resume on **any** worker, not
necessarily the one it parked on — the wakeup is a plain cross-thread
`scheduler_coro_ready()` into the global runq. If `dynpool_submit()` fails, the
commit callback returns `false`, the coroutine is rescheduled immediately, and
`runtime_submit()` returns `-1`.

The pool spawns threads on demand up to `max_threads` (default 512) and lets
idle threads exit after `idle_timeout` (default 10 s). Task submission holds
the pool mutex while it enqueues the task and updates worker lifecycle state;
the condition variable wakes idle workers when the queue or shutdown state
changes.

### DNS resolve arbitration

`addr_resolve()` has two completion sources: the pool job and an optional
deadline timer. Its heap context is reference-counted because `getaddrinfo()`
cannot be cancelled and may continue after the caller times out. A mutex guards
`PENDING / DONE / TIMEOUT` and the single waiter pointer.

The park commit submits the job and arms the timer before locking the context.
If either source already selected a terminal state, the callback returns false.
Otherwise it stores the coroutine pointer and unlocks as its final publication
operation. The worker and timer select one terminal state under the same lock,
detach the waiter, unlock, and call `scheduler_coro_ready()` outside the lock.
The losing source only drops its reference.

## 10. Concurrency invariants

- **While the scheduler is running, a parked coroutine is requeued exactly once
  after waiter publication.**
  The worker performs `RUNNING -> WAITING` before the commit callback. A
  successful callback publishes the waiter as its final shared operation;
  the single winning wake source then performs `WAITING -> RUNNABLE`.
- **At most one reader and one writer per `iowait` direction.** Enforced by the
  exchange-on-publish check; violations `abort()`.
- **Mux stream conditions and waiters share one lock.** Receive data, send-window
  updates, FIN/reset/close, and waiter commit all inspect or detach the waiter
  under the stream lock, then ready it after unlocking.
- **Stale completion events are rejected**, not tolerated — via generation tags.
- **No lost wakeups against idle workers.** Idle publication and wake reservation
  are serialized by `worker_state_lock`; polling workers are signalled through
  the wakeup fd.
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
| `max_threads` | `dynpool_opts_t` | 512 | Max blocking-pool threads. |
| `idle_timeout` | `dynpool_opts_t` | 10000 ms | Blocking-pool idle thread lifetime. |
| `coro_stack_size` | `xylem_opts_t` / `runtime_opts_t` / `scheduler_opts_t` | 128 KiB | Per-coroutine stack size. |

## 12. Related docs

- System overview and how protocols sit on the runtime:
  [`../architecture.md`](../architecture.md).
- Platform poller / socket / vmem details:
  [`platform.md`](platform.md).
- Per-protocol designs (TCP, UDP, TLS, …): [`../design/`](.).
- Runtime test design: `../test/runtime.md` *(planned)*.
