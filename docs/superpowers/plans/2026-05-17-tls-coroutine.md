# TLS Coroutine Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite the TLS module from callback-based to coroutine blocking-style, using Socket BIO + iowait, mirroring TCP's pattern exactly.

**Architecture:** TLS connections directly hold an fd + iowait handle (same as TCP). OpenSSL uses `SSL_set_fd` to operate on the non-blocking socket directly. When SSL operations return `WANT_READ`/`WANT_WRITE`, the coroutine parks via `iowait_read()`/`iowait_write()`. Framing, buffered reads, deadlines all mirror TCP's internal implementation.

**Tech Stack:** C11, OpenSSL (SSL_set_fd / Socket BIO), minicoro coroutines via scheduler, iowait for I/O parking.

**Spec:** `docs/superpowers/specs/2026-05-17-tls-coroutine-design.md`

**Key reference:** `src/net/xylem-tcp.c` — the TLS implementation mirrors this file's structure almost exactly. `tests/test-tcp.c` — tests use `xylem_run`, `xylem_spawn`, `xylem_channel`, `xylem_waitgroup`, `xylem_timer_after`, `xylem_sleep`, `xylem_shutdown`.

**Style:** Follow `c-project-style` skill. Comments must be ASCII-only, `/* */` style, explain why not what. All code is C11.

---

### Task 1: Add XYLEM_ERR_TLS Error Code

**Files:**
- Modify: `include/xylem/xylem-error.h:31-44`
- Modify: `src/xylem-error.c:24-40`

- [ ] **Step 1: Add XYLEM_ERR_TLS to the enum**

In `include/xylem/xylem-error.h`, add a new entry after `XYLEM_ERR_UNKNOWN`:

```c
    XYLEM_ERR_UNKNOWN      = 11, /*< Unmapped platform error. */
    XYLEM_ERR_TLS          = 12, /*< TLS/SSL layer error. */
} xylem_err_t;
```

- [ ] **Step 2: Add the tostring case**

In `src/xylem-error.c`, add inside the switch before the closing brace:

```c
    case XYLEM_ERR_UNKNOWN:      return "unknown error";
    case XYLEM_ERR_TLS:          return "tls error";
    }
```

- [ ] **Step 3: Build to verify**

Run: `cmake --build build --target xylem`
Expected: Clean build, no errors.

- [ ] **Step 4: Commit**

```bash
git add include/xylem/xylem-error.h src/xylem-error.c
git commit -m "feat(error): add XYLEM_ERR_TLS error code"
```

---

### Task 2: Rewrite TLS Public Header

**Files:**
- Rewrite: `include/xylem/net/xylem-tls.h`

- [ ] **Step 1: Replace the entire header**

Replace the contents of `include/xylem/net/xylem-tls.h` with:

```c
_Pragma("once")

#include "xylem/net/xylem-tcp.h"
#include "xylem/xylem-error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct xylem_tls_conn_s     xylem_tls_conn_t;
typedef struct xylem_tls_ctx_s      xylem_tls_ctx_t;
typedef struct xylem_tls_listener_s xylem_tls_listener_t;

typedef struct xylem_tls_opts_s {
    size_t      max_read_buf;       /*< Plaintext read buffer size, 0 = default 64KB. */
    bool        disable_mss_clamp;  /*< Disable MSS clamping on the socket. */
    uint64_t    connect_timeout_ms; /*< TCP connect + TLS handshake timeout, 0 = none. */
    const char* hostname;           /*< SNI hostname for certificate selection and verification. */
} xylem_tls_opts_t;

extern xylem_tls_ctx_t* xylem_tls_ctx_create(void);
extern void xylem_tls_ctx_destroy(xylem_tls_ctx_t* ctx);
extern int xylem_tls_ctx_load_cert(xylem_tls_ctx_t* ctx,
                                   const char* cert, const char* key);
extern int xylem_tls_ctx_set_ca(xylem_tls_ctx_t* ctx, const char* ca_file);
extern void xylem_tls_ctx_set_verify(xylem_tls_ctx_t* ctx, bool enable);
extern int xylem_tls_ctx_set_alpn(xylem_tls_ctx_t* ctx,
                                  const char** protocols, size_t count);
extern int xylem_tls_ctx_set_keylog(xylem_tls_ctx_t* ctx, const char* path);

/**
 * @brief Connect to a remote TLS endpoint.
 *
 * Suspends the calling coroutine until the TCP connection is established
 * and the TLS handshake completes, or connect_timeout_ms elapses.
 *
 * @param host  Remote hostname or IP address.
 * @param port  Remote port.
 * @param ctx   TLS context.
 * @param opts  TLS options, NULL for defaults.
 *
 * @return Connection handle, or NULL on failure or timeout.
 */
extern xylem_tls_conn_t* xylem_tls_dial(
    const char*       host,
    uint16_t          port,
    xylem_tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts);

/**
 * @brief Create a TLS listener bound to the given address.
 *
 * @param host  Bind address (e.g. "0.0.0.0"), or NULL for any.
 * @param port  Bind port.
 * @param ctx   TLS context with cert+key loaded.
 * @param opts  TLS options, NULL for defaults.
 *
 * @return Listener handle, or NULL on failure.
 */
extern xylem_tls_listener_t* xylem_tls_listen(
    const char*       host,
    uint16_t          port,
    xylem_tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts);

/**
 * @brief Accept a connection from the listener.
 *
 * Suspends the calling coroutine until a client connects and the
 * TLS handshake completes.
 *
 * @param ln  Listener handle.
 *
 * @return Accepted connection, or NULL if the listener is closing.
 */
extern xylem_tls_conn_t* xylem_tls_accept(xylem_tls_listener_t* ln);

/**
 * @brief Set the framing mode for subsequent recv/send calls.
 *
 * @param tls   Connection handle.
 * @param opts  Frame options, NULL to reset to raw mode.
 */
extern void xylem_tls_set_framing(
    xylem_tls_conn_t*       tls,
    xylem_tcp_frame_opts_t* opts);

/**
 * @brief Set the read deadline for the connection.
 *
 * @param tls          Connection handle.
 * @param deadline_ms  Monotonic deadline in ms, or 0 to clear.
 */
extern void xylem_tls_set_read_deadline(
    xylem_tls_conn_t* tls,
    uint64_t          deadline_ms);

/**
 * @brief Set the write deadline for the connection.
 *
 * @param tls          Connection handle.
 * @param deadline_ms  Monotonic deadline in ms, or 0 to clear.
 */
extern void xylem_tls_set_write_deadline(
    xylem_tls_conn_t* tls,
    uint64_t          deadline_ms);

/**
 * @brief Receive data or a complete frame from the connection.
 *
 * Behavior depends on the configured framing mode (same as TCP):
 *   - NONE:      returns 1~len available bytes.
 *   - FIXED:     returns exactly frame_opts.fixed.len bytes.
 *   - LENGTH:    reads header, decodes length, returns payload.
 *   - DELIMITER: reads until delimiter, returns data without it.
 *
 * @param tls  Connection handle.
 * @param buf  Destination buffer.
 * @param len  Buffer size.
 *
 * @return Bytes read (>0), 0 on peer close (NONE mode, error set
 *         to XYLEM_ERR_PEER_CLOSED), -1 on error/timeout.
 */
extern int64_t xylem_tls_recv(
    xylem_tls_conn_t* tls,
    void*             buf,
    size_t            len);

/**
 * @brief Send data or a framed message to the connection.
 *
 * All bytes are written before returning.
 *
 * @param tls   Connection handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to send.
 *
 * @return 0 on success, -1 on error or timeout.
 */
extern int xylem_tls_send(
    xylem_tls_conn_t* tls,
    const void*       data,
    size_t            len);

/**
 * @brief Close and destroy a connection.
 *
 * @param tls  Connection handle.
 */
extern void xylem_tls_close(xylem_tls_conn_t* tls);

/**
 * @brief Close and destroy a listener.
 *
 * @param ln  Listener handle.
 */
extern void xylem_tls_close_listener(xylem_tls_listener_t* ln);

/**
 * @brief Get the last error code from the connection.
 *
 * @param tls  Connection handle.
 *
 * @return Error code, or XYLEM_ERR_NONE if no error.
 */
extern xylem_err_t xylem_tls_get_error(xylem_tls_conn_t* tls);

extern int xylem_tls_remote_addr(
    xylem_tls_conn_t* tls,
    char*             host,
    size_t            host_len,
    uint16_t*         port);

extern int xylem_tls_local_addr(
    xylem_tls_conn_t* tls,
    char*             host,
    size_t            host_len,
    uint16_t*         port);

extern int xylem_tls_listener_addr(
    xylem_tls_listener_t* ln,
    char*                 host,
    size_t                host_len,
    uint16_t*             port);

extern const char* xylem_tls_get_alpn(xylem_tls_conn_t* tls);
```

