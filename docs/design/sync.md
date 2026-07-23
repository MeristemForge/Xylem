# Synchronization Primitives Design

Xylem's public synchronization primitives -- mutex, condition variable,
wait-group, semaphore, and channel -- are **cross-context**: a contended
operation blocks the caller in the way that fits its context. A coroutine parks
on the scheduler (the OS worker thread stays free); an external OS thread
blocks on a per-thread wake object. The two kinds can notify each other, so a
coroutine can hand off to an OS thread and vice versa. This document covers
their semantics, threading contracts, and the ordering rules that make them
correct. The internal `spin_t` and `thrd_wake_t` helpers are also described
because they back the public primitives.

Sources: public headers in `include/xylem/sync/`, implementations in
`src/sync/`. Per-thread OS wake support is in `src/sync/thrd-wake.{h,c}`;
the internal spinlock is in `src/sync/spin.{h,c}`.

Prerequisite: the parking model in [`runtime.md`](runtime.md)
(`scheduler_coro_park`, `scheduler_coro_ready`).

## 1. The common model

All five public primitives share one shape:

- **Blocking ops are context-adaptive.** `mutex_lock`, `cond_wait`,
  `waitgroup_wait`, `channel_recv`, and `sem_wait` may block. On a coroutine
  they park (the worker thread stays free); on any other thread they block that
  OS thread. None of them requires a coroutine context.
- **Public sync APIs use `[CONTEXT-ADAPTIVE]`.** In sync headers, this tag means
  the function is usable from coroutine and OS-thread contexts. Some calls do
  not block; those document their same-handle race and final-release rules in
  the function text.
- **Wakeup/non-blocking ops do not wait on the primitive state.** `unlock`,
  `signal`/`broadcast`, `add`/`done`, `send`/`close`, `post`, and all
  `create`/`destroy` are safe from any thread. Cross-context coroutine wakeups
  go through `scheduler_coro_ready()`; OS-thread wakeups use the primitive's
  platform blocking path. In coroutine context, `unlock`, `signal`,
  `broadcast`, `done`, `send`, and `post` may perform a cooperative yield for
  runtime fairness.
- **FIFO wake target selection where the primitive owns a waiter list.** Mutex
  and cond choose the FIFO-oldest waiter to wake, waitgroup drains waiters in
  FIFO order, and sem hands tokens to FIFO-oldest waiters. Only sem transfers a
  token directly to the waiter. Mutex waiters and cond waiters re-contend for
  their mutex after waking, so their return/acquire order is not guaranteed to
  be FIFO.
- **Abort on contract violation.** Counter underflow, double-close, and
  multi-receiver are logic bugs, so misuse aborts with a diagnostic rather than
  corrupting state silently. (None of the primitives aborts merely for being
  called off-coroutine anymore — that is the point of cross-context.)

| Primitive | Blocking op (any context) | Other context-adaptive ops | Pattern |
|-----------|---------------------------|----------------------------|---------|
| `xylem_mutex` | `lock` | `unlock`, `trylock`, create/destroy | cross-context lock |
| `xylem_cond` | `wait` | `signal`, `broadcast`, c/d | paired with a mutex |
| `xylem_waitgroup` | `wait` | `add`, `done`, c/d | countdown latch |
| `xylem_channel` | `recv` / `recv_timeout` | `send`, `close`, c/d | MPSC message passing |
| `xylem_sem` | `wait` / `timedwait` | `post`, c/d | counting semaphore |

### Waiter representation

The FIFO primitives keep their own guarded waiter lists: a `spin_t` guard plus
an intrusive `list_t` embedded in the primitive. A waiter record is tagged as a
parked coroutine or a blocked OS thread. Coroutine waiters store the `mco_coro*`
to reschedule with `scheduler_coro_ready()`; OS-thread waiters store a
`thrd_wake_t*`, a per-thread wake object created lazily and cached in TLS.

The waker selects or drains waiter records under the guard and copies out each
wake target before releasing the guard. This keeps a stack waiter valid until
the waker has entered `thrd_wake_signal()`, which takes an in-flight reference
before publishing the wake token. The target thread may then resume, return, and
release its TLS owner while the wake object remains alive through the platform
wake. The waker never accesses the stack waiter after handing the target to
`thrd_wake_signal()`. The channel keeps specialized machinery instead of a FIFO
waiter list, because it has a single-receiver wake slot and a different
timed-wait path.

### Cross-context wake cost

Because the wake path is chosen by the *waiter's* kind, the context pairings of
a blocking hand-off are not equally cheap. The current `benchmark/sync` suite
reports `cc`, `tt`, `ct`, and `tc` where each primitive and comparison language
can express that pairing. It does not include a separate raw handoff probe.

