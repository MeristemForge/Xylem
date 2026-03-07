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

#include "platform/platform-socket.h"

/**
 * @brief Unified network address wrapper.
 *
 * Encapsulates a sockaddr_storage so callers need not distinguish
 * between IPv4 and IPv6 at the API level.
 */
typedef struct xylem_addr_s {
    struct sockaddr_storage storage;
} xylem_addr_t;

/**
 * @brief Simple buffer descriptor.
 *
 * Lightweight {base, len} pair used for scatter/gather and
 * zero-copy patterns.
 */
typedef struct xylem_buf_s {
    char*  base;
    size_t len;
} xylem_buf_t;

/**
 * @brief Convert a host string and port into an xylem_addr_t.
 *
 * Supports both IPv4 and IPv6 address strings. Uses inet_pton
 * internally, trying AF_INET first, then AF_INET6.
 *
 * @param host  Host address string (e.g. "127.0.0.1", "::1").
 * @param port  Port number.
 * @param addr  Output address structure.
 *
 * @return 0 on success, -1 on failure (invalid address).
 */
extern int xylem_addr_pton(const char* host, uint16_t port, xylem_addr_t* addr);

/**
 * @brief Convert an xylem_addr_t into a host string and port.
 *
 * Extracts the human-readable address and port from the internal
 * sockaddr_storage. Uses inet_ntop internally.
 *
 * @param addr     Input address structure.
 * @param host     Output host string buffer.
 * @param hostlen  Size of the host buffer in bytes.
 * @param port     Output port number.
 *
 * @return 0 on success, -1 on failure (unsupported address family).
 */
extern int xylem_addr_ntop(const xylem_addr_t* addr,
                           char* host, size_t hostlen, uint16_t* port);
