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

#include "platform/platform-socket.h"

#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/un.h>

#define PLATFORM_TCPV4_MSS 536
#define PLATFORM_TCPV6_MSS 1220

void platform_socket_enable_nonblocking(platform_sock_t sock, bool on) {
    int flag = fcntl(sock, F_GETFL, 0);
    if (flag == -1) {
        return;
    }
    fcntl(sock, F_SETFL, on ? (flag | O_NONBLOCK) : (flag & ~O_NONBLOCK));
}

void platform_socket_set_rcvbuf(platform_sock_t sock, int val) {
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const void*)&val, sizeof(int));
}

void platform_socket_set_sndbuf(platform_sock_t sock, int val) {
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const void*)&val, sizeof(int));
}

void platform_socket_close(platform_sock_t sock) {
    close(sock);
}

static platform_sock_t _socket_accept(platform_sock_t sock, bool nonblocking) {
    platform_sock_t cli;
    do {
        cli = accept(sock, NULL, NULL);
    } while (cli == PLATFORM_SO_ERROR_INVALID_SOCKET
             && (errno == EINTR || errno == ECONNABORTED));
    if (cli != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_enable_nonblocking(cli, nonblocking);
    }
    return cli;
}

platform_sock_t platform_socket_accept(platform_sock_t sock, bool nonblocking) {
    platform_sock_t cli = _socket_accept(sock, nonblocking);
    if (cli == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }
    /**
     * Linux default sndbuf (~16KB) forces EAGAIN on every large send,
     * causing excessive coroutine park/re-arm cycles. rcvbuf left to
     * kernel autotuning (tcp_rmem) to avoid hurting high-BDP paths.
     */
    platform_socket_set_sndbuf(cli, 256 * 1024);
    return cli;
}

platform_sock_t platform_socket_accept_unix(platform_sock_t sock,
                                            bool nonblocking) {
    return _socket_accept(sock, nonblocking);
}

void platform_socket_enable_nodelay(platform_sock_t sock, bool on) {
    int val = on ? 1 : 0;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const void*)&val, sizeof(val));
}

void platform_socket_enable_reuseaddr(platform_sock_t sock, bool on) {
    int val = on ? 1 : 0;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const void*)&val, sizeof(val));
}

void platform_socket_enable_v6only(platform_sock_t sock, bool on) {
    int val = on ? 1 : 0;
    setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const void*)&val, sizeof(val));
}

void platform_socket_set_rcvtimeout(platform_sock_t sock, int timeout_ms) {
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    setsockopt(
        sock,
        SOL_SOCKET,
        SO_RCVTIMEO,
        (const void*)&tv,
        sizeof(struct timeval));
}

void platform_socket_set_sndtimeout(platform_sock_t sock, int timeout_ms) {
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    setsockopt(
        sock,
        SOL_SOCKET,
        SO_SNDTIMEO,
        (const void*)&tv,
        sizeof(struct timeval));
}

void platform_socket_enable_reuseport(platform_sock_t sock, bool on) {
    int val = on ? 1 : 0;
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const void*)&val, sizeof(val));
}

platform_sock_t platform_socket_listen(
    const char* restrict host,
    const char* restrict port,
    int                  socktype,
    bool                 nonblocking) {
    platform_sock_t  sock;
    struct addrinfo* res;
    struct addrinfo* rp;
    struct addrinfo  hints = {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = socktype,
        .ai_flags    = AI_PASSIVE,
    };

    if (getaddrinfo(host, port, &hints, &res)) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            continue;
        }
        if (rp->ai_family == AF_INET6) {
            platform_socket_enable_v6only(sock, false);
        }
        platform_socket_enable_reuseaddr(sock, true);
        platform_socket_enable_reuseport(sock, true);
        if (socktype == SOCK_DGRAM) {
            platform_socket_set_rcvbuf_max(sock, 16 * 1024 * 1024);
        }
        if (bind(sock, rp->ai_addr, rp->ai_addrlen) ==
            PLATFORM_SO_ERROR_SOCKET_ERROR) {
            platform_socket_close(sock);
            continue;
        }
        if (socktype == SOCK_STREAM) {
            if (listen(sock, SOMAXCONN) == PLATFORM_SO_ERROR_SOCKET_ERROR) {
                platform_socket_close(sock);
                continue;
            }
            platform_socket_enable_mss_clamp(sock, true);
            /* macOS TCP_NOOPT blocks NODELAY; must follow mss_clamp. */
            platform_socket_enable_nodelay(sock, true);
            platform_socket_enable_keepalive(sock, true);
        }
        /* Not inherited by accepted sockets. */
        platform_socket_enable_nonblocking(sock, nonblocking);
        break;
    }
    if (rp == NULL) {
        freeaddrinfo(res);
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return sock;
}

void platform_socket_startup(void) {
    signal(SIGPIPE, SIG_IGN);
}

void platform_socket_cleanup(void) {
}

