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
per-worker timer heap (`scheduler_timer_t`, see [`runtime.md`](runtime.md) §8), so it
**requires a running runtime** — the callback is dispatched by a scheduler
worker.

```c
xylem_timer_t* t = xylem_timer_after(500, on_fire, ud);  /* one-shot, 500 ms */
xylem_timer_t* e = xylem_timer_every(1000, on_tick, ud); /* periodic callback */
xylem_timer_reset(t, 1000);   /* re-arm 1000 ms from now, same cb/ud */
xylem_timer_stop(e);          /* callback-safe; keeps the handle alive */
xylem_timer_destroy(e);       /* final owner release */
xylem_timer_destroy(t);       /* destroy also stops as a fallback */
```

Semantics and the lifetime gotcha:

- `after(delay_ms, cb, ud)` arms a **one-shot** timer and returns a handle.
- `every(interval_ms, cb, ud)` arms a **periodic callback** timer. It is
  fixed-delay: the next fire is scheduled after the previous callback returns.
- Callback dispatch requires allocating a fire context and coroutine. Under
  allocation failure, dispatch is best-effort: a one-shot fire is dropped and
  not retried; a periodic timer skips that fire and continues with its next
  interval. No asynchronous error is reported.
- **The handle must always be released with `destroy()`**, even after the
  callback has already fired. `stop`/`reset` return `true` when they removed a
  queued fire or cancelled/overwrote a deferred reset from the current in-flight
  callback. The boolean is not a general "callback will not run" signal: an
  already dispatched callback may still complete, and `reset` may still re-arm
  the timer. `destroy` stops the timer as a cleanup fallback.
- Calls on different timer handles may run concurrently. Operations on the same
  public timer handle, including `stop`/`stop`, `reset`/`reset`, and
  `stop`/`reset`, require external synchronization. `stop` and `reset` may be
  called from the timer callback and do not consume the handle. `destroy`
  consumes the handle and must be called by the owner after any callback-side
  timer operations have finished, never from the callback itself. It does not
  wait for an already dispatched callback, so callback and user-data resources
  must remain alive until that callback returns.
- `ticker` remains the pull-based periodic API: it produces coalesced ticks for
  a consumer to receive, while `every` runs user callback code on each fire.

## 3. Utils (`xylem-utils`)

A small set of portable primitives used across the library:

- **`xylem_utils_getnow(precision)`** — the current UTC wall-clock time.
  `precision` is one of `SEC` / `MSEC` / `USEC` / `NSEC`. Deadline-taking APIs
  use absolute millisecond timestamps measured against `getnow(MSEC)`.
  Timer/ticker delay and interval parameters are relative durations; the
  scheduler converts them internally to absolute heap expiry times. Values may
  move if the system clock is adjusted.
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
- `xylem_serial_close` consumes the serial handle. `close(NULL)` is safe, but a
  non-NULL handle is invalid after `close` returns; do not close or use the same
  handle again.

Because serial I/O blocks, calling it from a coroutine stalls that worker;
offload it via `xylem_await()` ([`runtime.md`](runtime.md) §10) if you need to
combine serial with coroutine networking.

## 5. Related docs

- Runtime timer heaps and blocking offload: [`runtime.md`](runtime.md)
- Endianness companion: [`encoding.md`](encoding.md) §5
- CSPRNG / serial backends: [`platform.md`](platform.md)
- Conventions (options, lifetime, threading): [`../conventions.md`](../conventions.md)
- Tests: [`../test/strategy.md`](../test/strategy.md) *(planned)*
