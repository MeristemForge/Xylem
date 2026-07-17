# Xylem Architecture

This document gives a system-wide view of Xylem: what the major layers are,
how they depend on each other, and how the networking stack is built on the
coroutine runtime. Read this first, then drop into a per-module design doc
under [`docs/design/`](design/) for the details of any one component.

For library-wide naming, error, and data-structure rules, see
[`conventions.md`](conventions.md). For build, test, and coverage instructions,
see [`build.md`](build.md).

## 1. What Xylem is

Xylem is a cross-platform C11 **static library** (optionally shared) that
*supplements* the C11 standard. It bundles four broad capability areas:

- **Data structures** — intrusive and non-intrusive containers.
- **Crypto / encoding** — SHA, HMAC, AES, Base64, varint, JSON, gzip, FEC.
- **Concurrency** — mutex, cond, channel, wait-group, plus the coroutine runtime.
- **Async networking** — TCP, UDP, UDS, TLS, DTLS, RUDP, HTTP/1.1, WebSocket.

The networking modules are the reason the runtime exists: they are written in a
**synchronous, blocking style** but run on coroutines so that thousands of
connections can be served from a small pool of OS threads.

## 2. Layered view

Higher layers depend on lower layers; lower layers never depend up.

```
            +-----------------------------------------------------+
 App        |                  user coroutines                    |
            +-----------------------------------------------------+
 Protocols  |  HTTP/1.1   WebSocket    (router, cookie, cors,     |
            |     |          |          auth, form, multipart,    |
            |     |          |          fileserver, proxy)        |
            |     +----+-----+                                    |
            |          | transport iface (TCP | TLS-or-stub)      |
            |   TLS / DTLS (OpenSSL, optional)                    |
            |   RUDP (KCP + Reed-Solomon FEC)   MUX               |
            |   TCP    UDP    UDS                                 |
            +-----------------------------------------------------+
 Runtime    |  scheduler (work-stealing) | iowait | dynpool |    |
            |  timers | minicoro coroutines                       |
            +-----------------------------------------------------+
 Platform   |  poller (epoll/kqueue/wepoll) | socket | vmem |    |
            |  sem | string | info | serial                       |
            +-----------------------------------------------------+
 Base       |  containers | sync | crypto | encoding | logger |  |
            |  utils | timer | serial                             |
            +-----------------------------------------------------+
```

The **base layer** (containers, crypto, encoding, logging, utils) has no
dependency on the runtime and can be used standalone. (The **sync** primitives
sit here structurally but are the exception: the coroutine mutex/cond/wait-group/
channel park on the scheduler, so they do depend on the runtime — see
[`design/sync.md`](design/sync.md).) The **platform layer** hides OS differences
behind a uniform interface. The **runtime** turns the platform poller plus
coroutines into a scheduler. The **protocol layer** builds on the runtime,
presenting blocking-style APIs.

## 3. Source map

| Area | Public headers | Implementation |
|------|----------------|----------------|
| Entry / runtime API | `include/xylem.h` | `src/runtime/`, `src/xylem.c` |
| Containers | `include/xylem/container/` | `src/container/` |
| Sync | `include/xylem/sync/` | `src/sync/` |
| Crypto | `include/xylem/crypto/` | `src/crypto/` |
| Encoding | `include/xylem/encoding/` | `src/encoding/` |
| Core utils | `include/xylem/xylem-*.h` | `src/xylem-*.c` |
| Net (transport) | `include/xylem/net/` | `src/net/` |
| HTTP | `include/xylem/net/http/` | `src/net/http/` |
| WebSocket | `include/xylem/net/xylem-ws*.h` | `src/net/ws/` |
| Platform | `src/platform/*.h` | `src/platform/{unix,win}/` |

Bundled third-party code lives next to its consumer: `src/runtime/minicoro/`
(coroutines), `src/net/http/llhttp/` (HTTP parser), `src/net/rudp/kcp/`
(reliable UDP), and the Windows `wepoll` poller backend.

## 4. The runtime in one paragraph

