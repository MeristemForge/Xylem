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

#include "xylem/net/xylem-http.h"
#include "xylem/net/xylem-tls.h"

/* --- HTTPS Server --- */

typedef struct xylem_https_srv_s xylem_https_srv_t;

typedef struct {
    size_t                  max_body_size;     /**< 0 = default 1 MiB. */
    uint64_t                idle_timeout_ms;   /**< 0 = default 60000 ms. */
    uint64_t                header_timeout_ms; /**< 0 = default 10000 ms. */
    uint64_t                write_timeout_ms;  /**< 0 = default 30000 ms. */
    uint64_t                max_requests;      /**< 0 = unlimited. */
    xylem_http_handler_fn_t on_upgrade;        /**< Upgrade handler, NULL = reject 501. */
    void*                   upgrade_userdata;  /**< Passed to on_upgrade. */
} xylem_https_srv_opts_t;

extern xylem_https_srv_t* xylem_https_listen(
    const char*                        host,
    uint16_t                           port,
    xylem_http_handler_fn_t            handler,
    void*                              userdata,
    xylem_tls_ctx_t*                   tls_ctx,
    const xylem_https_srv_opts_t* opts);

extern void     xylem_https_close(xylem_https_srv_t* listener);
extern uint16_t xylem_https_srv_port(xylem_https_srv_t* listener);
extern void     xylem_https_srv_set_gzip(xylem_https_srv_t* listener,
                                              const xylem_http_gzip_opts_t* opts);

/* --- HTTPS Client --- */

extern xylem_http_res_t* xylem_https_get(const char* url,
                                         xylem_tls_ctx_t* tls_ctx,
                                         const xylem_http_opts_t* opts);

extern xylem_http_res_t* xylem_https_post(const char* url,
                                          const void* body, size_t body_len,
                                          const char* content_type,
                                          xylem_tls_ctx_t* tls_ctx,
                                          const xylem_http_opts_t* opts);

extern xylem_http_res_t* xylem_https_put(const char* url,
                                         const void* body, size_t body_len,
                                         const char* content_type,
                                         xylem_tls_ctx_t* tls_ctx,
                                         const xylem_http_opts_t* opts);

extern xylem_http_res_t* xylem_https_delete(const char* url,
                                            xylem_tls_ctx_t* tls_ctx,
                                            const xylem_http_opts_t* opts);

extern xylem_http_res_t* xylem_https_patch(const char* url,
                                           const void* body, size_t body_len,
                                           const char* content_type,
                                           xylem_tls_ctx_t* tls_ctx,
                                           const xylem_http_opts_t* opts);
