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

/*
 * Internal shared header for the HTTP module.
 *
 * Collects everything the HTTP translation units share: the transport
 * vtable, request/response/server structures, the protocol-engine entry
 * points, the transport factories (TCP/TLS), and the stateless helpers
 * (URL parsing, header utilities, request serialization, proxy support).
 *
 * Not part of the public API. Do not include outside the HTTP/WS modules.
 */

_Pragma("once")

#include "xylem/net/http/xylem-http.h"
#include "xylem/xylem-utils.h"

#include "platform/platform-socket.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Transport abstraction
 *
 * The protocol engine talks to the network exclusively through this
 * vtable. Whether the bytes travel over plain TCP or TLS is decided by
 * which factory builds the transport, never by a conditional inside the
 * engine. A build without TLS links the stub factories, which return a
 * NULL transport; no macros leak into the engine or the dispatch layer.
 */

typedef struct {
    void* conn;
    int  (*read)(void* conn, void* buf, int len);
    int  (*write)(void* conn, const void* data, int len);
    void (*close)(void* conn);
    void (*set_rd_deadline)(void* conn, uint64_t ms);
    void (*set_wr_deadline)(void* conn, uint64_t ms);
    int  (*remote_addr)(void* conn, char* host, size_t host_len, uint16_t* port);
    int  (*local_addr)(void* conn, char* host, size_t host_len, uint16_t* port);
    int  (*shutdown_wr)(void* conn); /*< May be NULL (e.g. TLS). */
} http_transport_t;

typedef http_transport_t (*http_dial_fn_t)(const char* host, uint16_t port,
                                           uint64_t timeout_ms, void* ctx);

/* URL / header value types and stateless helpers (http-utils.c) */

typedef struct {
    char     scheme[8];
    char     host[256];
    uint16_t port;
    char     path[2048];
} http_url_t;

typedef struct {
    char* name;
    char* value;
} http_header_t;

/**
 * @brief Parse a URL string into its components.
 *
 * Extracts scheme (http/https only), host, port (defaults 80/443),
 * and path (defaults "/").
 *
 * @param url  Null-terminated URL string.
 * @param out  Parsed result.
 *
 * @return 0 on success, -1 on invalid URL.
 */
extern int http_url_parse(const char* url, http_url_t* out);

/**
 * @brief Serialize a parsed URL back into a string.
 *
 * Omits the port when it matches the scheme default (80/443).
 *
 * @param url       Parsed URL.
 * @param buf       Output buffer.
 * @param buf_size  Buffer capacity in bytes.
 *
 * @return 0 on success, -1 on truncation.
 */
extern int http_url_serialize(const http_url_t* url, char* buf,
                              size_t buf_size);

/**
 * @brief Build an HTTP/1.1 request into a malloc'd buffer.
 *
 * Fills Host, Content-Length, Connection, Content-Type, and
 * optionally Expect headers. Custom headers are written first;
 * auto-generated headers whose names match a custom header
 * (case-insensitive) are skipped. When expect_continue is true
 * the body is NOT appended.
 *
 * @param method               HTTP method string (e.g. "GET").
 * @param url                  Parsed URL.
 * @param body                 Request body, or NULL.
 * @param body_len             Body length in bytes.
 * @param content_type         Content-Type value, or NULL.
 * @param expect_continue      If true, add Expect: 100-continue and omit body.
 * @param use_proxy            If true, use absolute-form request target.
 * @param out_len              Output: total serialized length.
 * @param custom_headers       Custom header array, or NULL.
 * @param custom_header_count  Number of custom headers.
 *
 * @return Allocated buffer on success, NULL on failure. Caller frees.
 */
extern char* http_req_serialize(const char* method, const http_url_t* url,
                                const void* body, size_t body_len,
                                const char* content_type,
                                bool expect_continue, bool use_proxy,
                                size_t* out_len,
                                const xylem_http_hdr_t* custom_headers,
                                size_t custom_header_count);

/**
 * @brief Case-insensitive ASCII comparison of two strings.
 *
 * @param a  First string.
 * @param b  Second string.
 *
 * @return true if equal (ignoring case).
 */
