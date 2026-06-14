# TLS Design

The TLS module wraps OpenSSL (>= 3.5) in the same coroutine-friendly,
park/resume networking model as TCP, so `xylem_tls_read` / `xylem_tls_write` /
`xylem_tls_accept` suspend the calling coroutine instead of blocking a thread.
It is an optional layer gated by `XYLEM_ENABLE_TLS`; when that is off the module
is replaced by a stub and `https://` / `wss://` are unavailable.

Sources: public API in `include/xylem/net/xylem-tls.h`, implementation in
`src/net/tls/tls.c` (engine) and `src/net/tls/xylem-tls.c` (public shim).
OpenSSL access is isolated behind an internal backend interface, so exactly one
source file includes `<openssl/...>`; see §11. DTLS
(`src/net/tls/xylem-dtls.c`) mirrors this design over datagrams.

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
  that touch the socket. (The `BIO_write`/`BIO_read` into those memory buffers
  now go through the backend as `tls_backend_conn_feed`/`drain`; see §11.)
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

A fourth lock, `hs_mu`, is unrelated to duplex I/O: it elects the single driver
of the lazy server handshake and is described in §8. It is taken on its own,
never nested inside the three locks above.

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
| `xylem_tls_ctx_load_system_ca` | the platform root store and/or a fallback CA bundle (public CAs) |

`load_system_ca(ctx, fallback_ca_file)` is a backend responsibility
(`tls_backend_ctx_load_system_ca`), not a platform shim. It loads trust anchors
**additively** from up to two sources into the same store, and succeeds if
*either* loads:

1. the platform's native system store, when OpenSSL can read it, and
2. `fallback_ca_file` (when non-NULL), loaded as a PEM CA bundle.

The native system store, by platform:

