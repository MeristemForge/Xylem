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

#include <stddef.h>
#include <stdint.h>

#define ADDR_TEXT_MAX 64

typedef struct addr_s {
    struct sockaddr_storage storage;
} addr_t;

/**
 * @brief Parse a numeric IP address string into an addr_t.
 *
 * IPv6 addresses may include a decimal scope ID, such as fe80::1%3.
 * Does not perform DNS resolution.
 *
 * @param src   Numeric IP string (IPv4 or IPv6).
 * @param port  Port number to embed in the result.
 * @param dst   Destination addr_t.
 *
 * @return 0 on success, -1 on invalid input.
 */
extern int addr_pton(const char* src, uint16_t port, addr_t* dst);

/**
 * @brief Return the socket address length for an addr_t.
 *
 * @param addr  Source addr_t.
 *
 * @return Socket address length, or 0 for NULL or an unsupported family.
 */
extern socklen_t addr_socklen(const addr_t* addr);

/**
 * @brief Convert an addr_t to a printable IP string and port.
 *
 * Either dst or port may be NULL to skip that output. If both are
 * NULL the call is a no-op that only validates the address family.
 *
 * @param addr     Source addr_t.
 * @param dst      Destination buffer for IP string, or NULL.
 * @param dst_len  Size of dst buffer (64 bytes recommended).
 *                 Must be large enough to hold the textual address;
 *                 ignored when dst is NULL.
 * @param port     Receives the port number, or NULL.
 *
 * @return 0 on success, -1 on error.
 */
extern int addr_ntop(
    const addr_t* addr,
    char*         dst,
    size_t        dst_len,
    uint16_t*     port);

/**
 * @brief Resolve a domain name to all addresses, with a timeout.
 *
 * Offloads the blocking getaddrinfo to the runtime thread pool and
 * yields the calling coroutine until the result is ready or the
 * timeout elapses. The result array is heap-allocated; the caller
 * must free *addrs when done (only valid on a 0 return).
 *
 * The supplied port is embedded into every returned sockaddr so
 * callers can use the result directly with bind/connect/sendto.
 *
 * Timeout semantics mirror Go's cgo resolver and Tokio's default
 * (spawn_blocking) resolver: getaddrinfo cannot be cancelled, so on
 * timeout the coroutine resumes with an error while the pool thread
 * keeps running the lookup in the background; its result is discarded
 * and its resources are released once it finally returns. Pass 0 to
 * wait indefinitely (no timeout).
 *
 * @param domain      Domain name to resolve.
 * @param port        Port to embed in each result.
 * @param timeout_ms  Resolve timeout in milliseconds, or 0 for none.
 * @param addrs       Receives a malloc'd array of results (caller frees).
 * @param count       Receives the number of results.
 *
 * @return 0 on success, -1 on failure or timeout.
 */
extern int addr_resolve(
    const char* domain,
    uint16_t    port,
    uint64_t    timeout_ms,
    addr_t**    addrs,
    size_t*     count);

/**
 * @brief Convert a numeric IP or resolve a hostname to addresses.
 *
 * Numeric IP addresses are converted directly without using the runtime
 * thread pool. Hostnames are resolved through addr_resolve(). The result
 * array is heap-allocated and must be freed by the caller.
 *
 * @param host        Numeric IP address or hostname.
 * @param port        Port to embed in each result.
 * @param timeout_ms  DNS timeout in milliseconds, or 0 for none.
 * @param addrs       Receives a malloc'd array of results.
 * @param count       Receives the number of results.
 *
 * @return 0 on success, -1 on invalid input, allocation failure, DNS failure,
 *         or timeout.
 */
extern int addr_lookup(
    const char* host,
    uint16_t    port,
    uint64_t    timeout_ms,
    addr_t**    addrs,
    size_t*     count);
