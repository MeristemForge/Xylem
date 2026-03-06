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

#include "xylem/xylem-addr.h"

#include <string.h>

int xylem_addr_pton(const char* host, uint16_t port, xylem_addr_t* addr) {
    if (!host || !addr) return -1;

    memset(&addr->storage, 0, sizeof(addr->storage));

    /* Try IPv4 first */
    {
        struct sockaddr_in* sin = (struct sockaddr_in*)&addr->storage;
        if (inet_pton(AF_INET, host, &sin->sin_addr) == 1) {
            sin->sin_family = AF_INET;
            sin->sin_port   = htons(port);
            return 0;
        }
    }

    /* Try IPv6 */
    {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&addr->storage;
        if (inet_pton(AF_INET6, host, &sin6->sin6_addr) == 1) {
            sin6->sin6_family = AF_INET6;
            sin6->sin6_port   = htons(port);
            return 0;
        }
    }

    return -1;
}

int xylem_addr_ntop(const xylem_addr_t* addr,
                    char* host, size_t hostlen, uint16_t* port) {
    if (!addr || !host || !port) return -1;

    switch (addr->storage.ss_family) {
    case AF_INET: {
        const struct sockaddr_in* sin =
            (const struct sockaddr_in*)&addr->storage;
        if (!inet_ntop(AF_INET, &sin->sin_addr, host, (socklen_t)hostlen))
            return -1;
        *port = ntohs(sin->sin_port);
        return 0;
    }
    case AF_INET6: {
        const struct sockaddr_in6* sin6 =
            (const struct sockaddr_in6*)&addr->storage;
        if (!inet_ntop(AF_INET6, &sin6->sin6_addr, host, (socklen_t)hostlen))
            return -1;
        *port = ntohs(sin6->sin6_port);
        return 0;
    }
    default:
        return -1;
    }
}
