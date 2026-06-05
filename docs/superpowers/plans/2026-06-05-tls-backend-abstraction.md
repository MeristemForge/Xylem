# TLS Backend Abstraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Isolate every OpenSSL dependency in the TLS/DTLS stack behind one backend-neutral internal interface, so an alternate SSL library can be added later by writing a single new backend source file.

**Architecture:** Cut the abstraction boundary at the SSL state machine. The engine (`tls.c`, `xylem-dtls.c`) keeps socket I/O, `iowait` parking, locking, refcounting, and the DTLS session/dispatcher machinery. A new backend (`tls-backend-openssl.c`) owns `SSL`/`BIO`/`X509`/cert parsing/SNI/ALPN/cookies/system-CA. The engine drives the backend through memory-buffer `feed`/`drain` plus a neutral `tls_backend_state_t` result enum. Backend selection is compile-time (one backend per build).

**Tech Stack:** C11, OpenSSL >= 3.5, CMake >= 3.25, existing `test-tls` / `test-dtls` suites under CTest.

**Design doc:** [`docs/design/tls-backend.md`](../../design/tls-backend.md). Engine internals: [`docs/design/tls.md`](../../design/tls.md).

---

## Nature of this work: refactor, not feature

This is a **behavior-preserving refactor**. The public `xylem_tls_*` / `xylem_dtls_*`
API and runtime behavior do not change. Therefore the verification model is
inverted from normal TDD: **no new test assertions are written.** The existing
`test-tls` and `test-dtls` suites are the regression harness, and they must stay
green after every task that touches buildable code. Each migration task ends
with a build + targeted test run as its "test passes" step.

**Baseline command set** (single-config Ninja shown; adapt per
[`docs/build.md`](../../build.md) for multi-config):

```bash
cmake -B out -G Ninja -DCMAKE_BUILD_TYPE=Debug -DXYLEM_ENABLE_TESTING=ON -DXYLEM_ENABLE_TLS=ON
cmake --build out -j 8
ctest --test-dir out -R tls  --output-on-failure
ctest --test-dir out -R dtls --output-on-failure
```

On a multi-config generator (Windows default), add `-C Debug` to `ctest` and
`--config Debug` to `--build`. `ctest -R tls` matches both `test-tls` and
`test-dtls`; use `-R "^test-tls$"` to isolate one.

---

## File structure after this plan

```
src/net/tls/
├── tls-backend.h           # NEW: neutral interface (tls_backend_* / dtls_backend_*)
├── tls-backend-openssl.c   # NEW: only file including <openssl/...>; absorbs system-CA
├── tls.h                   # MODIFIED: tls_conn_t/tls_ctx_t lose OpenSSL members
├── tls.c                   # MODIFIED: drives backend, no <openssl/...>
├── xylem-tls.c             # UNCHANGED
├── xylem-dtls.c            # MOVED here + MODIFIED: drives backend, no <openssl/...>
│                           #   (engine+API stays combined; split deferred — see Task 6)
├── xylem-tls-stub.c        # UNCHANGED
└── (http-transport-tls.c, ws-tls.c are NOT in this dir; unchanged)

REMOVED:
├── src/platform/platform-tls.h
├── src/platform/unix/platform-tls.c
└── src/platform/win/platform-tls.c

src/net/xylem-dtls.c        # REMOVED (relocated to src/net/tls/xylem-dtls.c)
```

---

## Task 1: Define the backend-neutral interface header

**Files:**
- Create: `src/net/tls/tls-backend.h`

This header declares the entire contract. No OpenSSL types. It is the single
artifact a future backend author implements against. No code calls it yet, so
verification here is "the header compiles as a translation unit".

- [ ] **Step 1: Write `src/net/tls/tls-backend.h`**

Use the project MIT license header (copy the 19-line block verbatim from the top
of `src/net/tls/tls.h`). Then:

```c
_Pragma("once")

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Backend-neutral TLS/DTLS engine interface.
 *
 * The engine (tls.c / dtls.c) owns sockets, iowait parking, locking,
 * refcounting, and the DTLS session machinery; it drives an SSL state
 * machine exclusively through this interface and includes no SSL-library
 * header. Each backend (tls-backend-openssl.c, and future wolfssl/mbedtls
 * variants) implements every function below. Backend selection is made at
 * compile time by the build; exactly one backend .c is compiled per build.
 *
 * CONCURRENCY CONTRACT (precondition every backend may assume): the engine
 * holds a per-connection mutex across every conn op that touches the state
 * machine, exactly as it serializes the underlying library calls today.
 * Backends therefore use a plain, non-thread-safe state-machine object and
 * perform NO internal locking. feed/drain are the only ops the engine may
 * call from a pump path; they must never block (memory-buffer transfers).
 */

typedef struct tls_backend_ctx_s  tls_backend_ctx_t;
typedef struct tls_backend_conn_s tls_backend_conn_t;

/* Result of a handshake/read/write step. */
typedef enum {
    TLS_BACKEND_OK,
    TLS_BACKEND_WANT_READ,
    TLS_BACKEND_WANT_WRITE,
    TLS_BACKEND_CLOSED,   /* clean peer shutdown */
    TLS_BACKEND_ERROR
} tls_backend_state_t;

/* Verify policy, computed by the engine from role + ctx intent. */
typedef enum {
    TLS_BACKEND_VERIFY_NONE,
    TLS_BACKEND_VERIFY_PEER,     /* verify chain; peer cert optional (client) */
    TLS_BACKEND_VERIFY_REQUIRE   /* verify chain; peer cert required (mTLS)   */
} tls_backend_verify_t;

typedef enum {
    TLS_BACKEND_PROTO_TLS,
    TLS_BACKEND_PROTO_DTLS
} tls_backend_proto_t;

/*
 * One-shot pre-handshake connection configuration. Filled by the engine
 * from neutral decisions it already owns. The backend MUST copy any string
 * it retains -- the pointers reference engine-owned temporaries.
 */
typedef struct {
    tls_backend_verify_t verify;
    const char*          sni_name;     /* client, non-IP only; else NULL */
    const char*          verify_host;  /* set only when verify != NONE; else NULL */
} tls_backend_handshake_cfg_t;

/* ===================================================================== *
 *  Context: shared configuration
 * ===================================================================== */

extern tls_backend_ctx_t* tls_backend_ctx_create(tls_backend_proto_t proto);
extern void               tls_backend_ctx_destroy(tls_backend_ctx_t* ctx);

extern int tls_backend_ctx_load_cert_file(tls_backend_ctx_t* ctx,
                                          const char* hostname,
                                          const char* cert_file,
                                          const char* key_file);
extern int tls_backend_ctx_load_cert_mem(tls_backend_ctx_t* ctx,
                                         const char* hostname,
                                         const void* cert_pem, size_t cert_len,
                                         const void* key_pem,  size_t key_len);
extern int tls_backend_ctx_load_ca_file(tls_backend_ctx_t* ctx,
                                        const char* ca_file);
extern int tls_backend_ctx_load_system_ca(tls_backend_ctx_t* ctx);
extern int tls_backend_ctx_set_alpn(tls_backend_ctx_t* ctx,
                                    const char** protocols, size_t count);
extern int tls_backend_ctx_set_keylog(tls_backend_ctx_t* ctx,
                                      const char* path);

/* ===================================================================== *
 *  Connection: one SSL state machine over memory buffers
 * ===================================================================== */

extern tls_backend_conn_t* tls_backend_conn_create(tls_backend_ctx_t* ctx,
                                                   bool is_server);
extern void tls_backend_conn_destroy(tls_backend_conn_t* c);

extern void tls_backend_conn_configure(tls_backend_conn_t* c,
                                       const tls_backend_handshake_cfg_t* cfg);

/* feed: hand inbound ciphertext to the state machine. Returns 0 on success,
 *       -1 on error.
 * drain: take pending outbound ciphertext. Returns byte count (>0), 0 when
 *        empty, -1 on error. */
extern int tls_backend_conn_feed(tls_backend_conn_t* c,
                                 const void* buf, int len);
extern int tls_backend_conn_drain(tls_backend_conn_t* c,
                                  void* buf, int cap);

extern tls_backend_state_t tls_backend_conn_handshake(tls_backend_conn_t* c);
extern tls_backend_state_t tls_backend_conn_read(tls_backend_conn_t* c,
                                                 void* buf, int len,
                                                 int* out_n);
extern tls_backend_state_t tls_backend_conn_write(tls_backend_conn_t* c,
                                                  const void* buf, int len,
                                                  int* out_n);

extern void tls_backend_conn_shutdown(tls_backend_conn_t* c);
extern void tls_backend_conn_get_alpn(tls_backend_conn_t* c,
                                      char* buf, size_t cap);

/* ===================================================================== *
 *  DTLS-only extensions (datagram specifics)
 * ===================================================================== */

extern void dtls_backend_conn_set_mtu(tls_backend_conn_t* c, uint16_t mtu);
extern void dtls_backend_conn_set_peer_addr(tls_backend_conn_t* c,
                                            const void* sockaddr,
                                            size_t salen);
extern bool dtls_backend_conn_get_timeout(tls_backend_conn_t* c,
                                          uint64_t* out_ms);
extern void dtls_backend_conn_handle_timeout(tls_backend_conn_t* c);
```

