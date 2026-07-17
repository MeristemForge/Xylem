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
#include "net/stream.h"
#include "platform/platform-socket.h"
#include "runtime/runtime.h"

#include "assert.h"
#include "utils.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TCP_HOST          "127.0.0.1"
#define TCP_PORT          18080

typedef void (*_coro_t)(void*);

typedef struct {
    xylem_channel_t*   ready;
    xylem_waitgroup_t* wg;
    uint16_t           port;
    _coro_t            server;
    _coro_t            client;
} _ctx_t;

typedef struct {
    stream_t*          stream;
    xylem_channel_t*   started;
    xylem_waitgroup_t* wg;
    char*              data;
    int                len;
    int                rc;
} _close_write_ctx_t;

static void _pair_main(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    ctx->ready  = xylem_channel_create();
    ctx->wg     = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, 2);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(ctx->server, ctx);
    xylem_spawn(ctx->client, ctx);
    xylem_waitgroup_wait(ctx->wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx->wg);
    xylem_channel_destroy(ctx->ready);
}

static void _run_pair(uint16_t port, _coro_t server, _coro_t client) {
    _ctx_t ctx = {.port = port, .server = server, .client = client};
    _pair_main(&ctx);
}

static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    char buf[256];
    int  n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_tcp_write(conn, buf, n) == 0);

    xylem_tcp_destroy(conn);
    xylem_tcp_destroy_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    const char* msg = "hello xylem";
    ASSERT(xylem_tcp_write(conn, msg, (int)strlen(msg)) == 0);

    char buf[64];
    int  n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tcp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_echo(void) {
    _run_pair(TCP_PORT, _echo_server, _echo_client);
}

static void _reader_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_write(conn, "ABCD", 4) == 0);
    xylem_sleep(30);
    ASSERT(xylem_tcp_write(conn, "EFGH", 4) == 0);
    xylem_tcp_destroy(conn);
    xylem_tcp_destroy_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _reader_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    xylem_reader_t* rd = xylem_reader_create(conn, XYLEM_READER_TCP, 256);
    ASSERT(rd != NULL);

    char result[8];
    ASSERT(xylem_reader_read_full(rd, result, 8) == 8);
    ASSERT(memcmp(result, "ABCDEFGH", 8) == 0);

    xylem_reader_destroy(rd);
    xylem_tcp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_reader_full(void) {
    _run_pair(TCP_PORT + 1, _reader_server, _reader_client);
}

static void _writer_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    xylem_writer_t* wr = xylem_writer_create(conn, XYLEM_WRITER_TCP, 256);
    ASSERT(wr != NULL);

    ASSERT(xylem_writer_write(wr, "hello", 5) == 0);
    ASSERT(xylem_writer_write(wr, " ", 1) == 0);
    ASSERT(xylem_writer_write(wr, "world", 5) == 0);
    ASSERT(xylem_writer_flush(wr) == 0);

    xylem_writer_destroy(wr);
    xylem_tcp_destroy(conn);
    xylem_tcp_destroy_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _writer_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    xylem_reader_t* rd = xylem_reader_create(conn, XYLEM_READER_TCP, 256);
    ASSERT(rd != NULL);

    char result[11];
    ASSERT(xylem_reader_read_full(rd, result, 11) == 11);
    ASSERT(memcmp(result, "hello world", 11) == 0);

    xylem_reader_destroy(rd);
    xylem_tcp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_writer_buffered(void) {
    _run_pair(TCP_PORT + 2, _writer_server, _writer_client);
}

static void _timeout_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_opts_t opts = {.connect_timeout_ms = 200};
    xylem_tcp_conn_t* conn = xylem_tcp_dial("192.0.2.1", 9999, &opts);
    ASSERT(conn == NULL);
    xylem_waitgroup_done(ctx->wg);
}

static void _timeout_main(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    ctx->wg     = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, 1);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(ctx->client, ctx);
    xylem_waitgroup_wait(ctx->wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx->wg);
}

static void test_dial_timeout(void) {
    _ctx_t ctx = {.client = _timeout_client};
    _timeout_main(&ctx);
}

static void _invalid_host_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    ASSERT(xylem_tcp_dial(NULL, 80, NULL) == NULL);
    ASSERT(xylem_tcp_dial("", 80, NULL) == NULL);
    xylem_waitgroup_done(ctx->wg);
}