extern bool http_header_eq(const char* a, const char* b);

/**
 * @brief Find a header value by name (case-insensitive).
 *
 * @param headers  Header array.
 * @param count    Number of headers.
 * @param name     Header name to look up.
 *
 * @return Header value string, or NULL if not found.
 */
extern const char* http_header_find(const http_header_t* headers,
                                    size_t count, const char* name);

/**
 * @brief Append a header to a growable array.
 *
 * Copies name and value into newly allocated strings.
 *
 * @param headers    Pointer to header array (may be reallocated).
 * @param count      Pointer to current header count.
 * @param cap        Pointer to current array capacity.
 * @param name       Header name.
 * @param name_len   Header name length.
 * @param value      Header value.
 * @param value_len  Header value length.
 *
 * @return 0 on success, -1 on allocation failure.
 */
extern int http_header_add(http_header_t** headers, size_t* count,
                           size_t* cap, const char* name, size_t name_len,
                           const char* value, size_t value_len);

/**
 * @brief Free all name/value strings and the header array itself.
 *
 * @param headers  Header array (may be NULL).
 * @param count    Number of headers.
 */
extern void http_headers_free(http_header_t* headers, size_t count);

/**
 * @brief Estimate custom header total size and detect overrides.
 *
 * Iterates custom_headers, summing "name: value\r\n" lengths.
 * For each name in check_names, sets the corresponding overridden
 * flag to true if a custom header matches (case-insensitive).
 *
 * @param headers      Custom header array, or NULL.
 * @param count        Number of custom headers.
 * @param check_names  Array of auto-generated header names to check.
 * @param overridden   Output flags, one per check_names entry.
 * @param check_count  Number of entries in check_names / overridden.
 *
 * @return Total estimated byte size of all custom headers.
 */
extern size_t http_header_scan(const xylem_http_hdr_t* headers, size_t count,
                               const char** check_names, bool* overridden,
                               size_t check_count);

/**
 * @brief Write a size_t value as decimal digits into a buffer.
 *
 * Does NOT null-terminate. Caller must ensure buf has at least 20 bytes.
 *
 * @param buf  Output buffer.
 * @param val  Value to convert.
 *
 * @return Number of bytes written.
 */
extern size_t http_write_uint(char* buf, size_t val);

/**
 * @brief ASCII lowercase lookup table (256 entries).
 *
 * Maps uppercase A-Z to lowercase a-z; all other bytes pass through.
 * Used for fast case-insensitive comparisons without calling tolower().
 */
extern const uint8_t http_lower_table[256];

/**
 * @brief Map an HTTP status code to its standard reason phrase.
 *
 * @param status  HTTP status code (e.g. 200, 404).
 *
 * @return Reason phrase string, or "" for unknown codes.
 */
extern const char* http_reason_phrase(int status);

/* Proxy support (http-utils.c + http-proxy.c) */

/**
 * @brief Build a proxy descriptor from the environment for a target URL.
 *
 * Honors http_proxy / https_proxy / no_proxy.
 *
 * @param url  Target URL.
 *
 * @return Allocated proxy descriptor, or NULL if none applies. Caller
 *         frees via http_proxy_from_env_free().
 */
extern xylem_http_proxy_t* http_proxy_from_env(const char* url);

/**
 * @brief Free a proxy descriptor returned by http_proxy_from_env(). NULL-safe.
 *
 * @param proxy  Descriptor to free.
 */
extern void http_proxy_from_env_free(xylem_http_proxy_t* proxy);

/**
 * @brief Connect to an HTTP CONNECT proxy and establish a tunnel.
 *
 * Dials the proxy, sends a CONNECT request, and waits for 200.
 * On success the returned fd is a transparent tunnel to the target.
 *
 * @param proxy_host   Proxy hostname or IP.
 * @param proxy_port   Proxy port.
 * @param target_host  Destination hostname.
 * @param target_port  Destination port.
 * @param timeout_ms   Connect timeout in ms, 0 = no timeout.
 * @param username     Proxy username, or NULL for no auth.
 * @param password     Proxy password, or NULL for no auth.
 *
 * @return Tunneled socket fd, or PLATFORM_SO_ERROR_INVALID_SOCKET on failure.
 */
