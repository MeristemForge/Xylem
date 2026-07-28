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

Listeners and connections borrow their context; they do not retain a context
reference. The context must remain alive until all listeners and connections
created from it have been destroyed and all operations on them have returned.
Finish context configuration before creating those objects, and do not change
configuration after passing the context to dial or listen.

## 2. Decoupling the SSL state machine from socket I/O

The central design choice: each connection drives OpenSSL through a custom
transport BIO. Its callbacks delegate ciphertext I/O to the engine's stream or
datagram transport, so OpenSSL never touches the socket directly.

```
   application                OpenSSL                 socket
   xylem_tls_read  <--  SSL_read(ssl)  <-- transport BIO <-- stream read
   xylem_tls_write -->  SSL_write(ssl) --> transport BIO --> stream write
```

- `SSL_new` is bound to one custom transport BIO via `SSL_set_bio`. Each BIO
  callback performs one non-blocking stream read or write; OpenSSL issues no
  socket syscalls of its own.
- When a callback reports `EAGAIN`, OpenSSL returns `WANT_READ` or `WANT_WRITE`.
  The engine releases `ssl_mu`, parks on that stream direction, then retries
  the SSL call after the scheduler resumes.

This is what lets a blocking-looking `SSL_read` cooperate with the coroutine
scheduler: the suspend point is the engine's stream wait, not OpenSSL.

`TLS_MAX_PLAINTEXT` limits each application write step to 16 KiB, the maximum
TLS record plaintext size. OpenSSL owns record assembly and buffering.

## 3. Concurrency: the three locks

A connection supports full duplex -- one coroutine reading while another writes
the same connection. Three locks make that safe:

| Lock | Guards | Granularity |
|------|--------|-------------|
| `ssl_mu` | the `SSL` object and its transport BIO | short: held only across a single SSL/BIO call |
| `rd_mu` | the stream **read** direction | long: held across a complete read-side wait |
| `wr_mu` | the stream **write** direction | long: held across a complete write operation |

Rationale:

- **`ssl_mu` exists because a single OpenSSL `SSL` object is not thread-safe.**
  `SSL_read`, `SSL_write`, `SSL_do_handshake`, and the transport BIO callbacks
  all mutate one shared state machine, so they are serialized.
- **`ssl_mu` is never held across a socket park.** The engine takes `ssl_mu` only
  for the instantaneous backend/BIO call, then releases it before
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

Note that a read can require transport writes (for example, a TLS 1.3 KeyUpdate
surfaced as `WANT_WRITE`), and a write can require transport reads. Both paths
can therefore wait on either socket direction; the lock split keeps that safe.

## 4. Verification policy (per role, per connection)

`SSL_VERIFY_*` has opposite meaning on each side, and the ctx is shared, so the
policy is stored as two booleans and applied to each new `SSL` during connection
configuration:

| Setter | Role | Default | Applied mode |
|--------|------|---------|--------------|
| `xylem_tls_ctx_verify_server` | client (`dial`) | **true** | `SSL_VERIFY_PEER`, or `NONE` if disabled |
| `xylem_tls_ctx_verify_client` | server (`listen`) | **false** | `SSL_VERIFY_PEER \| FAIL_IF_NO_PEER_CERT` (mTLS), or `NONE` |

Setting verify on the per-connection `SSL` (not the shared `SSL_CTX`) is what
keeps a reused ctx correct: a client dial still verifies the server even if the
same ctx also accepts connections that request no client cert. Defaults are
secure-by-default for the common case (clients verify servers; plain servers do
not challenge clients).

Peer **identity** is separate from chain trust. For `tls_dial`, the expected
identity defaults to the dial host; `opts->server_name` overrides it when the
network destination and certificate identity differ. A DNS identity drives the
SNI extension and hostname verification (`SSL_set1_host`). An IP identity is
not sent as SNI (RFC 6066) and is verified with
`X509_VERIFY_PARAM_set1_ip_asc`. Other client-handshake entry points that have
no identity log a MITM risk: the chain is trusted but the identity is unchecked.

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
-> `_tls_client_handshake` (set connect state, apply verify, apply expected
identity/SNI, drive handshake) -> cache ALPN. A single deadline derived from
`connect_timeout_ms` bounds DNS, TCP connect, and TLS handshake together. The
option is ignored by listeners.

**Accept (server):** `_tls_accept_fd` parks on the listener's read direction and
backs off on transient accept errors, then `xylem_tls_accept` returns the
connection **without running its handshake** and marks the connection
`HS_PENDING`. The handshake is driven lazily on the first
`tls_read`/`tls_write`, or eagerly via
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
- No server handshake timeout is installed automatically. To bound only the
  handshake, set both connection deadlines, call `tls_handshake`, then replace
  or clear the deadlines. If the handshake remains lazy, existing read/write
  deadlines also cover the first application I/O that triggered it.
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

- `xylem_tls_close` and `xylem_tls_close_listener` are idempotent and do not
  free their handles. Close interrupts blocked I/O; after all operations return,
  call the matching destroy function.
- `xylem_tls_destroy` and `xylem_tls_destroy_listener` must not race with any
  other operation. They close an open handle before releasing its resources.
