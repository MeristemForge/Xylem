# Library-Wide Conventions

These are the rules that hold across every Xylem module: naming, error
reporting, handle lifetimes, the two-tier data-structure scheme, threading
annotations, and the house C style. Per-module design docs assume these and
only call out deviations.

See also: [`architecture.md`](architecture.md) for the system overview,
[`build.md`](build.md) for build/test/coverage.

## 1. Public vs. internal surface

Xylem has a hard split between what ships in headers and what stays in `src/`.

| | Public API | Internal |
|---|------------|----------|
| Location | `include/xylem/**` | `src/**/*.h` |
| Symbol prefix | `xylem_` (types, funcs, macros) | none (`list_t`, `scheduler_*`, `platform_*`) |
| Stability | Stable, documented | May change freely |
| Umbrella header | `include/xylem.h` pulls in everything | — |

The single rule that follows from this: **anything a user can `#include` is
`xylem_`-prefixed; anything without the prefix is an implementation detail** and
must not appear in a public header.

## 2. Naming

### Public symbols

- **Functions:** `xylem_<module>_<verb>()` — e.g. `xylem_tcp_read`,
  `xylem_list_insert_tail`, `xylem_logger_init`.
- **Opaque handle types:** `xylem_<module>_t` over `typedef struct
  xylem_<module>_s xylem_<module>_t;` with the struct body hidden in the `.c`.
  Example: `xylem_tcp_conn_t`, `xylem_list_t`.
- **Options structs:** `xylem_<module>_opts_t`, passed by pointer, `NULL` for
  defaults (see §4).
- **Enums:** type `xylem_<thing>_t`, constants `XYLEM_<THING>_<VALUE>` —
  e.g. `XYLEM_LOGGER_LEVEL_INFO`, `XYLEM_TIME_PRECISION_MSEC`.
- **Macros:** `XYLEM_*` for constants, `xylem_*` for function-like macros
  (e.g. the `xylem_logd/i/w/e` log helpers).

### Internal symbols

- Lowercase module prefix, no `xylem_`: `scheduler_create`, `runq_push`,
  `iowait_read`, `platform_poller_wait`.
- File-local statics and helpers are prefixed `_`: `_sched_worker_entry`,
  `_iowait_park_cb`.
- Platform backends share one header (`src/platform/platform-*.h`) and a
  per-OS `.c` under `src/platform/{unix,win}/`.

## 3. Error reporting

No exceptions, no global `errno`-style state of our own. Results are in the
return value:

| Return type | Success | Failure |
|-------------|---------|---------|
| `int` status | `0` | `-1` |
| Pointer / handle | non-NULL | `NULL` |
| `bool` predicate | `true`/`false` as named | — |
| Byte count (I/O) | `> 0` bytes, `0` on EOF/peer-close | `-1` on error/timeout |

Conventions:
- Callers check the return value; the library does not `abort()` on ordinary
  runtime errors. (The deliberate exceptions are programming-contract
  violations such as an illegal second parker on an `iowait` direction, which
  abort with a diagnostic rather than corrupt state silently.)
- Diagnostics go through the logger (`xylem_loge(...)`), not `stderr` directly.
- Destroy functions return `void` where possible, accept `NULL`, and are
  idempotent unless a module documents otherwise. `xylem_ticker_destroy()` is
  one documented exception: it accepts `NULL`, but a non-NULL ticker handle is
  consumed and must not be destroyed again. Close functions that release their
  handle consume it; the handle is invalid after the call returns.

## 4. Options structs and the NULL-means-default rule

Constructors that take tuning parameters accept a pointer to an
`xylem_<module>_opts_t`. The standard rule is: **pass `NULL` to get sane
defaults**, and within a non-NULL struct a zero field usually means "default
for this field".

```c
xylem_tcp_listener_t* ln = xylem_tcp_listen("0.0.0.0", 8080, NULL);
```

The documented exception is the logger: because `xylem_logger_level_t` starts
at `DEBUG = 0`, a zeroed field cannot mean "default". When you pass a non-NULL
`xylem_logger_opts_t`, every field is taken verbatim, so initialize the ones
you care about explicitly. Per-module docs flag any similar exception.

