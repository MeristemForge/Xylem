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

#include <stddef.h>
#include <stdint.h>

typedef xylem_dtls_ctx_t      dtls_ctx_t;
typedef xylem_dtls_conn_t     dtls_conn_t;
typedef xylem_dtls_listener_t dtls_listener_t;

/**
 * @brief Dial and handshake a DTLS connection.
 *
 * @param host  Remote host.
 * @param port  Remote port.
 * @param ctx   Shared TLS-family context.
 * @param opts  DTLS options, or NULL for defaults.
 *
 * @return Connection handle, or NULL on failure.
 */
extern dtls_conn_t* dtls_dial(
    const char*        host,
    uint16_t           port,
    dtls_ctx_t*        ctx,
    xylem_dtls_opts_t* opts);

/**
 * @brief Create a DTLS listener.
 *
 * @param host  Local host.
 * @param port  Local port.
 * @param ctx   Shared TLS-family context.
 * @param opts  DTLS options, or NULL for defaults.
 *
 * @return Listener handle, or NULL on failure.
 */
extern dtls_listener_t* dtls_listen(
    const char*        host,
    uint16_t           port,
    dtls_ctx_t*        ctx,
    xylem_dtls_opts_t* opts);

/**
 * @brief Accept a handshaked DTLS connection.
 *
 * @param ln  Listener handle.
 *
 * @return Connection handle, or NULL when the listener is closed.
 */
extern dtls_conn_t* dtls_accept(dtls_listener_t* ln);

/**
 * @brief Read one DTLS application datagram.
 *
 * @param dtls  Connection handle.
 * @param buf   Destination buffer.
 * @param len   Destination capacity.
 *
 * @return Bytes read, 0 on peer close, or -1 on error.
 */
extern int dtls_read(dtls_conn_t* dtls, void* buf, int len);

/**
 * @brief Write one DTLS application datagram.
 *
 * @param dtls  Connection handle.
 * @param data  Datagram payload.
 * @param len   Payload length.
 *
 * @return 0 on success, or -1 on error.
 */
extern int dtls_write(dtls_conn_t* dtls, const void* data, int len);

/**
 * @brief Close a DTLS connection.
 *
 * @param dtls  Connection handle.
 */
extern void dtls_close(dtls_conn_t* dtls);

/**
 * @brief Destroy a DTLS connection. NULL-safe.
 *
 * Closes the connection if needed, then releases the owner reference.
 * Internal references may defer the actual free.
 *
 * @param dtls  Connection handle, or NULL.
 */
extern void dtls_destroy(dtls_conn_t* dtls);

/**
 * @brief Close a DTLS listener.
 *
 * @param ln  Listener handle.
 */
extern void dtls_close_listener(dtls_listener_t* ln);

/**
 * @brief Destroy a DTLS listener. NULL-safe.
 *
 * Closes the listener if needed, then releases the owner reference.
 * Internal references may defer the actual free.
 *
 * @param ln  Listener handle, or NULL.
 */
extern void dtls_destroy_listener(dtls_listener_t* ln);

/**
 * @brief Set the absolute read deadline.
 *
 * @param dtls         Connection handle.
 * @param deadline_ms  Absolute deadline in milliseconds, or 0 to clear.
 */
extern void dtls_set_read_deadline(
    dtls_conn_t* dtls,
    uint64_t     deadline_ms);

/**
 * @brief Set the absolute write deadline.
 *
 * @param dtls         Connection handle.
 * @param deadline_ms  Absolute deadline in milliseconds, or 0 to clear.
 */
extern void dtls_set_write_deadline(
    dtls_conn_t* dtls,
    uint64_t     deadline_ms);

/**
 * @brief Get the negotiated DTLS ALPN protocol.
 *
 * @param dtls  Connection handle.
 *
 * @return Protocol string, or NULL if none negotiated.
 */
extern const char* dtls_get_alpn(dtls_conn_t* dtls);

/**
 * @brief Get the DTLS peer address.
 *
 * @param dtls      Connection handle.
 * @param host      Buffer to receive the address string.
 * @param host_len  Size of host buffer.
 * @param port      Receives the peer port.
 *
 * @return 0 on success, or -1 on error.
 */
extern int dtls_remote_addr(
    dtls_conn_t* dtls,
    char*        host,
    size_t       host_len,
    uint16_t*    port);

/**
 * @brief Get the DTLS local address.
 *
 * @param dtls      Connection handle.
 * @param host      Buffer to receive the address string.
 * @param host_len  Size of host buffer.
 * @param port      Receives the local port.
 *
 * @return 0 on success, or -1 on error.
 */
extern int dtls_local_addr(
    dtls_conn_t* dtls,
    char*        host,
    size_t       host_len,
    uint16_t*    port);

/**
 * @brief Get the DTLS listener address.
 *
 * @param ln        Listener handle.
 * @param host      Buffer to receive the address string.
 * @param host_len  Size of host buffer.
 * @param port      Receives the local port.
 *
 * @return 0 on success, or -1 on error.
 */
extern int dtls_listener_addr(
    dtls_listener_t* ln,
    char*            host,
    size_t           host_len,
    uint16_t*        port);
