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

#include "xylem/net/xylem-dtls.h"
#include "xylem/net/xylem-tls.h"

#include "net/tls/tls-backend.h"

#include <stdbool.h>
#include <stddef.h>

typedef xylem_tls_ctx_t  tls_ctx_t;
typedef xylem_dtls_ctx_t dtls_ctx_t;

/**
 * @brief Create a TLS engine context.
 *
 * @return Context handle, or NULL on failure.
 */
extern tls_ctx_t* tls_ctx_create(void);

/**
 * @brief Destroy a TLS engine context. NULL-safe.
 *
 * @param ctx  Context handle.
 */
extern void       tls_ctx_destroy(tls_ctx_t* ctx);

/**
 * @brief Enable NSS key-log output for Wireshark decryption.
 *
 * @param ctx   Context handle.
 * @param path  Output file path, or NULL to disable.
 *
 * @return 0 on success, -1 on failure.
 */
extern int        tls_ctx_set_keylog(tls_ctx_t* ctx, const char* path);

/**
 * @brief Load a PEM certificate chain and private key from files.
 *
 * When hostname is NULL the identity becomes the ctx default; otherwise
 * it is registered for SNI selection on that hostname.
 *
 * @param ctx       Context handle.
 * @param hostname  SNI hostname, or NULL for the default identity.
 * @param cert      Path to the PEM certificate chain (leaf first).
 * @param key       Path to the PEM private key.
 *
 * @return 0 on success, -1 on failure.
 */
extern int tls_ctx_load_cert(
    tls_ctx_t*  ctx,
    const char* hostname,
    const char* cert,
    const char* key);

/**
 * @brief Load a PEM certificate chain and private key from memory.
 *
 * @param ctx       Context handle.
 * @param hostname  SNI hostname, or NULL for the default identity.
 * @param cert_pem  PEM certificate chain bytes (leaf first).
 * @param cert_len  Length of cert_pem in bytes.
 * @param key_pem   PEM private key bytes.
 * @param key_len   Length of key_pem in bytes.
 *
 * @return 0 on success, -1 on failure.
 */
extern int tls_ctx_load_cert_mem(
    tls_ctx_t*  ctx,
    const char* hostname,
    const void* cert_pem,
    size_t      cert_len,
    const void* key_pem,
    size_t      key_len);

/**
 * @brief Add a CA certificate file to the trust store.
 *
 * @param ctx      Context handle.
 * @param ca_file  Path to a PEM CA certificate (or bundle) file.
 *
 * @return 0 on success, -1 on failure.
 */
extern int tls_ctx_load_ca(tls_ctx_t* ctx, const char* ca_file);

/**
 * @brief Load public-CA trust anchors: the system store plus a fallback.
 *
 * @param ctx               Context handle.
 * @param fallback_ca_file  PEM CA bundle path, or NULL for none.
 *
 * @return 0 if at least one source loaded, -1 if none did.
 */
extern int tls_ctx_load_system_ca(
    tls_ctx_t*  ctx,
    const char* fallback_ca_file);

/**
 * @brief Set whether a client verifies the server certificate.
 *
 * @param ctx     Context handle.
 * @param enable  true to verify (default), false to skip.
 */
extern void tls_ctx_verify_server(tls_ctx_t* ctx, bool enable);

/**
 * @brief Set whether a server requires a client certificate (mTLS).
 *
 * @param ctx     Context handle.
 * @param enable  true to require and verify, false to request none.
 */
extern void tls_ctx_verify_client(tls_ctx_t* ctx, bool enable);

/**
 * @brief Set the ALPN protocol list.
 *
 * @param ctx        Context handle.
 * @param protocols  Array of protocol strings (e.g. "h2", "http/1.1").
 * @param count      Number of protocols.
 *
 * @return 0 on success, -1 on failure.
 */
extern int tls_ctx_set_alpn(
    tls_ctx_t*   ctx,
    const char** protocols,
    size_t       count);

/**
 * @brief Create a DTLS engine context.
 *
 * @return Context handle, or NULL on failure.
 */
extern dtls_ctx_t* dtls_ctx_create(void);

/**
 * @brief Destroy a DTLS engine context. NULL-safe.
 *
 * @param ctx  Context handle.
 */
extern void dtls_ctx_destroy(dtls_ctx_t* ctx);

/**
 * @brief Enable NSS key-log output for a DTLS context.
 *
 * @param ctx   Context handle.
 * @param path  Output file path, or NULL to disable.
 *
 * @return 0 on success, -1 on failure.
 */
extern int dtls_ctx_set_keylog(dtls_ctx_t* ctx, const char* path);