static void test_invalid_dial_host(void) {
    _ctx_t ctx = {.client = _invalid_host_client};
    _timeout_main(&ctx);
}

static void test_resolve_returns_unique_addresses(void) {
    addr_t* addrs = NULL;
    size_t  count = 0;
    ASSERT(addr_resolve("localhost", 80, 1000, &addrs, &count) == 0);

    for (size_t i = 0; i < count; i++) {
        char     ip[INET6_ADDRSTRLEN];
        uint16_t port = 0;
        ASSERT(addr_ntop(&addrs[i], ip, sizeof(ip), &port) == 0);

        for (size_t j = i + 1; j < count; j++) {
            char     other_ip[INET6_ADDRSTRLEN];
            uint16_t other_port = 0;
            ASSERT(addr_ntop(
                       &addrs[j],
                       other_ip,
                       sizeof(other_ip),
                       &other_port)
                   == 0);
            ASSERT(strcmp(ip, other_ip) != 0 || port != other_port);
        }
    }

    free(addrs);
}

static void test_resolve_completion_timeout_race(void) {
    for (int i = 0; i < 32; i++) {
        addr_t* addrs = NULL;
        size_t  count = 0;
        int rc = addr_resolve("localhost", 80, 1, &addrs, &count);

        if (rc == 0) {
            ASSERT(addrs != NULL);
            ASSERT(count > 0);
            free(addrs);
        } else {
            ASSERT(addrs == NULL);
            ASSERT(count == 0);
        }
    }
}

static void test_lookup_numeric_address(void) {
    addr_t* addrs = NULL;
    size_t  count = 0;
    ASSERT(addr_lookup(TCP_HOST, 1234, 1000, &addrs, &count) == 0);
    ASSERT(count == 1);

    char     host[INET6_ADDRSTRLEN];
    uint16_t port = 0;
    ASSERT(addr_ntop(&addrs[0], host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, TCP_HOST) == 0);
    ASSERT(port == 1234);

    free(addrs);
}

static void test_lookup_hostname(void) {
    addr_t* addrs = NULL;
    size_t  count = 0;
    ASSERT(addr_lookup("localhost", 4321, 1000, &addrs, &count) == 0);
    ASSERT(count > 0);

    for (size_t i = 0; i < count; i++) {
        uint16_t port = 0;
        ASSERT(addr_ntop(&addrs[i], NULL, 0, &port) == 0);
        ASSERT(port == 4321);
    }

    free(addrs);
}

static void test_lookup_rejects_invalid_args(void) {
    addr_t* addrs = (addr_t*)1;
    size_t  count = 1;
    ASSERT(addr_lookup(NULL, 80, 0, &addrs, &count) == -1);
    ASSERT(addrs == NULL);
    ASSERT(count == 0);

    ASSERT(addr_lookup(TCP_HOST, 80, 0, NULL, &count) == -1);
    ASSERT(addr_lookup(TCP_HOST, 80, 0, &addrs, NULL) == -1);
}

static void test_accept_error_classification(void) {
    ASSERT(platform_socket_accept_should_retry(PLATFORM_SO_ERROR_ECONNRESET));
    ASSERT(!platform_socket_accept_should_retry(-1));
}

static void test_dial_falls_back_to_next_resolved_address(void) {
    addr_t* addrs = NULL;
    size_t  count = 0;
    ASSERT(addr_resolve("localhost", 0, 1000, &addrs, &count) == 0);
    if (count < 2) {
        free(addrs);
        return;
    }

    char first_host[INET6_ADDRSTRLEN];
    ASSERT(addr_ntop(&addrs[0], first_host, sizeof(first_host), NULL) == 0);

    char bind_host[INET6_ADDRSTRLEN];
    bool found = false;
    for (size_t i = 1; i < count; i++) {
        ASSERT(addr_ntop(&addrs[i], bind_host, sizeof(bind_host), NULL) == 0);
        if (strcmp(first_host, bind_host) != 0) {
            found = true;
            break;
        }
    }
    free(addrs);
    ASSERT(found);

    listener_t* listener = listener_listen(bind_host, 0, false);
    ASSERT(listener != NULL);

    uint16_t port = 0;
    ASSERT(listener_addr(listener, NULL, 0, &port) == 0);

    stream_t* stream = stream_dial("localhost", port, 1000, false);
    bool connected = stream != NULL;
    if (stream) {
        stream_close(stream);
        stream_destroy(stream);
    }
    listener_close(listener);
    listener_destroy(listener);

    ASSERT(connected);
}

