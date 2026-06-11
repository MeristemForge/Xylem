# Synchronization Primitives Design

Xylem's public synchronization primitives — mutex, condition variable,
wait-group, and channel — are **cross-context**: a contended operation blocks
the caller in the way that fits its context. A coroutine parks on the scheduler
(the OS worker thread stays free); an external OS thread blocks on a per-thread
wake semaphore. The two kinds queue together and notify each other, so a
coroutine can hand off to an OS thread and vice versa. This document covers
their semantics, threading contracts, and the ordering rules that make them
correct. The internal `spin_t` and the shared `waiter` core are also
described because they back the others.

Sources: public headers in `include/xylem/sync/`, implementations in
`src/sync/`. Shared waiter core in `src/sync/waiter.{h,c}`, internal spinlock
in `src/sync/spin.{h,c}`.

Prerequisite: the parking model in [`runtime.md`](runtime.md) (`scheduler_park`,
`scheduler_schedule`).

## 1. The common model

All five public primitives share one shape:

- **Blocking ops are context-adaptive.** `mutex_lock`, `cond_wait`,
  `waitgroup_wait`, `channel_recv`, and `sem_wait` may block. On a coroutine
  they park (the worker thread stays free); on any other thread they block that
  OS thread. Both waiter kinds share one FIFO queue, so a coroutine and an
  external thread can notify each other across the boundary. None of them
  requires a coroutine context.
- **Wakeup/non-blocking ops never block.** `unlock`, `signal`/`broadcast`,
  `add`/`done`, `send`/`close`, `post`, and all `create`/`destroy` are safe from
  any thread — a cross-context wake is dispatched by the *waiter's* kind
  (`scheduler_schedule()` for a coroutine, `platform_sem_post()` for a thread).
- **FIFO wakeups.** Waiters are resumed in arrival order.
- **Abort on contract violation.** Counter underflow, double-close, and
  multi-receiver are logic bugs, so misuse aborts with a diagnostic rather than
  corrupting state silently. (None of the primitives aborts merely for being
  called off-coroutine anymore — that is the point of cross-context.)

| Primitive | Blocking op (any context) | Non-blocking ops | Pattern |
|-----------|---------------------------|------------------|---------|
| `xylem_mutex` | `lock` | `unlock`, `trylock`, create/destroy | hand-off lock |
| `xylem_cond` | `wait` | `signal`, `broadcast`, c/d | paired with a mutex |
| `xylem_waitgroup` | `wait` | `add`, `done`, c/d | countdown latch |
| `xylem_channel` | `recv` / `recv_timeout` | `send`, `close`, c/d | MPSC message passing |
| `xylem_sem` | `wait` / `timedwait` | `post`, c/d | counting semaphore |

### The shared waiter core (`waiter`)

`src/sync/waiter.{h,c}` owns the parts that are identical across primitives:

- `waiter_t` — one blocked party, tagged `WAITER_CO` (a parked coroutine, woken
  via `scheduler_schedule`) or `WAITER_THR` (a blocked OS thread, woken via its
  per-thread `platform_sem`). The thread's wake sem is created lazily on first
  block and cached in TLS for the thread's lifetime.
- `waiter_wake` — a waker copies the wake target out (a by-value `waiter_t`)
  under the lock, releases the lock, then wakes; the waiter's storage (often a
  stack frame) may vanish the instant it resumes, so it is never touched
  afterward.

The FIFO primitives (mutex, cond, waitgroup) each keep their **own** guarded
list of `waiter_t` — a `spin_t` guard plus an intrusive `list_t` embedded in the
primitive — and inline the same small block/wake mechanics directly:
`mco_running()` decides park vs. OS-thread block, the acquire/visibility step is
done under the guard so a racing waker is never lost, and a drain reads each
node's successor before waking so a vanishing stack waiter cannot strand it. The
channel and sem skip the list entirely, keeping their own specialised machinery
(a lock-free single-slot fast path, a timed-wait timer); all of them share only
the `waiter` representation and wake dispatch.

## 2. Mutex

A hand-off lock. Ownership is held between `lock()` and `unlock()` by whoever
acquired it — a coroutine or an OS thread — not by the OS thread identity. A
contended `lock()` blocks the caller (park or OS-thread block) and is resumed
FIFO when the holder unlocks.

