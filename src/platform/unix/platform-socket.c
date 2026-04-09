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
#include <string.h>

#define TCPv4_MSS 536
#define TCPv6_MSS 1220

void platform_socket_enable_nonblocking(platform_sock_t sock, bool on) {
    int flag = fcntl(sock, F_GETFL, 0);
    if (flag == -1) {
        return;
    }
    if (on) {
        fcntl(sock, F_SETFL, flag | O_NONBLOCK);
    } else {
        fcntl(sock, F_SETFL, flag & ~O_NONBLOCK);
    }
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

platform_sock_t platform_socket_accept(platform_sock_t sock, bool nonblocking) {
    platform_sock_t cli;
    do {
        cli = accept(sock, NULL, NULL);
    } while (cli == PLATFORM_SO_ERROR_INVALID_SOCKET && errno == EINTR);
    if (cli == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }
    platform_socket_enable_nonblocking(cli, nonblocking);
    return cli;
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
    int                  protocol,
    bool                 nonblocking) {
    platform_sock_t  sock;
    struct addrinfo  hints;
    struct addrinfo* res;
    struct addrinfo* rp;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = protocol;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_protocol = 0;
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

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
        if (protocol == SOCK_DGRAM) {
            platform_socket_set_rcvbuf(sock, INT32_MAX);
        }
        if (bind(sock, rp->ai_addr, rp->ai_addrlen) ==
            PLATFORM_SO_ERROR_SOCKET_ERROR) {
            platform_socket_close(sock);
            continue;
        }
        /* these options inherited by connection-socket */
        if (protocol == SOCK_STREAM) {
            if (listen(sock, SOMAXCONN) == PLATFORM_SO_ERROR_SOCKET_ERROR) {
                platform_socket_close(sock);
                continue;
            }
            platform_socket_enable_maxseg(sock, true);
            /* must be after enable maxseg due to TCP_NOOPT on macOS */
            platform_socket_enable_nodelay(sock, true);
            platform_socket_enable_keepalive(sock, true);
        }
        /* this option not inherited by connection-socket */
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
    int                  protocol,
    bool*                connected,
    bool                 nonblocking) {
    int              ret;
    platform_sock_t  sock = PLATFORM_SO_ERROR_INVALID_SOCKET;
    struct addrinfo  hints;
    struct addrinfo* res;
    struct addrinfo* rp;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = protocol;
    hints.ai_flags = 0;
    hints.ai_protocol = 0;
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;
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

        if (protocol == SOCK_STREAM) {
            platform_socket_enable_maxseg(sock, true);
            platform_socket_enable_nodelay(sock, true);
            platform_socket_enable_keepalive(sock, true);
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
    int       type;
    socklen_t len;

    len = sizeof(int);
    getsockopt(sock, SOL_SOCKET, SO_TYPE, &type, (socklen_t*)&len);
    return type;
}

ssize_t platform_socket_recv(platform_sock_t sock, void* buf, int size) {
    ssize_t n;
    do {
        n = recv(sock, buf, size, 0);
    } while (n == PLATFORM_SO_ERROR_SOCKET_ERROR && errno == EINTR);
    if (n == PLATFORM_SO_ERROR_SOCKET_ERROR) {
        return PLATFORM_SO_ERROR_SOCKET_ERROR;
    }
    return n;
}

ssize_t platform_socket_send(platform_sock_t sock, const void* buf, int size) {
    ssize_t n;
    do {
        n = send(sock, buf, size, 0);
    } while (n == PLATFORM_SO_ERROR_SOCKET_ERROR && errno == EINTR);
    if (n == PLATFORM_SO_ERROR_SOCKET_ERROR) {
        return PLATFORM_SO_ERROR_SOCKET_ERROR;
    }
    return n;
}

ssize_t platform_socket_recvall(platform_sock_t sock, void* buf, int size) {
    ssize_t off = 0;
    while (off < size) {
        ssize_t tmp;
        do {
            tmp = recv(sock, (char*)buf + off, size - (int)off, 0);
        } while (tmp == PLATFORM_SO_ERROR_SOCKET_ERROR && errno == EINTR);
        if (tmp == PLATFORM_SO_ERROR_SOCKET_ERROR) {
            return PLATFORM_SO_ERROR_SOCKET_ERROR;
        }
        if (tmp == 0) {
            return off;
        }
        off += tmp;
    }
    return off;
}

ssize_t platform_socket_sendall(platform_sock_t sock, const void* buf, int size) {
    ssize_t off = 0;
    while (off < size) {
        ssize_t tmp;
        do {
            tmp = send(sock, (const char*)buf + off, size - (int)off, 0);
        } while (tmp == PLATFORM_SO_ERROR_SOCKET_ERROR && errno == EINTR);
        if (tmp == PLATFORM_SO_ERROR_SOCKET_ERROR) {
            return PLATFORM_SO_ERROR_SOCKET_ERROR;
        }
        off += tmp;
    }
    return off;
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
    } while (n == PLATFORM_SO_ERROR_SOCKET_ERROR && errno == EINTR);
    if (n == PLATFORM_SO_ERROR_SOCKET_ERROR) {
        return PLATFORM_SO_ERROR_SOCKET_ERROR;
    }
    return n;
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
    } while (n == PLATFORM_SO_ERROR_SOCKET_ERROR && errno == EINTR);
    if (n == PLATFORM_SO_ERROR_SOCKET_ERROR) {
        return PLATFORM_SO_ERROR_SOCKET_ERROR;
    }
    return n;
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
    (void)(idx);
    struct sock_filter bpf_code[] = {
        {BPF_LD | BPF_W | BPF_ABS, 0, 0, SKF_AD_OFF | SKF_AD_CPU},
        {BPF_ALU | BPF_MOD, 0, 0, cores},
        {BPF_RET | BPF_A, 0, 0, 0}};

    struct sock_fprog bpf_config = {0};
    bpf_config.len = (sizeof(bpf_code) / sizeof(bpf_code[0]));
    bpf_config.filter = bpf_code;

    setsockopt(
        sock,
        SOL_SOCKET,
        SO_ATTACH_REUSEPORT_CBPF,
        (const void*)&bpf_config,
        sizeof(bpf_config));
}

int platform_socket_get_addressfamily(platform_sock_t sock) {
    int       af;
    socklen_t len;
    len = sizeof(int);
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

void platform_socket_enable_maxseg(platform_sock_t sock, bool on) {
    int af = platform_socket_get_addressfamily(sock);
    if (on) {
        int mss = af == AF_INET ? TCPv4_MSS : TCPv6_MSS;
        setsockopt(
            sock, IPPROTO_TCP, TCP_MAXSEG, (const void*)&mss, sizeof(int));
    } else {
        /* Reset to 0 lets the kernel use its default MSS. */
        int mss = 0;
        setsockopt(
            sock, IPPROTO_TCP, TCP_MAXSEG, (const void*)&mss, sizeof(int));
    }
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
    socklen_t               len;

    len = sizeof(struct sockaddr_storage);
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

void platform_socket_enable_maxseg(platform_sock_t sock, bool on) {
    int val = on ? 1 : 0;
    setsockopt(sock, IPPROTO_TCP, TCP_NOOPT, (const void*)&val, sizeof(int));
}
#endif
