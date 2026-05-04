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

#include "xylem.h"
#include "net/addr.h"
#include "assert.h"

#include <string.h>
#include <stdlib.h>

static void test_ipv4_roundtrip(void) {
    addr_t addr;
    char host[64];
    uint16_t port;

    ASSERT(addr_pton("127.0.0.1", 8080, &addr) == 0);
    ASSERT(addr_ntop(&addr, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, "127.0.0.1") == 0);
    ASSERT(port == 8080);
}

static void test_ipv6_roundtrip(void) {
    addr_t addr;
    char host[64];
    uint16_t port;

    ASSERT(addr_pton("::1", 9090, &addr) == 0);
    ASSERT(addr_ntop(&addr, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, "::1") == 0);
    ASSERT(port == 9090);
}

static void test_invalid_address(void) {
    addr_t addr;
    ASSERT(addr_pton("not_an_address", 80, &addr) == -1);
    ASSERT(addr_pton("999.999.999.999", 80, &addr) == -1);
}

static void test_null_params(void) {
    addr_t addr;
    char host[64];
    uint16_t port;

    ASSERT(addr_pton(NULL, 80, &addr) == -1);
    ASSERT(addr_pton("127.0.0.1", 80, NULL) == -1);
    ASSERT(addr_ntop(NULL, host, sizeof(host), &port) == -1);
    ASSERT(addr_ntop(&addr, NULL, 0, &port) == -1);
}

static void test_ipv4_wildcard(void) {
    addr_t addr;
    char host[64];
    uint16_t port;

    ASSERT(addr_pton("0.0.0.0", 0, &addr) == 0);
    ASSERT(addr_ntop(&addr, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, "0.0.0.0") == 0);
    ASSERT(port == 0);
}

static void _resolve_main(void* arg) {
    (void)arg;

    /* resolve localhost */
    addr_t* addrs = NULL;
    size_t count = 0;
    int rc = addr_resolve("localhost", &addrs, &count);
    ASSERT(rc == 0);
    ASSERT(count > 0);
    for (size_t i = 0; i < count; i++) {
        ASSERT(addrs[i].storage.ss_family == AF_INET ||
               addrs[i].storage.ss_family == AF_INET6);
    }
    free(addrs);

    /* resolve public hostname */
    addrs = NULL;
    count = 0;
    rc = addr_resolve("www.baidu.com", &addrs, &count);
    ASSERT(rc == 0);
    ASSERT(count > 0);
    free(addrs);

    /* resolve non-existent host */
    addrs = NULL;
    count = 0;
    rc = addr_resolve("this.host.does.not.exist.invalid", &addrs, &count);
    ASSERT(rc == -1);
    ASSERT(count == 0);

    /* NULL params */
    ASSERT(addr_resolve(NULL, &addrs, &count) == -1);
    ASSERT(addr_resolve("localhost", NULL, &count) == -1);
    ASSERT(addr_resolve("localhost", &addrs, NULL) == -1);

    xylem_runtime_stop();
}

int main(void) {
    test_ipv4_roundtrip();
    test_ipv6_roundtrip();
    test_invalid_address();
    test_null_params();
    test_ipv4_wildcard();

    xylem_runtime_start(_resolve_main, NULL, NULL);

    return 0;
}
