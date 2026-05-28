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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Common types --- */

typedef struct {
    const char* name;
    const char* value;
} xylem_http_hdr_t;

typedef struct xylem_http_req_s    xylem_http_req_t;
typedef struct xylem_http_res_s    xylem_http_res_t;
typedef struct xylem_http_srv_s xylem_http_srv_t;
typedef struct xylem_http_router_s xylem_http_router_t;
typedef struct xylem_http_cookie_jar_s xylem_http_cookie_jar_t;
typedef struct xylem_http_multipart_s  xylem_http_multipart_t;

/* --- Handler types --- */

typedef void (*xylem_http_handler_fn_t)(
    xylem_http_res_t* res,
    xylem_http_req_t* req,
    void*             userdata);

typedef int (*xylem_http_middleware_fn_t)(
    xylem_http_res_t* res,
    xylem_http_req_t* req,
    void*             userdata);

/* --- Server --- */

typedef struct {
    size_t                  max_body_size;
    uint64_t                idle_timeout_ms;
    xylem_http_handler_fn_t on_upgrade;
    void*                   upgrade_userdata;
} xylem_http_srv_opts_t;

extern xylem_http_srv_t* xylem_http_listen(
    const char*                       host,
    uint16_t                          port,
    xylem_http_handler_fn_t           handler,
    void*                             userdata,
    const xylem_http_srv_opts_t* opts);

extern void xylem_http_close(xylem_http_srv_t* listener);

/**
 * @brief Get the listening port of the server.
 *
 * Useful when the server was started with port 0 (OS-assigned).
 *
 * @param srv  Server handle.
 *
 * @return Port number, or 0 on error.
 */
extern uint16_t xylem_http_srv_port(xylem_http_srv_t* listener);

/* --- Server: Gzip --- */

typedef struct {
    bool        enabled;
    int         level;
    size_t      min_size;
    const char* mime_types;
} xylem_http_gzip_opts_t;

extern void xylem_http_srv_set_gzip(xylem_http_srv_t* listener,
                                         const xylem_http_gzip_opts_t* opts);

/* --- Request accessors --- */

extern const char*  xylem_http_req_method(const xylem_http_req_t* req);
extern const char*  xylem_http_req_url(const xylem_http_req_t* req);
extern const char*  xylem_http_req_header(const xylem_http_req_t* req,
                                          const char* name);
extern const void*  xylem_http_req_body(const xylem_http_req_t* req);
extern size_t       xylem_http_req_body_len(const xylem_http_req_t* req);
extern const char*  xylem_http_req_param(const xylem_http_req_t* req,
                                         const char* name);

/* --- Response (server write + client read) --- */

extern int  xylem_http_res_set_status(xylem_http_res_t* res, int code);
extern int  xylem_http_res_set_header(xylem_http_res_t* res,
                                      const char* name, const char* value);
extern int  xylem_http_res_write(xylem_http_res_t* res,
                                 const void* data, size_t len);
extern void xylem_http_res_close(xylem_http_res_t* res);
extern int  xylem_http_res_upgrade(xylem_http_res_t* res, void** transport);

extern int          xylem_http_res_status(const xylem_http_res_t* res);
extern const char*  xylem_http_res_header(const xylem_http_res_t* res,
                                          const char* name);
extern const void*  xylem_http_res_body(const xylem_http_res_t* res);
extern size_t       xylem_http_res_body_len(const xylem_http_res_t* res);
extern void         xylem_http_res_destroy(xylem_http_res_t* res);

/* --- Client --- */

typedef struct {
    uint64_t                 timeout_ms;
    int                      max_redirects;
    size_t                   max_body_size;
    const xylem_http_hdr_t*  headers;
    size_t                   header_count;
    const char*              range;
    xylem_http_cookie_jar_t* cookie_jar;
} xylem_http_opts_t;

extern xylem_http_res_t* xylem_http_get(const char* url,
                                        const xylem_http_opts_t* opts);
extern xylem_http_res_t* xylem_http_post(const char* url,
                                         const void* body, size_t body_len,
                                         const char* content_type,
                                         const xylem_http_opts_t* opts);
extern xylem_http_res_t* xylem_http_put(const char* url,
                                        const void* body, size_t body_len,
                                        const char* content_type,
                                        const xylem_http_opts_t* opts);
extern xylem_http_res_t* xylem_http_delete(const char* url,
                                           const xylem_http_opts_t* opts);
extern xylem_http_res_t* xylem_http_patch(const char* url,
                                          const void* body, size_t body_len,
                                          const char* content_type,
                                          const xylem_http_opts_t* opts);

/* --- Router --- */

extern xylem_http_router_t* xylem_http_router_create(void);
extern void xylem_http_router_destroy(xylem_http_router_t* r);
extern int  xylem_http_router_add(xylem_http_router_t* r,
                                  const char* method,
                                  const char* pattern,
                                  xylem_http_handler_fn_t handler,
                                  void* userdata);
extern int  xylem_http_router_use(xylem_http_router_t* r,
                                  xylem_http_middleware_fn_t mw,
                                  void* userdata);
extern int  xylem_http_router_dispatch(xylem_http_router_t* r,
                                       xylem_http_res_t* res,
                                       xylem_http_req_t* req);

/* --- Static file server --- */

typedef struct {
    const char* root;
    const char* index_file;
    int         max_age;
    bool        precompressed;
} xylem_http_static_opts_t;

extern int xylem_http_static_serve(xylem_http_router_t* r,
                                   const char* prefix,
                                   const xylem_http_static_opts_t* opts);

/* --- Cookie jar --- */

extern xylem_http_cookie_jar_t* xylem_http_cookie_jar_create(void);
extern void xylem_http_cookie_jar_destroy(xylem_http_cookie_jar_t* jar);

/* --- Utilities --- */

extern char* xylem_http_url_encode(const char* src, size_t src_len,
                                   size_t* out_len);
extern char* xylem_http_url_decode(const char* src, size_t src_len,
                                   size_t* out_len);

typedef struct {
    const char* allowed_origins;
    const char* allowed_methods;
    const char* allowed_headers;
    const char* expose_headers;
    int         max_age;
    bool        allow_credentials;
} xylem_http_cors_t;

extern size_t xylem_http_cors_headers(const xylem_http_cors_t* cors,
                                      const char* origin,
                                      bool is_preflight,
                                      xylem_http_hdr_t* out,
                                      size_t out_cap);

extern xylem_http_multipart_t* xylem_http_multipart_parse(
    const char* content_type, const void* body, size_t body_len);
extern size_t      xylem_http_multipart_count(const xylem_http_multipart_t* mp);
extern const char* xylem_http_multipart_name(const xylem_http_multipart_t* mp,
                                             size_t index);
extern const char* xylem_http_multipart_filename(const xylem_http_multipart_t* mp,
                                                 size_t index);
extern const char* xylem_http_multipart_content_type(
    const xylem_http_multipart_t* mp, size_t index);
extern const void* xylem_http_multipart_data(const xylem_http_multipart_t* mp,
                                             size_t index);
extern size_t      xylem_http_multipart_data_len(const xylem_http_multipart_t* mp,
                                                 size_t index);
extern void        xylem_http_multipart_destroy(xylem_http_multipart_t* mp);

extern char* xylem_http_sse_build(const char* event, const char* data,
                                  size_t* len);