- [ ] **Step 2: Verify the header is self-contained**

Create a throwaway TU and compile it (no link). On Windows use a Developer
Command Prompt.

```bash
echo "#include \"net/tls/tls-backend.h\"" > /tmp/tlsb_probe.c
echo "int main(void){return 0;}" >> /tmp/tlsb_probe.c
cc -I src -c /tmp/tlsb_probe.c -o /tmp/tlsb_probe.o
```

Expected: compiles with no errors (it depends only on stdbool/stddef/stdint).
Delete the probe afterward: `rm /tmp/tlsb_probe.c /tmp/tlsb_probe.o`.

- [ ] **Step 3: Commit**

```bash
git add src/net/tls/tls-backend.h
git commit -m "feat(tls): add backend-neutral interface header"
```

---

## Task 2: OpenSSL backend — context, certs, CA, ALPN, keylog, SNI

**Files:**
- Create: `src/net/tls/tls-backend-openssl.c`
- Modify: `CMakeLists.txt` (add the backend source under `XYLEM_ENABLE_TLS`)

This task builds the *context* half of the OpenSSL backend by relocating the
existing ctx-level OpenSSL code out of `tls.c`. The functions being relocated
exist today in `src/net/tls/tls.c` and are duplicated in `src/net/xylem-dtls.c`;
this is the file where the two copies converge. The backend compiles into the
library immediately but is unused until later tasks switch the engine over — no
symbol conflicts arise because every backend symbol is new.

> **Relocation note:** where a step says "port function `_tls_X`", copy the
> existing body from `src/net/tls/tls.c` verbatim, then apply only the listed
> deltas (rename, struct-field access change). Do not rewrite the logic.

- [ ] **Step 1: File header, includes, and backend structs**

Start `tls-backend-openssl.c` with the MIT license header (copy from
`src/net/tls/tls.h`), then:

```c
#include "net/tls/tls-backend.h"

#include "xylem/xylem-logger.h"
#include "xylem/crypto/xylem-hmac256.h"

#include "net/addr.h"
#include "platform/platform-io.h"
#include "platform/platform-string.h"
#include "thrds.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TLSB_COOKIE_SIZE 32

typedef struct _tlsb_sni_entry_s {
    char            hostname[256];
    X509*           cert;
    EVP_PKEY*       key;
    STACK_OF(X509)* chain;
} _tlsb_sni_entry_t;

struct tls_backend_ctx_s {
    SSL_CTX*            ssl_ctx;
    tls_backend_proto_t proto;
    uint8_t*           alpn_wire;
    size_t             alpn_wire_len;
    FILE*              keylog_file;
    _tlsb_sni_entry_t* sni_entries;
    size_t             sni_count;
    size_t             sni_cap;
    uint8_t            cookie_secret[TLSB_COOKIE_SIZE]; /* DTLS only */
};

struct tls_backend_conn_s {
    SSL*  ssl;
    BIO*  rbio;   /* inbound ciphertext: feed() -> SSL */
    BIO*  wbio;   /* outbound ciphertext: SSL -> drain() */
    /* DTLS server cookie binding: a copy of the peer sockaddr bytes. */
    struct sockaddr_storage peer;
    size_t                  peer_len;
};

static int _tlsb_ctx_ex_idx  = -1;  /* SSL_CTX -> tls_backend_ctx_t* */
static int _tlsb_conn_ex_idx = -1;  /* SSL     -> tls_backend_conn_t* (DTLS) */
static once_flag _tlsb_ex_once = ONCE_FLAG_INIT;

static void _tlsb_init_ex(void) {
    _tlsb_ctx_ex_idx  = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    _tlsb_conn_ex_idx = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}
```

- [ ] **Step 2: Port the SNI, keylog, and ALPN-select callbacks**

Port three static callbacks from `tls.c`, renaming `tls_ctx_t`→`tls_backend_ctx_t`
and `_tls_`→`_tlsb_`:

- `_tls_ctx_sni_cb` → `_tlsb_sni_cb`: body identical; iterate `ctx->sni_entries`,
  `platform_strcasecmp`, `SSL_use_certificate`/`SSL_use_PrivateKey`/`SSL_set1_chain`.
- `_tls_keylog_cb` → `_tlsb_keylog_cb`: resolve ctx via
  `SSL_CTX_get_ex_data(ssl_ctx, _tlsb_ctx_ex_idx)`, write to `ctx->keylog_file`.
- `_tls_alpn_select_cb` → `_tlsb_alpn_select_cb`: body identical
  (`SSL_select_next_proto` over `ctx->alpn_wire`).

- [ ] **Step 3: Port the PEM identity + install helpers**

Port these from `tls.c` verbatim (rename `_tls_`→`_tlsb_`, `tls_ctx_t`→
`tls_backend_ctx_t`); these are the functions that the DTLS duplicate is being
folded into:

- `_tls_parse_pem_identity` → `_tlsb_parse_pem_identity`
- `_tls_load_pem_identity` → `_tlsb_load_pem_identity`
- `_tls_load_pem_identity_mem` → `_tlsb_load_pem_identity_mem`
- `_tls_store_sni_identity` → `_tlsb_store_sni_identity`
- `_tls_apply_default_identity` → `_tlsb_apply_default_identity`
- `_tls_install_identity` → `_tlsb_install_identity`

- [ ] **Step 4: Implement `tls_backend_ctx_create` (proto-aware)**

Merge the TLS `tls_ctx_create` and DTLS `xylem_dtls_ctx_create` bodies, selecting
behavior by `proto`:

```c
tls_backend_ctx_t* tls_backend_ctx_create(tls_backend_proto_t proto) {
    tls_backend_ctx_t* ctx = (tls_backend_ctx_t*)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->proto = proto;

    const SSL_METHOD* method =
        (proto == TLS_BACKEND_PROTO_DTLS) ? DTLS_method() : TLS_method();
    ctx->ssl_ctx = SSL_CTX_new(method);
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    call_once(&_tlsb_ex_once, _tlsb_init_ex);
    SSL_CTX_set_ex_data(ctx->ssl_ctx, _tlsb_ctx_ex_idx, ctx);

    SSL_CTX_set_tlsext_servername_callback(ctx->ssl_ctx, _tlsb_sni_cb);
    SSL_CTX_set_tlsext_servername_arg(ctx->ssl_ctx, ctx);

    if (proto == TLS_BACKEND_PROTO_DTLS) {
        if (RAND_bytes(ctx->cookie_secret, sizeof(ctx->cookie_secret)) != 1) {
            SSL_CTX_free(ctx->ssl_ctx);
            free(ctx);
            return NULL;
        }
        SSL_CTX_set_mode(ctx->ssl_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        SSL_CTX_set_cookie_generate_cb(ctx->ssl_ctx, _tlsb_cookie_generate_cb);
        SSL_CTX_set_cookie_verify_cb(ctx->ssl_ctx, _tlsb_cookie_verify_cb);
        SSL_CTX_set_min_proto_version(ctx->ssl_ctx, DTLS1_2_VERSION);
    } else {
        SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION);
    }
    return ctx;
}
```

- [ ] **Step 5: Implement the DTLS cookie callbacks**

These replace `_dtls_cookie_generate_cb` / `_dtls_cookie_verify_cb` from
`xylem-dtls.c`. The peer address now comes from the backend conn (set via
`dtls_backend_conn_set_peer_addr`), and the secret from the backend ctx:

```c
static int _tlsb_cookie_peer(SSL* ssl, const uint8_t** out, size_t* out_len) {
    tls_backend_conn_t* c =
        (tls_backend_conn_t*)SSL_get_ex_data(ssl, _tlsb_conn_ex_idx);
    if (!c || c->peer_len == 0) {
        return -1;
    }
    *out     = (const uint8_t*)&c->peer;
    *out_len = c->peer_len;
    return 0;
}

static int _tlsb_cookie_generate_cb(SSL* ssl, unsigned char* cookie,
                                    unsigned int* cookie_len) {
    SSL_CTX* sc = SSL_get_SSL_CTX(ssl);
    tls_backend_ctx_t* ctx =
        (tls_backend_ctx_t*)SSL_CTX_get_ex_data(sc, _tlsb_ctx_ex_idx);
    const uint8_t* msg; size_t msg_len;
    if (!ctx || _tlsb_cookie_peer(ssl, &msg, &msg_len) < 0) {
        return 0;
    }
    xylem_hmac256_compute(ctx->cookie_secret, sizeof(ctx->cookie_secret),
                          msg, msg_len, cookie);
    *cookie_len = TLSB_COOKIE_SIZE;
    return 1;
}

static int _tlsb_cookie_verify_cb(SSL* ssl, const unsigned char* cookie,
                                  unsigned int cookie_len) {
    SSL_CTX* sc = SSL_get_SSL_CTX(ssl);
    tls_backend_ctx_t* ctx =
        (tls_backend_ctx_t*)SSL_CTX_get_ex_data(sc, _tlsb_ctx_ex_idx);
    const uint8_t* msg; size_t msg_len;
    if (!ctx || _tlsb_cookie_peer(ssl, &msg, &msg_len) < 0) {
        return 0;
    }
    uint8_t expected[TLSB_COOKIE_SIZE];
    xylem_hmac256_compute(ctx->cookie_secret, sizeof(ctx->cookie_secret),
                          msg, msg_len, expected);
    if (cookie_len != TLSB_COOKIE_SIZE) {
        return 0;
    }
    return CRYPTO_memcmp(cookie, expected, TLSB_COOKIE_SIZE) == 0 ? 1 : 0;
}
```

