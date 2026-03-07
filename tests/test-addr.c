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
#include "assert.h"

#define T(fn) { #fn, fn }

typedef void (*test_fn)(void);

typedef struct {
    const char* name;
    test_fn     fn;
} test_entry;

/* IPv4 pton + ntop round-trip */
static void test_ipv4_roundtrip(void) {
    xylem_addr_t addr;
    char host[64];
    uint16_t port;

    ASSERT(xylem_addr_pton("127.0.0.1", 8080, &addr) == 0);
    ASSERT(xylem_addr_ntop(&addr, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, "127.0.0.1") == 0);
    ASSERT(port == 8080);
}

/* IPv6 pton + ntop round-trip */
static void test_ipv6_roundtrip(void) {
    xylem_addr_t addr;
    char host[64];
    uint16_t port;

    ASSERT(xylem_addr_pton("::1", 9090, &addr) == 0);
    ASSERT(xylem_addr_ntop(&addr, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, "::1") == 0);
    ASSERT(port == 9090);
}

/* Invalid address returns -1 */
static void test_invalid_address(void) {
    xylem_addr_t addr;
    ASSERT(xylem_addr_pton("not_an_address", 80, &addr) == -1);
    ASSERT(xylem_addr_pton("999.999.999.999", 80, &addr) == -1);
}

/* NULL parameter handling */
static void test_null_params(void) {
    xylem_addr_t addr;
    char host[64];
    uint16_t port;

    ASSERT(xylem_addr_pton(NULL, 80, &addr) == -1);
    ASSERT(xylem_addr_pton("127.0.0.1", 80, NULL) == -1);
    ASSERT(xylem_addr_ntop(NULL, host, sizeof(host), &port) == -1);
    ASSERT(xylem_addr_ntop(&addr, NULL, 0, &port) == -1);
}

/* IPv4 wildcard address */
static void test_ipv4_wildcard(void) {
    xylem_addr_t addr;
    char host[64];
    uint16_t port;

    ASSERT(xylem_addr_pton("0.0.0.0", 0, &addr) == 0);
    ASSERT(xylem_addr_ntop(&addr, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, "0.0.0.0") == 0);
    ASSERT(port == 0);
}

int main(void) {
    platform_socket_startup();

    test_entry tests[] = {
        T(test_ipv4_roundtrip),
        T(test_ipv6_roundtrip),
        T(test_invalid_address),
        T(test_null_params),
        T(test_ipv4_wildcard),
    };

    size_t n = sizeof(tests) / sizeof(tests[0]);
    for (size_t i = 0; i < n; i++) {
        printf("  %s ... ", tests[i].name);
        tests[i].fn();
        printf("ok\n");
    }
    printf("all %zu addr tests passed\n", n);

    platform_socket_cleanup();
    return 0;
}