static void _eof_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_write(conn, "bye", 3) == 0);
    xylem_tcp_destroy(conn);
    xylem_tcp_destroy_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _eof_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    char buf[16];
    int  n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == 3);
    ASSERT(memcmp(buf, "bye", 3) == 0);

    n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == 0);

    ASSERT(runtime_consume_credit(UINT32_MAX));
    n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == 0);
    ASSERT(runtime_consume_credit(1));
    runtime_yield();

    xylem_tcp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_peer_close_eof(void) {
    _run_pair(TCP_PORT + 3, _eof_server, _eof_client);
}

static void _half_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    char buf[16];
    int  n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == 4);
    ASSERT(memcmp(buf, "ping", 4) == 0);

    n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == 0);

    ASSERT(xylem_tcp_write(conn, "pong", 4) == 0);
    xylem_tcp_destroy(conn);
    xylem_tcp_destroy_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _half_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_write(conn, "ping", 4) == 0);
    ASSERT(xylem_tcp_shutdown_wr(conn) == 0);

    char buf[16];
    int  n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == 4);
    ASSERT(memcmp(buf, "pong", 4) == 0);

    xylem_tcp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_half_close(void) {
    _run_pair(TCP_PORT + 4, _half_server, _half_client);
}

static void _shutdown_rd_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    xylem_sleep(50);
    ASSERT(xylem_tcp_write(conn, "data", 4) == 0);
    xylem_tcp_destroy(conn);
    xylem_tcp_destroy_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _shutdown_rd_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_shutdown_rd(conn) == 0);

    char buf[16];
    int  n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == -1);

    xylem_tcp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_shutdown_rd_read_fails(void) {
    _run_pair(TCP_PORT + 5, _shutdown_rd_server, _shutdown_rd_client);
}

static void _expired_read_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_write(conn, "late", 4) == 0);
    xylem_sleep(100);
    xylem_tcp_destroy(conn);
    xylem_tcp_destroy_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _expired_read_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    xylem_sleep(50);
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) - 1;
    xylem_tcp_set_read_deadline(conn, deadline);

    char buf[16];
    ASSERT(xylem_tcp_read(conn, buf, sizeof(buf)) == -1);

    xylem_tcp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_expired_read_deadline_blocks_ready_data(void) {
    _run_pair(TCP_PORT + 6, _expired_read_server, _expired_read_client);
}

static void _expired_write_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    xylem_sleep(100);
    xylem_tcp_destroy(conn);
    xylem_tcp_destroy_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _expired_write_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) - 1;
    xylem_tcp_set_write_deadline(conn, deadline);
    ASSERT(xylem_tcp_write(conn, "late", 4) == -1);

    xylem_tcp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_expired_write_deadline_blocks_ready_socket(void) {
    _run_pair(TCP_PORT + 7, _expired_write_server, _expired_write_client);
}

static void _invalid_io_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_write(conn, "go", 2) == 0);

    char buf[16];
    int  n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == 2);
    ASSERT(memcmp(buf, "ok", 2) == 0);

    xylem_tcp_destroy(conn);
    xylem_tcp_destroy_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _invalid_io_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    char buf[16];
    ASSERT(xylem_tcp_read(conn, buf, 0) == -1);
    ASSERT(xylem_tcp_read(conn, NULL, 1) == -1);
    ASSERT(xylem_tcp_read(conn, buf, -1) == -1);
    ASSERT(xylem_tcp_write(conn, NULL, 0) == 0);
    ASSERT(xylem_tcp_write(conn, NULL, 1) == -1);
    ASSERT(xylem_tcp_write(conn, "x", -1) == -1);

    int n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == 2);
    ASSERT(memcmp(buf, "go", 2) == 0);

    ASSERT(xylem_tcp_write(conn, "ok", 2) == 0);

    xylem_tcp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_invalid_io_args(void) {
    _run_pair(TCP_PORT + 8, _invalid_io_server, _invalid_io_client);
}