## 5. Handle lifetime

Two pairing styles, by allocation ownership:

- **Library-allocated handles:** `xylem_<m>_create()` / `xylem_<m>_destroy()`.
  `create` returns `NULL` on failure; `destroy(NULL)` is a no-op.
- **Caller-allocated / global subsystems:** `xylem_<m>_init()` /
  `xylem_<m>_deinit()` (e.g. the logger).

Rules:
- **`destroy` accepts `NULL` and is idempotent unless a module says
  otherwise.** `xylem_ticker_destroy()` accepts `NULL` but consumes a non-NULL
  ticker handle, so callers must not destroy the same ticker handle twice. A
  `close` function that releases the handle consumes that handle; callers must
  not use or close the same handle again after `close` returns. Atomic `closed`
  flags only coordinate concurrent close/read/write paths while another
  reference keeps the object alive.
- **Read before close.** For connections, query any state you need (e.g.
  `xylem_tcp_remote_addr`) *before* calling `close`; close may free backing
  state and wakes any coroutine blocked on the handle.
- **Cross-coroutine lifetime uses reference counting.** A connection can be
  closed from one coroutine while another is parked in `read`/`write` on it
  (and the close wakeup itself crosses threads via `scheduler_schedule()`), so
  an internal atomic refcount (`ref`/`unref`) keeps the handle alive until the
  last user drops it; `iowait` adds a generation tag so stale events are
  rejected rather than dereferenced. See
  [`design/runtime.md`](design/runtime.md).

## 6. Initialization order

The networking stack runs on the coroutine runtime, so the entry point is
`xylem_run()`:

```c
static void app(void* arg) {
    /* runs as the root coroutine; spawn, dial, listen here */
}

int main(void) {
    xylem_run(app, NULL, NULL);   /* boots scheduler + pools, blocks */
    return 0;
}
```

`xylem_run()` performs the one-time global setup itself — including
`platform_socket_startup()` (WSAStartup on Windows) — and tears it down on
return. There is no separate user-visible global `startup`/`cleanup` for
networking. The pure base-layer modules — containers, crypto, encoding, and the
core utils (bswap, base64, hashing, …) — need no global init and can be used
without the runtime. Note the **sync primitives are not** in that group: the
coroutine mutex/cond/wait-group/channel block by parking on the scheduler, so
their blocking operations require a running runtime (see
[`design/sync.md`](design/sync.md)). The logger is initialized independently via
`xylem_logger_init()`.

## 7. Data structures: intrusive (internal) vs. non-intrusive (public)

Xylem keeps two flavors of each core container, and the split maps onto the
public/internal boundary from §1:

- **Intrusive — internal.** Defined in `src/container/<c>.h` with **no**
  `xylem_` prefix: a `list_t` plus an embedded `list_node_t`, recovered with
  the `list_entry(ptr, type, member)` macro (same idea as the Linux kernel /
  nginx lists). Zero allocation per element — the node lives inside the
  caller's struct. These back the runtime and protocol code (`runq`, timer
  heaps, the scheduler registry, …). Variants: `list`, `lifo`/stack, `queue`,
  `heap`, `rbtree`, `mpsc`.
- **Non-intrusive — public.** Defined in `include/xylem/container/` with the
  `xylem_` prefix: allocating wrappers that store a `void* data` per element
  (`xylem_list_t`, `xylem_stack_t`, `xylem_queue_t`, `xylem_heap_t`,
  `xylem_rbtree_t`, `xylem_ringbuf_t`). They `create`/`destroy` and manage node
  memory for you.

```c
/* internal, intrusive: node embedded, recovered via *_entry */
typedef struct { int v; list_node_t link; } item_t;
list_t lst; list_init(&lst);
list_insert_tail(&lst, &it->link);
item_t* it = list_entry(node, item_t, link);

/* public, non-intrusive: stores void* data, allocates internally */
xylem_list_t* lst = xylem_list_create();
xylem_list_insert_tail(lst, ptr);
```

Rule of thumb: reach for the **public non-intrusive** containers in application
code; the intrusive ones exist for hot, allocation-sensitive internal paths.

