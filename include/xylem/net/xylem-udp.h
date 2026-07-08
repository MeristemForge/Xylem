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

#include <stddef.h>
#include <stdint.h>

typedef struct xylem_udp_chan_s xylem_udp_chan_t;

/**
 * @brief Create a bound UDP socket.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param host  Bind address (e.g. "0.0.0.0"), or NULL for any.
 * @param port  Bind port.
 *
 * @return UDP handle, or NULL on failure.
 */
extern xylem_udp_chan_t* xylem_udp_listen(const char* host, uint16_t port);

/**
 * @brief Create a connected UDP socket.
 *
 * @note [COROUTINE-ONLY]
 *
 * Calls connect() so subsequent send/recv use the default peer.
 *
 * The host parameter accepts either a numeric IP (IPv4/IPv6) or a
 * hostname. Hostnames are resolved via the runtime DNS resolver,
 * matching the behavior of xylem_tcp_dial / xylem_tls_dial.
 *
 * @param host  Remote hostname or IP address.
 * @param port  Remote port.
 *
 * @return UDP handle, or NULL on failure.
 */
extern xylem_udp_chan_t* xylem_udp_dial(const char* host, uint16_t port);

/**
 * @brief Receive a datagram.
 *
 * @note [COROUTINE-ONLY]
 *
 * Suspends the calling coroutine until a datagram arrives, the
 * deadline passes, or the handle is closed.
 *
 * For unconnected sockets, writes the sender address into host/port.
 * For connected sockets, host/port may be NULL.
 *
 * @param udp       UDP handle.
 * @param buf       Destination buffer.
 * @param len       Buffer size.
 * @param host      Buffer for sender IP (46 bytes recommended), or NULL.
 * @param host_len  Size of host buffer.
 * @param port      Receives sender port, or NULL.
 *
 * @return Bytes received (>=0), -1 on error.
 */
extern int xylem_udp_recv(
    xylem_udp_chan_t* udp,
    void*        buf,
    int          len,
    char*        host,
    size_t       host_len,
    uint16_t*    port);

/**
 * @brief Send a datagram.
 *
 * @note [COROUTINE-ONLY]
 *
 * Suspends the calling coroutine if the socket buffer is full until
 * writable, the deadline passes, or the handle is closed.
 *
 * For unconnected sockets, host/port specify the destination. The
 * host must be a numeric IP address (IPv4 or IPv6); hostnames are
 * not resolved.
 *
 * For connected sockets, host must be NULL.
 *
 * @param udp   UDP handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to send.
 * @param host  Destination IP (numeric), or NULL for connected socket.
 * @param port  Destination port (ignored when host is NULL).
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_udp_send(
    xylem_udp_chan_t* udp,
    const void*  data,
    int          len,
    const char*  host,
    uint16_t     port);

/**
 * @brief Set the read deadline.
 *
 * @note [COROUTINE-ONLY]
 *
 * Once the clock passes the deadline, in-flight and subsequent
 * xylem_udp_recv() calls return -1.
 *
 * @param udp          UDP handle.
 * @param deadline_ms  Absolute xylem_utils_getnow(MSEC) timestamp, or 0
 *                     to clear.
 */
extern void xylem_udp_set_read_deadline(
    xylem_udp_chan_t* udp, uint64_t deadline_ms);

/**
 * @brief Set the write deadline.
 *
 * @note [COROUTINE-ONLY]
 *
 * Once the clock passes the deadline, in-flight and subsequent
 * xylem_udp_send() calls return -1.
 *
 * @param udp          UDP handle.
 * @param deadline_ms  Absolute xylem_utils_getnow(MSEC) timestamp, or 0
 *                     to clear.
 */
extern void xylem_udp_set_write_deadline(
    xylem_udp_chan_t* udp, uint64_t deadline_ms);

/**
 * @brief Close the UDP handle.
 *
 * @note [COROUTINE-ONLY]
 *
 * Wakes any coroutine blocked in recv/send.
 * The UDP handle is invalid after this function returns.
 *
 * @param udp  UDP handle.
 */
extern void xylem_udp_close(xylem_udp_chan_t* udp);

/**
 * @brief Get the local bound address.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param udp       UDP handle.
 * @param host      Buffer for address string.
 * @param host_len  Size of host buffer.
 * @param port      Receives local port.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_udp_local_addr(
    xylem_udp_chan_t* udp,
    char*        host,
    size_t       host_len,
    uint16_t*    port);

/**
 * @brief Get the remote address (connected mode only).
 *
 * @note [COROUTINE-ONLY]
 *
 * @param udp       UDP handle.
 * @param host      Buffer for address string.
 * @param host_len  Size of host buffer.
 * @param port      Receives remote port.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_udp_remote_addr(
    xylem_udp_chan_t* udp,
    char*        host,
    size_t       host_len,
    uint16_t*    port);
