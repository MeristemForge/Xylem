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
#include "runtime/runtime.h"
#include "xylem/net/xylem-tcp.h"
#include "addr.h"
#include "xylem/xylem-logger.h"

#include "minicoro/minicoro.h"
#include "platform/platform-socket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct xylem_tcp_conn_s {
    loop_t*       loop;
    loop_io_t*    io;
    platform_sock_t     fd;
    mco_coro*           wait_coro;
    loop_timer_t* timeout_timer;
    loop_poller_op_t   revents;
    bool                timed_out;
    bool                closed;
    int                 last_error;
    addr_t        peer_addr;
    void*               userdata;
};

struct xylem_tcp_listener_s {
    loop_t*    loop;
    loop_io_t* io;
    platform_sock_t  fd;
    mco_coro*        wait_coro;
    bool             closing;
    void*            userdata;
};

/* --- I/O suspension primitive --- */

static void _tcp_io_cb(loop_t* loop, loop_io_t* io,
                       loop_poller_op_t revents, void* ud) {
    (void)loop; (void)io;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;
    tcp->revents = revents;
    if (tcp->wait_coro) {
        mco_coro* co = tcp->wait_coro;
        tcp->wait_coro = NULL;
        mco_resume(co);
    }
}

static void _tcp_timeout_cb(loop_t* loop, loop_timer_t* timer,
                            void* ud) {
    (void)loop; (void)timer;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;
    tcp->timed_out = true;
    if (tcp->wait_coro) {
        mco_coro* co = tcp->wait_coro;
        tcp->wait_coro = NULL;
        mco_resume(co);
    }
}

static loop_poller_op_t _tcp_wait_io(xylem_tcp_conn_t* tcp,
                                      loop_poller_op_t interest,
                                      uint64_t timeout_ms) {
    tcp->timed_out = false;
    tcp->revents = LOOP_POLLER_NO_OP;
    tcp->wait_coro = mco_running();

    loop_start_io(tcp->io, interest, _tcp_io_cb, tcp);

    if (timeout_ms > 0) {
        if (!tcp->timeout_timer)
            tcp->timeout_timer = loop_create_timer(tcp->loop);
        loop_start_timer(tcp->timeout_timer, _tcp_timeout_cb,
                               tcp, timeout_ms, 0);
    }

    mco_yield(mco_running());

    if (timeout_ms > 0 && tcp->timeout_timer)
        loop_stop_timer(tcp->timeout_timer);

    return tcp->timed_out ? LOOP_POLLER_NO_OP : tcp->revents;
}

/* --- Connection --- */

static xylem_tcp_conn_t* _tcp_conn_alloc(loop_t* loop,
                                         platform_sock_t fd) {
    xylem_tcp_conn_t* tcp =
        (xylem_tcp_conn_t*)calloc(1, sizeof(xylem_tcp_conn_t));
    if (!tcp) return NULL;

    tcp->loop = loop;
    tcp->fd = fd;
    tcp->io = loop_create_io(loop, (loop_poller_fd_t)fd);
    if (!tcp->io) {
        free(tcp);
        return NULL;
    }

    platform_socket_enable_nodelay(fd, true);
    return tcp;
}

/* --- Public API: dial --- */

xylem_tcp_conn_t* xylem_tcp_dial(const char* host,
                                 uint16_t port,
                                 xylem_tcp_opts_t* opts) {
    loop_t* loop = runtime_loop();
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    bool connected = false;
    platform_sock_t fd = platform_socket_dial(host, port_str,
                                              SOCK_STREAM,
                                              &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("tcp dial: socket creation failed for %s:%s",
                   host, port_str);
        return NULL;
    }

    if (opts && opts->disable_mss_clamp)
        platform_socket_enable_mss_clamp(fd, false);

    xylem_tcp_conn_t* tcp = _tcp_conn_alloc(loop, fd);
    if (!tcp) {
        platform_socket_close(fd);
        return NULL;
    }

    addr_pton(host, port, &tcp->peer_addr);

    if (!connected) {
        loop_poller_op_t ev = _tcp_wait_io(tcp, LOOP_POLLER_WR_OP, 0);
        if (ev == LOOP_POLLER_NO_OP) {
            tcp->last_error = PLATFORM_SO_ERROR_ETIMEDOUT;
            xylem_tcp_close(tcp);
            return NULL;
        }

        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen);
        if (err != 0) {
            tcp->last_error = err;
            xylem_loge("tcp dial fd=%d connect error=%d (%s)",
                       (int)fd, err, platform_socket_tostring(err));
            xylem_tcp_close(tcp);
            return NULL;
        }
    }

    xylem_logi("tcp conn fd=%d connected to %s:%s", (int)fd, host, port_str);
    return tcp;
}

