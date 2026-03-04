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

#define PLATFORM_POLLER_CQE_NUM 64

#if defined(__linux__) || defined(__APPLE__)
typedef int platform_poller_sq_t;
#endif

#if defined(_WIN32)
#include "platform-socket.h"
typedef HANDLE platform_poller_sq_t;
#endif

#if defined(__linux__) || defined(__APPLE__)
typedef int platform_poller_fd_t;
#endif

#if defined(_WIN32)
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

typedef struct platform_poller_sqe_s {
    platform_poller_op_t op;
    platform_poller_fd_t fd;
    void*                ud;
} platform_poller_sqe_t;

extern void platform_poller_init(platform_poller_sq_t* sq);
extern void platform_poller_destroy(platform_poller_sq_t* sq);
extern void platform_poller_add(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe);
extern void platform_poller_mod(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe);
extern void platform_poller_del(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe);
extern int  platform_poller_wait(platform_poller_sq_t* sq, platform_poller_cqe_t* cqe, int timeout);