> Naming note: the `*_entry` recovery macro is **internal** and unprefixed
> (`list_entry`, `heap_entry`, `queue_entry`). It is not part of the public API.

## 8. Time, randomness, endianness

- **Time source:** `xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)` returns the
  current UTC wall-clock time in milliseconds. All deadlines in the API are
  absolute millisecond timestamps against this clock (e.g.
  `xylem_tcp_set_read_deadline`). Other
  precisions: `SEC`, `USEC`, `NSEC`.
- **Randomness:** `xylem_utils_getprng(min, max)` for non-cryptographic use;
  `platform_info_getrandom()` (internal) for CSPRNG bytes (BCryptGenRandom /
  `/dev/urandom`).
- **Endianness:** `xylem_utils_getendian()` plus the `xylem-bswap` helpers.

## 9. Logging

Four level macros capture file/line automatically:

```c
xylem_logd("debug %d", x);   // DEBUG
xylem_logi("listening on %u", port);
xylem_logw("retrying");
xylem_loge("tcp fd=%d read error: %s", fd, msg);
```

`xylem_logger_init(filename, opts)` (NULL filename → stdout). Writes are
dispatched on a dedicated worker thread, so logging never blocks the caller on
file I/O. A custom sink can be installed with `xylem_logger_set_callback()`.

## 10. Threading annotations

Every public function's doc comment states its threading contract. The
recurring categories:

- **Any-thread.** Safe to call from any thread, including outside the runtime.
  Wakeup/non-blocking sync ops (`unlock`, `trylock`, `signal`/`broadcast`,
  `add`/`done`, channel `send`/`close`, and semaphore `post`) are examples.
  Thread-safe does not always mean concurrency-safe on one object: each API
  documents which same-handle races are forbidden.
- **Coroutine-only.** Must be called from inside a coroutine on the runtime.
  The **entire connection API** is coroutine-only — not just `read`/`write`/
  `accept`/`dial` (which may park), but also `close`, the read/write deadline
  setters, and the address getters (TCP/UDP/UDS/RUDP/TLS/DTLS). `close` is
  coroutine-only because teardown is serialized through the runtime; to cancel
  a connection whose reader/writer is parked, close it from *another*
  coroutine. `xylem_await` is also coroutine-only.
- **Context-adaptive.** Safe from either a coroutine or a plain OS thread; the
  call inspects its context when that matters and does the right thing. If the
  operation blocks, it parks a coroutine (the worker stays free) or blocks a
  plain OS thread. The observable semantics are identical in both contexts.
  This covers `xylem_spawn`, `xylem_shutdown`, `xylem_sleep`, timer
  arm/cancel/reset, ticker recv, and blocking sync ops such as `mutex_lock`,
  `cond_wait`, `waitgroup_wait`, `channel_recv`, and semaphore
  `wait`/`timedwait`. These deliberately do **not** abort off-coroutine --
  bridging the coroutine/OS-thread boundary is the point.
- **Single-owner.** One logical owner at a time, stated explicitly — e.g. one
  reader and one writer per `iowait` direction; a single deadline driver per
  direction.

When in doubt, the function's header comment is authoritative.

## 11. C style

- **C11**, no compiler extensions in headers. `_Pragma("once")` for include
  guards.
- Every file carries the MIT license header.
- Fixed-width integer types from `<stdint.h>` (`uint16_t`, `int32_t`, …); sizes
  and counts use `size_t`.
- `restrict` on non-aliasing pointer params where it matters (e.g. the logger).
- Atomics via C11 `<stdatomic.h>` (`_Atomic`, `atomic_*`); explicit memory
  orders on the hot/concurrent paths.
- Doc comments are Doxygen-style (`@brief`, `@param`, `@return`) on every
  public declaration.
- Formatting is enforced by `.clang-format`; run it before committing.

## 12. Related docs

- System overview: [`architecture.md`](architecture.md)
- Runtime internals: [`design/runtime.md`](design/runtime.md)
- Build / test / coverage: [`build.md`](build.md)
- Test strategy: [`test/strategy.md`](test/strategy.md) *(planned)*
