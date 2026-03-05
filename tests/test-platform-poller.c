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

#include "platform/platform.h"
#include "assert.h"
#include <string.h>

#define MAX_CQE 64

static void test_init_destroy(void) {
    platform_poller_sq_t sq;
    ASSERT(platform_poller_init(&sq) == 0);
    platform_poller_destroy(&sq);
}

static void test_add_and_wait_readable(void) {
    platform_sock_t pair[2];
    int ret = platform_socket_socketpair(0, SOCK_STREAM, 0, pair);
    ASSERT(ret == 0);

    platform_poller_sq_t sq;
    ASSERT(platform_poller_init(&sq) == 0);

    int32_t tag = 42;
    platform_poller_sqe_t sqe = {0};
    sqe.op = PLATFORM_POLLER_RD_OP;
    sqe.fd = pair[0];
    sqe.ud = &tag;
    ASSERT(platform_poller_add(&sq, &sqe) == 0);

    /* write to pair[1] so pair[0] becomes readable */
    const char* msg = "hi";
    platform_socket_send(pair[1], msg, 2);

    platform_poller_cqe_t cqe[MAX_CQE];
    int n = platform_poller_wait(&sq, cqe, MAX_CQE, 1000);
    ASSERT(n >= 1);
    ASSERT(cqe[0].ud == &tag);
    ASSERT(cqe[0].op & PLATFORM_POLLER_RD_OP);

    /* drain the data */
    char buf[4];
    platform_socket_recv(pair[0], buf, sizeof(buf));

    platform_poller_del(&sq, &sqe);

    platform_poller_destroy(&sq);
    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

static void test_writable(void) {
    platform_sock_t pair[2];
    int ret = platform_socket_socketpair(0, SOCK_STREAM, 0, pair);
    ASSERT(ret == 0);

    platform_poller_sq_t sq;
    ASSERT(platform_poller_init(&sq) == 0);

    int32_t tag = 99;
    platform_poller_sqe_t sqe = {0};
    sqe.op = PLATFORM_POLLER_WR_OP;
    sqe.fd = pair[0];
    sqe.ud = &tag;
    ASSERT(platform_poller_add(&sq, &sqe) == 0);

    /* a fresh socket should be writable immediately */
    platform_poller_cqe_t cqe[MAX_CQE];
    int n = platform_poller_wait(&sq, cqe, MAX_CQE, 1000);
    ASSERT(n >= 1);
    ASSERT(cqe[0].ud == &tag);
    ASSERT(cqe[0].op & PLATFORM_POLLER_WR_OP);

    platform_poller_del(&sq, &sqe);

    platform_poller_destroy(&sq);
    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

static void test_readwrite(void) {
    platform_sock_t pair[2];
    int ret = platform_socket_socketpair(0, SOCK_STREAM, 0, pair);
    ASSERT(ret == 0);

    platform_poller_sq_t sq;
    ASSERT(platform_poller_init(&sq) == 0);

    int32_t tag = 7;
    platform_poller_sqe_t sqe = {0};
    sqe.op = PLATFORM_POLLER_RW_OP;
    sqe.fd = pair[0];
    sqe.ud = &tag;
    ASSERT(platform_poller_add(&sq, &sqe) == 0);

    /* first wait: socket is writable, expect WR (and maybe RD if data ready) */
    platform_poller_cqe_t cqe[MAX_CQE];
    int n = platform_poller_wait(&sq, cqe, MAX_CQE, 1000);
    ASSERT(n >= 1);
    ASSERT(cqe[0].ud == &tag);
    ASSERT(cqe[0].op & PLATFORM_POLLER_WR_OP);

    /* write data so RD fires */
    platform_socket_send(pair[1], "x", 1);

    /* re-arm with RD-only to avoid WR completing immediately */
    sqe.op = PLATFORM_POLLER_RD_OP;
    platform_poller_mod(&sq, &sqe);

    platform_poller_op_t got = PLATFORM_POLLER_NO_OP;
    for (int attempt = 0; attempt < 5 && !(got & PLATFORM_POLLER_RD_OP); attempt++) {
        n = platform_poller_wait(&sq, cqe, MAX_CQE, 1000);
        for (int i = 0; i < n; i++) {
            if (cqe[i].ud == &tag) {
                got |= cqe[i].op;
            }
        }
        /* re-arm for next attempt if needed */
        if (!(got & PLATFORM_POLLER_RD_OP)) {
            sqe.op = PLATFORM_POLLER_RD_OP;
            platform_poller_mod(&sq, &sqe);
        }
    }
    ASSERT(got & PLATFORM_POLLER_RD_OP);

    char buf[4];
    platform_socket_recv(pair[0], buf, sizeof(buf));

    platform_poller_del(&sq, &sqe);

    platform_poller_destroy(&sq);
    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

static void test_mod(void) {
    platform_sock_t pair[2];
    int ret = platform_socket_socketpair(0, SOCK_STREAM, 0, pair);
    ASSERT(ret == 0);

    platform_poller_sq_t sq;
    ASSERT(platform_poller_init(&sq) == 0);

    int32_t tag = 55;
    platform_poller_sqe_t sqe = {0};
    sqe.op = PLATFORM_POLLER_RD_OP;
    sqe.fd = pair[0];
    sqe.ud = &tag;
    ASSERT(platform_poller_add(&sq, &sqe) == 0);

    /* mod to write-only */
    sqe.op = PLATFORM_POLLER_WR_OP;
    ASSERT(platform_poller_mod(&sq, &sqe) == 0);

    /* socket should be writable */
    platform_poller_cqe_t cqe[MAX_CQE];
    int n = platform_poller_wait(&sq, cqe, MAX_CQE, 1000);
    ASSERT(n >= 1);
    ASSERT(cqe[0].ud == &tag);
    ASSERT(cqe[0].op & PLATFORM_POLLER_WR_OP);

    platform_poller_del(&sq, &sqe);

    platform_poller_destroy(&sq);
    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

static void test_timeout(void) {
    platform_sock_t pair[2];
    int ret = platform_socket_socketpair(0, SOCK_STREAM, 0, pair);
    ASSERT(ret == 0);

    platform_poller_sq_t sq;
    ASSERT(platform_poller_init(&sq) == 0);

    int32_t tag = 1;
    platform_poller_sqe_t sqe = {0};
    sqe.op = PLATFORM_POLLER_RD_OP;
    sqe.fd = pair[0];
    sqe.ud = &tag;
    ASSERT(platform_poller_add(&sq, &sqe) == 0);

    /* no data written -- should timeout and return 0 */
    platform_poller_cqe_t cqe[MAX_CQE];
    int n = platform_poller_wait(&sq, cqe, MAX_CQE, 50);
    ASSERT(n == 0);

    platform_poller_del(&sq, &sqe);

    platform_poller_destroy(&sq);
    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

static void test_multiple_fds(void) {
    platform_sock_t pair1[2], pair2[2];
    int ret = platform_socket_socketpair(0, SOCK_STREAM, 0, pair1);
    ASSERT(ret == 0);
    ret = platform_socket_socketpair(0, SOCK_STREAM, 0, pair2);
    ASSERT(ret == 0);

    platform_poller_sq_t sq;
    ASSERT(platform_poller_init(&sq) == 0);

    int32_t tag1 = 1, tag2 = 2;
    platform_poller_sqe_t sqe1 = {0};
    sqe1.op = PLATFORM_POLLER_RD_OP;
    sqe1.fd = pair1[0];
    sqe1.ud = &tag1;
    ASSERT(platform_poller_add(&sq, &sqe1) == 0);

    platform_poller_sqe_t sqe2 = {0};
    sqe2.op = PLATFORM_POLLER_RD_OP;
    sqe2.fd = pair2[0];
    sqe2.ud = &tag2;
    ASSERT(platform_poller_add(&sq, &sqe2) == 0);

    /* write to both */
    platform_socket_send(pair1[1], "a", 1);
    platform_socket_send(pair2[1], "b", 1);

    platform_poller_cqe_t cqe[MAX_CQE];
    bool found1 = false, found2 = false;
    for (int attempt = 0; attempt < 5 && !(found1 && found2); attempt++) {
        int n = platform_poller_wait(&sq, cqe, MAX_CQE, 1000);
        for (int i = 0; i < n; i++) {
            if (cqe[i].ud == &tag1) found1 = true;
            if (cqe[i].ud == &tag2) found2 = true;
        }
        /* re-arm any that haven't fired yet */
        if (!found1) platform_poller_mod(&sq, &sqe1);
        if (!found2) platform_poller_mod(&sq, &sqe2);
    }
    ASSERT(found1);
    ASSERT(found2);

    platform_poller_del(&sq, &sqe1);
    platform_poller_del(&sq, &sqe2);

    platform_poller_destroy(&sq);
    platform_socket_close(pair1[0]);
    platform_socket_close(pair1[1]);
    platform_socket_close(pair2[0]);
    platform_socket_close(pair2[1]);
}

int main(void) {
    platform_socket_startup();

    test_init_destroy();
    test_add_and_wait_readable();
    test_writable();
    test_readwrite();
    test_mod();
    test_timeout();
    test_multiple_fds();

    platform_socket_cleanup();
    return 0;
}