- **`lock()` works from any context.** The uncontended fast path is a single
  lock-free CAS. On contention a coroutine parks and a thread blocks on its
  per-thread sem; the acquire predicate (the same CAS) is re-checked under the
  primitive's own guard, so an `unlock()` racing the block either hands the lock
  over or queues the caller for a later hand-off.
- **`unlock()` hands the lock off directly.** It pops the FIFO-oldest waiter and
  wakes it *without* clearing the owned flag, so the woken party returns from
  `lock()` already owning the mutex. Only an unlock that finds no waiter clears
  the flag. This is why ownership is independent of the unlocking thread.
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
  it takes its internal guard (later arrivals are unaffected).
- Destroying a cond that still has waiters is a caller bug (matches
  `pthread_cond_destroy`).

## 4. Wait-group

A countdown latch, modeled on Go's `sync.WaitGroup`:

- `add(delta)` registers pending work, **before** spawning the units it counts.
- `done()` decrements by one; when the counter hits zero, every parked waiter is
  released together in one broadcast (FIFO).
- `wait()` parks until the counter reaches zero; returns immediately if already
  zero. Multiple coroutines may `wait()` concurrently.

Contract: `done()` more times than `add()` ever promised underflows the counter
and **aborts** (Go's "negative WaitGroup counter" panic). `add()` racing a
`wait()` that is already in progress is a logic error and unsupported — add up
front.

## 5. Channel

An **MPSC** message queue: many senders, exactly one receiver. The receiver
may be **either a coroutine or a plain OS thread** — the data path is
lock-free (intrusive MPSC queue) and the single receiver, when it must block,
publishes itself into one atomic wakeup slot that a sender / `close` / the
deadline timer arbitrate with a single `atomic_exchange`. This lets a
coroutine producer hand work to an OS-thread consumer (and vice versa). The
cross-context waiter representation, per-thread wake semaphore, and wake
dispatch come from the shared `waiter` module (also used by `xylem_sem`).

- `send(msg)` — non-blocking, thread-safe, `msg` must be non-NULL. Returns
  `0` on success, `XYLEM_CHANNEL_FULL` when a bounded channel is at capacity,
  or `-1` (invalid input or allocation failure). **Never parks.**
- `recv()` — blocks the calling coroutine **or thread** until a message
  arrives or the channel is closed-and-drained (then returns NULL).
  `recv_timeout(ms)` is the general form with a three-state wait policy:
  - `0` — non-blocking try: pop if a message is immediately available, else
    return NULL at once without blocking. Use this to drain (e.g. drop down to
    the newest frame) without risking a block.
  - `(uint64_t)-1` — block forever, identical to `recv()`.
  - any other `n` — block up to `n` ms.
  A NULL return does **not** distinguish "nothing available" / "timed out" /
  "closed and empty" — track the reason out of band if you need it.
- `len()` / `cap()` — best-effort in-flight count and the configured capacity
  (`cap()` is 0 for an unbounded channel). Safe from any thread; useful for
  drop/backpressure decisions.
- **Single receiver.** Concurrent `recv()` (from two coroutines, two threads,
  or one of each) aborts.

### Capacity and backpressure

- `create(0)` — **unbounded**: `send` always queues (barring OOM), never
  reports full.
- `create(cap)` with `cap > 0` — caps the **in-flight** message count (sent but
  not yet received) at `cap`. When full, `send` returns `XYLEM_CHANNEL_FULL` so
  the caller can drop or retry; it does **not** park the producer. This is the
  only send mode: a deliberate choice so an external capture thread is never
  stalled by a slow consumer (drop a frame instead). **Blocking backpressure
  (parking the producer until space frees) is intentionally not provided** — it
  would force `send` to park/block and would need a multi-waiter set, breaking
  the lock-free, never-park contract. Backpressure is instead a receiver-side
  policy: watch `len()`/`cap()` and, once over a threshold, drain with
  `recv_timeout(ch, 0)` to drop the backlog (dropping oldest, keeping newest).

Implementation: a single `_Atomic size_t count` tracks in-flight messages.
`send` reserves a slot with `fetch_add` before pushing; on overshoot it backs
the slot out and returns full, so the count never exceeds `cap` under
concurrent producers (no check-then-act race). `recv` decrements after a
successful pop. The count is maintained for unbounded channels too, purely so
`len()` works. This bounds the **count**, not allocation: each message is still
a per-node `malloc`. A bounded, zero-allocation MPSC would need a preallocated
MPMC ring, which this is not.

Close/lifecycle semantics are strict and abort-on-misuse:

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
no waiter is remembered) and a **timed wait**. Its `wait()` is the same
context-adaptive block as the rest; what makes it distinct is the count and the
deadline support, which is why it keeps its own machinery like the FIFO
primitives rather than sharing a list type.

