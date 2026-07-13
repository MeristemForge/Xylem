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
#include <errno.h>
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
#define PLATFORM_SO_ERROR_ECONNREFUSED    ECONNREFUSED
#define PLATFORM_SO_ERROR_ETIMEDOUT       ETIMEDOUT
#define PLATFORM_SO_ERROR_ENETUNREACH     ENETUNREACH
#define PLATFORM_SO_ERROR_EHOSTUNREACH    EHOSTUNREACH
#define PLATFORM_SO_ERROR_INVALID_SOCKET  -1
#define PLATFORM_SO_ERROR_SOCKET_ERROR    -1
#define PLATFORM_SHUT_RD                  SHUT_RD
#define PLATFORM_SHUT_WR                  SHUT_WR
#define PLATFORM_SHUT_RDWR                SHUT_RDWR

typedef int platform_sock_t;
#endif

#if defined(_WIN32)
#undef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <mstcpip.h>
#include <ws2ipdef.h>

#define PLATFORM_SO_ERROR_EAGAIN          WSAEWOULDBLOCK
#define PLATFORM_SO_ERROR_EWOULDBLOCK     WSAEWOULDBLOCK
#define PLATFORM_SO_ERROR_ECONNRESET      WSAECONNRESET
#define PLATFORM_SO_ERROR_ECONNREFUSED    WSAECONNREFUSED
#define PLATFORM_SO_ERROR_ETIMEDOUT       WSAETIMEDOUT
#define PLATFORM_SO_ERROR_ENETUNREACH     WSAENETUNREACH
#define PLATFORM_SO_ERROR_EHOSTUNREACH    WSAEHOSTUNREACH
#define PLATFORM_SO_ERROR_INVALID_SOCKET  INVALID_SOCKET
#define PLATFORM_SO_ERROR_SOCKET_ERROR    SOCKET_ERROR
#define PLATFORM_SHUT_RD                  SD_RECEIVE
#define PLATFORM_SHUT_WR                  SD_SEND
#define PLATFORM_SHUT_RDWR                SD_BOTH

typedef SOCKET  platform_sock_t;
#ifndef _XYLEM_SSIZE_T
#define _XYLEM_SSIZE_T
typedef SSIZE_T ssize_t;
#endif

#pragma comment(lib, "ws2_32.lib")
#endif


/**
 * @brief Initialize the platform socket subsystem.
 *
 * On Windows, calls WSAStartup. No-op on Unix.
 */
extern void platform_socket_startup(void);

/**
 * @brief Clean up the platform socket subsystem.
 *
 * On Windows, calls WSACleanup. No-op on Unix.
 */
extern void platform_socket_cleanup(void);

/**
 * @brief Close a socket.
 *
 * @param sock  Socket to close.
 */
extern void platform_socket_close(platform_sock_t sock);

/**
 * @brief Receive data from a connected socket.
 *
 * @param sock  Connected socket.
 * @param buf   Buffer to receive into.
 * @param size  Maximum number of bytes to receive.
 *
 * @return Number of bytes received, 0 on connection closed, or -1 on error.
 */
extern ssize_t platform_socket_recv(platform_sock_t sock, void* buf, int size);

/**
 * @brief Send data on a connected socket.
 *
 * @param sock  Connected socket.
 * @param buf   Buffer containing data to send.
 * @param size  Number of bytes to send.
 *
 * @return Number of bytes sent, or -1 on error.
 */
extern ssize_t platform_socket_send(platform_sock_t sock, const void* buf, int size);


/**
 * @brief Receive data from an unconnected (datagram) socket.
 *
 * @param sock   Socket to receive from.
 * @param buf    Buffer to receive into.
 * @param size   Maximum number of bytes to receive.
 * @param ss     Pointer to receive the sender address.
 * @param sslen  Pointer to the address length (in/out).
 *
 * @return Number of bytes received, or -1 on error.
 */
extern ssize_t platform_socket_recvfrom(platform_sock_t sock, void* buf, int size, struct sockaddr_storage* ss, socklen_t* sslen);

/**
 * @brief Send data to a specific address (datagram socket).
 *
 * @param sock   Socket to send from.
 * @param buf    Buffer containing data to send.
 * @param size   Number of bytes to send.
 * @param ss     Pointer to the destination address.
 * @param sslen  Length of the destination address.
 *
 * @return Number of bytes sent, or -1 on error.
 */
extern ssize_t platform_socket_sendto(
    platform_sock_t                sock,
    const void*                    buf,
    int                            size,
    const struct sockaddr_storage* ss,
    socklen_t                      sslen);

/**
 * @brief Create a pair of connected sockets.
 *
 * On Unix uses socketpair(). On Windows emulates via loopback TCP.
 *
 * @param domain    Protocol family (ignored, hardcoded per platform).
 * @param type      Socket type (e.g. SOCK_STREAM).
 * @param protocol  Protocol (typically 0).
 * @param socks     Array of two sockets to receive the pair.
 *
 * @return 0 on success, -1 on failure.
 */
