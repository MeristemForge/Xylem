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
#define PLATFORM_SO_ERROR_ETIMEDOUT       ETIMEDOUT
#define PLATFORM_SO_ERROR_INVALID_SOCKET  -1
#define PLATFORM_SO_ERROR_SOCKET_ERROR    -1
#define PLATFORM_SHUT_WR                  SHUT_WR

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
#define PLATFORM_SO_ERROR_ETIMEDOUT       WSAETIMEDOUT
#define PLATFORM_SO_ERROR_INVALID_SOCKET  INVALID_SOCKET
#define PLATFORM_SO_ERROR_SOCKET_ERROR    SOCKET_ERROR
#define PLATFORM_SHUT_WR                  SD_SEND

typedef SOCKET  platform_sock_t;
typedef SSIZE_T ssize_t;

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
 * @brief Receive exactly size bytes from a connected socket.
 *
 * Loops internally until all bytes are received or an error occurs.
 *
 * @param sock  Connected socket.
 * @param buf   Buffer to receive into.
 * @param size  Exact number of bytes to receive.
 *
 * @return Total bytes received, or -1 on error.
 */
extern ssize_t platform_socket_recvall(platform_sock_t sock, void* buf, int size);

/**
 * @brief Send exactly size bytes on a connected socket.
 *
 * Loops internally until all bytes are sent or an error occurs.
 *
 * @param sock  Connected socket.
 * @param buf   Buffer containing data to send.
 * @param size  Exact number of bytes to send.
 *
 * @return Total bytes sent, or -1 on error.
 */
extern ssize_t platform_socket_sendall(platform_sock_t sock, const void* buf, int size);

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
extern ssize_t platform_socket_sendto(platform_sock_t sock, const void* buf, int size, struct sockaddr_storage* ss, socklen_t sslen);

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
 * @brief Accept an incoming connection.
 *
 * @param sock         Listening socket.
 * @param nonblocking  If true, set the accepted socket to non-blocking mode.
 *
 * @return Accepted socket, or PLATFORM_SO_ERROR_INVALID_SOCKET on failure.
 */
extern platform_sock_t platform_socket_accept(platform_sock_t sock, bool nonblocking);

/**
 * @brief Create a listening (server) socket.
 *
 * @param host         Bind address (e.g. "0.0.0.0", "::").
 * @param port         Bind port (e.g. "8080").
 * @param socktype     SOCK_STREAM or SOCK_DGRAM.
 * @param nonblocking  If true, set the socket to non-blocking mode.
 *
 * @return Listening socket, or PLATFORM_SO_ERROR_INVALID_SOCKET on failure.
 */
extern platform_sock_t platform_socket_listen(const char* restrict host, const char* restrict port, int socktype, bool nonblocking);

/**
 * @brief Create a client socket and connect to a remote host.
 *
 * @param host         Remote host address.
 * @param port         Remote port.
 * @param socktype     SOCK_STREAM or SOCK_DGRAM.
 * @param connected    Pointer to receive connection status (true if connected
 *                     immediately, false if in progress for non-blocking).
 * @param nonblocking  If true, set the socket to non-blocking mode.
 *
 * @return Connected socket, or PLATFORM_SO_ERROR_INVALID_SOCKET on failure.
 */
extern platform_sock_t platform_socket_dial(const char* restrict host, const char* restrict port, int socktype, bool* connected, bool nonblocking);

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
