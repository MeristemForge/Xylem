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

_Pragma("once")

#include <stdbool.h>
#include <stdint.h>

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/filter.h>
#include <sys/syscall.h>
#endif

#define PLATFORM_SO_ERROR_EAGAIN          EAGAIN
#define PLATFORM_SO_ERROR_EWOULDBLOCK     EWOULDBLOCK
#define PLATFORM_SO_ERROR_ECONNRESET      ECONNRESET
#define PLATFORM_SO_ERROR_ETIMEDOUT       ETIMEDOUT
#define PLATFORM_SO_ERROR_INVALID_SOCKET  -1
#define PLATFORM_SO_ERROR_SOCKET_ERROR    -1

typedef int platform_sock_t;
#endif

#if defined(_WIN32)
#undef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN

#include <WS2tcpip.h>
#include <WinSock2.h>
#include <Windows.h>
#include <mstcpip.h>
#include <ws2ipdef.h>

#define PLATFORM_SO_ERROR_EAGAIN          WSAEWOULDBLOCK
#define PLATFORM_SO_ERROR_EWOULDBLOCK     WSAEWOULDBLOCK
#define PLATFORM_SO_ERROR_ECONNRESET      WSAECONNRESET
#define PLATFORM_SO_ERROR_ETIMEDOUT       WSAETIMEDOUT
#define PLATFORM_SO_ERROR_INVALID_SOCKET  INVALID_SOCKET
#define PLATFORM_SO_ERROR_SOCKET_ERROR    SOCKET_ERROR

typedef SOCKET  platform_sock_t;
typedef SSIZE_T ssize_t;

#pragma comment(lib, "ws2_32.lib")
#endif

extern void    platform_socket_startup(void);
extern void    platform_socket_cleanup(void);
extern void    platform_socket_close(platform_sock_t sock);
extern ssize_t platform_socket_recv(platform_sock_t sock, void* buf, int size);
extern ssize_t platform_socket_send(platform_sock_t sock, const void* buf, int size);
extern ssize_t platform_socket_recvall(platform_sock_t sock, void* buf, int size);
extern ssize_t platform_socket_sendall(platform_sock_t sock, const void* buf, int size);
extern ssize_t platform_socket_recvfrom(platform_sock_t sock, void* buf, int size, struct sockaddr_storage* ss, socklen_t* sslen);
extern ssize_t platform_socket_sendto(platform_sock_t sock, const void* buf, int size, struct sockaddr_storage* ss, socklen_t sslen);
extern int     platform_socket_socketpair(int domain, int type, int protocol, platform_sock_t socks[2]);
extern char*   platform_socket_tostring(int error);
extern platform_sock_t platform_socket_accept(platform_sock_t sock, bool nonblocking);
extern platform_sock_t platform_socket_listen(const char* restrict host, const char* restrict port, int protocol, int idx, int cores, bool nonblocking);
extern platform_sock_t platform_socket_dial(const char* restrict host, const char* restrict port, int protocol, bool* connected, bool nonblocking);

extern void platform_socket_set_rcvtimeout(platform_sock_t sock, int timeout_ms);
extern void platform_socket_set_sndtimeout(platform_sock_t sock, int timeout_ms);
extern void platform_socket_set_rcvbuf(platform_sock_t sock, int val);
extern void platform_socket_set_sndbuf(platform_sock_t sock, int val);
extern void platform_socket_set_rss(platform_sock_t sock, uint16_t idx, int cores);
extern int  platform_socket_get_addressfamily(platform_sock_t sock);
extern int  platform_socket_get_socktype(platform_sock_t sock);
extern int  platform_socket_get_lasterror(void);

extern void platform_socket_enable_nodelay(platform_sock_t sock, bool on);
extern void platform_socket_enable_v6only(platform_sock_t sock, bool on);
extern void platform_socket_enable_keepalive(platform_sock_t sock, bool on);
extern void platform_socket_enable_maxseg(platform_sock_t sock, bool on);
extern void platform_socket_enable_nonblocking(platform_sock_t sock, bool on);
extern void platform_socket_enable_reuseaddr(platform_sock_t sock, bool on);
extern void platform_socket_enable_reuseport(platform_sock_t sock, bool on);