Keep the existing copyright header at the top (copy it from the current file).

- [ ] **Step 2: Build to verify header compiles**

Run: `cmake --build build --target xylem`
Expected: Build will fail because `src/net/xylem-tls.c` still has old code. That's expected — we verify the header syntax is correct by checking only for header-related errors.

- [ ] **Step 3: Commit**

```bash
git add include/xylem/net/xylem-tls.h
git commit -m "feat(tls): rewrite public header for coroutine API"
```

---

### Task 3: Implement TLS Core — Ctx, Structs, SSL Helpers

**Files:**
- Rewrite: `src/net/xylem-tls.c` (first half: includes, structs, ctx management, SSL I/O helpers)

This task replaces the entire `src/net/xylem-tls.c` file. We build it incrementally — this task covers everything except the public API functions (dial/listen/accept/recv/send/close), which come in Tasks 4-6.

- [ ] **Step 1: Write the file skeleton with includes, defines, structs, and ctx management**

Replace `src/net/xylem-tls.c` entirely with this content (keep existing copyright header):

```c
#include "xylem/net/xylem-tls.h"

#include "xylem/xylem-error.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "net/addr.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"
#include "runtime/runtime.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_READ_BUF_SIZE 65536

static int _tls_ex_data_idx = -1;
static once_flag _tls_ex_data_once = ONCE_FLAG_INIT;

static void _tls_init_ex_data(void) {
    _tls_ex_data_idx = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

struct xylem_tls_ctx_s {
    SSL_CTX* ssl_ctx;
    uint8_t* alpn_wire;
    size_t   alpn_wire_len;
    FILE*    keylog_file;
};

struct xylem_tls_conn_s {
    SSL*                   ssl;
    iowait_t*              waiter;
    platform_sock_t        fd;
    xylem_tls_ctx_t*       ctx;
    addr_t                 peer_addr;
    xylem_tcp_frame_opts_t frame_opts;
    char*                  read_buf;
    size_t                 read_buf_cap;
    size_t                 read_buf_pos;
    size_t                 read_buf_len;
    char                   alpn[256];
    xylem_err_t            err;
    _Atomic bool           closed;
};

struct xylem_tls_listener_s {
    iowait_t*        waiter;
    platform_sock_t  fd;
    xylem_tls_ctx_t* ctx;
    xylem_tls_opts_t opts;
    _Atomic bool     closing;
};

/* --- ctx management (unchanged from old implementation) --- */

static void _tls_keylog_cb(const SSL* ssl, const char* line) {
    SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
    xylem_tls_ctx_t* ctx =
        (xylem_tls_ctx_t*)SSL_CTX_get_ex_data(ssl_ctx, _tls_ex_data_idx);
    if (ctx && ctx->keylog_file) {
        fprintf(ctx->keylog_file, "%s\n", line);
        fflush(ctx->keylog_file);
    }
}

static int _tls_alpn_select_cb(SSL* ssl, const unsigned char** out,
                               unsigned char* outlen,
                               const unsigned char* in,
                               unsigned int inlen, void* arg) {
    xylem_tls_ctx_t* ctx = (xylem_tls_ctx_t*)arg;
    (void)ssl;

    if (SSL_select_next_proto((unsigned char**)out, outlen,
                              ctx->alpn_wire,
                              (unsigned int)ctx->alpn_wire_len,
                              in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}

xylem_tls_ctx_t* xylem_tls_ctx_create(void) {
    xylem_tls_ctx_t* ctx = (xylem_tls_ctx_t*)calloc(1, sizeof(xylem_tls_ctx_t));
    if (!ctx) {
        return NULL;
    }

    ctx->ssl_ctx = SSL_CTX_new(TLS_method());
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    SSL_CTX_set_mode(ctx->ssl_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION);

    call_once(&_tls_ex_data_once, _tls_init_ex_data);
    SSL_CTX_set_ex_data(ctx->ssl_ctx, _tls_ex_data_idx, ctx);

    return ctx;
}

void xylem_tls_ctx_destroy(xylem_tls_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->keylog_file) {
        fclose(ctx->keylog_file);
    }
    SSL_CTX_free(ctx->ssl_ctx);
    free(ctx->alpn_wire);
    free(ctx);
}

int xylem_tls_ctx_set_keylog(xylem_tls_ctx_t* ctx, const char* path) {
    if (!ctx) {
        return -1;
    }
    if (ctx->keylog_file) {
        fclose(ctx->keylog_file);
        ctx->keylog_file = NULL;
    }
    if (!path) {
        SSL_CTX_set_keylog_callback(ctx->ssl_ctx, NULL);
        return 0;
    }
    ctx->keylog_file = fopen(path, "a");
    if (!ctx->keylog_file) {
        return -1;
    }
    SSL_CTX_set_keylog_callback(ctx->ssl_ctx, _tls_keylog_cb);
    return 0;
}

int xylem_tls_ctx_load_cert(xylem_tls_ctx_t* ctx,
                            const char* cert, const char* key) {
    if (SSL_CTX_use_certificate_chain_file(ctx->ssl_ctx, cert) != 1) {
        xylem_loge("tls ctx: failed to load cert %s", cert);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, key,
                                    SSL_FILETYPE_PEM) != 1) {
        xylem_loge("tls ctx: failed to load key %s", key);
        return -1;
    }
    return 0;
}

int xylem_tls_ctx_set_ca(xylem_tls_ctx_t* ctx, const char* ca_file) {
    if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_file, NULL) != 1) {
        xylem_loge("tls ctx: failed to load CA %s", ca_file);
        return -1;
    }
    return 0;
}

void xylem_tls_ctx_set_verify(xylem_tls_ctx_t* ctx, bool enable) {
    int mode = enable ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
    SSL_CTX_set_verify(ctx->ssl_ctx, mode, NULL);
}

int xylem_tls_ctx_set_alpn(xylem_tls_ctx_t* ctx,
                           const char** protocols, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += 1 + strlen(protocols[i]);
    }

    uint8_t* wire = (uint8_t*)malloc(total);
    if (!wire) {
        return -1;
    }

    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        size_t plen = strlen(protocols[i]);
        wire[off++] = (uint8_t)plen;
        memcpy(wire + off, protocols[i], plen);
        off += plen;
    }

    free(ctx->alpn_wire);
    ctx->alpn_wire     = wire;
    ctx->alpn_wire_len = total;

    SSL_CTX_set_alpn_protos(ctx->ssl_ctx, wire, (unsigned int)total);
    SSL_CTX_set_alpn_select_cb(ctx->ssl_ctx, _tls_alpn_select_cb, ctx);

    return 0;
}

/* --- SSL I/O helpers --- */

static int _tls_do_handshake(xylem_tls_conn_t* tls) {
    for (;;) {
        ERR_clear_error();
        int ret = SSL_do_handshake(tls->ssl);
        if (ret == 1) {
            return 0;
        }

        int err = SSL_get_error(tls->ssl, ret);
        if (err == SSL_ERROR_WANT_READ) {
            iowait_result_t r = iowait_read(tls->waiter);
            if (r != IOWAIT_READY) {
                tls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
        } else if (err == SSL_ERROR_WANT_WRITE) {
            iowait_result_t r = iowait_write(tls->waiter);
            if (r != IOWAIT_READY) {
                tls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
        } else {
            tls->err = XYLEM_ERR_TLS;
            return -1;
        }
    }
}

static void _tls_cache_alpn(xylem_tls_conn_t* tls) {
    const unsigned char* alpn_proto = NULL;
    unsigned int         alpn_len   = 0;
    SSL_get0_alpn_selected(tls->ssl, &alpn_proto, &alpn_len);
    if (alpn_proto && alpn_len > 0 && alpn_len < sizeof(tls->alpn)) {
        memcpy(tls->alpn, alpn_proto, alpn_len);
        tls->alpn[alpn_len] = '\0';
    }
}

static int64_t _tls_raw_recv(xylem_tls_conn_t* tls, void* buf, size_t len) {
    if (atomic_load_explicit(&tls->closed, memory_order_acquire)) {
        tls->err = XYLEM_ERR_CLOSED;
        return -1;
    }

    for (;;) {
        ERR_clear_error();
        int n = SSL_read(tls->ssl, buf, (int)len);
        if (n > 0) {
            return n;
        }

        int err = SSL_get_error(tls->ssl, n);
        if (err == SSL_ERROR_ZERO_RETURN) {
            tls->err = XYLEM_ERR_PEER_CLOSED;
            return 0;
        }
        if (err == SSL_ERROR_WANT_READ) {
            iowait_result_t r = iowait_read(tls->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&tls->closed,
                                        memory_order_acquire)) {
                tls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            iowait_result_t r = iowait_write(tls->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&tls->closed,
                                        memory_order_acquire)) {
                tls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
            continue;
        }
        tls->err = XYLEM_ERR_TLS;
        return -1;
    }
}

static int _tls_raw_send(xylem_tls_conn_t* tls,
                         const void* data, size_t len) {
    if (atomic_load_explicit(&tls->closed, memory_order_acquire)) {
        tls->err = XYLEM_ERR_CLOSED;
        return -1;
    }

    const char* ptr = (const char*)data;
    size_t      rem = len;

    while (rem > 0) {
        ERR_clear_error();
        int n = SSL_write(tls->ssl, ptr, (int)rem);
        if (n > 0) {
            ptr += n;
            rem -= (size_t)n;
            continue;
        }

        int err = SSL_get_error(tls->ssl, n);
        if (err == SSL_ERROR_WANT_WRITE) {
            iowait_result_t r = iowait_write(tls->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&tls->closed,
                                        memory_order_acquire)) {
                tls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_READ) {
            iowait_result_t r = iowait_read(tls->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&tls->closed,
                                        memory_order_acquire)) {
                tls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
            continue;
        }
        tls->err = XYLEM_ERR_TLS;
        return -1;
    }
    return 0;
}
```

