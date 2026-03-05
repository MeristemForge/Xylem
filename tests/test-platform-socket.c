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

static void test_socketpair(void) {
    platform_sock_t pair[2];
    int ret = platform_socket_socketpair(0, SOCK_STREAM, 0, pair);
    ASSERT(ret == 0);
    ASSERT(pair[0] != PLATFORM_SO_ERROR_INVALID_SOCKET);
    ASSERT(pair[1] != PLATFORM_SO_ERROR_INVALID_SOCKET);
    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

static void test_send_recv(void) {
    platform_sock_t pair[2];
    int ret = platform_socket_socketpair(0, SOCK_STREAM, 0, pair);
    ASSERT(ret == 0);

    const char* msg = "hello";
    ssize_t n = platform_socket_send(pair[0], msg, 5);
    ASSERT(n == 5);

    char buf[16] = {0};
    n = platform_socket_recv(pair[1], buf, sizeof(buf));
    ASSERT(n == 5);
    ASSERT(memcmp(buf, "hello", 5) == 0);

    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

static void test_sendall_recvall(void) {
    platform_sock_t pair[2];
    int ret = platform_socket_socketpair(0, SOCK_STREAM, 0, pair);
    ASSERT(ret == 0);

    const char* msg = "abcdefghij";
    ssize_t n = platform_socket_sendall(pair[0], msg, 10);
    ASSERT(n == 10);

    char buf[16] = {0};
    n = platform_socket_recvall(pair[1], buf, 10);
    ASSERT(n == 10);
    ASSERT(memcmp(buf, "abcdefghij", 10) == 0);

    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

static void test_get_socktype(void) {
    platform_sock_t pair[2];
    int ret = platform_socket_socketpair(0, SOCK_STREAM, 0, pair);
    ASSERT(ret == 0);

    int type = platform_socket_get_socktype(pair[0]);
    ASSERT(type == SOCK_STREAM);

    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

static void test_nonblocking(void) {
    platform_sock_t pair[2];
    int ret = platform_socket_socketpair(0, SOCK_STREAM, 0, pair);
    ASSERT(ret == 0);

    /* set nonblocking and try recv on empty socket — should fail immediately */
    platform_socket_enable_nonblocking(pair[0], true);
    char buf[4];
    ssize_t n = platform_socket_recv(pair[0], buf, sizeof(buf));
    ASSERT(n == PLATFORM_SO_ERROR_SOCKET_ERROR);

    int err = platform_socket_get_lasterror();
    ASSERT(err == PLATFORM_SO_ERROR_EAGAIN ||
           err == PLATFORM_SO_ERROR_EWOULDBLOCK);

    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

static void test_tostring(void) {
    /* just verify it returns a non-NULL string */
    const char* s = platform_socket_tostring(0);
    ASSERT(s != NULL);
}

static void test_listen_dial(void) {
    /* listen on a random port */
    platform_sock_t srv = platform_socket_listen(
        "127.0.0.1", "0", SOCK_STREAM, false);
    ASSERT(srv != PLATFORM_SO_ERROR_INVALID_SOCKET);

    /* get the port the OS assigned */
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    getsockname(srv, (struct sockaddr*)&addr, &addrlen);
    char port[8];
    snprintf(port, sizeof(port), "%d", ntohs(addr.sin_port));

    /* dial to it */
    bool connected = false;
    platform_sock_t cli = platform_socket_dial(
        "127.0.0.1", port, SOCK_STREAM, &connected, false);
    ASSERT(cli != PLATFORM_SO_ERROR_INVALID_SOCKET);
    ASSERT(connected == true);

    /* accept */
    platform_sock_t acc = platform_socket_accept(srv, false);
    ASSERT(acc != PLATFORM_SO_ERROR_INVALID_SOCKET);

    /* send from client, recv on accepted */
    const char* msg = "test";
    ssize_t n = platform_socket_send(cli, msg, 4);
    ASSERT(n == 4);

    char buf[8] = {0};
    n = platform_socket_recv(acc, buf, sizeof(buf));
    ASSERT(n == 4);
    ASSERT(memcmp(buf, "test", 4) == 0);

    platform_socket_close(acc);
    platform_socket_close(cli);
    platform_socket_close(srv);
}

static void test_set_buffers(void) {
    platform_sock_t pair[2];
    int ret = platform_socket_socketpair(0, SOCK_STREAM, 0, pair);
    ASSERT(ret == 0);

    /* just verify these don't crash */
    platform_socket_set_rcvbuf(pair[0], 65536);
    platform_socket_set_sndbuf(pair[0], 65536);
    platform_socket_set_rcvtimeout(pair[0], 1000);
    platform_socket_set_sndtimeout(pair[0], 1000);

    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

int main(void) {
    platform_socket_startup();

    test_socketpair();
    test_send_recv();
    test_sendall_recvall();
    test_get_socktype();
    test_nonblocking();
    test_tostring();
    test_listen_dial();
    test_set_buffers();

    platform_socket_cleanup();
    return 0;
}
