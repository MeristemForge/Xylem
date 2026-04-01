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

#include "xylem/xylem-loop.h"
#include "platform/platform-poller.h"

typedef struct xylem_loop_io_s xylem_loop_io_t;

/**
 * @brief Callback invoked when an I/O event fires.
 *
 * @param loop     Loop handle.
 * @param io       I/O handle that triggered.
 * @param revents  Event mask (PLATFORM_POLLER_RD_OP, WR_OP, or both).
 * @param ud       User data pointer from xylem_loop_start_io().
 */
typedef void (*xylem_loop_io_fn_t)(xylem_loop_t* loop,
                                   xylem_loop_io_t* io,
                                   platform_poller_op_t revents,
                                   void* ud);

/**
 * @brief Create an I/O handle and bind it to a file descriptor.
 *
 * Does not start polling. Call xylem_loop_start_io() to begin.
 *
 * @param loop  Loop handle.
 * @param fd    File descriptor (socket) to monitor.
 *
 * @return I/O handle, or NULL on failure.
 */
extern xylem_loop_io_t* xylem_loop_create_io(xylem_loop_t* loop,
                                             platform_poller_fd_t fd);

/**
 * @brief Destroy an I/O handle.
 *
 * Stops polling if active and frees the handle. Decrements the
 * loop active handle count.
 *
 * @param io  I/O handle.
 */
extern void xylem_loop_destroy_io(xylem_loop_io_t* io);

/**
 * @brief Start or update polling on an I/O handle.
 *
 * Registers the fd in the poller with the given operation mask.
 *
 * @param io  I/O handle.
 * @param op  Event interest (PLATFORM_POLLER_RD_OP, WR_OP, or both).
 * @param cb  Callback invoked when the event fires.
 * @param ud  User data pointer passed to the callback.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_start_io(xylem_loop_io_t* io,
                               platform_poller_op_t op,
                               xylem_loop_io_fn_t cb,
                               void* ud);

/**
 * @brief Stop polling on an I/O handle.
 *
 * Removes the fd from the poller. The handle remains valid and can
 * be re-started with xylem_loop_start_io().
 *
 * @param io  I/O handle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_stop_io(xylem_loop_io_t* io);