extern int platform_socket_socketpair(int domain, int type, int protocol, platform_sock_t socks[2]);

/**
 * @brief Convert a socket error code to a human-readable string.
 *
 * @param error  Platform-specific error code.
 *
 * @return Pointer to a static string describing the error.
 */
extern const char* platform_socket_tostring(int error);

/**
 * @brief Accept an incoming connection on a TCP listening socket.
 *
 * Applies TCP-only data-connection tuning to the accepted socket. For AF_UNIX
 * listeners use platform_socket_accept_unix() instead.
 *
 * @param sock         Listening socket.
 * @param nonblocking  If true, set the accepted socket to non-blocking mode.
 *
 * @return Accepted socket, or PLATFORM_SO_ERROR_INVALID_SOCKET on failure.
 */
extern platform_sock_t platform_socket_accept(platform_sock_t sock, bool nonblocking);

/**
 * @brief Check whether an accept error may succeed on a later attempt.
 *
 * @param error  Platform-specific socket error code.
 *
 * @return true if a later accept attempt may succeed, false otherwise.
 */
extern bool platform_socket_accept_should_retry(int error);

/**
 * @brief Create a listening (server) socket.
 *
 * @param host              Bind address (e.g. "0.0.0.0", "::").
 * @param port              Bind port (e.g. "8080").
 * @param socktype          SOCK_STREAM or SOCK_DGRAM.
 * @param nonblocking       If true, set the socket to non-blocking mode.
 * @param enable_mss_clamp  If true, clamp MSS before listening on a stream.
 *
 * @return Listening socket, or PLATFORM_SO_ERROR_INVALID_SOCKET on failure.
 */
extern platform_sock_t platform_socket_listen(
    const char* restrict host,
    const char* restrict port,
    int                  socktype,
    bool                 nonblocking,
    bool                 enable_mss_clamp);

/**
 * @brief Create a client socket and connect to a remote host.
 *
 * @param host              Remote host address.
 * @param port              Remote port.
 * @param socktype          SOCK_STREAM or SOCK_DGRAM.
 * @param connected         Pointer to receive connection status (true if
 *                          connected immediately, false if in progress).
 * @param nonblocking       If true, set the socket to non-blocking mode.
 * @param enable_mss_clamp  If true, clamp MSS before connecting a stream socket.
 *
 * @return Connected socket, or PLATFORM_SO_ERROR_INVALID_SOCKET on failure.
 */
extern platform_sock_t platform_socket_dial(
    const char* restrict host,
    const char* restrict port,
    int                  socktype,
    bool*                connected,
    bool                 nonblocking,
    bool                 enable_mss_clamp);

/**
 * @brief Set the receive timeout on a socket.
 *
 * @param sock        Socket to configure.
 * @param timeout_ms  Timeout in milliseconds (0 to disable).
 */
extern void platform_socket_set_rcvtimeout(platform_sock_t sock, int timeout_ms);

/**
 * @brief Set the send timeout on a socket.
 *
 * @param sock        Socket to configure.
 * @param timeout_ms  Timeout in milliseconds (0 to disable).
 */
extern void platform_socket_set_sndtimeout(platform_sock_t sock, int timeout_ms);

/**
 * @brief Set the receive buffer size (SO_RCVBUF).
 *
 * @param sock  Socket to configure.
 * @param val   Buffer size in bytes.
 */
extern void platform_socket_set_rcvbuf(platform_sock_t sock, int val);

/**
 * @brief Set the send buffer size (SO_SNDBUF).
 *
 * @param sock  Socket to configure.
 * @param val   Buffer size in bytes.
 */
extern void platform_socket_set_sndbuf(platform_sock_t sock, int val);

/**
 * @brief Best-effort raise of SO_RCVBUF with platform-aware fallbacks.
 *
 * Intended for protocols where larger receive buffers reduce packet drops
 * under burst (UDP, RUDP). Behavior per platform:
 *
 *  - Linux: first tries SO_RCVBUFFORCE (bypasses net.core.rmem_max but
 *    requires CAP_NET_ADMIN); falls back to SO_RCVBUF which the kernel
 *    will clamp to rmem_max. Operators should raise net.core.rmem_max
 *    for best effect on unprivileged processes.
 *  - macOS: uses SO_RCVBUF; kernel clamps silently per sysctl
 *    kern.ipc.maxsockbuf.
 *  - Windows: uses SO_RCVBUF; AFD enforces its own NonPagedPool cap.
 *
 * If the desired size is not accepted, progressively smaller fallbacks
 * are attempted (64/16/8/4/1 MB).
 *
 * @param sock     Socket to configure.
 * @param desired  Preferred buffer size in bytes; the kernel may grant
 *                 less. Pass <= 0 to use a sensible default (16 MB).
 * @return         Actual SO_RCVBUF value reported by getsockopt after
 *                 the call (may be larger than requested on Linux due
 *                 to the documented `val * 2` accounting), or -1 if
 *                 neither setsockopt path succeeded.
 */
