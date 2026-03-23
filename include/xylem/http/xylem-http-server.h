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

#include "xylem/http/xylem-http-common.h"

#include <stddef.h>
#include <stdint.h>

typedef struct xylem_loop_s xylem_loop_t;

/* Opaque types */
typedef struct xylem_http_req_s  xylem_http_req_t;
typedef struct xylem_http_conn_s xylem_http_conn_t;
typedef struct xylem_http_srv_s  xylem_http_srv_t;

/**
 * @brief Server request callback.
 *
 * Invoked when a complete HTTP request has been received. The
 * connection and request handles are valid only within this callback.
 *
 * @param conn      Connection handle for sending the response.
 * @param req       Parsed request (method, URL, headers, body).
 * @param userdata  User-supplied pointer from the server config.
 */
typedef void (*xylem_http_on_request_fn_t)(xylem_http_conn_t* conn,
                                           xylem_http_req_t* req,
                                           void* userdata);


/**
 * @brief Server configuration.
 *
 * Caller allocates on the stack and passes to xylem_http_srv_create().
 * The strings pointed to by host, tls_cert, and tls_key must remain
 * valid until xylem_http_srv_destroy() is called.
 */
typedef struct xylem_http_srv_cfg_s {
    const char*                  host;            /* bind address, e.g. "0.0.0.0" */
    uint16_t                     port;            /* bind port */
    xylem_http_on_request_fn_t   on_request;      /* request callback */
    void*                        userdata;        /* passed to on_request */
    const char*                  tls_cert;        /* PEM cert path, NULL for plain HTTP */
    const char*                  tls_key;         /* PEM key path, NULL for plain HTTP */
    size_t                       max_body_size;   /* max request body, 0 = default 1 MiB */
    uint64_t                     idle_timeout_ms; /* idle timeout, 0 = disabled, default 60000 */
} xylem_http_srv_cfg_t;

/**
 * @brief Create an HTTP server.
 *
 * The event loop is passed as a separate parameter, consistent
 * with xylem_tcp_listen and xylem_tls_listen.
 *
 * @param loop  Event loop the server runs on.
 * @param cfg   Server configuration.
 *
 * @return Server handle on success, NULL on failure.
 */
extern xylem_http_srv_t* xylem_http_srv_create(xylem_loop_t* loop,
                                                const xylem_http_srv_cfg_t* cfg);

/**
 * @brief Start the HTTP server.
 *
 * Binds to the configured host and port and begins accepting
 * connections. Uses xylem_tcp or xylem_tls depending on whether
 * tls_cert and tls_key are set in the config.
 *
 * @param srv  Server handle.
 *
 * @return 0 on success, -1 on failure (e.g. bind error).
 */
extern int xylem_http_srv_start(xylem_http_srv_t* srv);

/**
 * @brief Stop the HTTP server.
 *
 * Stops accepting new connections. Existing connections continue
 * processing until they complete or are closed.
 *
 * @param srv  Server handle.
 */
extern void xylem_http_srv_stop(xylem_http_srv_t* srv);

/**
 * @brief Destroy the HTTP server and free all resources.
 *
 * @param srv  Server handle.
 */
extern void xylem_http_srv_destroy(xylem_http_srv_t* srv);

/**
 * @brief Get the request method string.
 *
 * @param req  Request handle.
 *
 * @return Method string (e.g. "GET", "POST").
 */
extern const char* xylem_http_req_method(const xylem_http_req_t* req);

/**
 * @brief Get the request URL path string.
 *
 * @param req  Request handle.
 *
 * @return URL path string.
 */
extern const char* xylem_http_req_url(const xylem_http_req_t* req);

/**
 * @brief Get a request header value by name.
 *
 * Performs case-insensitive ASCII matching per RFC 7230.
 *
 * @param req   Request handle.
 * @param name  Header name to look up.
 *
 * @return Header value string, or NULL if not found.
 */
extern const char* xylem_http_req_header(const xylem_http_req_t* req,
                                          const char* name);

/**
 * @brief Get the request body data.
 *
 * @param req  Request handle.
 *
 * @return Pointer to body bytes, or NULL if no body.
 */
extern const void* xylem_http_req_body(const xylem_http_req_t* req);

/**
 * @brief Get the request body length.
 *
 * @param req  Request handle.
 *
 * @return Body length in bytes, or 0 if no body.
 */
extern size_t xylem_http_req_body_len(const xylem_http_req_t* req);

/**
 * @brief Send an HTTP response on a connection.
 *
 * Serializes a complete HTTP/1.1 response with status line,
 * Content-Type, Content-Length, and body. Custom headers are
 * written before auto-generated headers; a custom header whose
 * name matches an auto-generated one (case-insensitive) overrides it.
 *
 * @param conn          Connection handle.
 * @param status_code   HTTP status code (e.g. 200, 404).
 * @param content_type  Content-Type header value.
 * @param body          Response body, or NULL for empty body.
 * @param body_len      Body length in bytes.
 * @param headers       Custom response headers, or NULL for none.
 * @param header_count  Number of custom response headers.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_conn_send(xylem_http_conn_t* conn,
                                 int status_code,
                                 const char* content_type,
                                 const void* body, size_t body_len,
                                 const xylem_http_hdr_t* headers,
                                 size_t header_count);

/**
 * @brief Begin a chunked transfer-encoded response.
 *
 * Sends the status line, Transfer-Encoding: chunked header,
 * Content-Type, and any custom headers. Does not send a body.
 * After this call, use xylem_http_conn_send_chunk() to send
 * body fragments and xylem_http_conn_end_chunked() to finish.
 *
 * @param conn          Connection handle.
 * @param status_code   HTTP status code (e.g. 200).
 * @param content_type  Content-Type header value, or NULL.
 * @param headers       Custom response headers, or NULL for none.
 * @param header_count  Number of custom response headers.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_conn_start_chunked(xylem_http_conn_t* conn,
                                         int status_code,
                                         const char* content_type,
                                         const xylem_http_hdr_t* headers,
                                         size_t header_count);

/**
 * @brief Send a single chunk in a chunked response.
 *
 * Formats the data as a chunked transfer-encoding frame
 * ({hex_size}\r\n{data}\r\n) and sends it on the connection.
 *
 * @param conn  Connection handle.
 * @param data  Chunk data.
 * @param len   Chunk length in bytes. If 0, this is a no-op.
 *
 * @return 0 on success, -1 on failure (connection closed or not
 *         in chunked mode).
 */
extern int xylem_http_conn_send_chunk(xylem_http_conn_t* conn,
                                      const void* data, size_t len);

/**
 * @brief End a chunked transfer-encoded response.
 *
 * Sends the terminating zero-length chunk (0\r\n\r\n) and
 * handles keep-alive: resets the parser for the next request
 * or closes the connection.
 *
 * @param conn  Connection handle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_conn_end_chunked(xylem_http_conn_t* conn);

/**
 * @brief Close a client connection.
 *
 * @param conn  Connection handle.
 */
extern void xylem_http_conn_close(xylem_http_conn_t* conn);