Declare both cookie callbacks and `_tlsb_cookie_peer` above
`tls_backend_ctx_create` (forward declarations or define earlier in the file)
so Step 4 can reference them.

- [ ] **Step 6: Implement `tls_backend_ctx_destroy` and the load/config ops**

```c
void tls_backend_ctx_destroy(tls_backend_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    for (size_t i = 0; i < ctx->sni_count; i++) {
        X509_free(ctx->sni_entries[i].cert);
        EVP_PKEY_free(ctx->sni_entries[i].key);
        sk_X509_pop_free(ctx->sni_entries[i].chain, X509_free);
    }
    free(ctx->sni_entries);
    if (ctx->keylog_file) {
        fclose(ctx->keylog_file);
    }
    SSL_CTX_free(ctx->ssl_ctx);
    free(ctx->alpn_wire);
    free(ctx);
}
```

Then implement, by porting the matching `tls_ctx_*` bodies from `tls.c`:

- `tls_backend_ctx_load_cert_file` ← `tls_ctx_load_cert` body (calls
  `_tlsb_load_pem_identity` + `_tlsb_install_identity`).
- `tls_backend_ctx_load_cert_mem` ← `tls_ctx_load_cert_mem` body.
- `tls_backend_ctx_load_ca_file` ← `tls_ctx_load_ca` body
  (`SSL_CTX_load_verify_locations`).
- `tls_backend_ctx_set_alpn` ← `tls_ctx_set_alpn` body (wire encode +
  `SSL_CTX_set_alpn_protos` + `SSL_CTX_set_alpn_select_cb(... _tlsb_alpn_select_cb ...)`).
- `tls_backend_ctx_set_keylog` ← `tls_ctx_set_keylog` body
  (`platform_io_fopen`, `SSL_CTX_set_keylog_callback(... _tlsb_keylog_cb)`).

- [ ] **Step 7: Implement `tls_backend_ctx_load_system_ca` (absorb platform-tls)**

This folds the OS-specific logic from `src/platform/{unix,win}/platform-tls.c`
directly into the backend:

```c
int tls_backend_ctx_load_system_ca(tls_backend_ctx_t* ctx) {
#if defined(_WIN32)
    /* OpenSSL's default verify paths are empty on Windows; the winstore
     * loader (OpenSSL 3.2+) reads the system ROOT store on demand. */
    if (SSL_CTX_load_verify_store(ctx->ssl_ctx,
                                  "org.openssl.winstore://") != 1) {
        xylem_loge("<tls> load system ca failed");
        return -1;
    }
#else
    if (SSL_CTX_set_default_verify_paths(ctx->ssl_ctx) != 1) {
        xylem_loge("<tls> load system ca failed");
        return -1;
    }
#endif
    return 0;
}
```

- [ ] **Step 8: Add the backend to the build (compiled, unused)**

In `CMakeLists.txt`, inside the `if(XYLEM_ENABLE_TLS)` `list(APPEND SRCS ...)`
block, add `src/net/tls/tls-backend-openssl.c`:

```cmake
	list(APPEND SRCS
		src/net/tls/tls-backend-openssl.c
		src/net/tls/tls.c
		src/net/tls/xylem-tls.c
		src/net/xylem-dtls.c
		src/net/http/http-transport-tls.c
		src/net/ws/ws-tls.c
	)
```

(Conn-side ops are not implemented yet, so the file references only ctx
functions plus the cookie/SNI/keylog callbacks defined within it. It links
cleanly as long as every function it *references* is defined; the conn ops it
does not yet define are simply absent — that is fine because nothing calls them
yet. If the compiler warns about the unused `_tlsb_conn_ex_idx`, leave it; it is
used in Task 3.)

- [ ] **Step 9: Build to verify the backend compiles**

```bash
cmake -B out -G Ninja -DCMAKE_BUILD_TYPE=Debug -DXYLEM_ENABLE_TESTING=ON -DXYLEM_ENABLE_TLS=ON
cmake --build out -j 8
```

Expected: builds successfully. `test-tls`/`test-dtls` still pass (engine
unchanged). Run them to confirm no regression:

```bash
ctest --test-dir out -R tls --output-on-failure
```

Expected: `test-tls` and `test-dtls` PASS.

- [ ] **Step 10: Commit**

```bash
git add src/net/tls/tls-backend-openssl.c CMakeLists.txt
git commit -m "feat(tls): add OpenSSL backend context layer"
```

---

## Task 3: OpenSSL backend — connection state machine

**Files:**
- Modify: `src/net/tls/tls-backend-openssl.c`

This task adds the conn-side ops: create/destroy, configure, feed/drain, the
three state-machine steps, shutdown, ALPN, and the DTLS conn extensions. These
encapsulate every `SSL_*` / `BIO_*` call the engine performs today.

- [ ] **Step 1: `tls_backend_conn_create` / `tls_backend_conn_destroy`**

Merges `_tls_init_ssl` + connect/accept-state selection:

```c
tls_backend_conn_t* tls_backend_conn_create(tls_backend_ctx_t* ctx,
                                            bool is_server) {
    tls_backend_conn_t* c = (tls_backend_conn_t*)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->ssl = SSL_new(ctx->ssl_ctx);
    if (!c->ssl) {
        free(c);
        return NULL;
    }
    c->rbio = BIO_new(BIO_s_mem());
    c->wbio = BIO_new(BIO_s_mem());
    if (!c->rbio || !c->wbio) {
        BIO_free(c->rbio);
        BIO_free(c->wbio);
        SSL_free(c->ssl);
        free(c);
        return NULL;
    }
    SSL_set_bio(c->ssl, c->rbio, c->wbio);   /* SSL owns both BIOs now */

    /* DTLS server cookie path needs SSL -> conn lookup. */
    SSL_set_ex_data(c->ssl, _tlsb_conn_ex_idx, c);

    if (is_server) {
        SSL_set_accept_state(c->ssl);
    } else {
        SSL_set_connect_state(c->ssl);
    }
    return c;
}

void tls_backend_conn_destroy(tls_backend_conn_t* c) {
    if (!c) {
        return;
    }
    if (c->ssl) {
        SSL_free(c->ssl);   /* frees the bound BIOs too */
    }
    free(c);
}
```

> Note: the engine currently distinguishes graceful unref (which does
> `SSL_shutdown` then `SSL_free`) from plain destroy (`SSL_free` only). In the
> new split the engine calls `tls_backend_conn_shutdown` (Step 6) explicitly on
> the graceful path *before* `tls_backend_conn_destroy`, so `destroy` only frees.

- [ ] **Step 2: `tls_backend_conn_configure`**

Folds `_tls_apply_verify` + `_tls_apply_server_name` into one op driven by the
neutral cfg:

```c
void tls_backend_conn_configure(tls_backend_conn_t* c,
                                const tls_backend_handshake_cfg_t* cfg) {
    int mode;
    switch (cfg->verify) {
        case TLS_BACKEND_VERIFY_REQUIRE:
            mode = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
            break;
        case TLS_BACKEND_VERIFY_PEER:
            mode = SSL_VERIFY_PEER;
            break;
        default:
            mode = SSL_VERIFY_NONE;
            break;
    }
    SSL_set_verify(c->ssl, mode, NULL);

    if (cfg->sni_name) {
        SSL_set_tlsext_host_name(c->ssl, cfg->sni_name);
    }
    if (cfg->verify_host) {
        SSL_set1_host(c->ssl, cfg->verify_host);   /* copies */
    }
}
```

The engine decides IP-vs-name and verify-peer (it owns `addr_pton`), so the
backend just applies what cfg says. `sni_name` is NULL for IP literals and for
the server role; `verify_host` is NULL when not verifying.

- [ ] **Step 3: `tls_backend_conn_feed` / `tls_backend_conn_drain`**

```c
int tls_backend_conn_feed(tls_backend_conn_t* c, const void* buf, int len) {
    return BIO_write(c->rbio, buf, len) == len ? 0 : -1;
}

int tls_backend_conn_drain(tls_backend_conn_t* c, void* buf, int cap) {
    int n = BIO_read(c->wbio, buf, cap);
    if (n > 0) {
        return n;
    }
    /* No pending bytes is not an error: a mem BIO returns <=0 with a
     * retry flag set. Distinguish "empty" from a hard failure. */
    return BIO_should_retry(c->wbio) ? 0 : (n < 0 ? -1 : 0);
}
```

