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

#include "xylem/net/xylem-framing.h"

typedef int64_t (*framing_recv_fn)(void* ctx, void* buf, size_t len);
typedef int (*framing_send_fn)(void* ctx, const void* data, size_t len);

typedef struct framing_s {
    framing_recv_fn recv_fn;
    framing_send_fn send_fn;
    void*           ctx;
    int             fd;
    char*           read_buf;
    size_t          read_buf_cap;
    size_t          read_buf_pos;
    size_t          read_buf_len;
} framing_t;

/**
 * @brief Initialize a framing instance.
 *
 * @param f        Framing instance to initialize.
 * @param recv_fn  Raw recv callback for the underlying transport.
 * @param send_fn  Raw send callback for the underlying transport.
 * @param ctx      Opaque context passed to callbacks.
 * @param fd       Socket fd, used only for log messages.
 * @param buf_cap  Internal read buffer capacity (bytes).
 */
extern void framing_init(framing_t*      f,
                         framing_recv_fn recv_fn,
                         framing_send_fn send_fn,
                         void*           ctx,
                         int             fd,
                         size_t          buf_cap);

/**
 * @brief Deinitialize a framing instance, freeing its internal buffer.
 *
 * @param f  Framing instance.
 */
extern void framing_deinit(framing_t* f);

/**
 * @brief Receive a framed message.
 *
 * For NONE: reads directly into buf via recv_fn.
 * For FIXED/LENGTH/DELIMITER: lazily allocates the internal read buffer
 * and dispatches to the corresponding framing decoder.
 *
 * @param f     Framing instance.
 * @param opts  Framing options.
 * @param buf   Destination buffer.
 * @param len   Destination buffer size.
 *
 * @return Bytes received on success, -1 on error.
 */
extern int64_t framing_recv(framing_t*                  f,
                            const xylem_framing_opts_t* opts,
                            void*                       buf,
                            size_t                      len);

/**
 * @brief Send a framed message.
 *
 * For NONE: sends payload directly via send_fn.
 * For FIXED: validates len == frame size, then sends.
 * For LENGTH: encodes length header + sends header + payload.
 * For DELIMITER: sends payload + delimiter.
 *
 * @param f     Framing instance.
 * @param opts  Framing options.
 * @param data  Payload to send.
 * @param len   Payload length.
 *
 * @return 0 on success, -1 on error.
 */
extern int framing_send(framing_t*                  f,
                        const xylem_framing_opts_t* opts,
                        const void*                 data,
                        size_t                      len);