extern int platform_socket_set_rcvbuf_max(platform_sock_t sock, int desired);

/**
 * @brief Configure RSS (Receive Side Scaling) on a socket.
 *
 * @param sock   Socket to configure.
 * @param idx    RSS queue index.
 * @param cores  Number of CPU cores.
 */
extern void platform_socket_set_rss(platform_sock_t sock, uint16_t idx, int cores);

/**
 * @brief Get the address family of a socket.
 *
 * @param sock  Socket to query.
 *
 * @return Address family (e.g. AF_INET, AF_INET6), or -1 on error.
 */
extern int platform_socket_get_addressfamily(platform_sock_t sock);

/**
 * @brief Get the socket type (SOCK_STREAM, SOCK_DGRAM, etc.).
 *
 * @param sock  Socket to query.
 *
 * @return Socket type, or -1 on error.
 */
extern int platform_socket_get_socktype(platform_sock_t sock);

/**
 * @brief Get the last socket error code.
 *
 * On Windows returns WSAGetLastError(), on Unix returns errno.
 *
 * @return Platform-specific error code.
 */
extern int platform_socket_get_lasterror(void);

/**
 * @brief Enable or disable TCP_NODELAY (Nagle's algorithm).
 *
 * @param sock  Socket to configure.
 * @param on    true to enable, false to disable.
 */
extern void platform_socket_enable_nodelay(platform_sock_t sock, bool on);

/**
 * @brief Enable or disable IPV6_V6ONLY.
 *
 * @param sock  Socket to configure.
 * @param on    true to enable, false to disable.
 */
extern void platform_socket_enable_v6only(platform_sock_t sock, bool on);

/**
 * @brief Enable or disable SO_KEEPALIVE.
 *
 * @param sock  Socket to configure.
 * @param on    true to enable, false to disable.
 */
extern void platform_socket_enable_keepalive(platform_sock_t sock, bool on);

/**
 * @brief Enable or disable MSS clamping to protocol minimum.
 *
 * @param sock  Socket to configure.
 * @param on    true to clamp MSS to minimum, false to use default.
 */
extern void platform_socket_enable_mss_clamp(platform_sock_t sock, bool on);

/**
 * @brief Enable or disable non-blocking mode on a socket.
 *
 * @param sock  Socket to configure.
 * @param on    true for non-blocking, false for blocking.
 */
extern void platform_socket_enable_nonblocking(platform_sock_t sock, bool on);

/**
 * @brief Enable or disable SO_REUSEADDR.
 *
 * @param sock  Socket to configure.
 * @param on    true to enable, false to disable.
 */
extern void platform_socket_enable_reuseaddr(platform_sock_t sock, bool on);

/**
 * @brief Enable or disable SO_REUSEPORT (Unix only, no-op on Windows).
 *
 * @param sock  Socket to configure.
 * @param on    true to enable, false to disable.
 */
extern void platform_socket_enable_reuseport(platform_sock_t sock, bool on);

/**
 * @brief Create a listening Unix domain socket.
 *
 * Unlinks the path first if it already exists, then creates an
 * AF_UNIX SOCK_STREAM socket, binds, and listens.
 *
 * @param path         Socket file path.
 * @param nonblocking  If true, set non-blocking mode.
 *
 * @return Listening socket, or PLATFORM_SO_ERROR_INVALID_SOCKET on failure.
 */
extern platform_sock_t platform_socket_listen_unix(const char* path,
                                                   bool nonblocking);

/**
 * @brief Accept an incoming connection on a Unix domain listening socket.
 *
 * Separate from platform_socket_accept() because the latter applies TCP-only
 * data-connection tuning.
 *
 * @param sock         Listening UDS socket.
 * @param nonblocking  If true, set the accepted socket to non-blocking mode.
 *
 * @return Accepted socket, or PLATFORM_SO_ERROR_INVALID_SOCKET on failure.
 */
extern platform_sock_t platform_socket_accept_unix(platform_sock_t sock,
                                                   bool nonblocking);

/**
 * @brief Connect to a Unix domain socket.
 *
 * Creates an AF_UNIX SOCK_STREAM socket and connects to the path.
 *
 * @param path         Socket file path.
 * @param connected    Pointer to receive connection status.
 * @param nonblocking  If true, set non-blocking mode.
 *
 * @return Socket, or PLATFORM_SO_ERROR_INVALID_SOCKET on failure.
 */
extern platform_sock_t platform_socket_dial_unix(const char* path,
                                                 bool* connected,
                                                 bool nonblocking);
