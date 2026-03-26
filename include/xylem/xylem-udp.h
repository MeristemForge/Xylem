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

#include "xylem/xylem-addr.h"
#include "xylem/xylem-loop.h"

typedef struct xylem_udp_s xylem_udp_t;

typedef struct xylem_udp_handler_s {
    void (*on_read)(xylem_udp_t* udp, void* data, size_t len,
                    xylem_addr_t* addr);
    void (*on_close)(xylem_udp_t* udp, int err);
} xylem_udp_handler_t;

/**
 * @brief Bind a UDP socket and start receiving.
 *
 * Creates a non-blocking UDP socket, binds to the specified address,
 * and registers with the event loop. Incoming datagrams trigger
 * handler->on_read.
 *
 * @param loop     Event loop.
 * @param addr     Bind address.
 * @param handler  Event callback set.
 *
 * @return UDP handle, or NULL on failure.
 */
extern xylem_udp_t* xylem_udp_listen(xylem_loop_t* loop,
                                    xylem_addr_t* addr,
                                    xylem_udp_handler_t* handler);

/**
 * @brief Create a connected UDP socket.
 *
 * Creates a non-blocking UDP socket connected to the specified
 * remote address. Subsequent sends use xylem_udp_send() with
 * dest=NULL. Incoming datagrams from the connected peer trigger
 * handler->on_read.
 *
 * @param loop     Event loop.
 * @param addr     Remote address to connect to.
 * @param handler  Event callback set.
 *
 * @return UDP handle, or NULL on failure.
 */
extern xylem_udp_t* xylem_udp_dial(xylem_loop_t* loop,
                                    xylem_addr_t* addr,
                                    xylem_udp_handler_t* handler);

/**
 * @brief Send a UDP datagram.
 *
 * If dest is NULL, sends on a connected socket (created with
 * xylem_udp_dial()). Otherwise sends to the specified address.
 *
 * @param udp   UDP handle.
 * @param dest  Destination address, or NULL for connected sockets.
 * @param data  Data to send.
 * @param len   Data length in bytes.
 *
 * @return Number of bytes sent, or -1 on failure.
 *
 * @note Not thread-safe. Must be called from the loop thread.
 *       Use xylem_loop_post() to send from other threads.
 */
extern int xylem_udp_send(xylem_udp_t* udp, xylem_addr_t* dest,
                          const void* data, size_t len);

/**
 * @brief Close a UDP handle.
 *
 * Closes the socket, unregisters from the event loop, stops all
 * timers, and calls handler->on_close.
 *
 * @param udp  UDP handle.
 *
 * @note Not thread-safe. Must be called from the loop thread.
 */
extern void xylem_udp_close(xylem_udp_t* udp);

/**
 * @brief Get user data attached to a UDP handle.
 *
 * @param udp  UDP handle.
 *
 * @return User data pointer.
 */
extern void* xylem_udp_get_userdata(xylem_udp_t* udp);

/**
 * @brief Set user data on a UDP handle.
 *
 * @param udp  UDP handle.
 * @param ud   User data pointer.
 */
extern void xylem_udp_set_userdata(xylem_udp_t* udp, void* ud);
