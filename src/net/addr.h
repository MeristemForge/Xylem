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

typedef struct addr_s {
    struct sockaddr_storage storage;
} addr_t;

/**
 * @brief Parse a numeric IP address string into an addr_t.
 *
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
 * @brief Convert an addr_t to a printable IP string and port.
 *
 * @param addr  Source addr_t.
 * @param dst   Destination buffer for IP string.
 * @param size  Size of dst buffer (INET6_ADDRSTRLEN recommended).
 * @param port  Receives the port number.
 *
 * @return 0 on success, -1 on error.
 */
extern int addr_ntop(
    const addr_t* addr,
    char* dst,
    size_t size,
    uint16_t* port);

/**
 * @brief Resolve a domain name to all addresses.
 *
 * Offloads getaddrinfo to the runtime thread pool and yields
 * until the result is ready. The result array is heap-allocated;
 * the caller must free *addrs when done.
 *
 * The supplied port is embedded into every returned sockaddr so
 * callers can use the result directly with bind/connect/sendto.
 *
 * @param domain  Domain name to resolve.
 * @param port    Port to embed in each result.
 * @param addrs   Receives a malloc'd array of results (caller frees).
 * @param count   Receives the number of results.
 *
 * @return 0 on success, -1 on failure.
 */
extern int addr_resolve(
    const char* domain,
    uint16_t port,
    addr_t** addrs,
    size_t* count);