The pure-coroutine reschedule is usually the cheapest hand-off. Crossing the
coroutine/OS-thread boundary costs extra dispatch work beyond the bare thread
wake/reschedule: waking a coroutine from an OS thread routes through
`scheduler_coro_ready()` to the global run queue and may need to wake a parked
worker, while waking a thread from a coroutine signals that thread's wake
object from off-scheduler.

Design consequence: keep a hot, tight hand-off inside a single context when you
can, and treat each boundary crossing as a real cost on the path. When a
producer/consumer pair must straddle the boundary, prefer **batching across it**
over a per-item ping-pong -- which is exactly why `xylem_channel` never waits
for capacity or a receiver (queue, drop by caller policy, or retry later
instead of a blocking back-and-forth; see section 5). A coroutine producer may
still cooperative-yield for runtime fairness after enqueue/wake.

## 2. Mutex

A cross-context lock. Ownership is held between `lock()` and `unlock()` by
whoever acquired it — a coroutine or an OS thread — not by the OS thread
identity. A contended `lock()` blocks the caller (park or OS-thread block).
When the holder unlocks, it releases the lock and wakes the FIFO-oldest waiter
if one is queued; the resumed waiter then re-contends for ownership.

- **`lock()` works from any context.** The uncontended fast path is a single
  lock-free CAS. On contention a coroutine parks and a thread blocks on its
  per-thread wake object; the acquire predicate (the same CAS) is re-checked
  under the primitive's own guard before enqueue, so an `unlock()` racing the
  block cannot be missed.
- **`unlock()` releases before wake.** It clears the owned flag, pops the
  FIFO-oldest waiter, then wakes it. The woken party loops back through the same
  CAS before returning from `lock()`. This avoids ownership tracking by OS
  thread identity, but it also means mutex acquisition is not strict FIFO.
  `trylock()` is a lock-free CAS, callable from any context.

## 3. Condition variable

Pairs with a mutex; pthread-style **edge-triggered** semantics — no missed
signals are accumulated, so the predicate must be re-checked in a `while` loop
to absorb spurious or bursty wakeups.

```c
xylem_mutex_lock(m);
while (!predicate()) {
    xylem_cond_wait(c, m);     // atomically: enqueue + unlock, park; re-lock on wake
}
/* predicate holds, still under m */
xylem_mutex_unlock(m);
```

### The enqueue-before-unlock ordering

`wait()` must atomically (1) put the caller on the cond's waiter list and (2)
release the mutex, then park. The order — **enqueue, then unlock** — is what
prevents a lost wakeup:

```
 Waiter coroutine            mutex m            cond c          Signaler
     |                         |(held by waiter) |                 |
     | cond_wait(c, m):        |                 |                 |
     |   enqueue self on c ----------------------> waiter list     |
     |   release m  ---------->|(now free)       |                 |
     |   park (yield)          |                 |                 |
     |                         | lock(m) <------------------------ | (blocked until
     |                         |(held by signaler)                 |  waiter released m)
     |                         |                 | signal(c) <----- |
     |                         |                 | sees waiter,    |
     |<------------------------------------------- schedule it     |
     |   (resumes, re-locks m) | unlock(m) <------------------------|
     |   re-check predicate    |                 |                 |
     v                         v                 v                 v
```

Because a signaler is serialized through `m`, it cannot observe the released
mutex until the waiter is already linked on `c` and therefore visible to
`signal()`/`broadcast()`. A signal sent while no one is parked is simply
dropped (edge-triggered) — which is exactly why the predicate `while`-loop is
mandatory.

A signaler that does **not** take `m` (for instance an external thread that
only flips an atomic flag) must avoid lost wakeups out of band: set the
predicate flag *before* calling `signal`/`broadcast`, so a waiter that has not
yet parked sees the flag on its next predicate check.

- `signal()` wakes one waiter; `broadcast()` wakes everyone parked at the moment
  it takes its internal guard (later arrivals are unaffected). Waiter selection
  and broadcast wake order are FIFO, but each waiter re-locks `m`, so return
  order is not guaranteed to be FIFO. `signal()` and `broadcast()` consume one
  cooperative runtime credit per call.
- Destroying a cond that still has waiters is a caller bug (matches
  `pthread_cond_destroy`).

## 4. Wait-group

A countdown latch, modeled on Go's `sync.WaitGroup`:

- `add(delta)` registers pending work, **before** spawning the units it counts.
- `done()` decrements by one; when the counter hits zero, every parked waiter is
  drained and woken in FIFO order.
- `wait()` parks until the counter reaches zero; returns immediately if already
  zero. Multiple coroutines and OS threads may `wait()` concurrently.

