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

#include "xylem/net/xylem-ws.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_HANDSHAKE_MAX_BYTES 16384

struct ws_transport_s;

typedef struct ws_handshake_result_s {
    char   pending[WS_HANDSHAKE_MAX_BYTES];
    size_t pending_len;
    bool   deflate;
    bool   no_context_takeover;
} ws_handshake_result_t;

/**
 * The core passes this back unchanged after preparing connection resources,
 * so sending 101 cannot precede receive-buffer or compression initialization.
 */
typedef struct ws_handshake_reply_s {
    char     data[512];
    size_t   len;
    uint64_t deadline;
} ws_handshake_reply_t;

/**
 * @brief Compute the RFC 6455 Sec-WebSocket-Accept value.
 *
 * @param key       Null-terminated Sec-WebSocket-Key.
 * @param out       Destination for the terminated Accept value.
 * @param out_size  Destination capacity, at least 29 bytes.
 *
 * @return 0 on success, -1 on invalid input or allocation failure.
 */
extern int
ws_handshake_compute_accept(const char* key, char* out, size_t out_size);

/**
 * @brief Validate a peer's Sec-WebSocket-Accept value.
 *
 * @param expected_key  Key sent in the client request.
 * @param accept_value  Accept value received from the peer.
 *
 * @return 0 on a match, -1 on invalid input, mismatch or allocation failure.
 */
extern int ws_handshake_validate_accept(
    const char* expected_key,
    const char* accept_value);

/**
 * @brief Complete a client Upgrade exchange within one deadline.
 *
 * @param transport  Borrowed transport, never closed or destroyed here.
 * @param host       Target host for the request.
 * @param port       Target port.
 * @param path       Request path and optional query.
 * @param opts       Handshake options, or NULL for defaults.
 * @param result     Negotiated settings and frame bytes, valid on success.
 *
 * @return 0 on success, -1 on failure.
 * @note Success clears transport deadlines. The caller owns failure teardown.
 */
extern int ws_handshake_client(
    const struct ws_transport_s* transport,
    const char*                  host,
    uint16_t                     port,
    const char*                  path,
    const xylem_ws_opts_t*       opts,
    ws_handshake_result_t*       result);

/**
 * @brief Validate a client Upgrade request and prepare its response.
 *
 * @param transport  Borrowed transport, never closed or destroyed here.
 * @param opts       Handshake options, or NULL for defaults.
 * @param result     Negotiated settings and frame bytes, valid on success.
 * @param reply      Response and original deadline, valid on success.
 *
 * @return 0 on success, -1 on failure.
 * @note No response is sent; deadlines remain armed during core setup.
 */
extern int ws_handshake_server_prepare(
    const struct ws_transport_s* transport,
    const xylem_ws_opts_t*       opts,
    ws_handshake_result_t*       result,
    ws_handshake_reply_t*        reply);

/**
 * @brief Send the prepared 101 response within the original deadline.
 *
 * @param transport  Borrowed transport, never closed or destroyed here.
 * @param reply      Unchanged output of a successful prepare operation.
 *
 * @return 0 on success, -1 on failure.
 * @note Call after core setup. Success clears transport deadlines.
 */
extern int ws_handshake_server_finish(
    const struct ws_transport_s* transport,
    const ws_handshake_reply_t*  reply);
