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

#include "net/addr.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct datagram_s datagram_t;

#define DATAGRAM_IO_AGAIN (-2)

/**
 * @brief Wrap an already opened datagram fd.
 *
 * The datagram takes ownership of fd on success.
 *
 * @param fd         Datagram socket.
 * @param connected  Whether fd has a connected peer.
 * @param peer_addr  Connected peer address, required when connected is true.
 *
 * @return Datagram handle, or NULL on allocation failure.
 */
extern datagram_t* datagram_from_fd(
    platform_sock_t fd,
    bool            connected,
    const addr_t*   peer_addr);

/**
 * @brief Bind a UDP datagram endpoint.
 *
 * @param host  Bind host, or NULL for any.
 * @param port  Bind port.
 *
 * @return Datagram handle, or NULL on failure.
 */
extern datagram_t* datagram_listen(const char* host, uint16_t port);

/**
 * @brief Dial a UDP peer and create a connected datagram endpoint.
 *
 * @param host  Remote hostname or address.
 * @param port  Remote port.
 *
 * @return Datagram handle, or NULL on failure.
 */
extern datagram_t* datagram_dial(const char* host, uint16_t port);

/**
 * @brief Interrupt blocked datagram operations without releasing ownership.
 *
 * @param datagram  Datagram handle.
 */
extern void datagram_interrupt(datagram_t* datagram);

/**
 * @brief Release one datagram ownership reference.
 *
 * @param datagram  Datagram handle.
 */
extern void datagram_release(datagram_t* datagram);

/**
 * @brief Set the datagram read deadline.
 *
 * @param datagram     Datagram handle.
 * @param deadline_ms  Absolute xylem_utils_getnow(MSEC) timestamp, or 0 to clear.
 */
extern void datagram_set_read_deadline(
    datagram_t* datagram,
    uint64_t    deadline_ms);

/**
 * @brief Set the datagram write deadline.
 *
 * @param datagram     Datagram handle.
 * @param deadline_ms  Absolute xylem_utils_getnow(MSEC) timestamp, or 0 to clear.
 */
extern void datagram_set_write_deadline(
    datagram_t* datagram,
    uint64_t    deadline_ms);

/**
 * @brief Receive one datagram.
 *
 * @param datagram  Datagram handle.
 * @param buf       Destination buffer.
 * @param len       Maximum bytes to read.
 * @param from      Receives source address, or NULL.
 *
 * @return Bytes read, or -1 on error/timeout/close.
 */
extern int datagram_recv(
    datagram_t* datagram,
    void*       buf,
    int         len,
    addr_t*     from);

/**
 * @brief Try one non-blocking datagram receive.
 *
 * @param datagram  Datagram handle.
 * @param buf       Destination buffer.
 * @param len       Maximum bytes to read.
 * @param from      Receives source address, or NULL.
 *
 * @return Bytes read, DATAGRAM_IO_AGAIN on EAGAIN, or -1 on error/timeout.
 */
extern int datagram_try_recv(
    datagram_t* datagram,
    void*       buf,
    int         len,
    addr_t*     from);

/**
 * @brief Wait until the datagram socket becomes readable.
 *
 * @param datagram  Datagram handle.
 *
 * @return Readiness result.
 */
extern iowait_result_t datagram_wait_read(datagram_t* datagram);

/**
 * @brief Send one datagram.
 *
 * For connected endpoints, to must be NULL. For unconnected endpoints, to
 * must point at the destination address.
 *
 * @param datagram  Datagram handle.
 * @param data      Source buffer.
 * @param len       Number of bytes to send.
 * @param to        Destination address, or NULL for connected endpoint.
 *
 * @return 0 on success, -1 on error/timeout/close.
 */
extern int datagram_send(
    datagram_t* datagram,
    const void* data,
    int         len,
    const addr_t* to);

/**
 * @brief Try one non-blocking datagram send.
 *
 * For connected endpoints, to must be NULL. For unconnected endpoints, to
 * must point at the destination address.
 *
 * @param datagram  Datagram handle.
 * @param data      Source buffer.
 * @param len       Number of bytes to send.
 * @param to        Destination address, or NULL for connected endpoint.
 *
 * @return Bytes written, DATAGRAM_IO_AGAIN on EAGAIN, or -1 on error/timeout.
 */
extern int datagram_try_send(
    datagram_t*  datagram,
    const void*  data,
    int          len,
    const addr_t* to);

/**
 * @brief Wait until the datagram socket becomes writable.
 *
 * @param datagram  Datagram handle.
 *
 * @return Readiness result.
 */
extern iowait_result_t datagram_wait_write(datagram_t* datagram);

/**
 * @brief Get the connected peer address.
 *
 * @param datagram  Datagram handle.
 * @param host      Output host buffer.
 * @param host_len  Output host buffer length.
 * @param port      Output port pointer.
 *
 * @return 0 on success, -1 if unconnected or on failure.
 */
extern int datagram_remote_addr(
    datagram_t* datagram,
    char*       host,
    size_t      host_len,
    uint16_t*   port);

/**
 * @brief Get the datagram endpoint's local address.
 *
 * @param datagram  Datagram handle.
 * @param host      Output host buffer.
 * @param host_len  Output host buffer length.
 * @param port      Output port pointer.
 *
 * @return 0 on success, -1 on failure.
 */
extern int datagram_local_addr(
    datagram_t* datagram,
    char*       host,
    size_t      host_len,
    uint16_t*   port);

/**
 * @brief Return the platform socket owned by the datagram endpoint.
 *
 * @param datagram  Datagram handle.
 *
 * @return Platform socket handle.
 */
extern platform_sock_t datagram_fd(datagram_t* datagram);