`xylem_run(main_fn, arg, opts)` boots a **work-stealing scheduler** across N
worker threads (default: CPU count) and a **dynamic blocking-task pool**, then
spawns `main_fn` as the root coroutine and blocks until every coroutine has
exited or `xylem_shutdown()` is called. Each worker owns a `runnext` slot, a
fixed-capacity SPMC FIFO work-stealing queue, and a timer heap; overflow and
cross-thread wakeups land in a shared mutex-protected MPMC queue. One idle
worker at a time becomes the **poll driver**
and blocks on the platform poller to service I/O and timers;
the rest park on a semaphore. Coroutines that wait on a socket suspend through
**`iowait`**, which arms the fd on the poller and resumes the coroutine when it
becomes ready, its deadline passes, or it is closed. Blocking work (anything
that can't be made non-blocking) is offloaded to the **dynpool** via
`xylem_await()`. The full design is in
[`docs/design/runtime.md`](design/runtime.md).

## 5. How a network call flows

Using `xylem_tcp_read()` as the canonical example:

1. The coroutine calls `platform_socket_recv()` on a non-blocking fd.
2. If data is available, it returns immediately — no scheduler involvement.
3. On `EAGAIN`/`EWOULDBLOCK`, the coroutine calls `iowait_read()`, which
   **parks** it: it arms the fd on the poller and yields to the scheduler.
4. The worker that parked it picks up other runnable coroutines.
5. When the poll driver sees the fd become readable, it looks up the parked
   coroutine via a generation-tagged slab index and reschedules it.
6. The coroutine resumes inside `iowait_read()`, loops back, and retries the
   `recv`.

The same park/resume pattern underlies `accept`, `connect`, `write`, UDP, and
the TLS/DTLS handshakes. Read deadlines and `close` are additional wake sources
that race through the same per-direction arbitrator and resume the coroutine
with a distinct result (`IOWAIT_TIMEOUT` / `IOWAIT_CLOSED`).

## 6. Thread-safety model

- **`read`/`write` are coroutine operations, not any-thread.** They may park
  the calling coroutine (when the socket would block), so they must run on a
  scheduler worker, and only one coroutine may drive each direction of a
  connection at a time.
- **`close` is coroutine-only, like the rest of the connection API.** It must
  run on a scheduler worker (its teardown may itself touch coroutine-only
  primitives, e.g. draining an inbox channel), so calling it off a coroutine
  aborts. To cancel a connection whose reader/writer is parked, close it from
  *another* coroutine: the cross-direction wakeup goes through
  `scheduler_coro_ready()`, which pushes to the global run queue and wakes a
  worker, so the closer and the parked coroutine need not be the same one.
- **`destroy` is the final non-concurrent release.** TCP, UDP, and UDS keep the
  socket and public wrapper alive during close. After parked operations return,
  destroy releases the internal transport, closes the socket, and frees the
  wrapper. Calling destroy directly is valid only when no operation can still
  be using the handle; otherwise use close, wait, then destroy.
- **Cross-thread lifetimes are explicit or reference counted.** TCP, UDP, and
  UDS use the close/wait/destroy contract instead of wrapper refcounts. Handles
  that can outlive their owner through callbacks may retain refcounts. `iowait`
  always carries internal refs plus a generation tag so a completion event for
  a recycled slot is rejected instead of waking the wrong coroutine.
- **One-reader / one-writer per direction.** At most one coroutine may park on
  a handle's read side and one on its write side at a time; violations abort
  with a diagnostic rather than corrupting state silently.

## 7. Platform abstraction

The platform layer (`src/platform/`) presents one interface with per-OS
backends in `unix/` and `win/`:

- **poller** — epoll (Linux) and kqueue (macOS) in **edge-triggered** mode;
  wepoll (Windows) in **level-triggered + one-shot** mode. The trigger mode is
  exposed as `PLATFORM_POLLER_TRIGGER_MODE`; the runtime adapts its arm/re-arm
  logic accordingly.
- **socket** — non-blocking sockets, socketpair (used for the scheduler's
  wakeup fd), and `startup`/`cleanup` (WSAStartup on Windows, `SIGPIPE` ignore
  on Unix).
- **vmem** — reserve/commit/protect, used to allocate coroutine stacks with a
  guard page.
- **sem / info / string / serial** — semaphores, CPU count, string helpers,
  and serial-port I/O.

Edge- vs. level-triggered is the single biggest behavioral split between
platforms and is handled centrally in `iowait`, so protocol code stays
platform-agnostic.

## 8. Optional features and feature gates

| Gate | Effect |
|------|--------|
| `XYLEM_ENABLE_TLS` | Compiles TLS/DTLS against OpenSSL ≥ 3.5. When off, the HTTP transport links a TLS stub and `wss://` is unavailable. |
| `XYLEM_ENABLE_TESTING` | Builds the `tests/` suite. |
| `XYLEM_ENABLE_DYNAMIC_LIBRARY` | Builds a shared library instead of static. |
| `XYLEM_ENABLE_ASAN` / `TSAN` / `UBSAN` | Sanitizer builds. |
| `XYLEM_ENABLE_COVERAGE` | Coverage instrumentation. |

See [`build.md`](build.md) for the full option matrix and per-platform notes.

## 9. Where to go next

- Runtime internals: [`docs/design/runtime.md`](design/runtime.md)
- Library-wide conventions: [`conventions.md`](conventions.md)
- Per-module designs: [`docs/design/`](design/)
- Test strategy: [`docs/test/strategy.md`](test/strategy.md)
