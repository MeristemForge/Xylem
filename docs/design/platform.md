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
| `platform-vmem` | Reserve/commit/protect (coroutine stacks) | mmap/mprotect | mmap/mprotect | VirtualAlloc |
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

`platform-vmem` exposes a reserve/commit/protect model so the scheduler can
allocate coroutine stacks with a guard page:

| Function | Unix | Windows |
|----------|------|---------|
| `reserve(size)` | `mmap(PROT_NONE)` | `VirtualAlloc(MEM_RESERVE)` |
| `commit(ptr,size)` | `mprotect(RW)` | `VirtualAlloc(MEM_COMMIT)` |
| `decommit(ptr,size)` | `madvise(DONTNEED)` / remap | `VirtualFree(MEM_DECOMMIT)` |
| `release(ptr,size)` | `munmap` | `VirtualFree(MEM_RELEASE)` |
| `protect(ptr,size,prot)` | `mprotect` | `VirtualProtect` |

The runtime uses this to reserve a stack, commit it, mark one page `PROT_NONE`
as an overflow guard, and `decommit` (not `release`) on free so the reservation
can be recycled by the coroutine pool. See [`runtime.md`](runtime.md) §6.

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