platform_sock_t platform_socket_dial(
    const char* restrict host,
    const char* restrict port,
    int                  socktype,
    bool*                connected,
    bool                 nonblocking) {
    int              ret;
    platform_sock_t  sock = PLATFORM_SO_ERROR_INVALID_SOCKET;
    struct addrinfo* res;
    struct addrinfo* rp;
    struct addrinfo  hints = {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = socktype,
    };
    if (getaddrinfo(host, port, &hints, &res)) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }
    *connected = false;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            continue;
        }
        platform_socket_enable_nonblocking(sock, nonblocking);

        if (socktype == SOCK_STREAM) {
            platform_socket_enable_mss_clamp(sock, true);
            platform_socket_enable_nodelay(sock, true);
            platform_socket_enable_keepalive(sock, true);
            /* See platform_socket_accept() for sndbuf rationale. */
            platform_socket_set_sndbuf(sock, 256 * 1024);
        }
        do {
            ret = connect(sock, rp->ai_addr, rp->ai_addrlen);
        } while (ret == PLATFORM_SO_ERROR_SOCKET_ERROR && errno == EINTR);
        if (ret == PLATFORM_SO_ERROR_SOCKET_ERROR) {
            if (errno != EINPROGRESS) {
                platform_socket_close(sock);
                continue;
            }
            break;
        }
        *connected = true;
        break;
    }
    if (rp == NULL) {
        freeaddrinfo(res);
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return sock;
}

int platform_socket_get_socktype(platform_sock_t sock) {
    int       type = 0;
    socklen_t len  = sizeof(int);
    getsockopt(sock, SOL_SOCKET, SO_TYPE, &type, &len);
    return type;
}

ssize_t platform_socket_recv(platform_sock_t sock, void* buf, int size) {
    ssize_t n;
    do {
        n = recv(sock, buf, size, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EPIPE || errno == ENOTCONN)) {
        errno = ECONNRESET;
    }
    return n < 0 ? PLATFORM_SO_ERROR_SOCKET_ERROR : n;
}

ssize_t platform_socket_send(platform_sock_t sock, const void* buf, int size) {
    ssize_t n;
    do {
        n = send(sock, buf, size, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EPIPE || errno == ENOTCONN)) {
        errno = ECONNRESET;
    }
    return n < 0 ? PLATFORM_SO_ERROR_SOCKET_ERROR : n;
}

ssize_t platform_socket_recvfrom(
    platform_sock_t          sock,
    void*                    buf,
    int                      size,
    struct sockaddr_storage* ss,
    socklen_t*               sslen) {
    ssize_t n;
    do {
        n = recvfrom(sock, buf, size, 0, (struct sockaddr*)ss, sslen);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? PLATFORM_SO_ERROR_SOCKET_ERROR : n;
}

ssize_t platform_socket_sendto(
    platform_sock_t          sock,
    const void*              buf,
    int                      size,
    struct sockaddr_storage* ss,
    socklen_t                sslen) {
    ssize_t n;
    do {
        n = sendto(sock, buf, size, 0, (struct sockaddr*)ss, sslen);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? PLATFORM_SO_ERROR_SOCKET_ERROR : n;
}

int platform_socket_socketpair(
    int domain, int type, int protocol, platform_sock_t socks[2]) {
    (void)domain;
    return socketpair(AF_LOCAL, type, protocol, socks);
}

const char* platform_socket_tostring(int error) {
    static _Thread_local char buf[512];
    strerror_r(error, buf, sizeof(buf));
    return buf;
}

int platform_socket_get_lasterror(void) {
    return errno;
}

#if defined(__linux__)
void platform_socket_set_rss(platform_sock_t sock, uint16_t idx, int cores) {
    (void)idx;
    struct sock_filter bpf_code[] = {
        {BPF_LD | BPF_W | BPF_ABS, 0, 0, SKF_AD_OFF | SKF_AD_CPU},
        {BPF_ALU | BPF_MOD, 0, 0, cores},
        {BPF_RET | BPF_A, 0, 0, 0},
    };
    struct sock_fprog bpf_config = {
        .len    = sizeof(bpf_code) / sizeof(bpf_code[0]),
        .filter = bpf_code,
    };
    setsockopt(
        sock,
        SOL_SOCKET,
        SO_ATTACH_REUSEPORT_CBPF,
        (const void*)&bpf_config,
        sizeof(bpf_config));
}

int platform_socket_get_addressfamily(platform_sock_t sock) {
    int       af  = 0;
    socklen_t len = sizeof(int);
    getsockopt(sock, SOL_SOCKET, SO_DOMAIN, &af, &len);
    return af;
}

void platform_socket_enable_keepalive(platform_sock_t sock, bool on) {
    if (!on) {
        return;
    }
    int val = 1;
    int d = 60;
    int i = 1;  /* 1 second; same as default on win32 */
    int c = 10; /* 10 retries; same as hardcoded on win32 since vista */

    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (const void*)&val, sizeof(val));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, (const void*)&d, sizeof(d));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, (const void*)&i, sizeof(i));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, (const void*)&c, sizeof(c));
}

