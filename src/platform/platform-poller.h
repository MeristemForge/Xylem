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

#if defined(__linux__)
#include <sys/epoll.h>
#endif

#if defined(__APPLE__)
#include <sys/event.h>
#endif

#if defined(__linux__) || defined(__APPLE__)
typedef int platform_poller_sq_t;
typedef int platform_poller_fd_t;
#endif

#if defined(_WIN32)
#include "platform-socket.h"
typedef HANDLE platform_poller_sq_t;
typedef SOCKET platform_poller_fd_t;
#endif

typedef enum platform_poller_op_e {
    PLATFORM_POLLER_NO_OP = 0,
    PLATFORM_POLLER_RD_OP = 1,
    PLATFORM_POLLER_WR_OP = 2,
    PLATFORM_POLLER_RW_OP = 3,
} platform_poller_op_t;

typedef struct platform_poller_cqe_s {
    platform_poller_op_t op;
    void*                ud;
} platform_poller_cqe_t;

/*
 * platform_poller_sqe_t is a persistent per-fd structure. The caller
 * allocates it, sets op/fd/ud, and passes it to add/mod/del. The same
 * sqe pointer must be used for all operations on that fd. The caller
 * is responsible for the sqe lifetime (must outlive the registration).
 *
 * All platforms use one-shot semantics: wait() delivers at most one
 * completion per sqe. The caller must call mod() to re-arm the poll
 * after processing an event.
 *
 * On Windows, internal IOCP/AFD state is embedded in the sqe via an
 * opaque reserved area. Do not touch _reserved.
 */
typedef struct platform_poller_sqe_s {
    platform_poller_op_t op;
    platform_poller_fd_t fd;
    void*                ud;
#if defined(_WIN32)
    char                 _reserved[256];
#endif
} platform_poller_sqe_t;

/**
 * @brief Initialize a poller instance.
 *
 * @param sq  Pointer to the poller handle to initialize.
 *
 * @return 0 on success, -1 on failure.
 */
extern int platform_poller_init(platform_poller_sq_t* sq);

/**
 * @brief Destroy a poller instance and release resources.
 *
 * @param sq  Pointer to the poller handle.
 */
extern void platform_poller_destroy(platform_poller_sq_t* sq);

/**
 * @brief Register a file descriptor with the poller.
 *
 * @param sq   Pointer to the poller handle.
 * @param sqe  Submission entry with op/fd/ud set by the caller.
 *
 * @return 0 on success, -1 on failure.
 */
extern int platform_poller_add(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe);

/**
 * @brief Modify (re-arm) a registered file descriptor.
 *
 * Must be called after wait() delivers a completion to re-arm the poll.
 *
 * @param sq   Pointer to the poller handle.
 * @param sqe  Submission entry with updated op.
 *
 * @return 0 on success, -1 on failure.
 */
extern int platform_poller_mod(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe);

/**
 * @brief Remove a file descriptor from the poller.
 *
 * @param sq   Pointer to the poller handle.
 * @param sqe  Submission entry identifying the fd to remove.
 *
 * @return 0 on success, -1 on failure.
 */
extern int platform_poller_del(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe);

/**
 * @brief Wait for I/O events.
 *
 * @param sq          Pointer to the poller handle.
 * @param cqe         Array to receive completion entries.
 * @param max_events  Maximum number of events to return (size of cqe array).
 * @param timeout     Timeout in milliseconds (-1 for infinite, 0 for non-blocking).
 *
 * @return Number of ready events (>= 0), or -1 on error.
 */
extern int platform_poller_wait(platform_poller_sq_t* sq,
                                platform_poller_cqe_t* cqe,
                                int max_events,
                                int timeout);
