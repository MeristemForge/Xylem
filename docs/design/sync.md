# Synchronization Primitives Design

Xylem's public synchronization primitives — mutex, condition variable,
wait-group, and channel — are **coroutine-aware**: a contended operation parks
the calling coroutine on the scheduler instead of blocking the OS worker
thread. This document covers their semantics, threading contracts, and the
ordering rules that make them correct. The internal `spin_t` is also described
because it backs the others.

Sources: public headers in `include/xylem/sync/`, implementations in
`src/sync/`. Internal spinlock in `src/sync/spin.{h,c}`.

Prerequisite: the parking model in [`runtime.md`](runtime.md) (`scheduler_park`,
`scheduler_schedule`).

## 1. The common model

All four public primitives share one shape:

- **Blocking ops are coroutine-only.** `mutex_lock`, `cond_wait`,
  `waitgroup_wait`, `channel_recv` may suspend, so they must run inside a
  coroutine on a scheduler worker. Calling them off-coroutine aborts. The one
  exception is `xylem_sem` (§6), whose `wait()` is *context-adaptive*: it parks
  a coroutine but blocks an OS thread, by design, so a coroutine and an
  external thread can notify each other across the boundary.
- **Wakeup/non-blocking ops are any-thread.** `unlock`, `signal`/`broadcast`,
  `add`/`done`, `send`/`close`, and all `create`/`destroy` are safe from any
  thread — a cross-thread wake is just a `scheduler_schedule()`.
- **FIFO wakeups.** Waiters are resumed in arrival order.
- **Abort on contract violation.** These are coroutine-grade invariants
  (off-coroutine blocking, counter underflow, double-close, multi-receiver),
  so misuse aborts with a diagnostic rather than corrupting state silently.

| Primitive | Blocking op (coroutine-only) | Any-thread ops | Pattern |
|-----------|------------------------------|----------------|---------|
| `xylem_mutex` | `lock` | `unlock`, create/destroy | coroutine-owned lock |
| `xylem_cond` | `wait` | `signal`, `broadcast`, c/d | paired with a mutex |
| `xylem_waitgroup` | `wait` | `add`, `done`, c/d | countdown latch |
| `xylem_channel` | `recv` / `recv_timeout` | `send`, `close`, c/d | MPSC message passing |
| `xylem_sem` | `wait` (any context) | `post`, `trywait`, c/d | cross-context counting semaphore |

## 2. Mutex

A coroutine-owned lock. Ownership is held by a *coroutine* (not an OS thread)
between `lock()` and `unlock()`; a contended `lock()` parks the caller and is
resumed FIFO when the holder unlocks.

- **`lock()` is coroutine-only even on the uncontended fast path.** An acquire
  from a non-coroutine thread cannot be parked if contention shows up later,
  which would leave the mutex in a state that only fails on the next contended
  acquire. The contract forbids it up front: off-coroutine `lock()` aborts.
- **`unlock()` is any-thread on purpose.** It lets a coroutine hand the lock
  off to another, and the wakeup only needs a `scheduler_schedule()`. So lock
  ownership is not tied to the unlocking thread.

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

External threads that can't take the coroutine-owned mutex must avoid lost
wakeups out of band: have the waiter's predicate read an atomic flag that the
external thread sets *before* calling `signal`/`broadcast`.

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

An **MPSC** message queue: many senders, exactly one receiving coroutine.

- `send(msg)` — non-blocking, thread-safe, `msg` must be non-NULL; returns
  `0`/`-1` (invalid input or allocation failure). **Unbounded**: there is no
  backpressure, send never blocks.
- `recv()` — parks the calling coroutine until a message arrives or the channel
  is closed-and-drained (then returns NULL). `recv_timeout(ms)` adds a relative
  timeout; a NULL return does **not** distinguish "timed out" from "closed and
  empty" — track the reason out of band if you need it.
- **Single receiver.** Concurrent `recv()` from two coroutines aborts.

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

A counting semaphore that **bridges coroutines and OS threads** — the one sync
object whose blocking op is callable from either context. It exists precisely so
a coroutine can notify an external thread, or an external thread can notify a
coroutine, without going through the runtime's coroutine-only contract.

- `wait()` decrements the count, or blocks if it is zero. It is
  **context-adaptive**: on a coroutine it parks (the worker thread stays free);
  on any other thread it blocks that OS thread. Both waiter kinds share one FIFO
  queue and one count.
- `post()` wakes the FIFO-oldest waiter, or increments the count if none is
  parked. `trywait()` decrements without ever blocking. `post`, `trywait`,
  `create`, `destroy` are callable from any thread or context and never park.

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

Unlike the other primitives, `xylem_sem` does **not** abort off-coroutine — that
is the whole point. A coroutine waiter does require a running scheduler (that is
how it is woken); a thread waiter needs no runtime at all.

## 7. Internal spinlock (`spin_t`)

`src/sync/spin.{h,c}` is a plain test-and-set spinlock used **internally** for
very short critical sections where parking would cost more than spinning — e.g.
the scheduler's coroutine-pool free list and registry. It does not interact with
the scheduler (no parking), so it must only guard sections that are short and
non-blocking. It is not part of the public API and carries no `xylem_` prefix.

The coroutine primitives above use it (or a `mtx_t`) internally to guard their
own waiter lists; the *waiters* park, but the list bookkeeping is protected by a
short spin/lock.

## 8. Choosing a primitive

- **Mutual exclusion across coroutines:** `xylem_mutex`.
- **Wait for a condition on shared state:** `xylem_cond` + a mutex + a predicate
  loop.
- **Fan-out then join N tasks:** `xylem_waitgroup`.
- **Stream values from producers to one consumer:** `xylem_channel`.
- **Signal across the coroutine/OS-thread boundary (either direction):**
  `xylem_sem`.
- **Guard a tiny, non-blocking critical section in internal code:** `spin_t`.

Note these are *coroutine* primitives (except `xylem_sem`, which spans both
worlds). For raw OS-thread synchronization the code uses C11 `<threads.h>`
(`mtx_t`, `cnd_t`) directly via `src/thrds.h`; don't mix a coroutine mutex with
an OS-thread-only context.

## 9. Related docs

- Parking / scheduling model: [`runtime.md`](runtime.md)
- Conventions (threading annotations, abort policy): [`../conventions.md`](../conventions.md)
- Tests: [`../test/strategy.md`](../test/strategy.md) *(planned)*