Note: the file is NOT complete yet — public API functions come in subsequent tasks. The file will not compile in isolation until all tasks are done.

- [ ] **Step 2: Commit**

```bash
git add src/net/xylem-tls.c
git commit -m "feat(tls): rewrite core — structs, ctx, SSL I/O helpers"
```

---

### Task 4: Implement Dial and Close

**Files:**
- Modify: `src/net/xylem-tls.c` (append dial, close, conn alloc/free)

- [ ] **Step 1: Add conn alloc helper and xylem_tls_dial**

Append to `src/net/xylem-tls.c`:

```c
/* --- conn allocation --- */

static xylem_tls_conn_t* _tls_conn_alloc(
    platform_sock_t fd, size_t max_read_buf) {
    xylem_tls_conn_t* tls
        = (xylem_tls_conn_t*)calloc(1, sizeof(xylem_tls_conn_t));
    if (!tls) {
        return NULL;
    }

    tls->fd     = fd;
    tls->waiter = iowait_create(fd);
    if (!tls->waiter) {
        free(tls);
        return NULL;
    }

    size_t buf_cap = max_read_buf > 0 ? max_read_buf : DEFAULT_READ_BUF_SIZE;
    tls->read_buf = (char*)malloc(buf_cap);
    if (!tls->read_buf) {
        iowait_destroy(tls->waiter);
        free(tls);
        return NULL;
    }
    tls->read_buf_cap = buf_cap;

    return tls;
}

static void _tls_conn_free(xylem_tls_conn_t* tls) {
    if (tls->ssl) {
        SSL_free(tls->ssl);
    }
    if (tls->waiter) {
        iowait_destroy(tls->waiter);
    }
    if (tls->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(tls->fd);
    }
    free(tls->read_buf);
    free(tls);
}

/* --- dial --- */

xylem_tls_conn_t* xylem_tls_dial(
    const char*       host,
    uint16_t          port,
    xylem_tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    const char* dial_host = host;
    char        resolved_ip[INET6_ADDRSTRLEN];
    addr_t      resolved_addr;

    if (addr_pton(host, port, &resolved_addr) != 0) {
        addr_t* addrs = NULL;
        size_t  count = 0;
        if (addr_resolve(host, &addrs, &count) != 0 || count == 0) {
            xylem_loge("tls dial: DNS resolution failed for %s", host);
            return NULL;
        }
        resolved_addr = addrs[0];
        free(addrs);
        uint16_t rport;
        addr_ntop(&resolved_addr, resolved_ip, sizeof(resolved_ip), &rport);
        dial_host = resolved_ip;
    }

    bool            connected = false;
    platform_sock_t fd        = platform_socket_dial(
        dial_host, port_str, SOCK_STREAM, &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("tls dial: socket creation failed for %s:%s",
                   host, port_str);
        return NULL;
    }

    if (opts && opts->disable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, false);
    }

    size_t             max_buf = opts ? opts->max_read_buf : 0;
    xylem_tls_conn_t*  tls     = _tls_conn_alloc(fd, max_buf);
    if (!tls) {
        platform_socket_close(fd);
        return NULL;
    }

    tls->ctx       = ctx;
    tls->peer_addr = resolved_addr;

    uint64_t connect_ms = opts ? opts->connect_timeout_ms : 0;
    uint64_t deadline   = 0;
    if (connect_ms > 0) {
        deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                   + connect_ms;
    }

    /* Wait for TCP connect completion. */
    if (!connected) {
        if (deadline > 0) {
            iowait_set_wr_deadline(tls->waiter, deadline);
        }
        iowait_result_t r = iowait_write(tls->waiter);
        if (r != IOWAIT_READY) {
            tls->err = XYLEM_ERR_TIMEOUT;
            _tls_conn_free(tls);
            return NULL;
        }

        int32_t   err    = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen);
        if (err != 0) {
            xylem_loge("tls dial fd=%d connect error=%d (%s)",
                       (int)fd, err, platform_socket_tostring(err));
            _tls_conn_free(tls);
            return NULL;
        }
    }

    /* Set up SSL for handshake — both directions need the deadline. */
    if (deadline > 0) {
        iowait_set_rd_deadline(tls->waiter, deadline);
        iowait_set_wr_deadline(tls->waiter, deadline);
    }

    tls->ssl = SSL_new(ctx->ssl_ctx);
    if (!tls->ssl) {
        xylem_loge("tls dial: SSL_new failed");
        _tls_conn_free(tls);
        return NULL;
    }
    SSL_set_fd(tls->ssl, (int)fd);
    SSL_set_connect_state(tls->ssl);

    const char* hostname = opts ? opts->hostname : NULL;
    if (hostname) {
        SSL_set_tlsext_host_name(tls->ssl, hostname);
        SSL_set1_host(tls->ssl, hostname);
    }

    if (_tls_do_handshake(tls) != 0) {
        xylem_loge("tls dial: handshake failed for %s:%s", host, port_str);
        _tls_conn_free(tls);
        return NULL;
    }

    /* Clear deadlines after handshake. */
    iowait_set_rd_deadline(tls->waiter, 0);
    iowait_set_wr_deadline(tls->waiter, 0);

    _tls_cache_alpn(tls);
    return tls;
}

/* --- close --- */

void xylem_tls_close(xylem_tls_conn_t* tls) {
    if (atomic_exchange(&tls->closed, true)) {
        return;
    }

    if (tls->ssl) {
        ERR_clear_error();
        SSL_shutdown(tls->ssl);
    }

    iowait_close(tls->waiter);
    _tls_conn_free(tls);
}
```

