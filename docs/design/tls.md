# TLS Design

The TLS module wraps OpenSSL (>= 3.5) in the same coroutine-friendly,
park/resume networking model as TCP, so `xylem_tls_read` / `xylem_tls_write` /
`xylem_tls_accept` suspend the calling coroutine instead of blocking a thread.
It is an optional layer gated by `XYLEM_ENABLE_TLS`; when that is off the module
is replaced by a stub and `https://` / `wss://` are unavailable.

Sources: public API in `include/xylem/net/xylem-tls.h`, implementation in
`src/net/tls/xylem-tls.c`, the system-CA platform shim in
`src/platform/{win,unix}/platform-tls.c`. DTLS (`src/net/xylem-dtls.c`) mirrors
this design over datagrams.

## 1. Object model

Three opaque handles, all created/destroyed by the module:

| Type | Role | Lifetime |
|------|------|----------|
| `xylem_tls_ctx_t` | Shared configuration (certs, CAs, ALPN, verify policy) | Created once, shared by many connections |
| `xylem_tls_listener_t` | Bound server socket | One per listening port |
| `xylem_tls_conn_t` | A single TLS connection | One per accepted/dialed peer |

A single `ctx` is reused for both the client (`xylem_tls_dial`) and server
(`xylem_tls_listen`) roles. This is why verification policy is stored as intent
on the ctx and applied per connection by role (see §4), rather than baked into
the shared `SSL_CTX`.

## 2. Decoupling the SSL state machine from socket I/O

The central design choice: each connection drives OpenSSL through a pair of
in-memory BIOs, never letting OpenSSL touch the socket directly.

```
   application                OpenSSL                 socket
  xylem_tls_read  <--  SSL_read(ssl)  <-- rbio <-- _tls_pump_in  <-- recv()
  xylem_tls_write -->  SSL_write(ssl) --> wbio --> _tls_pump_out --> send()
```

- `SSL_new` is bound to two memory BIOs (`rbio`, `wbio`) via `SSL_set_bio`.
  OpenSSL only ever reads/writes plaintext to the application and ciphertext
  to/from these memory buffers; it issues no syscalls of its own.
- `_tls_pump_in` moves inbound ciphertext `recv() -> rbio`; `_tls_pump_out`
  drains outbound ciphertext `wbio -> send()`. These are the only functions
  that touch the socket.
- When `SSL_read` / `SSL_write` / `SSL_do_handshake` return `WANT_READ` /
  `WANT_WRITE`, the driver loops: pump the socket in the requested direction
  (parking the coroutine via `iowait` if the kernel buffer is empty/full), then
  retry the SSL call.

This is what lets a blocking-looking `SSL_read` cooperate with the coroutine
scheduler: the suspend point is the socket pump, not OpenSSL.

A `TLS_IO_CHUNK` (16 KiB) scratch buffer per direction matches the TLS record
cap, so a full record moves per pump; OpenSSL reassembles records that span
chunks.

## 3. Concurrency: the three locks

A connection supports full duplex -- one coroutine reading while another writes
the same connection. Three locks make that safe:

| Lock | Guards | Granularity |
|------|--------|-------------|
| `ssl_mu` | the `SSL` object and both memory BIOs | short: held only across a single SSL/BIO call |
| `rd_mu` | the iowait **read** direction | long: held for an entire `_tls_pump_in` |
| `wr_mu` | the iowait **write** direction | long: held for an entire `_tls_pump_out` |

Rationale:

- **`ssl_mu` exists because a single OpenSSL `SSL` object is not thread-safe.**
  `SSL_read`, `SSL_write`, `SSL_do_handshake`, and the `BIO_read`/`BIO_write` on
  its memory BIOs all mutate one shared state machine, so they are serialized.
- **`ssl_mu` is never held across a socket park.** The pumps take `ssl_mu` only
  for the instantaneous `BIO_read`/`BIO_write`, then release it before
  `iowait_read`/`iowait_write` (which may suspend indefinitely). If a single
  lock were held across the park, a reader waiting for inbound data would block
  the writer forever -- a deadlock, since the data may only arrive in response
  to what the writer sends.
