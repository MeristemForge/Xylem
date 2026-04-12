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
#include "xylem/xylem-tcp.h"

#include <stddef.h>

/**
 * Transport-level callbacks. The WS layer sets these; the transport
 * implementation invokes them by forwarding from the underlying
 * xylem_tcp / xylem_tls handler callbacks.
 */
typedef struct {
    void (*on_connect)(void* handle, void* ctx);
    void (*on_accept)(void* handle, void* ctx);
    void (*on_read)(void* handle, void* ctx, void* data, size_t len);
    void (*on_write_done)(void* handle, void* ctx,
                          const void* data, size_t len, int status);
    void (*on_close)(void* handle, void* ctx, int err, const char* errmsg);
} ws_transport_cb_t;

/**
 * Virtual function table for a transport (TCP or TLS).
 *
 * Each function operates on an opaque handle returned by dial/listen.
 * The WS module never touches xylem_tcp or xylem_tls types directly.
 */
typedef struct {
    void* (*dial)(xylem_loop_t* loop, xylem_addr_t* addr,
                  ws_transport_cb_t* cb, void* ctx,
                  xylem_tcp_opts_t* opts);
    void* (*listen)(xylem_loop_t* loop, xylem_addr_t* addr,
                    ws_transport_cb_t* cb, void* ctx,
                    xylem_tcp_opts_t* opts,
                    const char* tls_cert, const char* tls_key);
    int   (*send)(void* handle, const void* data, size_t len);
    void  (*close_conn)(void* handle);
    void  (*close_server)(void* handle);
    void  (*set_userdata)(void* handle, void* ud);
    void* (*get_userdata)(void* handle);
    const xylem_addr_t* (*get_peer_addr)(void* handle);
} ws_transport_vt_t;

/**
 * @brief Get the TCP transport vtable.
 *
 * @return TCP transport vtable. Always available.
 */
extern const ws_transport_vt_t* ws_transport_tcp(void);

/**
 * @brief Get the TLS transport vtable.
 *
 * @return TLS transport vtable, or NULL when the library was built
 *         without XYLEM_ENABLE_TLS.
 */
extern const ws_transport_vt_t* ws_transport_tls(void);
