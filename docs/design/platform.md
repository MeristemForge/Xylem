# Platform Abstraction Design

The platform layer is the thin seam that hides OS differences from the rest of
Xylem. Everything above it — runtime, protocols, containers — is written once
against these interfaces and compiled unchanged on Linux, macOS, and Windows
(and the mobile variants built on those kernels).

Sources: headers in `src/platform/*.h`; backends in `src/platform/unix/` and
`src/platform/win/` (the latter bundling `wepoll/` for its poller).

Related: [`runtime.md`](runtime.md) is the primary consumer of the poller, vmem,
and semaphore interfaces.

## 1. Shape of the layer

One header per concern, two backend implementations:

```
   src/platform/
     platform.h              umbrella (pulls in the headers below)
     platform-poller.h  ──┐
     platform-socket.h    │
     platform-vmem.h      │   one shared interface header each
     platform-sem.h       │
     platform-io.h        │
     platform-info.h      │
     platform-string.h    │
     platform-serial.h  ──┘
     unix/  platform-*.c     epoll/kqueue, BSD sockets, mmap, POSIX sem, ...
     win/   platform-*.c     wepoll, Winsock, VirtualAlloc, ...
            wepoll/          bundled epoll-over-AFD shim for Windows
```

Design rules:
- **No `xylem_` prefix.** The platform layer is internal; symbols are
  `platform_<concern>_<verb>()` (see [`conventions.md`](../conventions.md) §1).
- **Compile-time dispatch.** Backends are selected with `#if defined(__linux__)
  / __APPLE__ / _WIN32`, not function pointers — there is no runtime indirection
  on the hot path.
- **Same signatures, same semantics.** A backend may differ in *mechanism*
  (epoll vs. kqueue vs. wepoll) but must present the same contract; behavioral
  splits that callers must know about are surfaced as compile-time constants
  (e.g. `PLATFORM_POLLER_TRIGGER_MODE`).
- **Error reporting** follows the library convention: `0`/`-1` for status,
  `NULL` for handles.

## 2. Concerns at a glance

| Header | Purpose | Linux | macOS | Windows |
|--------|---------|-------|-------|---------|
| `platform-poller` | Readiness I/O multiplexing | epoll (ET) | kqueue (ET) | wepoll (LT + one-shot) |
| `platform-socket` | Non-blocking sockets, socketpair, startup | BSD sockets | BSD sockets | Winsock2 |
| `platform-vmem` | Virtual-memory lifecycle | mmap/madvise | mmap/madvise | VirtualAlloc/VirtualFree |
| `platform-sem` | Counting semaphores (worker parking) | POSIX `sem_init` | GCD `dispatch_semaphore` | Win32 semaphore |
| `platform-info` | CPU count, tid/pid, time conversion, CSPRNG | syscalls | sysctl/pthread | Win32/BCrypt |
| `platform-io` | Portable fopen/vsnprintf/stat | stdio/stat | stdio/stat | `*_s` + `_stat64` |
| `platform-string` | Safe string helpers | — | — | `*_s` variants |
| `platform-serial` | Serial-port I/O | termios | termios | DCB/COM |

## 3. Poller — the important one

The poller is the single most consequential abstraction, because it is where
edge- vs. level-triggered behavior is reconciled. The interface is a tiny
submission/completion model:

- `platform_poller_init/deinit(sq)` — create/destroy the poller handle.
- `platform_poller_add/mod/del(sq, sqe)` — register / change interest / remove
  an fd. The caller owns the `sqe` and must keep it alive for the registration.
- `platform_poller_wait(sq, cqe[], timeout)` — block for up to `timeout` ms
  (`-1` infinite, `0` non-blocking); fills the `cqe[]` array, returns the count.

An `sqe` carries `{op, fd, ud}`; `op` is a mask of `RD`/`WR`. The `ud` pointer
is opaque to the poller and echoed back in the matching `cqe`. The runtime packs
a generation-tagged slab index into `ud` so it can reject stale completions —
see [`runtime.md`](runtime.md) §7.

### Trigger mode is a compile-time constant

```c
#if defined(__linux__) || defined(__APPLE__)
#define PLATFORM_POLLER_TRIGGER_MODE PLATFORM_POLLER_TRIGGER_ET
#endif
#if defined(_WIN32)
#define PLATFORM_POLLER_TRIGGER_MODE PLATFORM_POLLER_TRIGGER_LT
#endif
```

| | Linux / macOS | Windows |
|---|---------------|---------|
| Backend | epoll / kqueue | wepoll (epoll over `\Device\Afd`) |
| Trigger | **Edge-triggered** (`EPOLLET` / `EV_CLEAR`) | **Level-triggered + one-shot** |
| Register | once, RD+WR together | per readiness, re-armed each time |
| Caller obligation | drain fd to `EAGAIN` before re-parking | re-arm via `mod` after each event |

`iowait` (in the runtime) is the one place that branches on
`PLATFORM_POLLER_TRIGGER_MODE`: ET registers once and leaves the fd armed; LT
re-arms the still-parked directions after every event. Protocol code never sees
this difference.

### Backend specifics