extern platform_sock_t http_proxy_connect(const char* proxy_host,
                                          uint16_t proxy_port,
                                          const char* target_host,
                                          uint16_t target_port,
                                          uint64_t timeout_ms,
                                          const char* username,
                                          const char* password);

/* Request / response / server structures */

typedef struct {
    char* key;
    char* value;
} http_router_param_t;

typedef struct http_req_s {
    char              method[16];
    char*             url;
    size_t            url_len;
    http_header_t*    headers;
    size_t            header_count;
    size_t            header_cap;
    uint8_t*          body;
    size_t            body_len;
    size_t            body_cap;
    char              remote_host[INET6_ADDRSTRLEN];
    uint16_t          remote_port;
    http_router_param_t* router_params;
    size_t               router_param_count;
    void*                _mw_chain;
} http_req_t;

typedef struct http_res_s {
    int            status_code;
    http_header_t* headers;
    size_t         header_count;
    size_t         header_cap;
    uint8_t*       body;
    size_t         body_len;
    size_t         body_cap;
    http_transport_t* _transport;
    bool              _headers_sent;
    uint8_t*       _body_buf;
    size_t         _body_buf_len;
} http_res_t;

typedef struct http_srv_s {
    void*                   listener;
    void                    (*close_listener)(void* listener);
    xylem_http_handler_fn_t handler;
    void*                   userdata;
    char                    host[INET6_ADDRSTRLEN];
    uint16_t                port;
    xylem_http_handler_fn_t on_upgrade;
    void*                   upgrade_userdata;
    uint64_t                idle_timeout_ms;
    uint64_t                read_header_timeout_ms;
    uint64_t                write_timeout_ms;
    _Atomic int32_t         active_conns;
    bool                    closing;
} http_srv_t;

typedef struct {
    http_srv_t*      srv;
    http_transport_t transport;
    char             remote_host[INET6_ADDRSTRLEN];
    uint16_t         remote_port;
} http_srv_conn_ctx_t;

/* Protocol engine (xylem-http.c) */

/**
 * @brief Apply server options onto a freshly allocated server, filling
 *        in defaults for any unset timeout.
 *
 * @param srv   Server to initialize (handler/userdata already set).
 * @param opts  Server options, or NULL for all defaults.
 */
extern void http_srv_init(http_srv_t* srv, const xylem_http_srv_opts_t* opts);

/**
 * @brief Server connection coroutine entry.
 *
 * @param arg  Heap-allocated http_srv_conn_ctx_t (freed inside).
 */
extern void http_srv_conn_coroutine(void* arg);

/**
 * @brief Perform an HTTP client request over a caller-provided transport.
 *
 * @param method     HTTP method string.
 * @param url        Full URL.
 * @param body       Request body, or NULL.
 * @param body_len   Body length in bytes.
 * @param content_type  Content-Type value, or NULL.
 * @param headers    Custom headers, or NULL.
 * @param header_count  Number of custom headers.
 * @param opts       Client options, or NULL.
 * @param use_proxy  Use absolute-form request target.
 * @param dial_fn    Dial callback that creates a transport for a host.
 * @param dial_ctx   Opaque context passed to dial_fn.
 *
 * @return Response object on success, NULL on failure. Caller frees via
 *         xylem_http_res_destroy().
 */
extern xylem_http_res_t* http_do_request(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts,
    bool                         use_proxy,
    http_dial_fn_t               dial_fn,
    void*                        dial_ctx);

/**
 * Transport factories
 *
 * TCP factories live in transport-tcp.c (always built). TLS factories
 * live in transport-tls.c when XYLEM_ENABLE_TLS is set, otherwise in
 * transport-tls-stub.c where they return NULL.
 */

extern xylem_http_srv_t* http_tcp_listen(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts);

extern xylem_http_res_t* http_tcp_request(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts);

extern xylem_http_srv_t* http_tls_listen(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts);

extern xylem_http_res_t* http_tls_request(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts);
