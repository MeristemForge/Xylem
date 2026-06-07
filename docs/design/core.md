# Core Utilities Design

The "core" modules are the small, broadly-used helpers that don't belong to a
larger subsystem: the logger, the public timer, the utils grab-bag (time,
randomness, endianness), and serial-port I/O. They sit at the base of the
library and are pulled in by `include/xylem.h`.

Sources: `include/xylem/xylem-{logger,timer,utils,serial}.h`, implementations in
`src/xylem-*.c`. Serial backends live in `src/platform/{unix,win}/platform-serial.c`.

## 1. Logger (`xylem-logger`)

An asynchronous, leveled logger. The defining property: **log writes never block
the caller**. A dedicated worker thread owns the file/sink, so application code
(and scheduler workers) hand off a formatted message and move on, paying no file
I/O or callback latency on the hot path.

### API

```c
xylem_logger_init("app.log", NULL);   /* NULL filename -> stdout; NULL opts -> INFO */
xylem_logi("listening on %u", port);  /* DEBUG/INFO/WARN/ERROR macros */
xylem_loge("tcp fd=%d error: %s", fd, msg);
xylem_logger_deinit();
```

- Four macros — `xylem_logd/i/w/e` — capture `__FILE__`/`__LINE__` automatically
  and forward to `xylem_logger_log()` (which you don't call directly).
- Levels: `DEBUG < INFO < WARN < ERROR`; messages below the configured level are
  dropped.
- **Options have no "zero means default" rule** (the level enum starts at
  `DEBUG = 0`): when you pass a non-NULL `xylem_logger_opts_t`, every field is
  taken verbatim — see [`../conventions.md`](../conventions.md) §4. Fields are
  `level` and `max_file_size` (rollover threshold in bytes; `0` = no limit;
  ignored for stdout).

### Custom sink

`xylem_logger_set_callback(cb, ud)` redirects output to a user function instead
of the file — useful for routing into a host application's logging system. The
callback receives `(level, msg, ud)`.

### Threading

All log macros are **any-thread**: the library is the one place internal code
emits diagnostics (`xylem_loge`), so it must be callable from scheduler workers,
dynpool threads, and application threads alike. The async worker serializes the
actual writes.

## 2. Timer (`xylem-timer`)

The public, fire-a-callback timer. It is a thin facade over the scheduler's
internal timer wheel (`sched_timer`, see [`runtime.md`](runtime.md) §8), so it
**requires a running runtime** — the callback is dispatched by a scheduler
worker.

```c
xylem_timer_t* t = xylem_timer_after(500, on_fire, ud);  /* one-shot, 500 ms */
xylem_timer_reset(t, 1000);   /* re-arm 1000 ms from now, same cb/ud */
xylem_timer_cancel(t);        /* cancel + release the handle */
```

Semantics and the lifetime gotcha:

- `after(delay_ms, cb, ud)` arms a **one-shot** timer and returns a handle.
- **The handle must always be released with `cancel()`**, even after the
  callback has already fired. `cancel`/`reset` return `true` if they cancelled a
  *pending* fire before it ran — callers pairing the arm with reference-counted
  `ud` use that boolean to decide whether the callback will still run and drop
  the ref.
- `cancel`/`reset` are thread-safe but **must not run concurrently with each
  other** on the same handle. A callback already in flight may still complete
  after `cancel()`.
- `after` exposes only one-shot timers; periodic behavior is built by re-arming
  with `reset()` from inside the callback. (The internal `sched_timer` supports
  native periodic repeat, but that is not surfaced publicly.)

## 3. Utils (`xylem-utils`)

A small set of portable primitives used across the library:

- **`xylem_utils_getnow(precision)`** — the monotonic clock. `precision` is one
  of `SEC` / `MSEC` / `USEC` / `NSEC`. **All deadlines in the public API are
  absolute monotonic milliseconds** measured against `getnow(MSEC)` — TCP
  read/write deadlines, the scheduler's timer heap, and iowait deadlines all use
  this one clock. Using wall-clock time for a deadline would be a bug.
- **`xylem_utils_getprng(min, max)`** — a non-cryptographic PRNG returning an
  int in `[min, max]`. For key/IV material use the CSPRNG
  (`platform_info_getrandom`, surfaced through AES internally), never this.
- **`xylem_utils_getendian()`** — returns `XYLEM_ENDIAN_LE` / `_BE`. Pairs with
  the `xylem-bswap` macro ([`encoding.md`](encoding.md) §5) for portable
  serialization.

These are pure/thread-safe (the PRNG uses thread-appropriate state).

## 4. Serial (`xylem-serial`)

Synchronous serial-port I/O, wrapping termios (Unix) and the Win32 DCB/COM API
behind one handle. Unlike the socket modules, serial I/O is **blocking, not
coroutine-integrated** — it does not park on the scheduler.

```c
xylem_serial_opts_t opts = {
    .device   = "/dev/ttyUSB0",   /* or "COM3" */
    .baudrate = XYLEM_SERIAL_BAUDRATE_115200,
    .parity   = XYLEM_SERIAL_PARITY_NONE,
    .databits = XYLEM_SERIAL_DATABITS_8,
    .stopbits = XYLEM_SERIAL_STOPBITS_1,
    .timeout_ms = 100,            /* 0 = blocking read */
};
xylem_serial_t* s = xylem_serial_open(&opts);
int n = xylem_serial_read(s, buf, len);   /* >0, 0 on timeout, -1 error */
xylem_serial_write(s, data, len);         /* writes all, or -1 */
xylem_serial_close(s);
```

- Configuration is an all-fields options struct (baud / parity / data bits /
  stop bits / flow control / read timeout); `device` is required.
- `read` blocks until ≥1 byte, the `timeout_ms` expires (returns `0`), or error
  (`-1`). `write` blocks until all bytes are sent.
- **`close` is NOT idempotent** — unlike most Xylem destroy functions, calling
  it twice on the same non-NULL handle double-frees. `close(NULL)` is safe. This
  is the one notable deviation from the library-wide idempotent-destroy rule
  ([`../conventions.md`](../conventions.md) §5), so null out your handle after
  closing.

Because serial I/O blocks, calling it from a coroutine stalls that worker;
offload it via `xylem_submit()` ([`runtime.md`](runtime.md) §10) if you need to
combine serial with coroutine networking.

## 5. Related docs

- Runtime timer wheel and blocking offload: [`runtime.md`](runtime.md)
- Endianness companion: [`encoding.md`](encoding.md) §5
- CSPRNG / serial backends: [`platform.md`](platform.md)
- Conventions (options, lifetime, threading): [`../conventions.md`](../conventions.md)
- Tests: [`../test/strategy.md`](../test/strategy.md) *(planned)*