- **Linux (epoll).** `add`/`mod` set `EPOLLET` plus `EPOLLIN`/`EPOLLOUT`;
  `wait` translates back, folding `EPOLLHUP`/`EPOLLERR` into *both* RD and WR
  readiness so a hangup wakes whichever direction is parked. `EINTR` is retried.
- **macOS (kqueue).** Read and write are separate filters (`EVFILT_READ` /
  `EVFILT_WRITE`) added with `EV_CLEAR` (edge-triggered). Because kqueue reports
  them as separate events, `wait` **merges events by `udata`** into a single
  `cqe` per fd, matching epoll's one-entry-per-fd shape so the runtime sees a
  uniform stream.
- **Windows (wepoll).** Provides an epoll-compatible API over AFD; level-
  triggered with one-shot semantics, hence the re-arm contract above.

`PLATFORM_POLLER_CQE_NUM` (128) bounds how many completions a single
`wait` returns.

## 4. Socket

`platform-socket` wraps the BSD/Winsock divergence:

- **Lifecycle:** `platform_socket_startup()` / `cleanup()` — `WSAStartup` /
  `WSACleanup` on Windows; on Unix `startup` ignores `SIGPIPE` so a write to a
  closed peer returns `EPIPE` instead of killing the process. `xylem_run()`
  calls these for you.
- **Non-blocking:** `platform_socket_enable_nonblocking(fd, true)` — every fd
  the runtime parks on must be non-blocking.
- **socketpair:** `platform_socket_socketpair()` backs the scheduler's
  wakeup fd (a self-pipe used to interrupt a blocked poll driver).
- **I/O + errors:** `send`/`recv` plus `platform_socket_get_lasterror()` and
  `platform_socket_tostring()`, normalizing `EAGAIN`/`EWOULDBLOCK` across
  platforms (`PLATFORM_SO_ERROR_*`) so protocol code can test one set of codes.

## 5. Virtual memory

`platform-vmem` separates address-range ownership from the lifecycle of pages
inside that range:

| Function | Linux | macOS / iOS | Windows |
|----------|-------|-------------|---------|
| `page_size()` | `sysconf(_SC_PAGESIZE)` | `sysconf(_SC_PAGESIZE)` | `GetSystemInfo` |
| `reserve(size)` | anonymous private `mmap(RW)` | anonymous private `mmap(RW)` | `VirtualAlloc(MEM_RESERVE)` |
| `commit(ptr,size)` | no OS transition | `MADV_FREE_REUSE` | `VirtualAlloc(MEM_COMMIT)` |
| `decommit(ptr,size)` | `MADV_FREE` | `MADV_FREE_REUSABLE` | `VirtualFree(MEM_DECOMMIT)` |
| `release(ptr,size)` | `munmap` | `munmap` | `VirtualFree(MEM_RELEASE)` |

`reserve()` returns one page-aligned range. Callers treat pages as unavailable
until `commit()` succeeds. `decommit()` preserves the reservation but makes the
previous contents unspecified; the range must not be accessed again until a
later `commit()` succeeds. `release()` accepts only the complete reservation.

Linux maps the complete range read/write once. `commit()` therefore has no OS
mapping transition. `decommit()` uses `MADV_FREE`, allowing the kernel to
discard pages lazily under memory pressure. A range may still contain its old
bytes before reclamation, so callers must never depend on recommitted contents.
Linux also applies `MADV_NOHUGEPAGE` to the complete mapping as a best-effort
hint; failure of that hint does not fail the reservation.

Darwin uses `MADV_FREE_REUSABLE` and `MADV_FREE_REUSE` as a pair. Their reclaim
behavior is similar to lazy free, while the paired transitions keep reusable
memory accounting accurate for macOS and iOS.

Windows reserves address space with `PAGE_NOACCESS`, commits individual ranges
as `PAGE_READWRITE`, and decommits them with `MEM_DECOMMIT`. Decommitted pages no
longer consume system commit charge, while the containing address range remains
reserved. The system page size is immutable for the process and is cached after
the first `GetSystemInfo` query so cold-slot initialization does not repeat that
query.

The vmem operations do not change ASAN shadow state. Callers use
`VMEM_ASAN_POISON` and `VMEM_ASAN_UNPOISON` explicitly: unpoison before commit or
release when the range was previously poisoned, and poison after a successful
decommit. Non-ASAN builds compile these macros to no-ops.

### Coroutine slot page policy

Coroutine layout belongs to minicoro. `runtime.c` supplies `MCO_GET_PAGE_SIZE`
and the commit/decommit hooks using `platform-vmem`. The scheduler allocator
returns each slot with explicit cold/hot state, so minicoro can prepare cold
storage or restore hot storage without querying its virtual-memory mapping.
Arena and copool therefore do not depend on minicoro's metadata, context, or
stack layout.

On Windows x64 ASM, the embedded layout is:

```
low address
+------------------------------+
| metadata: committed RW       |
+------------------------------+ <- stack_low
| lower stack: uncommitted     |
+------------------------------+
| moving guard: RW | PAGE_GUARD|
+------------------------------+ <- initial StackLimit
| top stack page: committed RW |
+------------------------------+ <- stack high / StackBase
high address
```