static void test_once_io_does_not_yield(void) {
    platform_sock_t socks[2] = {
        PLATFORM_SO_ERROR_INVALID_SOCKET,
        PLATFORM_SO_ERROR_INVALID_SOCKET,
    };
    ASSERT(platform_socket_socketpair(AF_INET, SOCK_STREAM, 0, socks) == 0);
    platform_socket_enable_nonblocking(socks[0], true);
    platform_socket_enable_nonblocking(socks[1], true);

    stream_t* stream = stream_from_fd(socks[0]);
    ASSERT(stream != NULL);

    char byte = 'x';
    ASSERT(runtime_consume_credit(UINT32_MAX));
    ASSERT(stream_write(stream, &byte, 1) == 1);
    ASSERT(runtime_consume_credit(1));
    runtime_yield();

    ASSERT(platform_socket_send(socks[1], &byte, 1) == 1);
    ASSERT(stream_wait_read(stream) == IOWAIT_READY);
    ASSERT(runtime_consume_credit(UINT32_MAX));
    ASSERT(stream_read(stream, &byte, 1) == 1);
    ASSERT(runtime_consume_credit(1));
    runtime_yield();

    stream_destroy(stream);
    platform_socket_close(socks[1]);
}

static void _close_write_writer(void* arg) {
    _close_write_ctx_t* ctx = (_close_write_ctx_t*)arg;
    for (;;) {
        int n = stream_write(ctx->stream, ctx->data, ctx->len);
        if (n == STREAM_IO_AGAIN) {
            break;
        }
        ASSERT(n > 0);
    }
    ASSERT(xylem_channel_send(ctx->started, ctx) == 0);

    const char* ptr = ctx->data;
    int         rem = ctx->len;
    ctx->rc         = -1;
    while (rem > 0) {
        int n = stream_write(ctx->stream, ptr, rem);
        if (n > 0) {
            ptr += n;
            rem -= n;
            continue;
        }
        if (n != STREAM_IO_AGAIN
            || stream_wait_write(ctx->stream) != IOWAIT_READY) {
            break;
        }
    }
    if (rem == 0) {
        ctx->rc = 0;
    }
    xylem_waitgroup_done(ctx->wg);
}

static void _close_write_closer(void* arg) {
    _close_write_ctx_t* ctx = (_close_write_ctx_t*)arg;
    xylem_channel_recv(ctx->started);
    stream_close(ctx->stream);
    xylem_waitgroup_done(ctx->wg);
}

static void _close_write_main(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;

    platform_sock_t socks[2] = {
        PLATFORM_SO_ERROR_INVALID_SOCKET,
        PLATFORM_SO_ERROR_INVALID_SOCKET,
    };
    ASSERT(platform_socket_socketpair(AF_INET, SOCK_STREAM, 0, socks) == 0);
    platform_socket_enable_nonblocking(socks[0], true);
    platform_socket_enable_nonblocking(socks[1], true);
    platform_socket_set_rcvbuf(socks[0], 4096);
    platform_socket_set_sndbuf(socks[1], 4096);

    char fill[4096];
    memset(fill, 'x', sizeof(fill));
    for (;;) {
        ssize_t n = platform_socket_send(socks[1], fill, sizeof(fill));
        if (n > 0) {
            continue;
        }
        ASSERT(n == -1);
        int err = platform_socket_get_lasterror();
        ASSERT(err == PLATFORM_SO_ERROR_EAGAIN
               || err == PLATFORM_SO_ERROR_EWOULDBLOCK);
        break;
    }

    stream_t* stream = stream_from_fd(socks[1]);
    ASSERT(stream != NULL);

    enum { WRITE_LEN = 32 * 1024 * 1024 };
    char* data = (char*)calloc(1, WRITE_LEN);
    ASSERT(data != NULL);
    memset(data, 'x', WRITE_LEN);

    _close_write_ctx_t write_ctx = {
        .stream = stream,
        .started = xylem_channel_create(),
        .wg = xylem_waitgroup_create(),
        .data = data,
        .len = WRITE_LEN,
        .rc = 0,
    };
    ASSERT(write_ctx.started != NULL);
    ASSERT(write_ctx.wg != NULL);

    xylem_waitgroup_add(write_ctx.wg, 2);
    xylem_spawn(_close_write_writer, &write_ctx);
    xylem_spawn(_close_write_closer, &write_ctx);
    xylem_waitgroup_wait(write_ctx.wg);

    ASSERT(write_ctx.rc == -1);

    xylem_waitgroup_destroy(write_ctx.wg);
    xylem_channel_destroy(write_ctx.started);
    stream_destroy(stream);
    platform_socket_close(socks[0]);
    free(data);
    xylem_waitgroup_done(ctx->wg);
}