- A completed TLS connection attempts a best-effort `close_notify` while close
  owns the write direction. Incomplete handshakes and busy writers are closed
  at the transport level instead.

## 10. Security notes and boundaries

- **Secure by default for clients.** A freshly created ctx verifies server
  certificates; you only weaken this explicitly with
  `xylem_tls_ctx_verify_server(ctx, false)` (tests / trusted networks only).
- **Identity verification is enabled by default for `tls_dial`.** The dial host
  is checked unless `opts->server_name` supplies a different expected identity.
  Chain trust without identity verification accepts any trusted-CA cert and is
  a MITM risk for lower-level client-handshake entry points.
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
OpenSSL dependency behind one backend-neutral internal interface. OpenSSL is
the current supported backend; another SSL library would require a dedicated
backend source file with the same engine contract.

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
backend I/O callbacks and pump the socket separately (see §2). That "advance
the SSL state machine / park on WANT_READ or WANT_WRITE" model is the contract
any future backend must preserve. So the cut is:

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
 * OpenSSL; other backends must map their equivalent states here. */
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
 * whether the expected identity is an IP literal / whether the peer is verified,
 * both decided with the project's own addr_pton, not OpenSSL). The
 * backend must COPY any string it needs (e.g. SSL_set1_host copies):
 * the pointers reference engine-owned temporaries. */
typedef struct {
    tls_backend_verify_t verify;
    const char*          sni_name;          /* Client DNS SNI, or NULL. */
    const char*          verify_dns_name;   /* DNS identity, or NULL. */
    const char*          verify_ip_address; /* Numeric IP identity, or NULL. */
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

/* ---- connection: one SSL state machine over transport callbacks ---- */
tls_backend_conn_t* tls_backend_conn_create(tls_backend_ctx_t* ctx,
                                             bool is_server,
                                             const tls_backend_io_t* io);
void tls_backend_conn_destroy(tls_backend_conn_t* c);

/* One-shot pre-handshake config (verify + SNI + identity). See §11.3. */
int tls_backend_conn_configure(tls_backend_conn_t* c,
                               const tls_backend_handshake_cfg_t* cfg);

/* State-machine steps. The transport BIO invokes io as needed. The engine
 * parks in the requested direction on WANT_READ/WANT_WRITE, then retries.
 * read/write report the plaintext byte count through out_n on OK. */
tls_backend_state_t tls_backend_conn_handshake(tls_backend_conn_t* c);
tls_backend_state_t tls_backend_conn_read (tls_backend_conn_t* c,
                                           void* buf, int len, int* out_n);
tls_backend_state_t tls_backend_conn_write(tls_backend_conn_t* c,
                                           const void* buf, int len, int* out_n);

/* Best-effort close_notify through the transport BIO; never parks. */
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

### 11.5 Concurrency contract

The backend does **no locking of its own**. The engine continues to hold
`ssl_mu` across every `tls_backend_conn_*` / `dtls_backend_conn_*` call that
touches the state machine, exactly as it holds it across the OpenSSL calls
today (see §3). A transport callback may attempt immediate non-blocking I/O
under `ssl_mu`, but it never parks. On `TLS_BACKEND_IO_AGAIN`, the backend
returns a WANT state; the engine releases `ssl_mu`, parks in that socket
direction, then retries. This contract lets the backend use a plain,
non-thread-safe state-machine object (an OpenSSL `SSL`, an mbedTLS
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
    tls_backend_conn_t* be;          /* replaces direct SSL/BIO fields */
    xylem_mutex_t *ssl_mu, *rd_mu, *wr_mu, *hs_mu;
    stream_t* stream; tls_ctx_t* ctx;
    char alpn[256];
    _Atomic uint64_t rd_deadline, wr_deadline;
    _Atomic int hs_state;            /* HS_DONE / HS_PENDING / HS_FAILED */
    _Atomic bool closed;
};
```

- Transport BIO callbacks invoke the backend-neutral `tls_backend_io_t` read and
  write functions. Socket I/O and parking remain in the engine.
- handshake/read/write loops: `SSL_*` + `SSL_get_error` switch →
  `tls_backend_conn_handshake/read/write` returning `tls_backend_state_t`. The
  `WANT_READ`/`WANT_WRITE`/`OK`/`CLOSED`/`ERROR` arms map 1:1 to the existing
  branches.
- `_tls_configure_client` -> build a
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

A future backend must implement each `tls_backend_*` / `dtls_backend_*` op
against its native API, reusing the engine's socket pumping, locking, and DTLS
session machinery as-is, plus `xylem_hmac256` for cookies where applicable.
System-CA loading remains a backend responsibility rather than a leaked
`SSL_CTX*` platform shim.

## 12. Related docs

- Park/resume and per-direction arbitration: [`../architecture.md`](../architecture.md) §6,
  [`runtime.md`](runtime.md).
- Always-available (non-OpenSSL) crypto: [`crypto.md`](crypto.md).
- System-CA platform shim being removed, and the platform-code rule:
  [`platform.md`](platform.md).
- Feature gate `XYLEM_ENABLE_TLS`: [`../architecture.md`](../architecture.md) §8,
  [`../build.md`](../build.md).
