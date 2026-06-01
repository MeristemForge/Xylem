---
inclusion: auto
description: "Xylem project overview and architecture"
---

# Xylem

Xylem is a cross-platform C11 static library that supplements (not replaces) the C11 standard. It provides data structures, crypto primitives, concurrency utilities, and async networking.

## Architecture

### Coroutine Runtime (`runtime.h` / `xylem_run`)

The networking stack is built on a coroutine runtime, not a callback event loop. `xylem_run()` boots a work-stealing scheduler over N worker threads plus a dynamic blocking-task pool, then runs the user's root coroutine. Code is written in a synchronous, blocking style; calls like `xylem_tcp_read()` suspend (park) the calling coroutine and resume it when the socket is ready, the deadline passes, or the handle is closed.

Public entry points (in `include/xylem.h`):
- `xylem_run(main_fn, arg, opts)` — boot the runtime, run `main_fn` as the root coroutine, block until all coroutines exit or `xylem_shutdown()` is called.
- `xylem_spawn(fn, arg)` — spawn a coroutine (thread-safe).
- `xylem_sleep(ms)` — suspend the current coroutine.
- `xylem_submit(fn, arg)` — run a blocking function on the blocking pool, suspending the caller until it returns.
- `xylem_shutdown()` — signal the runtime to stop (thread-safe).

Internally (`src/runtime/`):
- `scheduler` — N worker threads, three-tier runnable pool (per-worker `runnext` slot, per-worker work-stealing deque `wsdeque`, global `runq`), plus per-worker timer heaps. One idle worker becomes the poll "driver" via CAS; the rest park on a semaphore.
- `iowait` — per-fd / per-direction coroutine parking on top of the platform poller, with a generation-tagged slab allocator to reject stale completion events.
- `dynpool` — dynamic thread pool that backs `xylem_submit` for blocking work.
- `minicoro` (bundled) — stackful coroutine primitive.

### Platform Poller

The scheduler drives I/O through a thin platform poller abstraction (`src/platform/platform-poller.h`): epoll on Linux and kqueue on macOS (both edge-triggered), wepoll on Windows (level-triggered + one-shot). Edge-triggered platforms require draining the fd until `EAGAIN` before parking; the one-shot platform re-arms via `platform_poller_mod()`.

### Thread Safety Model

- `read`/`write` (and `accept`/`dial`) are **coroutine operations**: they may park the calling coroutine, so they must run on a scheduler worker, and at most one coroutine drives each direction of a connection.
- `close` on connections is safe from **any thread**, even while a coroutine is parked in `read`/`write` on the same connection. Cross-thread wakeups go through `scheduler_schedule()`, which routes to the global runq and wakes a worker.
- Connections and `iowait` handles use reference counting (`ref`/`unref`, generation tags) to prevent use-after-free across threads.
- `iowait` is one-reader / one-writer per direction: at most one coroutine may park on read and one on write of the same handle at a time.

### Protocol Stack

```
HTTP / WebSocket
    └── Transport interface (http-transport.h / ws-transport.h)
            ├── TCP transport
            └── TLS transport (or stub when TLS disabled)
TLS / DTLS
    └── OpenSSL (optional, gated by XYLEM_ENABLE_TLS)
RUDP
    └── KCP (bundled) + FEC (Reed-Solomon)
TCP / UDP / UDS
    └── Coroutine runtime (scheduler + iowait) + platform sockets
```

### Data Structures

Two flavors of each core container, split along the public/internal boundary.
**Non-intrusive** (`xylem_`-prefixed, public, in `include/xylem/container/`)
are allocating wrappers storing a `void* data` per element. **Intrusive**
(internal, unprefixed, in `src/container/`) embed a `list_node_t`-style node in
the caller's struct and recover the container with the internal `*_entry()`
macro (e.g. `list_entry`); they back the runtime and protocol code. The
`*_entry()` recovery macro is internal, not part of the public API.

## Design Philosophy

- Zero external dependencies beyond the C11 stdlib (bundled third-party: minicoro, llhttp, KCP, wepoll).
- Intrusive data structures (internal) embed an unprefixed `*_node_t` and recover the container via the internal `*_entry()` macro; the public containers are the non-intrusive `xylem_`-prefixed wrappers.
- Synchronous-style coroutine networking over a work-stealing scheduler; no user-visible callbacks for the I/O path.
- Error codes (typically `-1`) over exceptions or global state.
- Cross-platform: Windows (MSVC/Clang-cl) + Unix (GCC/Clang), Linux/macOS/Android/iOS.

## Documentation Layout

Documentation lives under `docs/`:
- `docs/architecture.md` — system-wide overview and module map (start here).
- `docs/conventions.md` — library-wide naming, error, and data-structure conventions.
- `docs/build.md` — build, test, sanitizer, and coverage instructions.
- `docs/design/<module>.md` — per-module / per-group design docs.
- `docs/test/<module>.md` — per-module test designs, plus `docs/test/strategy.md` for the overall test strategy.

Before modifying a module, read its design doc at `docs/design/<module>.md` and, where present, its test design at `docs/test/<module>.md`.