> Rationale: today the engine loops `while ((n = BIO_read(...)) > 0)` and treats
> `n <= 0` as "drained". `drain` returning 0 for the empty case preserves that
> exactly; the engine's pump loops on `> 0`.

- [ ] **Step 4: Internal `_tlsb_state` mapper + `tls_backend_conn_handshake`**

```c
static tls_backend_state_t _tlsb_state(SSL* ssl, int ret) {
    if (ret == 1) {
        return TLS_BACKEND_OK;
    }
    int err = SSL_get_error(ssl, ret);
    switch (err) {
        case SSL_ERROR_WANT_READ:   return TLS_BACKEND_WANT_READ;
        case SSL_ERROR_WANT_WRITE:  return TLS_BACKEND_WANT_WRITE;
        case SSL_ERROR_ZERO_RETURN: return TLS_BACKEND_CLOSED;
        default: {
            unsigned long e = ERR_peek_error();
            xylem_loge("<tls> ssl op failed ssl_err=%d reason=%s", err,
                       ERR_reason_error_string(e)
                           ? ERR_reason_error_string(e) : "unknown");
            return TLS_BACKEND_ERROR;
        }
    }
}

tls_backend_state_t tls_backend_conn_handshake(tls_backend_conn_t* c) {
    ERR_clear_error();
    int ret = SSL_do_handshake(c->ssl);
    return _tlsb_state(c->ssl, ret);
}
```

- [ ] **Step 5: `tls_backend_conn_read` / `tls_backend_conn_write`**

```c
tls_backend_state_t tls_backend_conn_read(tls_backend_conn_t* c,
                                          void* buf, int len, int* out_n) {
    ERR_clear_error();
    int n = SSL_read(c->ssl, buf, len);
    if (n > 0) {
        *out_n = n;
        return TLS_BACKEND_OK;
    }
    *out_n = 0;
    return _tlsb_state(c->ssl, n);
}

tls_backend_state_t tls_backend_conn_write(tls_backend_conn_t* c,
                                           const void* buf, int len,
                                           int* out_n) {
    ERR_clear_error();
    int n = SSL_write(c->ssl, buf, len);
    if (n > 0) {
        *out_n = n;
        return TLS_BACKEND_OK;
    }
    *out_n = 0;
    return _tlsb_state(c->ssl, n);
}
```

- [ ] **Step 6: `tls_backend_conn_shutdown` / `tls_backend_conn_get_alpn`**

```c
void tls_backend_conn_shutdown(tls_backend_conn_t* c) {
    if (c->ssl) {
        ERR_clear_error();
        SSL_shutdown(c->ssl);   /* queues close_notify into wbio */
    }
}

void tls_backend_conn_get_alpn(tls_backend_conn_t* c, char* buf, size_t cap) {
    const unsigned char* proto = NULL;
    unsigned int         plen  = 0;
    SSL_get0_alpn_selected(c->ssl, &proto, &plen);
    if (proto && plen > 0 && (size_t)plen < cap) {
        memcpy(buf, proto, plen);
        buf[plen] = '\0';
    } else if (cap > 0) {
        buf[0] = '\0';
    }
}
```

- [ ] **Step 7: DTLS conn extensions**

```c
void dtls_backend_conn_set_mtu(tls_backend_conn_t* c, uint16_t mtu) {
    if (mtu == 0) {
        return;
    }
    SSL_set_options(c->ssl, SSL_OP_NO_QUERY_MTU);
    DTLS_set_link_mtu(c->ssl, mtu);
}

void dtls_backend_conn_set_peer_addr(tls_backend_conn_t* c,
                                     const void* sockaddr, size_t salen) {
    if (salen > sizeof(c->peer)) {
        salen = sizeof(c->peer);
    }
    memcpy(&c->peer, sockaddr, salen);
    c->peer_len = salen;
}

bool dtls_backend_conn_get_timeout(tls_backend_conn_t* c, uint64_t* out_ms) {
    struct timeval tv;
    if (!DTLSv1_get_timeout(c->ssl, &tv)) {
        return false;
    }
    uint64_t ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
    *out_ms = (ms == 0) ? 1 : ms;
    return true;
}

void dtls_backend_conn_handle_timeout(tls_backend_conn_t* c) {
    DTLSv1_handle_timeout(c->ssl);
}
```

- [ ] **Step 8: Build to verify the full backend compiles**

```bash
cmake --build out -j 8
```

Expected: builds cleanly; `_tlsb_conn_ex_idx` is now used (no warning).
`test-tls`/`test-dtls` still pass (engine still uses its own OpenSSL code; the
backend remains unreferenced by the engine until Task 5/9).

```bash
ctest --test-dir out -R tls --output-on-failure
```

Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add src/net/tls/tls-backend-openssl.c
git commit -m "feat(tls): add OpenSSL backend connection state machine"
```

---

## Task 4: Migrate the TLS engine header (`tls.h`)

**Files:**
- Modify: `src/net/tls/tls.h`

Strip OpenSSL from the engine's internal header. The struct bodies lose their
OpenSSL members and gain a backend handle. The public `tls_*` engine API
(consumed by `xylem-tls.c` and `http-transport-tls.c`) is unchanged.

- [ ] **Step 1: Replace the OpenSSL include and SNI typedef**

In `src/net/tls/tls.h`, remove:

```c
#include <openssl/ssl.h>
```

and add (with the other internal includes):

```c
#include "net/tls/tls-backend.h"
```

Delete the `_tls_sni_entry_t` struct entirely (it moves into the backend; the
engine no longer tracks per-SNI certs):

```c
/* DELETE this whole struct from tls.h: */
typedef struct _tls_sni_entry_s {
    char            hostname[256];
    X509*           cert;
    EVP_PKEY*       key;
    STACK_OF(X509)* chain;
} _tls_sni_entry_t;
```

- [ ] **Step 2: Rewrite `struct tls_ctx_s`**

Replace the existing definition with:

```c
struct tls_ctx_s {
    tls_backend_ctx_t* be;
    /* Verification intent (neutral); applied per connection by role.
     * See docs/design/tls.md §4. */
    bool               verify_server;
    bool               verify_client;
};
```

- [ ] **Step 3: Rewrite `struct tls_conn_s`**

Replace the `SSL*`/`BIO*` members with the backend handle:

```c
struct tls_conn_s {
    tls_backend_conn_t* be;     /* replaces ssl, rbio, wbio */
    char*            rbuf;      /* pump_in scratch, owned by rd_mu. */
    char*            wbuf;      /* pump_out scratch, owned by wr_mu. */
    xylem_mutex_t*   ssl_mu;    /* serializes all backend conn access. */
    xylem_mutex_t*   rd_mu;     /* sole owner of iowait read direction. */
    xylem_mutex_t*   wr_mu;     /* sole owner of iowait write direction. */
    iowait_t*        waiter;
    platform_sock_t  fd;
    tls_ctx_t*       ctx;
    addr_t           peer_addr;
    char             alpn[32];
    _Atomic int32_t  refcnt;
    _Atomic bool     closed;
};
```

The `extern tls_*` function declarations below the structs are unchanged.

- [ ] **Step 4: Verify (header-only; full build happens in Task 5)**

`tls.h` is included by `tls.c`, `xylem-tls.c`, and `http-transport-tls.c`. It
will not compile-check meaningfully until `tls.c` is migrated (Task 5), because
`tls.c` still references `tls->ssl`. So do **not** build yet — this task's change
is committed together conceptually but verified at the end of Task 5.

- [ ] **Step 5: Commit**

```bash
git add src/net/tls/tls.h
git commit -m "refactor(tls): move engine header off OpenSSL types"
```

---

## Task 5: Migrate the TLS engine implementation (`tls.c`)

**Files:**
- Modify: `src/net/tls/tls.c`

Rewire every OpenSSL call in the engine to a backend call. Socket pumping,
locking, parking, refcounting, dial/listen/accept all stay. After this task the
TLS engine includes no OpenSSL header.

- [ ] **Step 1: Replace includes and delete relocated helpers**

Remove all four OpenSSL includes (`<openssl/bio.h>`, `<openssl/err.h>`,
`<openssl/pem.h>`, `<openssl/ssl.h>`) and the `platform-tls.h` include. The
`tls-backend.h` include comes transitively via `tls.h`, but add it explicitly
for clarity.

Delete these now-relocated statics from `tls.c` (they live in the backend):
`_tls_init_ex_data`, `_tls_ex_data_once`, `_tls_ex_data_idx`, `_tls_ctx_sni_cb`,
`_tls_keylog_cb`, `_tls_alpn_select_cb`, `_tls_init_ssl`, `_tls_apply_verify`,
`_tls_apply_server_name`, `_tls_cache_alpn`, `_tls_parse_pem_identity`,
`_tls_load_pem_identity`, `_tls_load_pem_identity_mem`, `_tls_store_sni_identity`,
`_tls_apply_default_identity`, `_tls_install_identity`.

- [ ] **Step 2: Rewrite `_tls_conn_create` (drop BIO/SSL init)**

The conn no longer creates the SSL here — that becomes a backend call made by
the handshake paths. Keep mutex/iowait/buffer allocation; just remove any SSL
references (there were none in the original `_tls_conn_create`, so this is
unchanged except it stays as-is). No edit needed if the original had no SSL —
confirm and move on.

- [ ] **Step 3: Rewrite the pumps to use feed/drain**

In `_tls_pump_out`, replace the `BIO_read` line:

```c
        xylem_mutex_lock(tls->ssl_mu);
        int n = tls_backend_conn_drain(tls->be, tls->wbuf, TLS_IO_CHUNK);
        xylem_mutex_unlock(tls->ssl_mu);
        if (n <= 0) {
            break;
        }