- [ ] **Step 2: Commit**

```bash
git add src/net/xylem-tls.c
git commit -m "feat(tls): implement dial and close"
```

---

### Task 5: Implement Listen, Accept, Close Listener

**Files:**
- Modify: `src/net/xylem-tls.c` (append listen, accept, close_listener)

- [ ] **Step 1: Add listen, accept, and close_listener**

Append to `src/net/xylem-tls.c`:

```c
/* --- listen --- */

xylem_tls_listener_t* xylem_tls_listen(
    const char*       host,
    uint16_t          port,
    xylem_tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd
        = platform_socket_listen(host, port_str, SOCK_STREAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("tls listen: failed for %s:%s", host, port_str);
        return NULL;
    }

    if (opts && opts->disable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, false);
    }

    xylem_tls_listener_t* ln = (xylem_tls_listener_t*)calloc(
        1, sizeof(xylem_tls_listener_t));
    if (!ln) {
        platform_socket_close(fd);
        return NULL;
    }

    ln->fd  = fd;
    ln->ctx = ctx;
    if (opts) {
        ln->opts = *opts;
    }

    ln->waiter = iowait_create(fd);
    if (!ln->waiter) {
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    return ln;
}

/* --- accept --- */

xylem_tls_conn_t* xylem_tls_accept(xylem_tls_listener_t* ln) {
    uint64_t backoff_ms = 5;

    for (;;) {
        if (atomic_load_explicit(&ln->closing, memory_order_acquire)) {
            return NULL;
        }

        platform_sock_t fd = platform_socket_accept(ln->fd, true);
        if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN
                || err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                if (iowait_read(ln->waiter) != IOWAIT_READY) {
                    return NULL;
                }
                continue;
            }

            xylem_logw("tls listener fd=%d accept error=%d (%s)",
                       (int)ln->fd, err,
                       platform_socket_tostring(err));
            runtime_sleep(backoff_ms);
            if (backoff_ms < 1000) {
                backoff_ms *= 2;
            }
            continue;
        }

        backoff_ms = 5;

        size_t max_buf = ln->opts.max_read_buf;
        xylem_tls_conn_t* tls = _tls_conn_alloc(fd, max_buf);
        if (!tls) {
            platform_socket_close(fd);
            continue;
        }

        tls->ctx = ln->ctx;

        socklen_t peer_len = sizeof(tls->peer_addr.storage);
        getpeername(
            fd, (struct sockaddr*)&tls->peer_addr.storage, &peer_len);

        tls->ssl = SSL_new(ln->ctx->ssl_ctx);
        if (!tls->ssl) {
            xylem_loge("tls accept: SSL_new failed");
            _tls_conn_free(tls);
            continue;
        }
        SSL_set_fd(tls->ssl, (int)fd);
        SSL_set_accept_state(tls->ssl);

        if (_tls_do_handshake(tls) != 0) {
            xylem_logw("tls accept: handshake failed");
            _tls_conn_free(tls);
            continue;
        }

        _tls_cache_alpn(tls);
        return tls;
    }
}

/* --- close listener --- */

void xylem_tls_close_listener(xylem_tls_listener_t* ln) {
    if (atomic_exchange(&ln->closing, true)) {
        return;
    }

    iowait_close(ln->waiter);
    iowait_destroy(ln->waiter);
    platform_socket_close(ln->fd);
    free(ln);
}
```

