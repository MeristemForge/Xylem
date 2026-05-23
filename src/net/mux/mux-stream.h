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

#include "mux-frame.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct mco_coro    mco_coro;
typedef struct xylem_mux_s xylem_mux_t;

#define MUX_DEFAULT_WINDOW 262144  /* 256KB */

typedef enum {
    MUX_STREAM_INIT,
    MUX_STREAM_SYN_SENT,
    MUX_STREAM_ESTABLISHED,
    MUX_STREAM_LOCAL_CLOSE,
    MUX_STREAM_REMOTE_CLOSE,
    MUX_STREAM_CLOSED
} _mux_stream_state_t;

struct xylem_mux_stream_s {
    xylem_mux_t*        mux;
    uint32_t            id;
    _mux_stream_state_t state;
    /* recv side */
    uint8_t*            recv_buf;
    size_t              recv_len;
    size_t              recv_cap;
    uint32_t            recv_window;
    _Atomic(mco_coro*)  recv_park;
    /* send side */
    uint32_t            send_window;
    _Atomic(mco_coro*)  send_park;
    /* deadline */
    uint64_t            rd_deadline;
    uint64_t            wr_deadline;
    _Atomic bool        closed;
    _Atomic int32_t     refcnt;
};

extern struct xylem_mux_stream_s* mux_stream_create(
    xylem_mux_t* mux, uint32_t id, uint32_t window);
extern void mux_stream_ref(struct xylem_mux_stream_s* s);
extern void mux_stream_unref(struct xylem_mux_stream_s* s);
extern void mux_stream_push_data(
    struct xylem_mux_stream_s* s, const void* data, size_t len);
extern void mux_stream_update_send_window(
    struct xylem_mux_stream_s* s, uint32_t delta);
extern void mux_stream_notify_remote_fin(struct xylem_mux_stream_s* s);
extern void mux_stream_notify_reset(struct xylem_mux_stream_s* s);
