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

#include <afunix.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)

static atomic_flag initialized = ATOMIC_FLAG_INIT;

static inline void _socket_disable_udp_connreset(platform_sock_t sock) {
    int   on = 0;
    DWORD unused;
    WSAIoctl(
        sock,
        SIO_UDP_CONNRESET,
        &on,
        sizeof(int),
        NULL,
        0,
        &unused,
        NULL,
        NULL);
}

void platform_socket_set_rcvtimeout(platform_sock_t sock, int timeout_ms) {
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout_ms, sizeof(int));
}

void platform_socket_set_sndtimeout(platform_sock_t sock, int timeout_ms) {
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout_ms, sizeof(int));
}

void platform_socket_set_rcvbuf(platform_sock_t sock, int val) {
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&val, sizeof(int));
}

void platform_socket_set_sndbuf(platform_sock_t sock, int val) {
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&val, sizeof(int));
}

static int _socket_try_rcvbuf(platform_sock_t sock, int val) {
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                   (const char*)&val, sizeof(val)) != 0) {
        return -1;
    }
    int actual = 0;
    int len    = sizeof(actual);
    if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&actual, &len) == 0) {
        return actual;
    }
    return val;
}

/* AFD caps rcvbuf against NonPagedPool; step down until accepted. */
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

void platform_socket_enable_nodelay(platform_sock_t sock, bool on) {
    int val = on ? 1 : 0;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&val, sizeof(val));
}

void platform_socket_enable_v6only(platform_sock_t sock, bool on) {
    int val = on ? 1 : 0;
    setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&val, sizeof(int));
}

void platform_socket_set_rss(platform_sock_t sock, uint16_t idx, int cores) {
    (void)(cores);
    DWORD unused;
    WSAIoctl(
        sock,
        SIO_CPU_AFFINITY,
        &idx,
        sizeof(uint16_t),
        NULL,
        0,
        &unused,
        NULL,
        NULL);
}

void platform_socket_enable_keepalive(platform_sock_t sock, bool on) {
    if (!on) {
        return;
    }
    int val = 1;
    int d = 60;
    int i = 1;
    int c = 10;

    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (const char*)&val, sizeof(val));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, (const char*)&d, sizeof(d));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, (const char*)&i, sizeof(i));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, (const char*)&c, sizeof(c));
}

void platform_socket_enable_mss_clamp(platform_sock_t sock, bool on) {
    int af  = platform_socket_get_addressfamily(sock);
    int val = on ? IP_PMTUDISC_DONT : IP_PMTUDISC_DO;
    /* Windows lacks TCP_MAXSEG; PMTUD_DONT forces protocol-minimum MSS. */
    if (af == AF_INET) {
        setsockopt(
            sock, IPPROTO_IP, IP_MTU_DISCOVER, (const char*)&val, sizeof(int));
    } else if (af == AF_INET6) {
        setsockopt(
            sock,
            IPPROTO_IPV6,
            IPV6_MTU_DISCOVER,
            (const char*)&val,
            sizeof(int));
    }
}

void platform_socket_enable_nonblocking(platform_sock_t sock, bool on) {
    u_long val = on ? 1 : 0;
    ioctlsocket(sock, FIONBIO, &val);
}

void platform_socket_enable_reuseaddr(platform_sock_t sock, bool on) {
    int val = on ? 1 : 0;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&val, sizeof(val));
}

void platform_socket_enable_reuseport(platform_sock_t sock, bool on) {
    (void)sock;
    (void)on;
}

int platform_socket_get_addressfamily(platform_sock_t sock) {
    WSAPROTOCOL_INFOW info;
    socklen_t         len = sizeof(info);
    getsockopt(sock, SOL_SOCKET, SO_PROTOCOL_INFO, (char*)&info, &len);
    return info.iAddressFamily;
}