- [ ] **Step 2: Commit**

```bash
git add src/net/xylem-tls.c
git commit -m "feat(tls): implement listen, accept, close_listener"
```

---

### Task 6: Implement Framing, Recv, Send, Deadline, Info Queries

**Files:**
- Modify: `src/net/xylem-tls.c` (append remaining public APIs)

- [ ] **Step 1: Add framing, recv, send, and query functions**

Append to `src/net/xylem-tls.c`:

```c
/* --- framing / buffered read (mirrors TCP) --- */

static int _tls_read_exact(xylem_tls_conn_t* tls, void* buf, size_t len) {
    char*  ptr = (char*)buf;
    size_t rem = len;

    while (rem > 0) {
        size_t avail = tls->read_buf_len - tls->read_buf_pos;
        if (avail > 0) {
            size_t copy = avail < rem ? avail : rem;
            memcpy(ptr, tls->read_buf + tls->read_buf_pos, copy);
            tls->read_buf_pos += copy;
            ptr += copy;
            rem -= copy;
            continue;
        }

        tls->read_buf_pos = 0;
        tls->read_buf_len = 0;

        int64_t n = _tls_raw_recv(tls, tls->read_buf, tls->read_buf_cap);
        if (n <= 0) {
            return -1;
        }
        tls->read_buf_len = (size_t)n;
    }
    return 0;
}

static int64_t
_tls_buffered_read(xylem_tls_conn_t* tls, void* buf, size_t len) {
    size_t avail = tls->read_buf_len - tls->read_buf_pos;
    if (avail > 0) {
        size_t copy = avail < len ? avail : len;
        memcpy(buf, tls->read_buf + tls->read_buf_pos, copy);
        tls->read_buf_pos += copy;
        return (int64_t)copy;
    }

    if (len >= tls->read_buf_cap) {
        return _tls_raw_recv(tls, buf, len);
    }

    tls->read_buf_pos = 0;
    tls->read_buf_len = 0;

    int64_t n = _tls_raw_recv(tls, tls->read_buf, tls->read_buf_cap);
    if (n <= 0) {
        return n;
    }
    tls->read_buf_len = (size_t)n;

    size_t copy = (size_t)n < len ? (size_t)n : len;
    memcpy(buf, tls->read_buf, copy);
    tls->read_buf_pos = copy;
    return (int64_t)copy;
}

static int64_t
_tls_recv_fixed(xylem_tls_conn_t* tls, void* buf, size_t len) {
    size_t frame_len = tls->frame_opts.fixed.len;
    if (frame_len > len) {
        tls->err = XYLEM_ERR_UNKNOWN;
        return -1;
    }
    if (_tls_read_exact(tls, buf, frame_len) != 0) {
        return -1;
    }
    return (int64_t)frame_len;
}

static int64_t
_tls_recv_length(xylem_tls_conn_t* tls, void* buf, size_t len) {
    uint8_t  hdr[16];
    uint32_t hdr_sz = tls->frame_opts.length.header_size;

    if (hdr_sz > sizeof(hdr)) {
        tls->err = XYLEM_ERR_UNKNOWN;
        return -1;
    }

    if (_tls_read_exact(tls, hdr, hdr_sz) != 0) {
        return -1;
    }

    uint64_t body_len = 0;
    uint8_t* field    = hdr + tls->frame_opts.length.field_offset;

    if (tls->frame_opts.length.big_endian) {
        for (uint32_t i = 0; i < tls->frame_opts.length.field_size; i++) {
            body_len = (body_len << 8) | field[i];
        }
    } else {
        for (uint32_t i = 0; i < tls->frame_opts.length.field_size; i++) {
            body_len |= (uint64_t)field[i] << (i * 8);
        }
    }

    int64_t adjusted
        = (int64_t)body_len + tls->frame_opts.length.adjustment;
    if (adjusted < 0) {
        tls->err = XYLEM_ERR_UNKNOWN;
        return -1;
    }

    size_t payload_len = (size_t)adjusted;
    if (payload_len > len) {
        tls->err = XYLEM_ERR_UNKNOWN;
        return -1;
    }

    if (payload_len > 0 && _tls_read_exact(tls, buf, payload_len) != 0) {
        return -1;
    }
    return (int64_t)payload_len;
}

static int64_t
_tls_recv_delimiter(xylem_tls_conn_t* tls, void* buf, size_t len) {
    const char* delim     = tls->frame_opts.delimiter.delim;
    size_t      delim_len = tls->frame_opts.delimiter.delim_len;
    if (delim_len == 0) {
        delim_len = strlen(delim);
    }

    char*  dst = (char*)buf;
    size_t pos = 0;

    while (pos < len) {
        size_t avail = tls->read_buf_len - tls->read_buf_pos;
        if (avail == 0) {
            tls->read_buf_pos = 0;
            tls->read_buf_len = 0;
            int64_t n
                = _tls_raw_recv(tls, tls->read_buf, tls->read_buf_cap);
            if (n <= 0) {
                return -1;
            }
            tls->read_buf_len = (size_t)n;
            avail             = (size_t)n;
        }

        char* src = tls->read_buf + tls->read_buf_pos;
        for (size_t i = 0; i < avail && pos < len; i++) {
            dst[pos++] = src[i];
            tls->read_buf_pos++;

            if (pos >= delim_len
                && memcmp(dst + pos - delim_len, delim, delim_len) == 0) {
                pos -= delim_len;
                dst[pos] = '\0';
                return (int64_t)pos;
            }
        }
    }

    tls->err = XYLEM_ERR_UNKNOWN;
    return -1;
}

static int
_tls_send_length(xylem_tls_conn_t* tls, const void* data, size_t len) {
    uint8_t  hdr[16];
    uint32_t hdr_sz = tls->frame_opts.length.header_size;

    if (hdr_sz > sizeof(hdr)) {
        tls->err = XYLEM_ERR_UNKNOWN;
        return -1;
    }

    int64_t wire_len = (int64_t)len - tls->frame_opts.length.adjustment;
    if (wire_len < 0) {
        tls->err = XYLEM_ERR_UNKNOWN;
        return -1;
    }

    memset(hdr, 0, hdr_sz);
    uint8_t* field = hdr + tls->frame_opts.length.field_offset;
    uint64_t val   = (uint64_t)wire_len;

    if (tls->frame_opts.length.big_endian) {
        for (int32_t i = (int32_t)tls->frame_opts.length.field_size - 1;
             i >= 0;
             i--) {
            field[i] = (uint8_t)(val & 0xFF);
            val >>= 8;
        }
    } else {
        for (uint32_t i = 0; i < tls->frame_opts.length.field_size; i++) {
            field[i] = (uint8_t)(val & 0xFF);
            val >>= 8;
        }
    }

    if (_tls_raw_send(tls, hdr, hdr_sz) != 0) {
        return -1;
    }
    return _tls_raw_send(tls, data, len);
}

/* --- public recv / send --- */

void xylem_tls_set_framing(
    xylem_tls_conn_t* tls, xylem_tcp_frame_opts_t* opts) {
    if (opts) {
        tls->frame_opts = *opts;
    } else {
        memset(&tls->frame_opts, 0, sizeof(tls->frame_opts));
    }
}

int64_t
xylem_tls_recv(xylem_tls_conn_t* tls, void* buf, size_t len) {
    switch (tls->frame_opts.type) {
    case XYLEM_TCP_FRAME_NONE:
        return _tls_buffered_read(tls, buf, len);
    case XYLEM_TCP_FRAME_FIXED:
        return _tls_recv_fixed(tls, buf, len);
    case XYLEM_TCP_FRAME_LENGTH:
        return _tls_recv_length(tls, buf, len);
    case XYLEM_TCP_FRAME_DELIMITER:
        return _tls_recv_delimiter(tls, buf, len);
    default:
        return -1;
    }
}

int xylem_tls_send(xylem_tls_conn_t* tls, const void* data, size_t len) {
    switch (tls->frame_opts.type) {
    case XYLEM_TCP_FRAME_LENGTH:
        return _tls_send_length(tls, data, len);
    default:
        return _tls_raw_send(tls, data, len);
    }
}

/* --- deadline --- */

void xylem_tls_set_read_deadline(
    xylem_tls_conn_t* tls, uint64_t deadline_ms) {
    iowait_set_rd_deadline(tls->waiter, deadline_ms);
}

void xylem_tls_set_write_deadline(
    xylem_tls_conn_t* tls, uint64_t deadline_ms) {
    iowait_set_wr_deadline(tls->waiter, deadline_ms);
}

/* --- info queries --- */

xylem_err_t xylem_tls_get_error(xylem_tls_conn_t* tls) {
    return tls->err;
}

int xylem_tls_remote_addr(
    xylem_tls_conn_t* tls,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    return addr_ntop(&tls->peer_addr, host, host_len, port);
}

int xylem_tls_local_addr(
    xylem_tls_conn_t* tls,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    addr_t addr;
    socklen_t alen = sizeof(addr.storage);
    if (getsockname(tls->fd, (struct sockaddr*)&addr.storage, &alen) != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}

int xylem_tls_listener_addr(
    xylem_tls_listener_t* ln,
    char*                 host,
    size_t                host_len,
    uint16_t*             port) {
    addr_t addr;
    socklen_t alen = sizeof(addr.storage);
    if (getsockname(ln->fd, (struct sockaddr*)&addr.storage, &alen) != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}

const char* xylem_tls_get_alpn(xylem_tls_conn_t* tls) {
    return tls->alpn[0] ? tls->alpn : NULL;
}
```