```

In `_tls_pump_in`, replace the `BIO_write` line:

```c
            xylem_mutex_lock(tls->ssl_mu);
            tls_backend_conn_feed(tls->be, tls->rbuf, (int)n);
            xylem_mutex_unlock(tls->ssl_mu);
```

- [ ] **Step 4: Rewrite `_tls_do_handshake`**

```c
static int _tls_do_handshake(tls_conn_t* tls) {
    for (;;) {
        xylem_mutex_lock(tls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_handshake(tls->be);
        xylem_mutex_unlock(tls->ssl_mu);

        if (_tls_pump_out(tls) != 0) {
            return -1;
        }
        if (st == TLS_BACKEND_OK) {
            return 0;
        }
        if (st == TLS_BACKEND_WANT_READ) {
            if (_tls_pump_in(tls) <= 0) {
                return -1;
            }
            continue;
        }
        if (st == TLS_BACKEND_WANT_WRITE) {
            continue;
        }
        return -1;   /* ERROR/CLOSED: backend already logged the reason */
    }
}
```

- [ ] **Step 5: Rewrite `_tls_read_loop`**

```c
static int _tls_read_loop(tls_conn_t* tls, void* buf, int len) {
    for (;;) {
        int n = 0;
        xylem_mutex_lock(tls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_read(tls->be, buf, len, &n);
        xylem_mutex_unlock(tls->ssl_mu);

        if (st == TLS_BACKEND_OK) {
            return n;
        }
        if (st == TLS_BACKEND_CLOSED) {
            return 0;
        }
        if (st == TLS_BACKEND_WANT_READ) {
            if (_tls_pump_in(tls) <= 0) {
                return -1;
            }
            continue;
        }
        if (st == TLS_BACKEND_WANT_WRITE) {
            if (_tls_pump_out(tls) != 0) {
                return -1;
            }
            continue;
        }
        return -1;
    }
}
```

- [ ] **Step 6: Rewrite `_tls_write_loop`**

```c
static int _tls_write_loop(tls_conn_t* tls, const void* data, int len) {
    const char* ptr = (const char*)data;
    int         rem = len;
    while (rem > 0) {
        int n = 0;
        xylem_mutex_lock(tls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_write(tls->be, ptr, rem, &n);
        xylem_mutex_unlock(tls->ssl_mu);

        if (st == TLS_BACKEND_OK) {
            if (_tls_pump_out(tls) != 0) {
                return -1;
            }
            ptr += n;
            rem -= n;
            continue;
        }
        if (st == TLS_BACKEND_WANT_WRITE) {
            if (_tls_pump_out(tls) != 0) {
                return -1;
            }
            continue;
        }
        if (st == TLS_BACKEND_WANT_READ) {
            if (_tls_pump_out(tls) != 0) {
                return -1;
            }
            if (_tls_pump_in(tls) <= 0) {
                return -1;
            }
            continue;
        }
        return -1;
    }
    return 0;
}
```

- [ ] **Step 7: Rewrite the client handshake helper**

Replace `_tls_client_handshake` (which called `_tls_init_ssl`,
`SSL_set_connect_state`, `_tls_apply_verify`, `_tls_apply_server_name`):

```c
static int _tls_client_handshake(tls_conn_t* tls, const char* server_name) {
    tls->be = tls_backend_conn_create(tls->ctx->be, false);
    if (!tls->be) {
        return -1;
    }

    tls_backend_handshake_cfg_t cfg = {0};
    cfg.verify = tls->ctx->verify_server ? TLS_BACKEND_VERIFY_PEER
                                         : TLS_BACKEND_VERIFY_NONE;
    bool verify_peer = (cfg.verify != TLS_BACKEND_VERIFY_NONE);

    if (!server_name && verify_peer) {
        xylem_loge("<tls> dial server_name=NULL with verify_peer; "
                   "peer identity unchecked (MITM risk)");
    }
    if (server_name) {
        addr_t tmp;
        if (addr_pton(server_name, 0, &tmp) != 0) {   /* not an IP literal */
            cfg.sni_name = server_name;
        }
        if (verify_peer) {
            cfg.verify_host = server_name;
        }
    }
    tls_backend_conn_configure(tls->be, &cfg);

    if (_tls_do_handshake(tls) != 0) {
        return -1;
    }
    tls_backend_conn_get_alpn(tls->be, tls->alpn, sizeof(tls->alpn));
    return 0;
}
```

> Signature change: it no longer takes `SSL_CTX*` (used `tls->ctx->be`
> internally). Update its two call sites in `tls_dial` and
> `tls_client_handshake_fd`: change `_tls_client_handshake(tls, ctx->ssl_ctx,
> opts ? opts->server_name : NULL)` to `_tls_client_handshake(tls, opts ?
> opts->server_name : NULL)`.

- [ ] **Step 8: Rewrite `_tls_server_handshake`**

Replace the `_tls_init_ssl` + `SSL_set_accept_state` + `_tls_apply_verify` block:

```c
    tls->be = tls_backend_conn_create(ln->ctx->be, true);
    if (!tls->be) {
        xylem_loge("<tls> accept ssl init failed");
        _tls_conn_destroy(tls);
        return NULL;
    }
    tls_backend_handshake_cfg_t cfg = {0};
    cfg.verify = ln->ctx->verify_client ? TLS_BACKEND_VERIFY_REQUIRE
                                        : TLS_BACKEND_VERIFY_NONE;
    tls_backend_conn_configure(tls->be, &cfg);

    _tls_set_deadline(tls, _tls_make_deadline(ln->opts.handshake_timeout_ms));

    if (_tls_do_handshake(tls) != 0) {
        _tls_conn_destroy(tls);
        return NULL;
    }
    _tls_set_deadline(tls, 0);
    tls_backend_conn_get_alpn(tls->be, tls->alpn, sizeof(tls->alpn));
    return tls;
```

- [ ] **Step 9: Rewrite the ctx lifecycle and teardown**

`tls_ctx_create`:

```c
tls_ctx_t* tls_ctx_create(void) {
    tls_ctx_t* ctx = (tls_ctx_t*)calloc(1, sizeof(tls_ctx_t));
    if (!ctx) {
        return NULL;
    }
    ctx->be = tls_backend_ctx_create(TLS_BACKEND_PROTO_TLS);
    if (!ctx->be) {
        free(ctx);
        return NULL;
    }
    ctx->verify_server = true;
    ctx->verify_client = false;
    return ctx;
}
```

`tls_ctx_destroy`: replace the OpenSSL teardown with
`tls_backend_ctx_destroy(ctx->be); free(ctx);` (guard NULL).

`tls_ctx_load_cert` → `tls_backend_ctx_load_cert_file(ctx->be, hostname, cert, key)`.
`tls_ctx_load_cert_mem` → `tls_backend_ctx_load_cert_mem(ctx->be, ...)`.
`tls_ctx_load_ca` → `tls_backend_ctx_load_ca_file(ctx->be, ca_file)`.
`tls_ctx_load_system_ca` → `tls_backend_ctx_load_system_ca(ctx->be)`.
`tls_ctx_set_alpn` → `tls_backend_ctx_set_alpn(ctx->be, protocols, count)`.
`tls_ctx_set_keylog` → `tls_backend_ctx_set_keylog(ctx->be, path)`.
`tls_ctx_verify_server` / `verify_client`: unchanged (set the bools).

- [ ] **Step 10: Rewrite conn teardown in `_tls_conn_unref` / `_tls_conn_destroy`**

`_tls_conn_unref` (graceful): replace the `SSL_shutdown`/`SSL_free` block:

```c
    if (tls->be) {
        tls_backend_conn_shutdown(tls->be);
        tls_backend_conn_destroy(tls->be);
    }
    _tls_conn_free(tls);
```

`_tls_conn_destroy` (plain): replace `if (tls->ssl) SSL_free(tls->ssl);` with:

```c
    if (tls->be) {
        tls_backend_conn_destroy(tls->be);
    }
    _tls_conn_free(tls);
```

- [ ] **Step 11: Build and test**

```bash
cmake --build out -j 8
ctest --test-dir out -R tls --output-on-failure
```

Expected: builds with no OpenSSL include in `tls.c`; `test-tls` PASS.
(`test-dtls` still passes — DTLS engine not yet touched.) Confirm `tls.c` is
OpenSSL-free:

```bash
grep -n "openssl" src/net/tls/tls.c
```

Expected: no matches.

- [ ] **Step 12: Commit**

```bash
git add src/net/tls/tls.c
git commit -m "refactor(tls): drive TLS engine through backend interface"
```

---

## Task 6: Relocate the DTLS file into `src/net/tls/`

**Files:**
- Move: `src/net/xylem-dtls.c` → `src/net/tls/xylem-dtls.c`
- Modify: `CMakeLists.txt` (update the source path)

> **Deviation from the design doc:** the design proposed splitting DTLS into a
> `dtls.c` engine + `xylem-dtls.c` thin shim to mirror TLS. That split is a large
> mechanical change (DTLS currently defines the public `xylem_dtls_*` structs
> directly as its engine structs) and is **orthogonal** to removing OpenSSL.
> This plan defers the engine/shim split and keeps DTLS as one relocated file.
> The code-reuse goal is still met: the duplicated cert/SNI/ALPN logic converges
> in the OpenSSL backend (Task 2), not in a shared engine struct. The file keeps
> its name (`xylem-dtls.c`) to honestly signal it is still engine+API combined.

This task is a pure move with no logic change — it isolates the relocation in
its own commit so the later content migration diffs cleanly.

- [ ] **Step 1: Move the file with git**

```bash
git mv src/net/xylem-dtls.c src/net/tls/xylem-dtls.c
```

The file's includes are all rooted at `src/` (e.g. `"net/addr.h"`,
`"platform/platform-tls.h"`, `"runtime/iowait.h"`) or angle-bracket system/
OpenSSL headers, so moving the `.c` does not break any include path.

- [ ] **Step 2: Update the CMake source path**

In `CMakeLists.txt`, inside the `if(XYLEM_ENABLE_TLS)` source list, change
`src/net/xylem-dtls.c` to `src/net/tls/xylem-dtls.c`:

```cmake
	list(APPEND SRCS
		src/net/tls/tls-backend-openssl.c
		src/net/tls/tls.c
		src/net/tls/xylem-tls.c
		src/net/tls/xylem-dtls.c
		src/net/http/http-transport-tls.c
		src/net/ws/ws-tls.c
	)
```

- [ ] **Step 3: Build and test (no behavior change expected)**

```bash
cmake -B out -G Ninja -DCMAKE_BUILD_TYPE=Debug -DXYLEM_ENABLE_TESTING=ON -DXYLEM_ENABLE_TLS=ON
cmake --build out -j 8
ctest --test-dir out -R tls --output-on-failure
```

Expected: `test-tls` and `test-dtls` PASS (the move changed nothing functional).

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "refactor(tls): relocate DTLS engine into tls/ directory"
```

(The `git mv` is already staged from Step 1; `git add` picks up the CMake edit.)

---

## Task 7: Migrate the DTLS engine to the backend

**Files:**
- Modify: `src/net/tls/xylem-dtls.c`

Rewire every OpenSSL call in DTLS to the backend, mirroring Task 5 for the
shared ops and using the `dtls_backend_*` extensions for the datagram specifics.
After this task DTLS includes no OpenSSL header. The session rbtree, dispatcher,
inbox channel, and retransmit/handshake timers are untouched.

- [ ] **Step 1: Replace includes; delete relocated ctx code**

Remove the OpenSSL includes (`<openssl/err.h>`, `<openssl/pem.h>`,
`<openssl/rand.h>`, `<openssl/ssl.h>`) and the `platform-tls.h` include. Add:

```c
#include "net/tls/tls-backend.h"
```

Delete these now-relocated statics and the `_dtls_sni_entry_t` struct (all live
in the backend now): `_dtls_ex_data_idx`, `_dtls_peer_addr_idx`,
`_dtls_ex_data_once`, `_dtls_init_ex_data`, `_dtls_get_ctx`,
`_dtls_get_peer_addr`, `_dtls_keylog_cb`, `_dtls_cookie_generate_cb`,
`_dtls_cookie_verify_cb`, `_dtls_alpn_select_cb`, `_dtls_ctx_sni_cb`,
`_dtls_parse_pem_identity`, `_dtls_load_pem_identity`,
`_dtls_load_pem_identity_mem`, `_dtls_store_sni_identity`,
`_dtls_apply_default_identity`, `_dtls_install_identity`, `_dtls_init_ssl`,
`_dtls_apply_mtu`.

- [ ] **Step 2: Rewrite the two structs**

`struct xylem_dtls_ctx_s`:

```c
struct xylem_dtls_ctx_s {
    tls_backend_ctx_t* be;
    bool               verify_server;
    bool               verify_client;
};
```

In `struct xylem_dtls_conn_s`, replace the three OpenSSL members
(`SSL* ssl; BIO* read_bio; BIO* write_bio;`) with a single backend handle:

```c
    tls_backend_conn_t* be;   /* replaces ssl, read_bio, write_bio */
```

Keep every other field (peer_addr, alpn, closed, refcnt, handshake_done,
rd_buf, wr_buf, buf_sz, waiter, fd, ssl_mu, rd_mu, wr_mu, inbox, inbox_len,
retransmit_timer, handshake_timer, listener, server_node, rd_deadline_ms,
wr_deadline_ms) exactly as-is.

- [ ] **Step 3: Rewrite the ctx lifecycle + config forwards**

```c
xylem_dtls_ctx_t* xylem_dtls_ctx_create(void) {
    xylem_dtls_ctx_t* ctx = (xylem_dtls_ctx_t*)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->be = tls_backend_ctx_create(TLS_BACKEND_PROTO_DTLS);
    if (!ctx->be) {
        free(ctx);
        return NULL;
    }
    ctx->verify_server = true;
    ctx->verify_client = false;
    return ctx;
}

void xylem_dtls_ctx_destroy(xylem_dtls_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    tls_backend_ctx_destroy(ctx->be);
    free(ctx);
}
```

Forward the loaders and config to the backend (drop the OpenSSL bodies):
- `xylem_dtls_ctx_load_cert` → `tls_backend_ctx_load_cert_file(ctx->be, hostname, cert, key)`
- `xylem_dtls_ctx_load_cert_mem` → `tls_backend_ctx_load_cert_mem(ctx->be, ...)`
  (keep the existing NULL/zero-length guard)
- `xylem_dtls_ctx_load_ca` → `tls_backend_ctx_load_ca_file(ctx->be, ca_file)`
- `xylem_dtls_ctx_load_system_ca` → `tls_backend_ctx_load_system_ca(ctx->be)`
- `xylem_dtls_ctx_set_alpn` → `tls_backend_ctx_set_alpn(ctx->be, protocols, count)`
- `xylem_dtls_ctx_set_keylog` → `tls_backend_ctx_set_keylog(ctx->be, path)`
- `xylem_dtls_ctx_verify_server` / `verify_client`: unchanged (set the bools)

- [ ] **Step 4: Rewrite conn teardown in `_dtls_conn_unref`**

Replace `if (dtls->ssl) { SSL_free(dtls->ssl); }` with:

```c
    if (dtls->be) {
        tls_backend_conn_destroy(dtls->be);
    }
```

(The rest of `_dtls_conn_unref` — waiter, fd, mutexes, buffers, timers, inbox
drain — is unchanged.)

- [ ] **Step 5: Rewrite the server write paths (drain replaces BIO_read)**

`_dtls_server_flush_write_bio`: replace the `BIO_read(dtls->write_bio, ...)`
loop with `tls_backend_conn_drain`:

```c
    int n;
    while ((n = tls_backend_conn_drain(dtls->be, buf, (int)bufsz)) > 0) {
        platform_socket_sendto(dtls->listener->fd, buf, n,
                               &dtls->peer_addr.storage, addrlen);
    }
```

`_dtls_server_send_record`: replace `SSL_write` + `BIO_read`:

```c
    int wn = 0;
    if (tls_backend_conn_write(dtls->be, data, len, &wn) != TLS_BACKEND_OK) {
        return -1;
    }
    xylem_dtls_listener_t* ln = dtls->listener;
    int rd = tls_backend_conn_drain(dtls->be, ln->send_buf,
                                    (int)ln->send_buf_sz);
    if (rd <= 0) {
        return -1;
    }
    /* (unchanged) sendto loop with EAGAIN parking on ln->waiter */
```

- [ ] **Step 6: Rewrite the client pumps**

`_dtls_client_pump_out`: replace the `BIO_read(dtls->write_bio, ...)` call with
`tls_backend_conn_drain(dtls->be, buf, (int)bufsz)` (still under `ssl_mu`, still
loops while `> 0`).

`_dtls_client_pump_in`: replace `BIO_write(dtls->read_bio, buf, (int)n)` with
`tls_backend_conn_feed(dtls->be, buf, (int)n)` (still under `ssl_mu`).

- [ ] **Step 7: Rewrite `_dtls_client_do_handshake`**

Mirror the TLS handshake loop but keep the DTLS retransmit-deadline logic,
swapping the SSL calls for backend calls:

```c
static int _dtls_client_do_handshake(xylem_dtls_conn_t* dtls,
                                     uint64_t deadline) {
    for (;;) {
        xylem_mutex_lock(dtls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_handshake(dtls->be);
        xylem_mutex_unlock(dtls->ssl_mu);

        if (_dtls_client_pump_out(dtls) != 0) {
            return -1;
        }
        if (st == TLS_BACKEND_OK) {
            return 0;
        }
        if (st == TLS_BACKEND_WANT_READ) {
            uint64_t rd_dl = deadline;
            uint64_t to_ms;
            xylem_mutex_lock(dtls->ssl_mu);
            bool have_to = dtls_backend_conn_get_timeout(dtls->be, &to_ms);
            xylem_mutex_unlock(dtls->ssl_mu);
            if (have_to) {
                uint64_t rt_dl =
                    xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + to_ms;
                if (rd_dl == 0 || rt_dl < rd_dl) {
                    rd_dl = rt_dl;
                }
            }
            iowait_set_rd_deadline(dtls->waiter, rd_dl);

            int rc = _dtls_client_pump_in(dtls);
            if (rc == DTLS_PUMP_TIMEOUT) {
                uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
                if (deadline > 0 && now >= deadline) {
                    return -1;
                }
                xylem_mutex_lock(dtls->ssl_mu);
                dtls_backend_conn_handle_timeout(dtls->be);
                xylem_mutex_unlock(dtls->ssl_mu);
                continue;
            }
            if (rc <= 0) {
                return -1;
            }
            continue;
        }
        if (st == TLS_BACKEND_WANT_WRITE) {
            continue;
        }
        return -1;
    }
}
```

- [ ] **Step 8: Rewrite `_dtls_cache_alpn`, retransmit timer, client recv/send**

`_dtls_cache_alpn`:

```c
static void _dtls_cache_alpn(xylem_dtls_conn_t* dtls) {
    tls_backend_conn_get_alpn(dtls->be, dtls->alpn, sizeof(dtls->alpn));
}
```

`_dtls_arm_retransmit`: replace `DTLSv1_get_timeout(dtls->ssl, &tv)` /ms-math
with `dtls_backend_conn_get_timeout(dtls->be, &ms)`:

```c
static void _dtls_arm_retransmit(xylem_dtls_conn_t* dtls) {
    uint64_t ms;
    if (dtls_backend_conn_get_timeout(dtls->be, &ms)) {
        sched_timer_start(dtls->retransmit_timer,
                          _dtls_retransmit_cb, dtls, ms, 0);
    }
}
```

`_dtls_retransmit_cb`: replace `DTLSv1_handle_timeout(dtls->ssl)` with
`dtls_backend_conn_handle_timeout(dtls->be)`.

`_dtls_client_recv`: replace the `SSL_read`/`SSL_get_error` block with
`tls_backend_conn_read(dtls->be, buf, len, &n)`, mapping `OK`→return n,
`CLOSED`→return 0, `WANT_READ`→`_dtls_client_pump_in` (≤0 breaks),
`WANT_WRITE`→`_dtls_client_pump_out` (≠0 breaks), else break. (Same shape as the
TLS `_tls_read_loop` but with the DTLS pumps.)

`_dtls_client_send`: replace the `SSL_write`/`SSL_get_error` block with
`tls_backend_conn_write(dtls->be, data, len, &n)`, mapping `OK`→
`_dtls_client_pump_out` then return, `WANT_WRITE`→pump_out (≠0 breaks),
`WANT_READ`→pump_out then pump_in (≤0 breaks), else break. (Same shape as TLS
`_tls_write_loop`.)

- [ ] **Step 9: Rewrite `_dtls_server_recv`**

Replace the `SSL_read`/`SSL_get_error` block (the surrounding inbox-channel
recv + feed loop is unchanged, but `BIO_write` becomes `feed`):

```c
            BIO_write(...)  /* -> */ tls_backend_conn_feed(dtls->be,
                                        dgram->data, (int)dgram->len);

            int n = 0;
            tls_backend_state_t st =
                tls_backend_conn_read(dtls->be, buf, len, &n);
            if (st == TLS_BACKEND_OK) {
                ret = n;
                break;
            }
            if (st == TLS_BACKEND_CLOSED) {
                ret = 0;
                break;
            }
            if (st != TLS_BACKEND_WANT_READ) {
                break;
            }
            /* WANT_READ: loop for the next datagram */
```

- [ ] **Step 10: Rewrite `xylem_dtls_dial` (conn create + configure)**

Replace the `_dtls_init_ssl` + `SSL_set_connect_state` + `_dtls_apply_verify` +
`_dtls_apply_mtu` + SNI/`SSL_set1_host` block with:

```c
    dtls->be = tls_backend_conn_create(ctx->be, false);
    if (!dtls->be) {
        _dtls_conn_unref(dtls);
        return NULL;
    }
    dtls_backend_conn_set_mtu(dtls->be, opts ? opts->mtu : 0);

    tls_backend_handshake_cfg_t cfg = {0};
    cfg.verify = ctx->verify_server ? TLS_BACKEND_VERIFY_PEER
                                    : TLS_BACKEND_VERIFY_NONE;
    const char* server_name = opts ? opts->server_name : NULL;
    if (server_name) {
        addr_t tmp;
        if (addr_pton(server_name, 0, &tmp) != 0) {   /* not an IP literal */
            cfg.sni_name = server_name;
        }
        if (ctx->verify_server) {
            cfg.verify_host = server_name;
        }
    } else if (ctx->verify_server) {
        xylem_loge("<dtls> dial server_name=NULL with verify_server; "
                   "peer identity unchecked (MITM risk)");
    }
    tls_backend_conn_configure(dtls->be, &cfg);
```

(The `_dtls_client_do_handshake` call and the post-handshake deadline reset +
`_dtls_cache_alpn` below it are unchanged.)

- [ ] **Step 11: Rewrite `_dtls_handshake_coro` (server)**

Replace `_dtls_init_ssl` + `SSL_set_accept_state` +
`SSL_set_ex_data(... &dtls->peer_addr)` + `_dtls_apply_verify` +
`_dtls_apply_mtu`:

```c
    dtls->be = tls_backend_conn_create(ln->ctx->be, true);
    if (!dtls->be) {
        /* (unchanged) remove from session tree, double-unref, return */
    }
    {
        socklen_t salen =
            (dtls->peer_addr.storage.ss_family == AF_INET6)
                ? (socklen_t)sizeof(struct sockaddr_in6)
                : (socklen_t)sizeof(struct sockaddr_in);
        dtls_backend_conn_set_peer_addr(dtls->be, &dtls->peer_addr.storage,
                                        salen);
    }
    dtls_backend_conn_set_mtu(dtls->be, ln->opts.mtu);

    tls_backend_handshake_cfg_t cfg = {0};
    cfg.verify = ln->ctx->verify_client ? TLS_BACKEND_VERIFY_REQUIRE
                                        : TLS_BACKEND_VERIFY_NONE;
    tls_backend_conn_configure(dtls->be, &cfg);
```

Inside the handshake `while` loop, replace `BIO_write(dtls->read_bio, ...)` with
`tls_backend_conn_feed(dtls->be, dgram->data, (int)dgram->len)`, and replace the
`SSL_do_handshake`/`SSL_get_error` decision:

```c
        xylem_mutex_lock(dtls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_handshake(dtls->be);
        xylem_mutex_unlock(dtls->ssl_mu);
        if (st == TLS_BACKEND_OK) {
            _dtls_server_flush_write_bio(dtls);
            dtls->handshake_done = true;
            success = true;
            break;
        }
        if (st == TLS_BACKEND_WANT_READ || st == TLS_BACKEND_WANT_WRITE) {
            _dtls_server_flush_write_bio(dtls);
            _dtls_arm_retransmit(dtls);
            continue;
        }
        _dtls_server_flush_write_bio(dtls);
        break;
```

> Note: the server handshake loop runs the SSL step without holding `ssl_mu` in
> the original (it is single-threaded per session during handshake). Holding
> `ssl_mu` here as shown is harmless and keeps the contract uniform; keep it.

- [ ] **Step 12: Rewrite `_dtls_server_close_conn` shutdown**

Replace `SSL_shutdown(dtls->ssl)` with `tls_backend_conn_shutdown(dtls->be)`
(the guard `if (dtls->handshake_done && dtls->be)` and the following
`_dtls_server_flush_write_bio` are unchanged).

- [ ] **Step 13: Build, test, and confirm OpenSSL-free**

```bash
cmake --build out -j 8
ctest --test-dir out -R tls --output-on-failure
grep -n "openssl" src/net/tls/xylem-dtls.c
```

Expected: build clean; `test-tls` and `test-dtls` PASS; grep returns no matches.

- [ ] **Step 14: Commit**

```bash
git add src/net/tls/xylem-dtls.c
git commit -m "refactor(tls): drive DTLS engine through backend interface"
```

---

## Task 8: Remove the platform-tls shim and finalize CMake

**Files:**
- Delete: `src/platform/platform-tls.h`
- Delete: `src/platform/unix/platform-tls.c`
- Delete: `src/platform/win/platform-tls.c`
- Modify: `CMakeLists.txt`

The system-CA logic now lives in the OpenSSL backend (Task 2 Step 7), so the
`SSL_CTX*`-leaking platform shim is dead. Removing it closes the last
OpenSSL-typed surface outside the backend.

- [ ] **Step 1: Confirm nothing still includes platform-tls.h**

```bash
grep -rn "platform-tls" src
```

Expected: no matches (Task 5 Step 1 and Task 7 Step 1 removed the includes from
`tls.c` and `xylem-dtls.c`; `tls-backend-openssl.c` does not include it).
If anything remains, fix it before deleting.

- [ ] **Step 2: Delete the three files**

```bash
git rm src/platform/platform-tls.h \
       src/platform/unix/platform-tls.c \
       src/platform/win/platform-tls.c
```

- [ ] **Step 3: Remove the platform-tls entries from CMake**

In `CMakeLists.txt`, delete this block from inside `if(XYLEM_ENABLE_TLS)`:

```cmake
	# platform-tls depends on OpenSSL, so it lives in the TLS block
	# rather than the unconditional platform source lists above.
	if(WIN32)
		list(APPEND SRCS src/platform/win/platform-tls.c)
	endif()
	if(UNIX)
		list(APPEND SRCS src/platform/unix/platform-tls.c)
	endif()
```

The final `if(XYLEM_ENABLE_TLS)` source list should read:

```cmake
if(XYLEM_ENABLE_TLS)
	find_package(OpenSSL 3.5 REQUIRED)

	set_target_properties(OpenSSL::SSL PROPERTIES SYSTEM OFF)
	set_target_properties(OpenSSL::Crypto PROPERTIES SYSTEM OFF)

	list(APPEND SRCS
		src/net/tls/tls-backend-openssl.c
		src/net/tls/tls.c
		src/net/tls/xylem-tls.c
		src/net/tls/xylem-dtls.c
		src/net/http/http-transport-tls.c
		src/net/ws/ws-tls.c
	)
else()
	list(APPEND SRCS
		src/net/http/http-transport-tls-stub.c
		src/net/tls/xylem-tls-stub.c
		src/net/ws/ws-tls-stub.c
	)
endif()
```

- [ ] **Step 4: Reserve the backend-selection variable (forward-looking)**

Immediately after `option(XYLEM_ENABLE_TLS ...)` near the top of
`CMakeLists.txt`, add:

```cmake
set(XYLEM_TLS_BACKEND "openssl" CACHE STRING
    "TLS backend implementation (currently: openssl)")
set_property(CACHE XYLEM_TLS_BACKEND PROPERTY STRINGS openssl)
```

Then make the backend source selection use it, replacing the hardcoded
`src/net/tls/tls-backend-openssl.c` line with:

```cmake
		src/net/tls/tls-backend-${XYLEM_TLS_BACKEND}.c
```

This adds the seam for a future `tls-backend-wolfssl.c` /
`tls-backend-mbedtls.c` without enabling them yet (only `openssl` is a valid
value). Validate the choice:

```cmake
	if(NOT XYLEM_TLS_BACKEND STREQUAL "openssl")
		message(FATAL_ERROR "unsupported XYLEM_TLS_BACKEND: ${XYLEM_TLS_BACKEND}")
	endif()
```

Place this validation just inside `if(XYLEM_ENABLE_TLS)` before the
`find_package`.

- [ ] **Step 5: Reconfigure, build, and test from clean**

```bash
cmake -B out -G Ninja -DCMAKE_BUILD_TYPE=Debug -DXYLEM_ENABLE_TESTING=ON -DXYLEM_ENABLE_TLS=ON
cmake --build out -j 8
ctest --test-dir out -R tls --output-on-failure
```

Expected: configures (winstore/default-paths code is compiled inside the backend
per-OS), builds, and `test-tls` + `test-dtls` PASS.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt
git commit -m "refactor(tls): fold system-CA into backend, drop platform-tls shim"
```

---

## Task 9: Whole-tree verification

**Files:** none (verification only)

- [ ] **Step 1: Prove the engine and platform layers are OpenSSL-free**

```bash
grep -rn "openssl" src --include=*.c --include=*.h
```

Expected: matches **only** in `src/net/tls/tls-backend-openssl.c`. No matches in
`tls.c`, `tls.h`, `xylem-dtls.c`, or anywhere under `src/platform`.

- [ ] **Step 2: Prove the TLS-disabled build still works**

The stub path must be unaffected by the refactor.

```bash
cmake -B out-notls -G Ninja -DCMAKE_BUILD_TYPE=Debug -DXYLEM_ENABLE_TESTING=ON
cmake --build out-notls -j 8
ctest --test-dir out-notls --output-on-failure
```

Expected: configures without OpenSSL, builds, full suite PASS (tls/dtls tests
are not registered when `XYLEM_ENABLE_TLS=OFF`).

- [ ] **Step 3: Run the full TLS-enabled suite (not just tls/dtls)**

```bash
ctest --test-dir out --output-on-failure
```

Expected: every test PASS — confirms HTTPS/WSS transports (which sit on the TLS
engine) are unbroken.

- [ ] **Step 4: ASan + UBSan run**

```bash
cmake -B out-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DXYLEM_ENABLE_TESTING=ON -DXYLEM_ENABLE_TLS=ON -DXYLEM_ENABLE_ASAN=ON -DXYLEM_ENABLE_UBSAN=ON
cmake --build out-asan -j 8
ctest --test-dir out-asan -R tls --output-on-failure
```

Expected: `test-tls` + `test-dtls` PASS with no sanitizer reports. This is the
key safety gate — the lock split, refcounting, and BIO ownership all moved, so
ASan/UBSan must confirm no leak (e.g. a dropped `tls_backend_conn_destroy`), no
use-after-free, and no UB.

- [ ] **Step 5: TSan run (Unix only)**

```bash
cmake -B out-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DXYLEM_ENABLE_TESTING=ON -DXYLEM_ENABLE_TLS=ON -DXYLEM_ENABLE_TSAN=ON
cmake --build out-tsan -j 8
ctest --test-dir out-tsan -R tls --output-on-failure
```

Expected: PASS with no data-race reports — confirms the `ssl_mu`/`rd_mu`/`wr_mu`
contract still holds across the backend boundary (the duplex read-while-write
and write-while-close races). On WSL, prefix with `setarch -R`.

- [ ] **Step 6: Clean up throwaway build dirs**

```bash
rm -rf out-notls out-asan out-tsan
```

(Leave `out` for ongoing work; these scratch dirs are not committed — `build/`
and `out/` are already untracked.)

- [ ] **Step 7: Update the design docs to reflect the shipped state**

In `docs/design/tls.md`:
- §5 "Trust anchors": replace the `platform_tls_load_system_ca` shim description
  with "system-CA loading is a backend responsibility
  (`tls_backend_ctx_load_system_ca`); the Windows winstore / Unix
  default-verify-paths split lives inside the OpenSSL backend".
- Add a one-line pointer near the top: "OpenSSL access is isolated behind the
  backend interface — see [`tls-backend.md`](tls-backend.md)."

In `docs/design/tls-backend.md`: change the Status line from "Proposed" to
"Implemented", and update §7 to note the DTLS engine/shim split was deferred
(the file remains `src/net/tls/xylem-dtls.c`, combined engine+API).

In `docs/design/platform.md`: remove or amend any reference to
`platform_tls_load_system_ca` / the platform-tls shim, pointing to the backend
instead.

- [ ] **Step 8: Commit the doc updates**

```bash
git add docs/design/tls.md docs/design/tls-backend.md docs/design/platform.md
git commit -m "docs(tls): document backend abstraction as shipped"
```

---

## Self-review summary

- **Spec coverage:** §1 motivation/scope → Tasks 1-8; §2 boundary → Tasks 5,7;
  §3 neutral types → Task 1; §4 interface → Tasks 1-3; §5 concurrency contract →
  Task 1 (documented) + preserved in Tasks 5,7; §6 engine structs → Tasks 4,7;
  §7 file layout/migration → Tasks 6,8 (split deferred — deviation flagged);
  §8 verification → Task 9; §9 future backends → Task 8 (CMake seam).
- **Deviation:** the DTLS engine/shim split (design §7) is deferred; the file is
  relocated and migrated but not split. Reuse goal is met via backend
  convergence. Flagged in Task 6 and recorded in Task 9 Step 7.
- **Platform-tls removal** (the one OpenSSL leak the user specifically called
  out) is Task 8, with the OS split absorbed into the backend in Task 2 Step 7.
- **Type consistency:** `tls_backend_*`/`dtls_backend_*` names, the
  `tls_backend_state_t` arms, and `tls_backend_handshake_cfg_t` fields are used
  identically across Tasks 1, 3, 5, 7.