| Platform | Native store mechanism |
|----------|------------------------|
| Linux (desktop/server) | `SSL_CTX_set_default_verify_paths` -> distro CA bundle |
| Windows | `SSL_CTX_load_verify_store("org.openssl.winstore://")` -> system ROOT store |
| macOS | `SSL_CTX_set_default_verify_paths` -> bundle shipped with the linked OpenSSL (e.g. Homebrew's `cert.pem`), **not** the Keychain |
| Android / iOS | none -- KeyStore / SecTrust are not reachable from OpenSSL |

Why additive (rather than "system, else fallback"): OpenSSL's default-paths
*CApath* is lazy -- it is consulted per-subject-hash at verify time, never
eagerly loaded -- so there is no reliable way to count whether the system store
actually holds any certificates. Loading both sources and taking the union
avoids a false "system store is empty" probe.

The `fallback_ca_file` is what makes this portable where the native store is
absent or unusable: mobile (Android/iOS), a statically linked or cross-compiled
OpenSSL whose build-time `OPENSSLDIR` does not exist on the target, or a custom
OpenSSL install with no CA bundle. Point it at a CA bundle shipped with the app
(e.g. curl's `cacert.pem` from <https://curl.se/ca/cacert.pem>); pass NULL to
use only the native store.

Notes:

- **Windows** default verify paths point at a build-time directory that is empty
  on Windows, so they load nothing; the winstore loader (OpenSSL 3.2+) reads the
  ROOT store on demand during chain building -- hence the dedicated branch.
- **Android / iOS** have no OpenSSL-readable system store, so on those platforms
  `load_system_ca` succeeds only if a `fallback_ca_file` is supplied.

`load_system_ca` and `load_ca` compose (public roots plus a private CA). A
server doing mTLS should use `load_ca` alone, since adding the public roots would
let any client cert chaining to a public CA authenticate.

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
backs off on transient accept errors, then `xylem_tls_accept` returns the
connection **without running its handshake** -- it only records the peer
address and the handshake timeout and marks the connection `HS_PENDING`. The
handshake is driven lazily on the first `tls_read`/`tls_write`, or eagerly via
`tls_handshake`, inside the per-connection handler coroutine. This is the key
scalability change: handshakes (a multi-round-trip, CPU-heavy exchange) run in
the handler and parallelize across the scheduler instead of serializing every
client behind the single acceptor.

Consequences of deferring:

- `xylem_tls_accept` returns NULL **only** when the listener is closed; it no
  longer signals a handshake failure. A failure (bad cert, protocol mismatch,
  timeout) surfaces as `-1` from the first `tls_read`/`tls_write`/`tls_handshake`,
  so a handler treats a read error as "drop this connection and keep accepting"
  -- one bad client still cannot tear down the accept loop.
- `handshake_timeout_ms` is measured from when the handshake begins (first I/O),
  not from accept. A handler that accepts but never reads is not bounded by it;
  rely on prompt handler reads plus fd/backlog limits.
- The negotiated ALPN (`tls_get_alpn`) and the peer certificate are empty until
  the handshake completes, so call `tls_handshake` first if you need them before
  any read/write.

Exactly one coroutine drives the lazy handshake. Because the handshake is a
single state machine pumping both socket directions and `iowait` permits only
one parker per direction, two concurrent drivers would double-step the backend.
A per-connection `hs_mu` elects the driver: the first caller into
`_tls_ensure_handshake` runs the handshake while a second (xylem permits one
reader + one writer per connection) blocks on `hs_mu` until the result is
published, then reads it. `hs_mu` is taken alone, never nested inside
`ssl_mu`/`rd_mu`/`wr_mu`, so there is no lock-order inversion. An `HS_DONE`
fast path skips the lock entirely once handshaked, so every client connection
and every already-handshaked server connection pays no extra cost.

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
- **Classic key exchange by default.** At ctx creation the backend pins the
  key-exchange groups to `X25519:P-256:P-384:P-521` (via
  `tls_backend_ctx_set_kx_groups`) rather than inheriting OpenSSL 3.5's
  default, which prefers a post-quantum hybrid (`X25519MLKEM768`). The hybrid
  adds KEM keygen/encapsulation work and a larger key_share to every
  handshake; choosing classic curves keeps handshakes light (higher
  handshakes/sec) at the cost of post-quantum protection. There is no public
  knob to change this; the internal backend setter exists for builds that want
  a different set (e.g. the hybrid first to retain post-quantum security).
- **Keylog is a debugging aid.** `xylem_tls_ctx_set_keylog` writes NSS keylog
  lines for Wireshark; it exposes session secrets and must not be enabled in
  production.

## 11. Backend abstraction (OpenSSL isolation)

This is a pure internal refactor: the public `xylem_tls_*` and `xylem_dtls_*`
API, ABI, and runtime behavior are unchanged. The goal is to isolate every
OpenSSL dependency behind one backend-neutral internal interface so an
alternative SSL library (wolfSSL via its OpenSSL-compatibility layer, or
mbedTLS with a native implementation) can be added later by writing a single
new backend source file, with zero edits to the engine or the layers above it.

### 11.1 Motivation and scope

Before this change OpenSSL was reached directly from three places:

- `src/net/tls/tls.c` + `tls.h` — the TLS engine (`SSL_CTX`/`SSL`/`BIO`/
  `X509`/`EVP_PKEY`).
- `src/net/xylem-dtls.c` — the DTLS engine, which additionally uses DTLS-only
  OpenSSL surface (`DTLS_method`, cookie callbacks, `DTLSv1_get_timeout`/
  `handle_timeout`, `RAND_bytes`).
- `src/platform/{unix,win}/platform-tls.c` + `platform-tls.h` — system root
  certificate loading, whose signature took a raw `SSL_CTX*` and whose
  mechanism was OpenSSL-specific.

The layers *above* the engine were already clean: `http-transport-tls.c` talks
to the internal `tls_*` engine API and `ws-tls.c` to the public `xylem_tls_*`
shim; neither includes OpenSSL. So the abstraction boundary is drawn at the
engine, not above it.

**Scope (agreed):** TLS, DTLS, and system-CA loading all move behind the
backend interface. After this change, exactly one source file
(`tls-backend-openssl.c`) includes `<openssl/...>`.

**Out of scope:** runtime multi-backend selection. Backend choice is a
**compile-time** decision (one backend per build, no function-pointer
indirection, no per-call cost). The current default and only backend is
OpenSSL.

### 11.2 The boundary: what stays, what moves

The key enabler is that both engines already drive OpenSSL through a pair of
in-memory BIOs and pump the socket separately (see §2). That "feed ciphertext
in / drain ciphertext out" model maps cleanly onto every candidate backend
(OpenSSL memory BIOs, mbedTLS `mbedtls_ssl_set_bio` callbacks, wolfSSL I/O
callbacks). So the cut is:

| Concern | Owner after refactor |
|---|---|
| Socket send/recv, `iowait` parking, coroutine scheduling | **Engine** (`tls.c`, `dtls.c`) — written once |
| The three locks (`ssl_mu`/`rd_mu`/`wr_mu`) and the duplex contract | **Engine** |
| Connection refcounting, deadlines, listener/accept loop, DTLS session table / dispatcher / retransmit timer | **Engine** |
| SSL state machine (handshake/read/write), certificate parsing, SNI table, ALPN wire encoding, verify flags, keylog, DTLS cookies, system-CA loading | **Backend** (`tls-backend-openssl.c`) |

The engine includes only `tls-backend.h`. It no longer includes any
`<openssl/...>` header, and `tls_conn_t` no longer holds `SSL*`/`BIO*`.

### 11.3 Backend-neutral types

All declared in `src/net/tls/tls-backend.h`. No OpenSSL types appear here.

```c
typedef struct tls_backend_ctx_s  tls_backend_ctx_t;   /* opaque */
typedef struct tls_backend_conn_s tls_backend_conn_t;  /* opaque */

/* Result of a handshake/read/write step. Maps from SSL_get_error on
 * OpenSSL; maps from MBEDTLS_ERR_SSL_WANT_READ/WRITE on a future mbedTLS
 * backend. */
typedef enum {
    TLS_BACKEND_OK,
    TLS_BACKEND_WANT_READ,
    TLS_BACKEND_WANT_WRITE,
    TLS_BACKEND_CLOSED,     /* clean peer shutdown (SSL_ERROR_ZERO_RETURN) */
    TLS_BACKEND_ERROR
} tls_backend_state_t;

/* Verify policy, computed by the engine from role + ctx intent. */
typedef enum {
    TLS_BACKEND_VERIFY_NONE,     /* SSL_VERIFY_NONE */
    TLS_BACKEND_VERIFY_PEER,     /* SSL_VERIFY_PEER (client) */
    TLS_BACKEND_VERIFY_REQUIRE   /* PEER | FAIL_IF_NO_PEER_CERT (mTLS server) */
} tls_backend_verify_t;

typedef enum {
    TLS_BACKEND_PROTO_TLS,
    TLS_BACKEND_PROTO_DTLS
} tls_backend_proto_t;

/* One-shot, pre-handshake connection configuration snapshot. The engine
 * fills this from neutral decisions it already owns (role -> verify, and
 * whether server_name is an IP literal / whether the peer is verified,
 * both decided with the project's own addr_pton, not OpenSSL). The
 * backend must COPY any string it needs (e.g. SSL_set1_host copies):
 * the pointers reference engine-owned temporaries. */
typedef struct {
    tls_backend_verify_t verify;
    const char*          sni_name;     /* client, non-IP only; else NULL */
    const char*          verify_host;  /* set only when verify != NONE;  else NULL */
} tls_backend_handshake_cfg_t;
```

**Why three verify states (not two).** `VERIFY_PEER` and `VERIFY_REQUIRE`
differ only by OpenSSL's `SSL_VERIFY_FAIL_IF_NO_PEER_CERT` flag. A client uses
`PEER` (the server always sends a certificate); an mTLS server uses `REQUIRE`
to reject a client that presents none. Collapsing them would silently downgrade
an mTLS server to "verify if presented, allow if absent" — a security
regression. The three states are a faithful map of the §4 verify logic. The
engine computes the state in one neutral expression (replacing the duplicated
TLS and DTLS copies):

```c
verify = is_server ? (ctx->verify_client ? TLS_BACKEND_VERIFY_REQUIRE
                                          : TLS_BACKEND_VERIFY_NONE)
                   : (ctx->verify_server ? TLS_BACKEND_VERIFY_PEER
                                         : TLS_BACKEND_VERIFY_NONE);
```

### 11.4 Backend interface (shared by TLS and DTLS)

```c
/* ---- context: shared configuration ---- */
tls_backend_ctx_t* tls_backend_ctx_create(tls_backend_proto_t proto);
void               tls_backend_ctx_destroy(tls_backend_ctx_t* ctx);

int tls_backend_ctx_load_cert_file(tls_backend_ctx_t* ctx, const char* hostname,
                                   const char* cert_file, const char* key_file);
int tls_backend_ctx_load_cert_mem (tls_backend_ctx_t* ctx, const char* hostname,
                                   const void* cert_pem, size_t cert_len,
                                   const void* key_pem,  size_t key_len);
int tls_backend_ctx_load_ca_file  (tls_backend_ctx_t* ctx, const char* ca_file);
int tls_backend_ctx_load_system_ca(tls_backend_ctx_t* ctx,
                                   const char* fallback_ca_file);
int tls_backend_ctx_set_alpn      (tls_backend_ctx_t* ctx,
                                   const char** protocols, size_t count);
int tls_backend_ctx_set_kx_groups (tls_backend_ctx_t* ctx, const char* groups);
int tls_backend_ctx_set_keylog    (tls_backend_ctx_t* ctx, const char* path);

/* ---- connection: one SSL state machine over memory buffers ---- */
tls_backend_conn_t* tls_backend_conn_create(tls_backend_ctx_t* ctx,
                                            bool is_server);
void tls_backend_conn_destroy(tls_backend_conn_t* c);

/* One-shot pre-handshake config (verify + SNI + verify_host). See §11.3. */
void tls_backend_conn_configure(tls_backend_conn_t* c,
                                const tls_backend_handshake_cfg_t* cfg);

/* Ciphertext transfer to/from the backend's inbound/outbound buffers.
 * feed: hand inbound ciphertext to the state machine (was BIO_write).
 * drain: take pending outbound ciphertext (was BIO_read); returns the
 *        byte count (>0), 0 when empty, -1 on error. */
int tls_backend_conn_feed (tls_backend_conn_t* c, const void* buf, int len);
int tls_backend_conn_drain(tls_backend_conn_t* c, void* buf, int cap);

/* State-machine steps. The engine pumps the socket in the requested
 * direction on WANT_READ/WANT_WRITE, then retries. read/write report the
 * plaintext byte count through out_n on TLS_BACKEND_OK. */
tls_backend_state_t tls_backend_conn_handshake(tls_backend_conn_t* c);
tls_backend_state_t tls_backend_conn_read (tls_backend_conn_t* c,
                                           void* buf, int len, int* out_n);
tls_backend_state_t tls_backend_conn_write(tls_backend_conn_t* c,
                                           const void* buf, int len, int* out_n);

/* Best-effort close_notify into the outbound buffer (engine drains it). */
void tls_backend_conn_shutdown(tls_backend_conn_t* c);

/* Copy the negotiated ALPN protocol into a caller buffer (NUL-terminated,
 * empty string if none). The engine caches this once post-handshake. */
void tls_backend_conn_get_alpn(tls_backend_conn_t* c, char* buf, size_t cap);
```

**DTLS-only extensions.** DTLS reuses the same `tls_backend_conn_t`; it only
adds datagram-specific steps. These live under the `dtls_backend_` prefix to
keep the shared surface honest:

```c
void dtls_backend_conn_set_mtu(tls_backend_conn_t* c, uint16_t mtu);
void dtls_backend_conn_set_peer_addr(tls_backend_conn_t* c,
                                     const void* sockaddr, size_t salen);
bool dtls_backend_conn_get_timeout(tls_backend_conn_t* c, uint64_t* out_ms);
void dtls_backend_conn_handle_timeout(tls_backend_conn_t* c);
```

`set_mtu` wraps `DTLS_set_link_mtu` + `SSL_OP_NO_QUERY_MTU`; `set_peer_addr`
hands the cookie machinery the address to bind against; `get_timeout` /
`handle_timeout` wrap `DTLSv1_get_timeout` / `DTLSv1_handle_timeout`.

**DTLS cookies belong to the backend.** The HMAC-over-peer-address cookie
scheme is Xylem's own policy, but the *registration mechanism* is
OpenSSL-specific (`SSL_CTX_set_cookie_generate_cb`/`verify_cb`; mbedTLS uses
`mbedtls_ssl_conf_dtls_cookies`). So the whole cookie plumbing moves into the
backend's DTLS `ctx_create`: the backend generates the secret (OpenSSL:
`RAND_bytes`) and registers the gen/verify callbacks. The callback body keeps
computing the cookie with `xylem_hmac256` — a non-OpenSSL primitive a future
backend can reuse verbatim.

Backends without a cookie API skip this entirely. aws-lc/BoringSSL expose no
cookie callbacks and their DTLS server sends no HelloVerifyRequest, so the
aws-lc backend registers nothing and `set_peer_addr` is a no-op; wolfSSL
computes the cookie internally from the peer bound via `wolfSSL_dtls_set_peer`.
In both cases anti-spoofing leans on the engine's per-peer datagram demux
(`xylem-dtls.c`) rather than an in-stack cookie round-trip.

### 11.5 Concurrency contract

The backend does **no locking of its own**. The engine continues to hold
`ssl_mu` across every `tls_backend_conn_*` / `dtls_backend_conn_*` call that
touches the state machine, exactly as it holds it across the OpenSSL calls
today (see §3). `ssl_mu` is never held across a socket park; `feed`/`drain` are
the only ops called under it from the pump paths, and they never block
(memory-buffer transfers). This contract is documented in `tls-backend.h` as a
precondition every backend implementation may assume — it lets the backend use
a plain, non-thread-safe state-machine object (an OpenSSL `SSL`, an mbedTLS
`mbedtls_ssl_context`) without internal synchronization.

### 11.6 Engine-side structures after the cut

```c
/* tls.c */
struct tls_ctx_s {
    tls_backend_ctx_t* be;
    bool verify_server;   /* neutral intent, stays in the engine */
    bool verify_client;
};
struct tls_conn_s {
    tls_backend_conn_t* be;          /* replaces SSL*/rbio/wbio */
    char* rbuf; char* wbuf;          /* pump scratch (unchanged) */
    xylem_mutex_t *ssl_mu, *rd_mu, *wr_mu, *hs_mu;
    iowait_t* waiter; platform_sock_t fd; tls_ctx_t* ctx;
    addr_t peer_addr; char alpn[32];
    uint64_t hs_timeout_ms;          /* copied from listener opts at accept */
    _Atomic int hs_state;            /* HS_DONE / HS_PENDING / HS_FAILED */
    _Atomic int32_t refcnt; _Atomic bool closed;
};
```

- `_tls_pump_in`/`_tls_pump_out`: `BIO_write`/`BIO_read` → `tls_backend_conn_feed`/
  `tls_backend_conn_drain`. Socket I/O and parking unchanged.
- handshake/read/write loops: `SSL_*` + `SSL_get_error` switch →
  `tls_backend_conn_handshake/read/write` returning `tls_backend_state_t`. The
  `WANT_READ`/`WANT_WRITE`/`OK`/`CLOSED`/`ERROR` arms map 1:1 to the existing
  branches.
- `_tls_apply_verify` + `_tls_apply_server_name` → build a
  `tls_backend_handshake_cfg_t` and call `tls_backend_conn_configure` once,
  after `conn_create`, before the first `handshake`.

DTLS mirrors this; its engine additionally calls `dtls_backend_conn_set_mtu`
(after create), `set_peer_addr` (server, for cookies), and uses
`get_timeout`/`handle_timeout` in the retransmit paths. The session rbtree,
dispatcher coroutine, inbox channel, and retransmit/handshake timers stay in
the engine untouched.

### 11.7 File layout

```
src/net/tls/
├── tls-backend.h           # neutral interface (tls_backend_* / dtls_backend_*)
├── tls-backend-openssl.c   # the ONLY file including <openssl/...>; also
│                           #   absorbs the former platform-tls system-CA code
├── tls.h / tls.c           # TLS engine; includes only tls-backend.h
├── xylem-tls.c             # public TLS shim (unchanged)
├── xylem-dtls.c            # DTLS engine + public API in one file (relocated
│                           #   from src/net/; engine/shim split deferred)
└── xylem-tls-stub.c / ...  # stubs (unchanged)
```

What changed during migration:

- `src/net/xylem-dtls.c` (engine + public API in one ~1800-line file) was
  relocated under `src/net/tls/` and migrated onto the backend interface. The
  proposed split into `dtls.c` (engine) + a thin `xylem-dtls.c` shim was
  **deferred**: it remains a single combined engine + public API file.
- `src/platform/{win,unix}/platform-tls.c` and `src/platform/platform-tls.h`
  were **removed**. The Windows/Unix system-CA split was OpenSSL-specific and is
  absorbed into the OpenSSL backend.
- The duplicated PEM parsing, SNI table, and ALPN wire encoding in the TLS and
  DTLS engines converged into the single backend implementation (`load_cert_*`,
  `set_alpn`).
- CMake: under `XYLEM_ENABLE_TLS`, add `tls-backend-openssl.c`, drop
  `platform/*/platform-tls.c`. OpenSSL is the only backend; its source is
  compiled in directly when TLS is enabled.

### 11.8 Verification

This is a behavior-preserving refactor, so correctness is judged against the
existing suites with no assertion changes:

- `test-tls` and `test-dtls` pass unchanged.
- Run under ASan/UBSan and TSan (the duplex read/write-while-close races are
  exactly what the lock split and refcounting protect — see §3, §9).
- Public headers and the first-member-equivalence shim trick are untouched, so
  no API/ABI drift.

### 11.9 Adding a backend later

- **wolfSSL:** built with `--enable-opensslextra`, its OpenSSL-compat layer
  satisfies most of the surface; a `tls-backend-wolfssl.c` is largely the
  OpenSSL file with header/init differences.
- **mbedTLS:** native API; `tls-backend-mbedtls.c` reimplements each
  `tls_backend_*`/`dtls_backend_*` op against `mbedtls_ssl_*`, reusing the
  engine's socket pumping, locking, and DTLS session machinery as-is, plus
  `xylem_hmac256` for cookies. System-CA loading uses an mbedTLS-native
  strategy (there is no one-call system trust store), which is precisely why
  that concern is a backend responsibility rather than a leaked `SSL_CTX*`
  platform shim.

## 12. Related docs

- Park/resume and per-direction arbitration: [`../architecture.md`](../architecture.md) §6,
  [`runtime.md`](runtime.md).
- Always-available (non-OpenSSL) crypto: [`crypto.md`](crypto.md).
- System-CA platform shim being removed, and the platform-code rule:
  [`platform.md`](platform.md).
- Feature gate `XYLEM_ENABLE_TLS`: [`../architecture.md`](../architecture.md) §8,
  [`../build.md`](../build.md).