- [ ] **Step 2: Build**

Run: `cmake --build build --target xylem`
Expected: Clean build. The library now compiles with the complete new TLS implementation.

- [ ] **Step 3: Commit**

```bash
git add src/net/xylem-tls.c
git commit -m "feat(tls): implement framing, recv, send, deadline, info queries"
```

---

### Task 7: Rewrite Tests

**Files:**
- Rewrite: `tests/test-tls.c`

The tests follow the TCP test pattern: `xylem_run(_main, NULL, NULL)` with spawned server/client coroutines, `xylem_channel` for synchronization, `xylem_waitgroup` for join, `xylem_timer_after` for watchdog.

- [ ] **Step 1: Replace tests/test-tls.c entirely**

Replace `tests/test-tls.c` with (keep existing copyright header):

```c
#include "xylem.h"
#include "xylem/net/xylem-tls.h"
#include "assert.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <string.h>

#define TLS_HOST          "127.0.0.1"
#define TLS_PORT          14433
#define SAFETY_TIMEOUT_MS 10000

typedef struct {
    xylem_channel_t*      ready;
    xylem_waitgroup_t*    wg;
    xylem_tls_ctx_t*      srv_ctx;
    xylem_tls_ctx_t*      cli_ctx;
    uint16_t              port;
} _ctx_t;

static void _watchdog_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    (void)ud;
    ASSERT(0 && "test timed out");
}

/* --- PEM generation helpers (unchanged) --- */

static int _write_pem_to_file(const char* path,
                              int (*write_fn)(BIO*, void*),
                              void* obj) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) {
        return -1;
    }
    if (write_fn(bio, obj) != 1) {
        BIO_free(bio);
        return -1;
    }
    char* data = NULL;
    long  len  = BIO_get_mem_data(bio, &data);
    FILE* f    = fopen(path, "wb");
    if (!f) {
        BIO_free(bio);
        return -1;
    }
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
    BIO_free(bio);
    return 0;
}

static int _write_cert_pem(BIO* bio, void* obj) {
    return PEM_write_bio_X509(bio, (X509*)obj);
}

static int _write_key_pem(BIO* bio, void* obj) {
    return PEM_write_bio_PrivateKey(bio, (EVP_PKEY*)obj,
                                    NULL, NULL, 0, NULL, NULL);
}

static int _gen_self_signed(const char* cert_path, const char* key_path) {
    EVP_PKEY* pkey = EVP_PKEY_new();
    if (!pkey) {
        return -1;
    }
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!pctx) {
        EVP_PKEY_free(pkey);
        return -1;
    }
    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048);
    EVP_PKEY_keygen(pctx, &pkey);
    EVP_PKEY_CTX_free(pctx);

    X509* x509 = X509_new();
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 3600);
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char*)"localhost", -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());

    int rc = 0;
    if (_write_pem_to_file(cert_path, _write_cert_pem, x509) != 0) {
        rc = -1;
    }
    if (rc == 0 && _write_pem_to_file(key_path, _write_key_pem, pkey) != 0) {
        rc = -1;
    }

    X509_free(x509);
    EVP_PKEY_free(pkey);
    return rc;
}

/* --- test: ctx create/destroy --- */

static void test_ctx_create_destroy(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    xylem_tls_ctx_destroy(ctx);
}

/* --- test: load cert valid/invalid --- */

static void test_load_cert_valid(void) {
    const char* cert = "test_tls_cert.pem";
    const char* key  = "test_tls_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx, cert, key) == 0);
    xylem_tls_ctx_destroy(ctx);
    remove(cert);
    remove(key);
}

static void test_load_cert_invalid(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx, "nonexistent.pem",
                                   "nonexistent.pem") == -1);
    xylem_tls_ctx_destroy(ctx);
}

/* --- test: set ca --- */

static void test_set_ca(void) {
    const char* cert = "test_tls_ca.pem";
    const char* key  = "test_tls_ca_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_set_ca(ctx, cert) == 0);
    xylem_tls_ctx_destroy(ctx);
    remove(cert);
    remove(key);
}

/* --- test: set verify --- */

static void test_set_verify(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    xylem_tls_ctx_set_verify(ctx, true);
    xylem_tls_ctx_set_verify(ctx, false);
    xylem_tls_ctx_destroy(ctx);
}

/* --- test: set alpn --- */

static void test_set_alpn(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    const char* protos[] = {"h2", "http/1.1"};
    ASSERT(xylem_tls_ctx_set_alpn(ctx, protos, 2) == 0);
    xylem_tls_ctx_destroy(ctx);
}

/* --- test: handshake + echo --- */

static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    char buf[256];
    int64_t n = xylem_tls_recv(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_tls_send(conn, buf, (size_t)n) == 0);

    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    const char* msg = "hello xylem tls";
    ASSERT(xylem_tls_send(conn, msg, strlen(msg)) == 0);

    char buf[64];
    int64_t n = xylem_tls_recv(conn, buf, sizeof(buf));
    ASSERT(n == (int64_t)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_echo_cert.pem";
    const char* key  = "test_tls_echo_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_echo_server, &ctx);
    xylem_spawn(_echo_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_handshake_and_echo(void) {
    xylem_run(_echo_main, NULL, NULL);
}

/* --- test: handshake failure (wrong CA) --- */

static void _fail_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn == NULL);
    xylem_waitgroup_done(ctx->wg);
}

static void _fail_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    /* Accept will fail because client handshake fails. */
    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    /* May or may not be NULL depending on timing. */
    if (conn) {
        xylem_tls_close(conn);
    }

    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _fail_main(void* arg) {
    (void)arg;
    const char* cert  = "test_tls_fail_cert.pem";
    const char* key   = "test_tls_fail_key.pem";
    const char* cert2 = "test_tls_fail_cert2.pem";
    const char* key2  = "test_tls_fail_key2.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);
    ASSERT(_gen_self_signed(cert2, key2) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, true);
    ASSERT(xylem_tls_ctx_set_ca(cli_ctx, cert2) == 0);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 1,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_fail_server, &ctx);
    xylem_spawn(_fail_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    remove(cert2);
    remove(key2);
    xylem_shutdown();
}

static void test_handshake_failure(void) {
    xylem_run(_fail_main, NULL, NULL);
}

/* --- test: ALPN negotiation --- */

static void _alpn_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    const char* alpn = xylem_tls_get_alpn(conn);
    ASSERT(alpn != NULL);
    ASSERT(strcmp(alpn, "h2") == 0);

    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _alpn_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    const char* alpn = xylem_tls_get_alpn(conn);
    ASSERT(alpn != NULL);
    ASSERT(strcmp(alpn, "h2") == 0);

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _alpn_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_alpn_cert.pem";
    const char* key  = "test_tls_alpn_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    const char* protos[] = {"h2", "http/1.1"};

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);
    ASSERT(xylem_tls_ctx_set_alpn(srv_ctx, protos, 2) == 0);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, false);
    ASSERT(xylem_tls_ctx_set_alpn(cli_ctx, protos, 2) == 0);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 2,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_alpn_server, &ctx);
    xylem_spawn(_alpn_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_alpn_negotiation(void) {
    xylem_run(_alpn_main, NULL, NULL);
}

/* --- test: framing (length-prefix) --- */

static const xylem_tcp_frame_opts_t _len_frame = {
    .type   = XYLEM_TCP_FRAME_LENGTH,
    .length = {
        .header_size  = 2,
        .field_offset = 0,
        .field_size   = 2,
        .adjustment   = 0,
        .big_endian   = true,
    },
};

static void _frame_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    xylem_tcp_frame_opts_t frame = _len_frame;
    xylem_tls_set_framing(conn, &frame);
    ASSERT(xylem_tls_send(conn, "FRAME1", 6) == 0);

    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _frame_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_tcp_frame_opts_t frame = _len_frame;
    xylem_tls_set_framing(conn, &frame);

    char buf[64];
    int64_t n = xylem_tls_recv(conn, buf, sizeof(buf));
    ASSERT(n == 6);
    ASSERT(memcmp(buf, "FRAME1", 6) == 0);

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _frame_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_frame_cert.pem";
    const char* key  = "test_tls_frame_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 3,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_frame_server, &ctx);
    xylem_spawn(_frame_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_framing(void) {
    xylem_run(_frame_main, NULL, NULL);
}

/* --- test: read deadline --- */

static void _deadline_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    /* Hold connection open, send nothing. */
    xylem_sleep(2000);
    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _deadline_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 100;
    xylem_tls_set_read_deadline(conn, deadline);

    char buf[64];
    int64_t n = xylem_tls_recv(conn, buf, sizeof(buf));
    ASSERT(n == -1);
    ASSERT(xylem_tls_get_error(conn) == XYLEM_ERR_TIMEOUT);

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _deadline_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_dl_cert.pem";
    const char* key  = "test_tls_dl_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 4,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_deadline_server, &ctx);
    xylem_spawn(_deadline_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_read_deadline(void) {
    xylem_run(_deadline_main, NULL, NULL);
}

/* --- test: send after close --- */

static void _sac_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    if (conn) {
        xylem_sleep(500);
        xylem_tls_close(conn);
    }
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _sac_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_tls_close(conn);
    /* conn is freed — cannot use after close. */

    xylem_waitgroup_done(ctx->wg);
}

static void _sac_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_sac_cert.pem";
    const char* key  = "test_tls_sac_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 5,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_sac_server, &ctx);
    xylem_spawn(_sac_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_close(void) {
    xylem_run(_sac_main, NULL, NULL);
}

/* --- test: close listener --- */

static void _cl_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ln);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn == NULL);

    xylem_waitgroup_done(ctx->wg);
}

static void _cl_closer(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        (xylem_tls_listener_t*)xylem_channel_recv(ctx->ready);
    xylem_sleep(100);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cl_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_cl_cert.pem";
    const char* key  = "test_tls_cl_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .port    = TLS_PORT + 6,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_cl_server, &ctx);
    xylem_spawn(_cl_closer, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_close_listener(void) {
    xylem_run(_cl_main, NULL, NULL);
}

/* --- test: keylog --- */

static void _kl_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    if (conn) {
        xylem_tls_close(conn);
    }
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _kl_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);
    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _kl_main(void* arg) {
    (void)arg;
    const char* cert   = "test_tls_kl_cert.pem";
    const char* key    = "test_tls_kl_key.pem";
    const char* keylog = "test_keylog.txt";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, false);
    ASSERT(xylem_tls_ctx_set_keylog(cli_ctx, keylog) == 0);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 7,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_kl_server, &ctx);
    xylem_spawn(_kl_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    FILE* f = fopen(keylog, "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    ASSERT(sz > 0);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    remove(keylog);
    xylem_shutdown();
}

static void test_keylog(void) {
    xylem_run(_kl_main, NULL, NULL);
}

/* --- main --- */

int main(void) {
    test_ctx_create_destroy();
    test_load_cert_valid();
    test_load_cert_invalid();
    test_set_ca();
    test_set_verify();
    test_set_alpn();
    test_handshake_and_echo();
    test_handshake_failure();
    test_alpn_negotiation();
    test_framing();
    test_read_deadline();
    test_close();
    test_close_listener();
    test_keylog();
    return 0;
}
```