static void test_close_stops_inflight_write(void) {
    _ctx_t ctx = {.client = _close_write_main};
    _timeout_main(&ctx);
}

static void test_closed_leaf_operations(void) {
    listener_t* listener = listener_listen(TCP_HOST, 0, false);
    ASSERT(listener != NULL);

    uint16_t port = 0;
    ASSERT(listener_addr(listener, NULL, 0, &port) == 0);
    stream_t* stream = stream_dial(TCP_HOST, port, 1000, false);
    ASSERT(stream != NULL);

    char host[INET6_ADDRSTRLEN];
    ASSERT(stream_local_addr(stream, host, sizeof(host), &port) == 0);
    ASSERT(stream_remote_addr(stream, host, sizeof(host), &port) == 0);

    listener_close(listener);
    ASSERT(listener_addr(listener, NULL, 0, &port) == -1);
    listener_destroy(listener);

    stream_close(stream);
    stream_set_read_deadline(stream, 1);
    stream_set_write_deadline(stream, 1);
    ASSERT(stream_local_addr(stream, host, sizeof(host), &port) == -1);
    ASSERT(stream_remote_addr(stream, host, sizeof(host), &port) == -1);
    ASSERT(stream_shutdown_rd(stream) == -1);
    ASSERT(stream_shutdown_wr(stream) == -1);

    stream_destroy(stream);
}

static void test_closed_public_listener_operations(void) {
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, 0, NULL);
    ASSERT(listener != NULL);

    uint16_t port = 0;
    ASSERT(xylem_tcp_listener_addr(listener, NULL, 0, &port) == 0);

    xylem_tcp_close_listener(listener);
    xylem_tcp_close_listener(listener);
    ASSERT(xylem_tcp_listener_addr(listener, NULL, 0, &port) == -1);
    xylem_tcp_destroy_listener(listener);
    xylem_tcp_destroy_listener(NULL);
}

static void test_closed_public_connection_operations(void) {
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, 0, NULL);
    ASSERT(listener != NULL);

    uint16_t port = 0;
    ASSERT(xylem_tcp_listener_addr(listener, NULL, 0, &port) == 0);
    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, port, NULL);
    ASSERT(conn != NULL);

    xylem_tcp_close(conn);
    xylem_tcp_close(conn);
    xylem_tcp_set_read_deadline(conn, 1);
    xylem_tcp_set_write_deadline(conn, 1);

    char byte = 0;
    char host[INET6_ADDRSTRLEN];
    ASSERT(xylem_tcp_read(conn, &byte, 1) == -1);
    ASSERT(xylem_tcp_write(conn, &byte, 1) == -1);
    ASSERT(xylem_tcp_local_addr(conn, host, sizeof(host), &port) == -1);
    ASSERT(xylem_tcp_remote_addr(conn, host, sizeof(host), &port) == -1);
    ASSERT(xylem_tcp_shutdown_rd(conn) == -1);
    ASSERT(xylem_tcp_shutdown_wr(conn) == -1);

    xylem_tcp_destroy(conn);
    xylem_tcp_destroy(NULL);

    xylem_tcp_destroy_listener(listener);
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_echo();
    test_reader_full();
    test_writer_buffered();
    test_dial_timeout();
    test_invalid_dial_host();
    test_resolve_returns_unique_addresses();
    test_resolve_completion_timeout_race();
    test_lookup_numeric_address();
    test_lookup_hostname();
    test_lookup_rejects_invalid_args();
    test_accept_error_classification();
    test_dial_falls_back_to_next_resolved_address();
    test_peer_close_eof();
    test_half_close();
    test_shutdown_rd_read_fails();
    test_expired_read_deadline_blocks_ready_data();
    test_expired_write_deadline_blocks_ready_socket();
    test_invalid_io_args();
    test_once_io_does_not_yield();
    test_closed_leaf_operations();
    test_closed_public_listener_operations();
    test_closed_public_connection_operations();
    test_close_stops_inflight_write();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_opts_t opts = {.workers = 1};
    xylem_run(_test_run_all, NULL, &opts);
    return 0;
}