Metadata is nonempty, page-aligned, and ends at `stack_low`. The stack contains
at least the guard and top pages; pages below the guard begin uncommitted.
`StackLimit` is the low address of the first ordinary read/write stack page
immediately above the guard. Windows stack growth moves that limit and guard
downward while the coroutine runs, and minicoro saves the current limit in its
context so a coroutine may resume on another worker. Minicoro's delayed
stack-range and magic-number check remains the overflow fallback.

For a cold Windows x64 ASM slot, `mco_create()` validates the page-aligned layout
after its allocator returns, commits metadata and the initial guard/top pair,
applies `PAGE_GUARD` directly through `VirtualProtect()`, and installs the initial
`StackLimit`. If stack commit or guard setup fails after metadata commit,
minicoro decommits the complete slot before returning the error. The library
does not catch `EXCEPTION_STACK_OVERFLOW` or call `_resetstkoflw()`. Code that
catches that exception and intends to continue on the native thread must restore
the thread's overflow state itself before normal execution resumes.

Cold Windows ASM initialization still requires separate metadata and initial
stack commits plus guard protection. A producer that outpaces all workers can
therefore measure cold-slot setup rather than hot reuse. Once slots circulate
through worker-local or shared caches, allocation and return perform no VM
operation. A hot slot retains its current committed stack extent and moving
guard until it spills back to arena.

Windows Fiber commits the arena slot as one block before minicoro writes its
metadata; `CreateFiberEx()` / `DeleteFiber()` own the separate Fiber stack. Unix
uses whole-slot policy for ASM/ucontext-style embedded layouts. Both remain hot
without a platform reuse operation.

The arena owns ASAN shadow transitions for coroutine storage. It unpoisons a
complete slot before returning it from `arena_alloc()`, allowing Windows moving
guard pages to enter any reserved stack page. It poisons a slot only after a
successful `platform_vmem_decommit()`. Region release unpoisons the complete
reservation before `platform_vmem_release()` and restores poison on failure.

### Coroutine arena lifecycle

The scheduler owns the arena independently from the local and shared copools.
Its minicoro allocator callback checks those pools before requesting cold slots
from `arena -> platform-vmem`. Minicoro prepares allocator storage inside
`mco_create()` after the callback returns a slot. The lower-level `mco_init()`
requires caller-provided storage to already be accessible and only initializes
coroutine metadata and context. An arena eagerly reserves and fully decommits
its first complete multi-slot region, then grows when its free-slot array cannot
satisfy an allocation.

Cold arena addresses may enter worker-local or shared caches unchanged. Minicoro
prepares a cold slot when it is first used; completed coroutines return as hot
slots without a VM operation. Cache overflow returns a complete slot to arena,
whose successful full decommit makes it cold and republishes the address. A
failed decommit is logged but not added to the free array.

Neither cache eviction nor slot decommit releases part of a reservation.
Scheduler teardown destroys each worker-local pool and the shared pool before
calling `arena_destroy()`, which releases every complete arena-backed region.
Consequently, arena-backed slots share region mappings, and Unix VMA consumption
grows once per successfully reserved region rather than once per coroutine.

## 6. Semaphore

`platform-sem` is a counting semaphore with `create/destroy/post/wait/timedwait`.
The scheduler uses one per worker for parking idle workers, and `runtime.c` uses
one as the run/shutdown gate. `timedwait` takes a millisecond timeout so an idle
worker can still wake to service its next due timer.

## 7. Info, I/O, string, serial

- **`platform-info`** — `getcpus()` (default worker count), `gettid`/`getpid`,
  `getlocaltime`/`gmtime`/`mkgmtime` (logger timestamps, HTTP dates), and
  `getrandom()` for CSPRNG bytes (`BCryptGenRandom` / `/dev/urandom`). Note the
  per-OS `platform_tid_t`/`platform_pid_t` typedefs.
- **`platform-io`** — `fopen`/`vsprintf`/`stat` wrappers that pick the MSVC
  `*_s` / `_stat64` forms where required. `platform_io_stat_t` is the trimmed
  stat used by the HTTP file server. `PLATFORM_PATH_SEPARATOR` abstracts `/` vs
  `\`.
- **`platform-string`** — safe string helpers normalizing the `*_s` MSVC
  variants.
- **`platform-serial`** — serial-port configuration and I/O (termios vs. Win32
  DCB), backing the public `xylem-serial` module.

## 8. Adding a platform or concern

- **New OS backend:** add `src/platform/<os>/platform-*.c`, implement every
  interface header, and define `PLATFORM_POLLER_TRIGGER_MODE`. If the poller is
  edge-triggered and reports RD/WR separately (like kqueue), merge by `ud` in
  `wait` to keep the one-cqe-per-fd contract.
- **New concern:** add `platform-<concern>.h`, both backends, and include it
  from `platform.h`. Keep the `0`/`-1`/`NULL` error convention.

## 9. Related docs

- Runtime (primary consumer): [`runtime.md`](runtime.md)
- System overview: [`../architecture.md`](../architecture.md)
- Conventions: [`../conventions.md`](../conventions.md)
