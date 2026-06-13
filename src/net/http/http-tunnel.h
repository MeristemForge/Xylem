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

/**
 * HTTP CONNECT tunnel (transport-agnostic).
 *
 * Establishes a TCP tunnel to a target host through an HTTP proxy via the
 * CONNECT method, returning a raw socket that talks end-to-end with the
 * target. Used by any transport that must traverse a proxy opaquely
 * (TLS today; usable by plain HTTP too). Plain HTTP proxying normally
 * uses absolute-form requests instead and does not need this.
 *
 * Not part of the public API.
 */

_Pragma("once")

#include "platform/platform-socket.h"

#include <stdint.h>

/**
 * @brief Connect to an HTTP CONNECT proxy and establish a tunnel.
 *
 * Dials the proxy, sends a CONNECT request, and waits for 200.
 * On success the returned fd is a transparent tunnel to the target.
 *
 * @param proxy_host   Proxy hostname or IP.
 * @param proxy_port   Proxy port.
 * @param target_host  Destination hostname.
 * @param target_port  Destination port.
 * @param timeout_ms   Connect timeout in ms, 0 = no timeout.
 * @param username     Proxy username, or NULL for no auth.
 * @param password     Proxy password, or NULL for no auth.
 *
 * @return Tunneled socket fd, or PLATFORM_SO_ERROR_INVALID_SOCKET on failure.
 */
extern platform_sock_t http_tunnel_connect(const char* proxy_host,
                                           uint16_t proxy_port,
                                           const char* target_host,
                                           uint16_t target_port,
                                           uint64_t timeout_ms,
                                           const char* username,
                                           const char* password);