xylem_tcp_conn_t* xylem_tcp_dial_timeout(const char* host,
                                         uint16_t port,
                                         xylem_tcp_opts_t* opts,
                                         uint64_t ms) {
    loop_t* loop = runtime_loop();
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    bool connected = false;
    platform_sock_t fd = platform_socket_dial(host, port_str,
                                              SOCK_STREAM,
                                              &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("tcp dial: socket creation failed for %s:%s",
                   host, port_str);
        return NULL;
    }

    if (opts && opts->disable_mss_clamp)
        platform_socket_enable_mss_clamp(fd, false);

    xylem_tcp_conn_t* tcp = _tcp_conn_alloc(loop, fd);
    if (!tcp) {
        platform_socket_close(fd);
        return NULL;
    }

    addr_pton(host, port, &tcp->peer_addr);

    if (!connected) {
        loop_poller_op_t ev = _tcp_wait_io(tcp, LOOP_POLLER_WR_OP, ms);
        if (ev == LOOP_POLLER_NO_OP) {
            tcp->last_error = PLATFORM_SO_ERROR_ETIMEDOUT;
            xylem_tcp_close(tcp);
            return NULL;
        }

        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen);
        if (err != 0) {
            tcp->last_error = err;
            xylem_tcp_close(tcp);
            return NULL;
        }
    }

    return tcp;
}

/* --- Public API: recv --- */

ssize_t xylem_tcp_recv(xylem_tcp_conn_t* tcp, void* buf, size_t len) {
    if (tcp->closed) return -1;

    for (;;) {
        ssize_t n = platform_socket_recv(tcp->fd, buf, (int)len);
        if (n > 0) return n;
        if (n == 0) return 0;

        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN &&
            err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            tcp->last_error = err;
            return -1;
        }

        loop_poller_op_t ev = _tcp_wait_io(tcp, LOOP_POLLER_RD_OP, 0);
        if (ev == LOOP_POLLER_NO_OP || tcp->closed) {
            tcp->last_error = PLATFORM_SO_ERROR_ECONNRESET;
            return -1;
        }
    }
}

ssize_t xylem_tcp_recv_timeout(xylem_tcp_conn_t* tcp,
                               void* buf, size_t len,
                               uint64_t ms) {
    if (tcp->closed) return -1;

    for (;;) {
        ssize_t n = platform_socket_recv(tcp->fd, buf, (int)len);
        if (n > 0) return n;
        if (n == 0) return 0;

        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN &&
            err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            tcp->last_error = err;
            return -1;
        }

        loop_poller_op_t ev = _tcp_wait_io(tcp, LOOP_POLLER_RD_OP, ms);
        if (ev == LOOP_POLLER_NO_OP || tcp->closed) {
            tcp->last_error = PLATFORM_SO_ERROR_ETIMEDOUT;
            return -1;
        }
    }
}

int xylem_tcp_recv_exact(xylem_tcp_conn_t* tcp, void* buf, size_t len) {
    char* ptr = (char*)buf;
    size_t rem = len;

    while (rem > 0) {
        ssize_t n = xylem_tcp_recv(tcp, ptr, rem);
        if (n <= 0) return -1;
        ptr += n;
        rem -= (size_t)n;
    }
    return 0;
}

/* --- Public API: send --- */

