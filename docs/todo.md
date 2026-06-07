# TODO

## TLS: defer the server handshake to first I/O (lazy handshake)

### Problem

`tls_accept` does `accept()` **plus the full TLS handshake inline** in the
single acceptor coroutine (`tls.c` → `tls_accept` → `_tls_server_handshake` →
`_tls_do_handshake`). Handshakes are therefore serialized: while the acceptor
drives connection A's multi-round-trip handshake (parked on iowait), it cannot
accept/handshake B.

Benchmark evidence (TLS, loopback): xylem only ever established ~150–170
concurrent TLS connections regardless of the target (1k/10k), while go/rust
reached the full 1k/10k and libuv/boost reached 500/2500. The low server RSS
(21–37 MB) was an artifact of there being only ~170 coroutines, not efficiency.
ConnRate was also the weakest (~700–900/s vs ~1800–2000/s).

### Fix (option A): accept returns immediately, handshake runs on first read/write

Match the Go/Rust model: `tls_accept` returns a *pending* connection without
handshaking; the handshake is driven lazily on the first `tls_read`/`tls_write`
(or an explicit `tls_handshake()`), inside the per-connection handler coroutine,
so handshakes parallelize across the scheduler. A single acceptor then suffices
(≈ Go's single-accept-loop model); multi-listener `SO_REUSEPORT` drops from
"required" to an optional way to spread the `accept()` syscall itself.

### Handshake state

Reuse the calloc 0-value as "no lazy handshake needed":

```
enum { HS_DONE = 0, HS_PENDING = 1, HS_FAILED = 2 };
```

- client/dial: eager handshake as today, `hs_state` stays `HS_DONE`.
- lazy accept: set `HS_PENDING`.
- `ensure` sees `HS_DONE` → return 0 immediately (fast path for client conns and
  already-handshaked server conns).

### 1. `tls.h` — struct + new API

Add to `struct tls_conn_s`:

```c
xylem_mutex_t*  hs_mu;          // handshake driver lock (elects one driver)
uint64_t        hs_timeout_ms;  // copied from ln->opts at accept time
_Atomic int     hs_state;       // HS_DONE / HS_PENDING / HS_FAILED
```

Declare: `extern int tls_handshake(tls_conn_t* tls);`

### 2. `tls.c`

**a.** `_tls_conn_create`: also create `hs_mu`; `_tls_conn_free`: destroy it.
`hs_state` defaults to 0 (`HS_DONE`) via calloc.

**b.** Split `_tls_server_handshake` into cheap accept setup + a drive function
that does **not** destroy on failure (the handler's `tls_close` owns teardown):

```c
static int _tls_server_handshake_drive(tls_conn_t* tls) {
    tls->be = tls_backend_conn_create(tls->ctx->be, true);
    if (!tls->be) return -1;
    tls_backend_handshake_cfg_t cfg = {0};
    cfg.verify = tls->ctx->verify_client ? TLS_BACKEND_VERIFY_REQUIRE
                                          : TLS_BACKEND_VERIFY_NONE;
    tls_backend_conn_configure(tls->be, &cfg);
    _tls_set_deadline(tls, _tls_make_deadline(tls->hs_timeout_ms));
    int rc = _tls_do_handshake(tls);
    _tls_set_deadline(tls, 0);
    if (rc == 0) tls_backend_conn_get_alpn(tls->be, tls->alpn, sizeof tls->alpn);
    return rc;
}
```

**c.** `_tls_ensure_handshake` — single driver via `hs_mu` + atomic flag (no
cond needed; a second coroutine simply blocks on `lock(hs_mu)` until the driver
unlocks, then reads the result):

```c
static int _tls_ensure_handshake(tls_conn_t* tls) {
    int st = atomic_load_explicit(&tls->hs_state, memory_order_acquire);
    if (st == HS_DONE)   return 0;
    if (st == HS_FAILED) return -1;
    xylem_mutex_lock(tls->hs_mu);
    st = atomic_load_explicit(&tls->hs_state, memory_order_acquire);
    if (st == HS_DONE)   { xylem_mutex_unlock(tls->hs_mu); return 0; }
    if (st == HS_FAILED) { xylem_mutex_unlock(tls->hs_mu); return -1; }
    int rc = _tls_server_handshake_drive(tls);   // holds hs_mu across iowait park
    atomic_store_explicit(&tls->hs_state, rc == 0 ? HS_DONE : HS_FAILED,
                          memory_order_release);
    xylem_mutex_unlock(tls->hs_mu);
    return rc;
}
```

Why the wait exists: xylem allows one reader + one writer coroutine on the same
conn concurrently. Pre-handshake, both might call `ensure`. The handshake is a
single state machine driving BOTH socket directions, so exactly one coroutine
must own it (two drivers would double-step the backend and double-park a
direction, violating iowait's one-parker-per-direction rule). The non-driver
can't do app I/O until the handshake completes, so it waits. The common
single-coroutine echo handler never hits this path.

**d.** `tls_accept` — accept only, no handshake (no more looping over handshake
failures):

```c
for (;;) {
    fd = _tls_accept_fd(ln);
    if (fd == INVALID) break;
    conn = _tls_conn_create(fd);
    if (!conn) { platform_socket_close(fd); break; }
    conn->ctx = ln->ctx;
    socklen_t pl = sizeof conn->peer_addr.storage;
    getpeername(fd, (struct sockaddr*)&conn->peer_addr.storage, &pl);
    conn->hs_timeout_ms = ln->opts.handshake_timeout_ms;
    atomic_store(&conn->hs_state, HS_PENDING);
    ready = conn; break;          // return immediately; handshake deferred
}
```

**e.** `tls_read`/`tls_write` — after the existing ref + `closed` check, before
the loop:

```c
if (!closed && _tls_ensure_handshake(tls) == 0)
    ret = _tls_read_loop(...);    // else ret stays -1
```

**f.** New `tls_handshake(tls)`: `_tls_conn_ref` → `closed` check →
`_tls_ensure_handshake` → `_tls_conn_unref`; returns 0/-1.

**g.** `tls_get_alpn`: leave non-blocking (empty before handshake). Callers that
need ALPN early call `tls_handshake()` first.

**h.** client/dial path unchanged (eager handshake, `hs_state` stays `HS_DONE`):
`tls_dial`, `_tls_client_handshake`, `tls_client_handshake_fd`.

### 3. Public API — `xylem-tls.h` / `xylem-tls.c`

- Header: `extern int xylem_tls_handshake(xylem_tls_conn_t* tls);` (doc: accept
  no longer completes the handshake; call this to force it before reading
  ALPN / peer cert).
- Shim: `return tls ? tls_handshake(&tls->internal) : -1;`

### 4. `http-transport-tls.c` (required to preserve HTTPS behavior)

The HTTPS accept loop relies on the handshake/ALPN being done after `tls_accept`
(to dispatch h2 vs http/1.1). After `tls_accept`, call `tls_handshake(conn)`
explicitly; on failure close + continue (equivalent to the old absorbed
per-connection failure); on success dispatch by ALPN. Place it at the **start of
the per-connection handler coroutine** (not inline in the accept loop) so HTTPS
also gets concurrent handshakes. Read this file first to confirm
accept-then-spawn vs inline structure before placing the call.

### Correctness notes

- `hs_mu` held across iowait parks is fine (coroutine mutex; contended `lock`
  yields). Non-driver takes only `hs_mu` → no lock-order inversion vs the
  driver's inner `ssl_mu`/`rd_mu`/`wr_mu`.
- `ensure` runs under the ref already taken by read/write/handshake, so the conn
  can't be freed mid-handshake.
- Race with `tls_close`: `closed` + `iowait_close` wakes the parked handshake →
  `_tls_do_handshake` returns -1 → `HS_FAILED`. Safe.
- `_tls_conn_destroy` / `_tls_flush_close_notify` already null-check `be`, so a
  half-open (never-handshaked, `be == NULL`) conn closes cleanly.

### Tradeoffs (document in headers)

- Error surface moves: handshake failure now appears at the first
  `tls_read`/`tls_write` or `tls_handshake` (returns -1), not as a NULL from
  `tls_accept`. NULL from accept means only "listener closed".
- Timeout semantics: `handshake_timeout_ms` now starts at first I/O (when the
  handshake begins), not at accept. A connection whose handler never reads is
  not bounded by the handshake timeout until then — inherent to lazy; rely on
  prompt handler reads + fd/backlog limits.
- Half-open pending conns accumulate between accept and handshake; bounded by
  listen backlog and fd limits. Add a per-listener pending cap later if needed.

### DTLS

Out of scope here (separate `xylem_dtls_accept` path that already caches ALPN
per session). Can mirror this model later.

### Validation

1. Unit tests: lazy handshake success; failure (bad cert, timeout) surfaces at
   first I/O; explicit `tls_handshake`; split reader+writer coroutines both
   triggering pre-handshake (the `hs_mu` wait path); handshake-timeout reclaim.
2. HTTPS h2/http1.1 ALPN dispatch regression.
3. Re-run the TLS benchmark matrix: expect xylem established connections to rise
   from ~170 toward 1k/10k and connrate to climb; then compare throughput/memory
   against go/rust at equal connection counts.