- **`rd_mu` / `wr_mu` enforce a single parker per direction.** `iowait` allows
  only one coroutine to wait on each direction of a socket; these locks make
  the reader the sole owner of the read direction and the writer the sole owner
  of the write direction. Because they are separate locks, read and write
  proceed concurrently.

Note that `_tls_pump_out` is called from the read path too (to flush a TLS 1.3
KeyUpdate or renegotiation message surfaced as `WANT_WRITE` during `SSL_read`),
and `_tls_pump_in` from the write path, so both pumps are reachable from either
coroutine -- the lock split keeps that safe.

## 4. Verification policy (per role, per connection)

`SSL_VERIFY_*` has opposite meaning on each side, and the ctx is shared, so the
policy is stored as two booleans and applied to each new `SSL` at handshake time
by `_tls_apply_verify`:

| Setter | Role | Default | Applied mode |
|--------|------|---------|--------------|
| `xylem_tls_ctx_verify_server` | client (`dial`) | **true** | `SSL_VERIFY_PEER`, or `NONE` if disabled |
| `xylem_tls_ctx_verify_client` | server (`listen`) | **false** | `SSL_VERIFY_PEER \| FAIL_IF_NO_PEER_CERT` (mTLS), or `NONE` |

Setting verify on the per-connection `SSL` (not the shared `SSL_CTX`) is what
keeps a reused ctx correct: a client dial still verifies the server even if the
same ctx also accepts connections that request no client cert. Defaults are
secure-by-default for the common case (clients verify servers; plain servers do
not challenge clients).

Peer **identity** is separate from chain trust. `opts->server_name` drives both
the SNI extension sent by a client and the hostname/IP checked against the peer
certificate (`SSL_set1_host`). IP literals are not sent as SNI (RFC 6066) but
are still used for identity verification. A verified client with no
`server_name` is logged as a MITM risk: the chain is trusted but the identity is
unchecked.

## 5. Trust anchors

| Function | Adds to trust store |
|----------|---------------------|
| `xylem_tls_ctx_load_ca` | the CAs in a PEM file (narrow trust: private PKI / mTLS) |
| `xylem_tls_ctx_load_system_ca` | the platform root store (public CAs) |

`load_system_ca` is a thin platform shim (`platform_tls_load_system_ca`):

- **Unix:** `SSL_CTX_set_default_verify_paths` -- OpenSSL's default paths
  resolve to the system CA bundle.
- **Windows:** `SSL_CTX_load_verify_store("org.openssl.winstore://")`. OpenSSL's
  default verify paths point at a build-time directory that is empty on Windows,
  so they silently load nothing; the winstore loader (OpenSSL 3.2+) reads the
  Windows ROOT store on demand during chain building. This is the one
  OS-specific behavior, isolated in the platform layer per the platform-code
  rule.

The two calls compose (system roots plus a private CA). A server doing mTLS
should use `load_ca` alone, since adding the public roots would let any
client cert chaining to a public CA authenticate.

## 6. Certificates and SNI

Certificates are loaded as a *TLS identity* = leaf cert + intermediate chain +
matching private key, parsed once from PEM into independent OpenSSL objects.

- `xylem_tls_ctx_load_cert` reads from files; `load_cert_mem` reads the same PEM
  from memory buffers (for certs sourced from a secret store, env, or embedded
  resource). Both share `_tls_parse_pem_identity`, which also rejects a
  cert/key mismatch up front rather than mid-handshake.
- With `hostname == NULL` the identity is installed as the ctx **default** cert.
  With a hostname it is stored in an SNI table.