int xylem_tcp_send(xylem_tcp_conn_t* tcp, const void* data, size_t len) {
    if (tcp->closed) return -1;

    const char* ptr = (const char*)data;
    size_t rem = len;

    while (rem > 0) {
        ssize_t n = platform_socket_send(tcp->fd, ptr, (int)rem);
        if (n > 0) {
            ptr += n;
            rem -= (size_t)n;
            continue;
        }

        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN &&
            err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            tcp->last_error = err;
            return -1;
        }

        loop_poller_op_t ev = _tcp_wait_io(tcp, LOOP_POLLER_WR_OP, 0);
        if (ev == LOOP_POLLER_NO_OP || tcp->closed) {
            tcp->last_error = PLATFORM_SO_ERROR_ECONNRESET;
            return -1;
        }
    }
    return 0;
}

/* --- Public API: framing helpers --- */

void* xylem_tcp_recv_frame(xylem_tcp_conn_t* tcp,
                           xylem_tcp_frame_opts_t* opts,
                           size_t* out_len) {
    uint8_t hdr[16];
    uint32_t hdr_sz = opts->header_size;

    if (hdr_sz > sizeof(hdr)) {
        tcp->last_error = -1;
        return NULL;
    }

    if (xylem_tcp_recv_exact(tcp, hdr, hdr_sz) != 0)
        return NULL;

    /* Decode the length field. */
    uint64_t body_len = 0;
    uint8_t* field = hdr + opts->field_offset;

    if (opts->big_endian) {
        for (uint32_t i = 0; i < opts->field_size; i++)
            body_len = (body_len << 8) | field[i];
    } else {
        for (uint32_t i = 0; i < opts->field_size; i++)
            body_len |= (uint64_t)field[i] << (i * 8);
    }

    int64_t adjusted = (int64_t)body_len + opts->adjustment;
    if (adjusted <= 0) {
        tcp->last_error = -1;
        return NULL;
    }

    size_t payload_len = (size_t)adjusted;
    void* payload = malloc(payload_len);
    if (!payload) return NULL;

    if (xylem_tcp_recv_exact(tcp, payload, payload_len) != 0) {
        free(payload);
        return NULL;
    }

    *out_len = payload_len;
    return payload;
}

int xylem_tcp_send_frame(xylem_tcp_conn_t* tcp,
                         xylem_tcp_frame_opts_t* opts,
                         const void* data, size_t len) {
    uint8_t hdr[16];
    uint32_t hdr_sz = opts->header_size;

    if (hdr_sz > sizeof(hdr)) return -1;

    memset(hdr, 0, hdr_sz);

    /* Encode the length field. */
    int64_t wire_len = (int64_t)len - opts->adjustment;
    if (wire_len < 0) return -1;

    uint8_t* field = hdr + opts->field_offset;
    uint64_t val = (uint64_t)wire_len;

    if (opts->big_endian) {
        for (int i = (int)opts->field_size - 1; i >= 0; i--) {
            field[i] = (uint8_t)(val & 0xFF);
            val >>= 8;
        }
    } else {
        for (uint32_t i = 0; i < opts->field_size; i++) {
            field[i] = (uint8_t)(val & 0xFF);
            val >>= 8;
        }
    }

    if (xylem_tcp_send(tcp, hdr, hdr_sz) != 0)
        return -1;

    return xylem_tcp_send(tcp, data, len);
}

ssize_t xylem_tcp_recv_line(xylem_tcp_conn_t* tcp, char* buf, size_t max) {
    size_t pos = 0;

    while (pos < max - 1) {
        char ch;
        ssize_t n = xylem_tcp_recv(tcp, &ch, 1);
        if (n <= 0) return -1;

        if (ch == '\n') {
            /* Strip trailing \r if present. */
            if (pos > 0 && buf[pos - 1] == '\r')
                pos--;
            buf[pos] = '\0';
            return (ssize_t)pos;
        }

        buf[pos++] = ch;
    }

    /* Line too long. */
    buf[pos] = '\0';
    return (ssize_t)pos;
}

/* --- Public API: server --- */

static void _tcp_server_io_cb(loop_t* loop, loop_io_t* io,
                              loop_poller_op_t revents, void* ud) {
    (void)loop; (void)io; (void)revents;
    xylem_tcp_listener_t* server = (xylem_tcp_listener_t*)ud;
    if (server->wait_coro) {
        mco_coro* co = server->wait_coro;
        server->wait_coro = NULL;
        mco_resume(co);
    }
}