- [ ] **Step 2: Build and run tests**

Run: `cmake --build build --target test-tls && ctest --test-dir build -R test-tls --output-on-failure`
Expected: All 14 tests pass.

- [ ] **Step 3: Commit**

```bash
git add tests/test-tls.c
git commit -m "test(tls): rewrite all tests for coroutine API"
```

---

### Task 8: Update Design Doc

**Files:**
- Rewrite: `docs/tls-design.md`

- [ ] **Step 1: Replace the design doc**

Replace `docs/tls-design.md` with the content from the spec document `docs/superpowers/specs/2026-05-17-tls-coroutine-design.md`, trimming the test plan and file change list sections (those are implementation artifacts, not design).

- [ ] **Step 2: Commit**

```bash
git add docs/tls-design.md
git commit -m "docs(tls): update design doc for coroutine architecture"
```

---

### Task 9: Final Build + Full Test Suite

- [ ] **Step 1: Clean build**

Run: `cmake --build build --clean-first`
Expected: Clean build, no warnings.

- [ ] **Step 2: Run full test suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: All tests pass (including TLS tests).

- [ ] **Step 3: Run TLS tests with sanitizers (if available)**

Run: `cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DXYLEM_ENABLE_ASAN=ON -DXYLEM_ENABLE_TLS=ON .. && cmake --build build-asan --target test-tls && ctest --test-dir build-asan -R test-tls --output-on-failure`
Expected: No sanitizer errors.