- The SNI callback (`_tls_ctx_sni_cb`, registered once at ctx creation) selects
  a per-host cert by setting it directly on the connection's `SSL` -- the shared
  ctx config (keylog, ALPN, verify) stays in force. This mirrors Go's
  `GetCertificate` and rustls' cert resolver: SNI only picks a cert, never swaps
  the whole configuration. No host match leaves the default cert in place. The
  callback is a no-op until an SNI entry exists, so registering it
  unconditionally keeps the `SSL_CTX` read-only once connections start.

## 7. ALPN

`xylem_tls_ctx_set_alpn` stores one wire-format protocol list and configures
both roles from it: the client offers the list, and the server's select callback
picks the first mutually supported protocol. The unused half is inert per role.
After the handshake, `_tls_cache_alpn` copies OpenSSL's borrowed,
non-NUL-terminated result into the connection's own `alpn` buffer once;
`xylem_tls_get_alpn` then reads connection-local state
with no lock and no OpenSSL call.

## 8. Connection lifecycle

**Dial (client):** resolve host (numeric literal used as-is, else DNS first
result) -> non-blocking `connect`, parking on the write direction until writable
-> `_tls_client_handshake` (set connect state, apply verify, apply server_name,
drive handshake) -> cache ALPN. A single deadline derived from
`handshake_timeout_ms` bounds connect + handshake together.

**Accept (server):** `_tls_accept_fd` parks on the listener's read direction and
backs off on transient accept errors -> `_tls_server_handshake` per connection
(set accept state, apply verify, arm handshake deadline, drive handshake).
**Per-connection handshake failures are absorbed**: the bad connection is
dropped and accept keeps looping, so one bad client cannot tear down the accept
loop. `xylem_tls_accept` returns NULL only when the listener is closed.

**Proxy / upgrade:** `tls_client_handshake_fd` (internal, in `tls.h`)
wraps an already-connected fd -- e.g. after an HTTP CONNECT tunnel -- and runs
the client handshake, verifying `server_name` rather than the proxy address.

## 9. Shutdown and teardown safety

- `xylem_tls_close` and `xylem_tls_close_listener` are idempotent via an atomic
  `closed` flag (`atomic_exchange`), and call `iowait_close` to wake any parked
  coroutine.
- Connections are reference counted. `xylem_tls_read` / `write` take a reference
  **before** testing `closed`, closing the race where a concurrent `close` on
  another thread could free the connection between the test and use; the final
  unref does teardown.
- `_tls_conn_free` disarms the deadline timer before destroying the waiter --
  an armed timer holds an iowait reference, so skipping this would keep a
  connection alive until the timer fires (e.g. a failed handshake lingering for
  the whole handshake timeout).
- Teardown distinguishes a graceful path (`_tls_conn_unref` does `SSL_shutdown`
  to send `close_notify`) from a plain destroy (`_tls_conn_destroy`, used on
  handshake failure where no clean shutdown is owed).

## 10. Security notes and boundaries

- **Secure by default for clients.** A freshly created ctx verifies server
  certificates; you only weaken this explicitly with
  `xylem_tls_ctx_verify_server(ctx, false)` (tests / trusted networks only).
- **Always set `opts->server_name` on a verifying client.** Chain trust without
  identity verification accepts any trusted-CA cert and is a MITM risk.
- **TLS 1.2 is the floor.** `SSL_CTX_set_min_proto_version(TLS1_2_VERSION)` is
  set at ctx creation; there is no public knob to lower it.
- **Keylog is a debugging aid.** `xylem_tls_ctx_set_keylog` writes NSS keylog
  lines for Wireshark; it exposes session secrets and must not be enabled in
  production.

## 11. Related docs

- Park/resume and per-direction arbitration: [`../architecture.md`](../architecture.md) §6,
  [`runtime.md`](runtime.md).
- Always-available (non-OpenSSL) crypto: [`crypto.md`](crypto.md).
- System-CA platform shim and the platform-code rule: [`platform.md`](platform.md).
- Feature gate `XYLEM_ENABLE_TLS`: [`../architecture.md`](../architecture.md) §8,
  [`../build.md`](../build.md).