void platform_socket_startup(void) {
    if (!atomic_flag_test_and_set(&initialized)) {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
}

void platform_socket_cleanup(void) {
    if (atomic_flag_test_and_set(&initialized)) {
        atomic_flag_clear(&initialized);
        WSACleanup();
    }
}

void platform_socket_close(platform_sock_t sock) {
    closesocket(sock);
}

int platform_socket_get_socktype(platform_sock_t sock) {
    int type = 0;
    int len  = sizeof(int);
    getsockopt(sock, SOL_SOCKET, SO_TYPE, (char*)&type, &len);
    return type;
}

ssize_t platform_socket_recv(platform_sock_t sock, void* buf, int size) {
    return recv(sock, buf, size, 0);
}

ssize_t platform_socket_send(platform_sock_t sock, const void* buf, int size) {
    return send(sock, buf, size, 0);
}

ssize_t platform_socket_recvfrom(
    platform_sock_t          sock,
    void*                    buf,
    int                      size,
    struct sockaddr_storage* ss,
    socklen_t*               sslen) {
    return recvfrom(sock, buf, size, 0, (struct sockaddr*)ss, sslen);
}

ssize_t platform_socket_sendto(
    platform_sock_t          sock,
    const void*              buf,
    int                      size,
    struct sockaddr_storage* ss,
    socklen_t                sslen) {
    return sendto(sock, buf, size, 0, (struct sockaddr*)ss, sslen);
}

int platform_socket_socketpair(
    int domain, int type, int protocol, platform_sock_t socks[2]) {
    (void)domain;
    if (type != SOCK_STREAM || protocol != 0) {
        return -1;
    }

    SOCKADDR_IN addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    socklen_t addrlen = sizeof(addr);

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        return -1;
    }
    if (bind(srv, (SOCKADDR*)&addr, addrlen) ==
        PLATFORM_SO_ERROR_SOCKET_ERROR) {
        closesocket(srv);
        return -1;
    }
    if (getsockname(srv, (SOCKADDR*)&addr, &addrlen) ==
        PLATFORM_SO_ERROR_SOCKET_ERROR) {
        closesocket(srv);
        return -1;
    }
    if (listen(srv, 1) == PLATFORM_SO_ERROR_SOCKET_ERROR) {
        closesocket(srv);
        return -1;
    }
    SOCKET cli = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (cli == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        closesocket(srv);
        return -1;
    }
    if (connect(cli, (SOCKADDR*)&addr, addrlen) ==
        PLATFORM_SO_ERROR_SOCKET_ERROR) {
        closesocket(srv);
        closesocket(cli);
        return -1;
    }
    socks[0] = accept(srv, NULL, NULL);
    if (socks[0] == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        closesocket(srv);
        closesocket(cli);
        return -1;
    }
    closesocket(srv);
    socks[1] = cli;
    return 0;
}

const char* platform_socket_tostring(int error) {
    static _Thread_local char buffer[512];
    FormatMessage(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buffer,
        sizeof(buffer),
        NULL);
    return buffer;
}

int platform_socket_get_lasterror(void) {
    return WSAGetLastError();
}

static platform_sock_t _socket_accept(platform_sock_t sock, bool nonblocking) {
    platform_sock_t cli = accept(sock, NULL, NULL);
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
     * Default sndbuf (~8KB on Windows) forces EWOULDBLOCK on every large
     * send, causing excessive coroutine park/re-arm cycles.
     */
    platform_socket_set_sndbuf(cli, 256 * 1024);
    return cli;
}

platform_sock_t platform_socket_accept_unix(platform_sock_t sock,
                                            bool nonblocking) {
    return _socket_accept(sock, nonblocking);
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
            _socket_disable_udp_connreset(sock);
            platform_socket_set_rcvbuf_max(sock, 16 * 1024 * 1024);
        }
        if (bind(sock, rp->ai_addr, (int)rp->ai_addrlen) ==
            PLATFORM_SO_ERROR_SOCKET_ERROR) {
            platform_socket_close(sock);
            continue;
        }
        if (socktype == SOCK_STREAM) {
            if (listen(sock, SOMAXCONN) == PLATFORM_SO_ERROR_SOCKET_ERROR) {
                platform_socket_close(sock);
                continue;
            }
            platform_socket_enable_nodelay(sock, true);
            platform_socket_enable_keepalive(sock, true);
        }
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

platform_sock_t platform_socket_dial(
    const char* restrict host,
    const char* restrict port,
    int   socktype,
    bool* connected,
    bool  nonblocking) {
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
            platform_socket_enable_nodelay(sock, true);
            platform_socket_enable_keepalive(sock, true);
            /* See platform_socket_accept() for sndbuf rationale. */
            platform_socket_set_sndbuf(sock, 256 * 1024);
        }
        if (socktype == SOCK_DGRAM) {
            _socket_disable_udp_connreset(sock);
        }
        if (connect(sock, rp->ai_addr, (int)rp->ai_addrlen)) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
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

platform_sock_t platform_socket_listen_unix(const char* path,
                                            bool nonblocking) {
    if (!path || strlen(path) == 0) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};

    if (strlen(path) >= sizeof(addr.sun_path)) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }
    strncpy_s(addr.sun_path, sizeof(addr.sun_path), path,
              sizeof(addr.sun_path) - 1);

    /* bind() fails with WSAEADDRINUSE if a previous socket file remains. */
    remove(path);

    platform_sock_t sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }

    if (bind(sock, (struct sockaddr*)&addr, (int)sizeof(addr)) ==
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
    if (!path || strlen(path) == 0) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};

    if (strlen(path) >= sizeof(addr.sun_path)) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }
    strncpy_s(addr.sun_path, sizeof(addr.sun_path), path,
              sizeof(addr.sun_path) - 1);

    platform_sock_t sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }

    platform_socket_enable_nonblocking(sock, nonblocking);

    *connected = false;
    if (connect(sock, (struct sockaddr*)&addr, (int)sizeof(addr))) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            platform_socket_close(sock);
            return PLATFORM_SO_ERROR_INVALID_SOCKET;
        }
    } else {
        *connected = true;
    }

    return sock;
}