xylem_tcp_listener_t* xylem_tcp_listen(const char* host,
                                       uint16_t port,
                                       xylem_tcp_opts_t* opts) {
    loop_t* loop = runtime_loop();
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd = platform_socket_listen(host, port_str,
                                                SOCK_STREAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("tcp listen: failed for %s:%s", host, port_str);
        return NULL;
    }

    if (opts && opts->disable_mss_clamp)
        platform_socket_enable_mss_clamp(fd, false);

    xylem_tcp_listener_t* server =
        (xylem_tcp_listener_t*)calloc(1, sizeof(xylem_tcp_listener_t));
    if (!server) {
        platform_socket_close(fd);
        return NULL;
    }

    server->loop = loop;
    server->fd = fd;
    server->io = loop_create_io(loop, (loop_poller_fd_t)fd);
    if (!server->io) {
        platform_socket_close(fd);
        free(server);
        return NULL;
    }

    xylem_logi("tcp server fd=%d listening on %s:%s",
               (int)fd, host, port_str);
    return server;
}

xylem_tcp_conn_t* xylem_tcp_accept(xylem_tcp_listener_t* server) {
    for (;;) {
        if (server->closing) return NULL;

        platform_sock_t fd = platform_socket_accept(server->fd, true);
        if (fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
            xylem_tcp_conn_t* tcp = _tcp_conn_alloc(server->loop, fd);
            if (!tcp) {
                platform_socket_close(fd);
                continue;
            }

            socklen_t peer_len = sizeof(tcp->peer_addr.storage);
            getpeername(fd, (struct sockaddr*)&tcp->peer_addr.storage,
                        &peer_len);

            xylem_logi("tcp server fd=%d accepted conn fd=%d",
                       (int)server->fd, (int)fd);
            return tcp;
        }

        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN &&
            err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            xylem_logw("tcp server fd=%d accept error=%d (%s)",
                       (int)server->fd, err,
                       platform_socket_tostring(err));
            continue;
        }

        /* Wait for readable. */
        server->wait_coro = mco_running();
        loop_start_io(server->io, LOOP_POLLER_RD_OP,
                            _tcp_server_io_cb, server);
        mco_yield(mco_running());
    }
}

void xylem_tcp_close_listener(xylem_tcp_listener_t* server) {
    if (server->closing) return;

    xylem_logi("tcp server fd=%d closing", (int)server->fd);
    server->closing = true;

    /* Wake any coroutine blocked in accept. */
    if (server->wait_coro) {
        mco_coro* co = server->wait_coro;
        server->wait_coro = NULL;
        mco_resume(co);
    }

    loop_destroy_io(server->io);
    server->io = NULL;
    platform_socket_close(server->fd);
    free(server);
}

/* --- Public API: close --- */

void xylem_tcp_close(xylem_tcp_conn_t* tcp) {
    if (tcp->closed) return;
    tcp->closed = true;

    if (tcp->timeout_timer)
        loop_destroy_timer(tcp->timeout_timer);
    if (tcp->io)
        loop_destroy_io(tcp->io);

    shutdown(tcp->fd, PLATFORM_SHUT_WR);
    platform_socket_close(tcp->fd);
    free(tcp);
}

/* --- Public API: accessors --- */

int xylem_tcp_get_error(xylem_tcp_conn_t* tcp) {
    return tcp->last_error;
}

int xylem_tcp_remote_addr(xylem_tcp_conn_t* tcp,
                          char host[ADDR_MAXHOST],
                          uint16_t* port) {
    return addr_ntop(&tcp->peer_addr, host, ADDR_MAXHOST, port);
}


void* xylem_tcp_get_userdata(xylem_tcp_conn_t* tcp) {
    return tcp->userdata;
}

void xylem_tcp_set_userdata(xylem_tcp_conn_t* tcp, void* ud) {
    tcp->userdata = ud;
}

void* xylem_tcp_listener_get_userdata(xylem_tcp_listener_t* server) {
    return server->userdata;
}

void xylem_tcp_listener_set_userdata(xylem_tcp_listener_t* server, void* ud) {
    server->userdata = ud;
}