- `wait()` decrements the count, or blocks if it is zero. It is
  **context-adaptive**: on a coroutine it parks (the worker thread stays free);
  on any other thread it blocks that OS thread. Both waiter kinds share one FIFO
  queue and one count.
- `timedwait(ms)` is `wait()` with a deadline: it returns `true` once a token is
  acquired, or `false` if `ms` elapses first. `timedwait(0)` is a non-blocking
  try (acquire-or-fail, never parks) and replaces the old `trywait`.
- `post()` wakes the FIFO-oldest waiter, or increments the count if none is
  parked. `post`, `create`, `destroy` are callable from any thread or context
  and never park.

### Who waits decides how it wakes

The poster does not care what it is waking — each waiter records its own wake
mechanism when it enqueues:

- a **coroutine waiter** stores its `mco_coro*` and the scheduler it parked
  under, and is woken with `scheduler_schedule()` (thread-safe from any caller);
- a **thread waiter** stores a per-thread OS semaphore (a `platform_sem`,
  created lazily on the thread's first wait and cached in thread-local storage,
  reclaimed by the TLS destructor), blocks on it, and is released with
  `platform_sem_post()`.

So "thread posts → coroutine waits" reschedules the coroutine, and "coroutine
posts → thread waits" releases the thread's OS semaphore — the branch is on the
*waiter's* kind, not the poster's.

### Direct hand-off, no lost wakeups

A short spin `guard` serialises every count/queue mutation. When a waiter is
queued, `post()` transfers the token straight to the FIFO-oldest waiter and
never touches the count; the woken waiter returns from `wait()` without
re-decrementing. The coroutine fast path re-checks the count inside the park
callback under the guard, so a `post()` racing the park either hands over the
count (park declines) or finds the waiter already queued.

Each thread waiter gets its *own* OS semaphore rather than sharing one, so
`post()` releases exactly the head-of-queue thread and FIFO order is preserved
with no thundering herd.

### Timeout and waiter lifetime

Most waiters live on the blocked party's stack: an infinite coroutine wait stays
valid because the coroutine is suspended, and every thread wait (timed or not)
stays valid because the thread is blocked inside `platform_sem_(timed)wait`.
`post()` copies the wake target out under the guard and never touches the record
again, so a stack waiter is always safe.

A **timed coroutine wait** is the exception. It arms a scheduler timer that can
pull the waiter out of the FIFO from another worker, and that timer callback may
run concurrently with the resumed coroutine — so a stack record would be a
use-after-free. The timed coroutine waiter is therefore a small refcounted heap
object (one reference for the waiting coroutine, one for the armed timer), freed
by whichever side drops the last reference, exactly as `iowait` does for I/O
deadlines. The timer is armed *under the guard* inside the park callback so a
racing `post()` cannot dequeue and resume the coroutine before the timer is
live; on resume the waiter cancels the timer and reports timeout-vs-token from a
flag the timeout callback set while holding the guard.

The thread-side timeout needs no timer: `platform_sem_timedwait` does the
blocking, and on wakeup the thread resolves the race with a concurrent `post()`
under the guard — if it is still queued it removes itself and reports timeout;
if a `post()` already dequeued it, it consumes the handed-off token (so the
token is not lost) and reports success.

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

The FIFO primitives (mutex, cond, waitgroup) use it internally to guard their
waiter lists; the *waiters* block, but the list bookkeeping is protected by a short
spin.

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
- Conventions (threading annotations, abort policy): [`../conventions.md`](../conventions.md)
- Tests: [`../test/strategy.md`](../test/strategy.md) *(planned)*