Contract: `done()` more times than `add()` ever promised underflows the counter
and **aborts** (Go's "negative WaitGroup counter" panic). `add()` racing a
`wait()` that is already in progress is a logic error and unsupported — add up
front.

## 5. Channel

An **MPSC** message queue: many senders, exactly one receiver. The receiver
may be **either a coroutine or a plain OS thread** — the data path is
lock-free (intrusive MPSC queue) and the single receiver, when it must block,
publishes itself into one atomic wakeup slot. That slot stores `NONE`, the
coroutine park reservation `WAIT`, the durable `CLOSED` state, or a concrete
waiter pointer. Send, close, and timeout use CAS transitions on this slot to
select at most one waker without losing closure. This lets a coroutine producer
hand work to an OS-thread consumer (and vice versa). The receiver wake path uses
the same per-thread `thrd_wake_t` and scheduler reschedule mechanisms as the
FIFO primitives, but with channel-specific state instead of a shared waiter
module.

- `send(msg)` — context-adaptive, `msg` must be non-NULL. It never waits for
  capacity or a receiver; in coroutine context it may only cooperative-yield for
  runtime fairness after enqueue/wake. Returns `0` on success or `-1` (invalid
  input or allocation failure).
- `recv()` — blocks the calling coroutine **or thread** until a message
  arrives or the channel is closed-and-drained (then returns NULL).
  `recv_timeout(ms)` is the general form with a three-state wait policy:
  - `0` — non-blocking try: pop if a message is immediately available, else
    return NULL at once without blocking. Use this to drain (e.g. drop down to
    the newest frame) without risking a block.
  - `(uint64_t)-1` — block forever, identical to `recv()`.
  - any other `n` — block until the relative timeout expires according to
    `xylem_utils_getnow(MSEC)`. Because this is wall-clock based, system clock
    adjustments may shorten or extend the real elapsed wait.
  A NULL return does **not** distinguish "nothing available" / "timed out" /
  "closed and empty" — track the reason out of band if you need it.
- `len()` — context-adaptive best-effort in-flight count; useful for
  observability and soft drop/backoff thresholds.
- `create()`, `destroy()`, and `close()` are any-context operations. `create()`
  requires the runtime to be running so the channel can bind to the scheduler.
  `close()` may race with `recv()` to wake the receiver, but must not race with
  `send()`. `destroy()` must not race with any other channel API.
- **Single receiver.** Concurrent `recv()` (from two coroutines, two threads,
  or one of each) aborts.

### Capacity and backpressure

Channels are **unbounded**: `send` always queues (barring OOM) and never reports
full. In coroutine context it may cooperative-yield for runtime fairness, but it
does not wait for capacity or a receiver. Hard capacity is intentionally not
part of the channel contract. A correct hard bound would require an atomic
reservation inside `send` or an external synchronization primitive; a caller-side
`len() < threshold` check is only a snapshot and races with other senders.

Backpressure is therefore a caller policy. Use `len()` as a soft signal to drop,
yield, or retry above a threshold, or compose a semaphore with the channel when
a hard bound is required. Receivers can drain with `recv_timeout(ch, 0)` to drop
old backlog (dropping oldest, keeping newest).

Implementation: a single `_Atomic size_t count` tracks in-flight messages for
`len()`. `send` increments it for each queued node; `recv` decrements after a
successful pop. This is an observation count, not an allocation bound: each
message is still a per-node `malloc`. A bounded, zero-allocation MPSC would need
a preallocated MPMC ring, which this is not.

Close/lifecycle semantics are strict and abort-on-misuse:

| Operation race | Contract |
|----------------|----------|
| `close()` vs `recv()` | allowed; close wakes the receiver |
| `close()` vs `send()` | forbidden; callers stop producers first |
| `destroy()` vs any channel API | forbidden |

| After `close()` | Behavior |
|-----------------|----------|
| `recv()` | drains remaining messages, then returns NULL |
| `send()` | **aborts** |
| `close()` again, or `close(NULL)` | **aborts** |

| `destroy()` | frees node wrappers (not payloads); accepts NULL |

Payload ownership is always the caller's; the channel only manages its node
wrappers. `destroy()` should follow a drain.

## 6. Semaphore (`xylem_sem`)

A counting semaphore. Like the other primitives it bridges coroutines and OS
threads, and it adds two things they don't have: a **count** (so a `post()` with
no waiter is remembered) and a **timed wait**. Coroutine and OS-thread waiters
share one FIFO list. A posted token is handed directly to the FIFO-oldest
waiter; only posts that find no waiter increment the count.

- `wait()` decrements the count, or blocks if it is zero. It is
  **context-adaptive**: on a coroutine it parks (the worker thread stays free);
  on any other thread it blocks that OS thread. Waiters are FIFO across
  coroutine and OS-thread callers.
