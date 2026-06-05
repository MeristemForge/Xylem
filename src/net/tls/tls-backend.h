/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */

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
extern int tls_backend_ctx_load_system_ca(tls_backend_ctx_t* ctx,
                                          const char* fallback_ca_file);
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