void platform_socket_enable_mss_clamp(platform_sock_t sock, bool on) {
    int af  = platform_socket_get_addressfamily(sock);
    int mss = on ? (af == AF_INET ? PLATFORM_TCPV4_MSS : PLATFORM_TCPV6_MSS) : 0;
    setsockopt(sock, IPPROTO_TCP, TCP_MAXSEG, (const void*)&mss, sizeof(int));
}

static int _socket_try_rcvbuf(platform_sock_t sock, int val) {
    /* FORCE bypasses net.core.rmem_max with CAP_NET_ADMIN. */
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE,
                   (const void*)&val, sizeof(val)) != 0
        && setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                      (const void*)&val, sizeof(val)) != 0) {
        return -1;
    }
    int       actual = 0;
    socklen_t len    = sizeof(actual);
    if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (void*)&actual, &len) == 0) {
        return actual;
    }
    return val;
}

int platform_socket_set_rcvbuf_max(platform_sock_t sock, int desired) {
    static const int ladder[] = {
        8 * 1024 * 1024,
        4 * 1024 * 1024,
        1 * 1024 * 1024,
    };
    if (desired <= 0) {
        desired = 16 * 1024 * 1024;
    }
    int rc = _socket_try_rcvbuf(sock, desired);
    if (rc >= 0) {
        return rc;
    }
    for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); ++i) {
        if (ladder[i] >= desired) {
            continue;
        }
        rc = _socket_try_rcvbuf(sock, ladder[i]);
        if (rc >= 0) {
            return rc;
        }
    }
    return -1;
}
#endif

#if defined(__APPLE__)
void platform_socket_set_rss(platform_sock_t sock, uint16_t idx, int cores) {
    (void)(sock);
    (void)(idx);
    (void)(cores);
}

int platform_socket_get_addressfamily(platform_sock_t sock) {
    struct sockaddr_storage ss;
    socklen_t              len = sizeof(ss);
    getsockname(sock, (struct sockaddr*)&ss, &len);
    return ss.ss_family;
}

void platform_socket_enable_keepalive(platform_sock_t sock, bool on) {
    if (!on) {
        return;
    }
    int val = 1;
    int d = 60;
    int i = 1;  /* 1 second; same as default on win32 */
    int c = 10; /* 10 retries; same as hardcoded on win32 since vista */

    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (const void*)&val, sizeof(val));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPALIVE, (const void*)&d, sizeof(d));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, (const void*)&i, sizeof(i));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, (const void*)&c, sizeof(c));
}

void platform_socket_enable_mss_clamp(platform_sock_t sock, bool on) {
    int val = on ? 1 : 0;
    setsockopt(sock, IPPROTO_TCP, TCP_NOOPT, (const void*)&val, sizeof(int));
}

static int _socket_try_rcvbuf(platform_sock_t sock, int val) {
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                   (const void*)&val, sizeof(val)) != 0) {
        return -1;
    }
    int       actual = 0;
    socklen_t len    = sizeof(actual);
    if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (void*)&actual, &len) == 0) {
        return actual;
    }
    return val;
}

int platform_socket_set_rcvbuf_max(platform_sock_t sock, int desired) {
    /* macOS has no SO_RCVBUFFORCE; kern.ipc.maxsockbuf clamps silently. */
    static const int ladder[] = {
        8 * 1024 * 1024,
        4 * 1024 * 1024,
        1 * 1024 * 1024,
    };
    if (desired <= 0) {
        desired = 16 * 1024 * 1024;
    }
    int rc = _socket_try_rcvbuf(sock, desired);
    if (rc >= 0) {
        return rc;
    }
    for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); ++i) {
        if (ladder[i] >= desired) {
            continue;
        }
        rc = _socket_try_rcvbuf(sock, ladder[i]);
        if (rc >= 0) {
            return rc;
        }
    }
    return -1;
}
#endif

platform_sock_t platform_socket_listen_unix(const char* path,
                                            bool nonblocking) {
    if (!path || !*path) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(addr.sun_path)) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    /* bind() fails with EADDRINUSE if a previous socket file remains. */
    remove(path);

    platform_sock_t sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) ==
        PLATFORM_SO_ERROR_SOCKET_ERROR) {
        platform_socket_close(sock);
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }

    if (listen(sock, SOMAXCONN) == PLATFORM_SO_ERROR_SOCKET_ERROR) {
        platform_socket_close(sock);
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }

    platform_socket_enable_nonblocking(sock, nonblocking);
    return sock;
}

platform_sock_t platform_socket_dial_unix(const char* path,
                                          bool* connected,
                                          bool nonblocking) {
    if (!path || !*path) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(addr.sun_path)) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    platform_sock_t sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }

    platform_socket_enable_nonblocking(sock, nonblocking);

    *connected = false;
    int ret;
    do {
        ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    } while (ret == PLATFORM_SO_ERROR_SOCKET_ERROR && errno == EINTR);

    if (ret == PLATFORM_SO_ERROR_SOCKET_ERROR) {
        if (errno != EINPROGRESS) {
            platform_socket_close(sock);
            return PLATFORM_SO_ERROR_INVALID_SOCKET;
        }
    } else {
        *connected = true;
    }

    return sock;
}