- `timedwait(ms)` is `wait()` with a relative timeout measured against
  `xylem_utils_getnow(MSEC)`: it returns `true` once a token is acquired, or
  `false` if the timeout elapses first. Because this is wall-clock based, system
  clock adjustments may shorten or extend the real elapsed wait. `timedwait(0)`
  is a non-blocking try (acquire-or-fail, never parks) and replaces the old
  `trywait`.
- `post()` hands the token directly to the FIFO-oldest waiter. If nobody is
  waiting, `post()` banks the token in the count. `create` and `destroy` never
  park; `post()` does not block on semaphore state, but may cooperative-yield
  from a coroutine.

### Who waits decides how it wakes

The poster chooses the wake path from the blocked party's kind:

- a **coroutine waiter** stores its `mco_coro*` and the scheduler it parked
  under, and is woken with `scheduler_coro_ready()` (callable from any caller);
- a **thread waiter** stores its per-thread futex wake object and blocks on
  that wake object until the posted token is handed to it.

So "thread posts -> coroutine waits" reschedules the coroutine, and "coroutine
posts -> thread waits" signals that thread's wake object. The branch is on the
blocked party's kind, not the poster's.

### Direct hand-off, no lost wakeups

A short spin `guard` serialises waiter queue mutations and count banking.
When waiters exist, `post()` transfers the token straight to the FIFO-oldest
waiter and never touches the count; the woken party returns from `wait()`
already owning the token. The wait fast path re-checks the count while holding
the guard during enqueue, so a `post()` racing the block either banks a token
that the waiter consumes immediately, or finds the waiter already queued and
hands the token to it.

### Timeout and waiter lifetime

Most waiters live on the blocked caller's stack. An infinite coroutine waiter
stays valid while the coroutine is suspended. For an OS-thread waiter, `post()`
copies the TLS wake-object target under the guard and enters
`thrd_wake_signal()` before the stack record can disappear. The signal path
takes an in-flight reference before publishing the token, so the target thread
may resume and release its TLS owner while the wake object remains alive through
the platform wake. `post()` never touches the stack waiter after handing off the
wake target.

A **timed coroutine wait** is the exception. It arms a scheduler timer that can
pull the waiter out of the FIFO from another worker, and that timer callback may
run concurrently with the resumed coroutine — so a stack record would be a
use-after-free. The timed coroutine waiter is therefore a small refcounted heap
object (one reference for the waiting coroutine, one for the armed timer), freed
by whichever side drops the last reference, exactly as `iowait` does for I/O
deadlines. The timer is armed *under the guard* inside the park commit callback
so a racing `post()` cannot dequeue and resume the coroutine before the timer is
live; on resume the waiter cancels the timer and reports timeout-vs-token from a
flag the timeout callback set while holding the guard.

The thread-side timeout needs no scheduler timer: `thrd_wake_timedwait` does the
blocking. On timeout the thread resolves the race with a concurrent `post()`
under the guard. If it is still queued it removes itself and reports timeout;
if a `post()` already dequeued it, it consumes the handed-off wake token and
reports success.

Like the other primitives, `xylem_sem` works from any context. A coroutine
waiter does require a running scheduler (that is how it is woken, and a timed
wait needs the scheduler's timer); a thread waiter needs no runtime at all.

## 7. Internal spinlock (`spin_t`)

`src/sync/spin.{h,c}` is a plain test-and-set spinlock used **internally** for
very short critical sections where parking would cost more than spinning — e.g.
the scheduler's coroutine-pool free list and registry, and the per-primitive
guard that serialises each FIFO primitive's waiter list. It does not interact
with the scheduler (no parking), so it must only guard sections that are short
and non-blocking. It is not part of the public API and carries no `xylem_` prefix.

The FIFO primitives (mutex, cond, waitgroup, sem) use it internally to guard
their waiter lists; the *waiters* block, but the list bookkeeping is protected
by a short spin.

## 8. Choosing a primitive

- **Mutual exclusion (coroutines, threads, or a mix):** `xylem_mutex`.
- **Wait for a condition on shared state:** `xylem_cond` + a mutex + a predicate
  loop.
- **Fan-out then join N tasks:** `xylem_waitgroup`.
- **Stream values from producers to one consumer:** `xylem_channel`.
- **Signal with a count or a deadline across the coroutine/OS-thread boundary:**
  `xylem_sem`.
- **Guard a tiny, non-blocking critical section in internal code:** `spin_t`.

All five public primitives span both the coroutine and OS-thread worlds. For raw
OS-thread-only synchronization the code can still use C11 `<threads.h>`
(`mtx_t`, `cnd_t`) directly via `xylem/xylem-threads.h`; these primitives are
preferred when a coroutine is (or may be) involved.

## 9. Related docs

- Parking / scheduling model: [`runtime.md`](runtime.md)
- Conventions (context annotations, abort policy): [`../conventions.md`](../conventions.md)
- Tests: [`../test/strategy.md`](../test/strategy.md) *(planned)*