/**
 * @brief Load a DTLS certificate chain and key from files.
 *
 * @param ctx       Context handle.
 * @param hostname  SNI hostname, or NULL for the default identity.
 * @param cert      Path to the PEM certificate chain.
 * @param key       Path to the PEM private key.
 *
 * @return 0 on success, -1 on failure.
 */
extern int dtls_ctx_load_cert(
    dtls_ctx_t* ctx,
    const char* hostname,
    const char* cert,
    const char* key);

/**
 * @brief Load a DTLS certificate chain and key from memory.
 *
 * @param ctx       Context handle.
 * @param hostname  SNI hostname, or NULL for the default identity.
 * @param cert_pem  PEM certificate chain bytes.
 * @param cert_len  Length of cert_pem in bytes.
 * @param key_pem   PEM private key bytes.
 * @param key_len   Length of key_pem in bytes.
 *
 * @return 0 on success, -1 on failure.
 */
extern int dtls_ctx_load_cert_mem(
    dtls_ctx_t* ctx,
    const char* hostname,
    const void* cert_pem,
    size_t      cert_len,
    const void* key_pem,
    size_t      key_len);

/**
 * @brief Add a CA certificate file to a DTLS context.
 *
 * @param ctx      Context handle.
 * @param ca_file  Path to a PEM CA certificate or bundle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int dtls_ctx_load_ca(dtls_ctx_t* ctx, const char* ca_file);

/**
 * @brief Load system and fallback trust anchors for a DTLS context.
 *
 * @param ctx               Context handle.
 * @param fallback_ca_file  PEM CA bundle path, or NULL for none.
 *
 * @return 0 if at least one source loaded, -1 otherwise.
 */
extern int dtls_ctx_load_system_ca(
    dtls_ctx_t* ctx,
    const char* fallback_ca_file);

/**
 * @brief Set whether a DTLS client verifies the server certificate.
 *
 * @param ctx     Context handle.
 * @param enable  true to verify, false to skip.
 */
extern void dtls_ctx_verify_server(dtls_ctx_t* ctx, bool enable);

/**
 * @brief Set whether a DTLS server requires a client certificate.
 *
 * @param ctx     Context handle.
 * @param enable  true to require and verify, false to request none.
 */
extern void dtls_ctx_verify_client(dtls_ctx_t* ctx, bool enable);

/**
 * @brief Set the DTLS ALPN protocol list.
 *
 * @param ctx        Context handle.
 * @param protocols  Array of protocol strings.
 * @param count      Number of protocols.
 *
 * @return 0 on success, -1 on failure.
 */
extern int dtls_ctx_set_alpn(
    dtls_ctx_t* ctx,
    const char** protocols,
    size_t       count);

/**
 * @brief Get the backend context owned by a TLS context.
 *
 * @param ctx  TLS context.
 *
 * @return Backend context.
 */
extern tls_backend_ctx_t* tls_ctx_get_backend(tls_ctx_t* ctx);

/**
 * @brief Get the backend context owned by a DTLS context.
 *
 * @param ctx  DTLS context.
 *
 * @return Backend context.
 */
extern tls_backend_ctx_t* dtls_ctx_get_backend(dtls_ctx_t* ctx);

/**
 * @brief Build a TLS client handshake configuration.
 *
 * @param ctx       TLS context.
 * @param identity  Peer identity, or NULL.
 * @param module    Log module name.
 * @param cfg       Output handshake configuration.
 *
 * @return 0 on success, -1 on failure.
 */
extern int tls_ctx_build_client_config(
    tls_ctx_t*                   ctx,
    const char*                  identity,
    const char*                  module,
    tls_backend_handshake_cfg_t* cfg);

/**
 * @brief Build a DTLS client handshake configuration.
 *
 * @param ctx       DTLS context.
 * @param identity  Peer identity, or NULL.
 * @param module    Log module name.
 * @param cfg       Output handshake configuration.
 *
 * @return 0 on success, -1 on failure.
 */
extern int dtls_ctx_build_client_config(
    dtls_ctx_t*                  ctx,
    const char*                  identity,
    const char*                  module,
    tls_backend_handshake_cfg_t* cfg);

/**
 * @brief Build a TLS server handshake configuration.
 *
 * @param ctx  TLS context.
 * @param cfg  Output handshake configuration.
 */
extern void tls_ctx_build_server_config(
    tls_ctx_t*                   ctx,
    tls_backend_handshake_cfg_t* cfg);

/**
 * @brief Build a DTLS server handshake configuration.
 *
 * @param ctx  DTLS context.
 * @param cfg  Output handshake configuration.
 */
extern void dtls_ctx_build_server_config(
    dtls_ctx_t*                  ctx,
    tls_backend_handshake_cfg_t* cfg);