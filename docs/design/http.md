# HTTP/HTTPS and WebSocket

HTTP provides an HTTP/1.1 client for REST APIs. Requests and responses use
in-memory byte buffers; callers choose the Content-Type and encode JSON or
other formats themselves. Custom headers carry Bearer tokens or API keys.
The module retains llhttp, gzip decoding, HTTP proxy forwarding, HTTPS
CONNECT tunnels, redirects, and idle connection reuse.

WebSocket provides both clients and standalone servers for ws and wss.
Its TCP/TLS listener factories own the listener and TLS configuration.
Applications call listen, accept, and spawn their own connection coroutines.
Accept completes each TLS/WS handshake serially within handshake_timeout_ms;
invalid or timed-out peers are discarded before accepting the next peer.
Closing a listener cancels accept and any pending handshake. Destroy the
listener after accept returns; existing connections continue independently.
The bounded llhttp Upgrade parser validates the handshake, enforces its
deadline and header limits, and retains bytes following the headers as
WebSocket frames. It does not depend on an HTTP server or response writer.

## Client options and lifetime

Pass NULL options for defaults: ten redirects, no total request timeout,
a 16 MiB response-body limit, normal request-body transmission, and verified
TLS. A supplied options struct with max_redirects = 0 disables redirects.
timeout_ms bounds the entire call, including DNS, connect, TLS, Expect,
response reads and redirects. max_response_body bounds both encoded body
storage and decompressed gzip output; zero selects the 16 MiB default.
expect_continue opts into Expect: 100-continue; an explicit Expect header
can also select this behavior. The wait is at most one second and cannot
extend the total deadline.

Each redirect resolves its new transport and effective proxy. Proxy
configuration comes from explicit options or http_proxy/https_proxy and
no_proxy. A non-NULL proxy descriptor with a NULL host disables environment
proxy selection. HTTPS uses CONNECT; proxy credentials do not become
origin headers. A connection-pool key hashes the origin, effective proxy
and credentials, and TLS verification, CA and client-certificate settings.
Configuration strings are compared by value, not by pointer. Clear the pool
before replacing certificate or CA files at the same path.

Cross-origin redirects remove Authorization, Cookie, X-API-Key and API-Key
and regenerate Host. Arbitrary application-specific secret headers cannot
be identified automatically; disable automatic redirects when using them.
HTTPS-to-HTTP redirects fail unless allow_https_downgrade is explicitly set.
307/308 preserve the method and body. 303 selects GET except for HEAD;
301/302 convert POST to GET. Entity headers are removed when the body is
dropped. The response at the configured redirect limit is returned.

Responses belong to the caller and must be destroyed with
xylem_http_res_destroy. The body is bytes plus a length, not a C string.
Transport, parsing, timeout, body-limit and decompression failures return
NULL. Ordinary HTTP error statuses return a response.

After all requests have returned, call xylem_http_pool_clear from a
coroutine before shutting down the runtime or starting another run.
It closes idle connections and releases pool storage, including TLS
contexts. It must not race with requests.

## API migration

HTTP server types and functions, ResponseWriter, routing, authentication
middleware, CORS, file serving, Cookie Jar, form parsing and multipart
parsing have been removed. xylem_http_request no longer takes a jar
argument. Convenience methods and response accessors retain their names.
Manually supplied Cookie headers and arbitrary fixed-size request bodies
remain supported.

The old HTTP-writer-based xylem_ws_accept has been removed. The new
xylem_ws_accept takes a standalone listener created by the three-argument
xylem_ws_listen(host, port, opts), for both ws and wss. Each returned
connection belongs to the caller, who must eventually call xylem_ws_destroy.
xylem_ws_close no longer frees a connection. It attempts a bounded close
handshake when I/O is idle; with active I/O it cancels communication and
wakes the operations. Destroy only after every operation has returned.
Listener close and destroy follow the same two-phase lifetime as TCP.
xylem_ws_listener_addr replaces xylem_ws_listener_port; connection local
and remote address queries are also available.

Read and write deadlines use the same absolute millisecond clock as TCP,
with zero clearing the deadline. They cover complete messages, including
all fragments and buffered input. One receiving and one sending coroutine
may run concurrently; send and ping share the single-sender contract.
An I/O failure or timeout permanently closes the WS connection, since a
partial frame or compression state cannot safely be resumed. Destroy the
handle after operations return. A short receive buffer is recoverable: the
connection retains the complete message for a larger-buffer retry.

Message I/O uses caller-owned buffers:
xylem_ws_send(conn, data, len, type) returns 0 on success;
xylem_ws_recv(conn, buf, len, type_out) returns the complete message length.
Both use an int byte length, with the message type last. Receive type_out
may be NULL. Zero is a valid empty-message length; close and failure return
-1. Text is UTF-8 payload bytes without an appended terminator. Messages
are never silently truncated or combined. A short buffer returns -1 without
modifying the buffer or type output, retaining that message ahead of later
messages. Read deadlines still apply to retries of a retained message.

xylem_ws_msg_t, xylem_ws_msg_free and xylem_ws_close_code have been removed.
There is no received-message allocation for the caller to release.
The public text/binary enum uses ordinary zero-based values, mapped
internally to WebSocket wire opcodes 0x1 and 0x2.

HTTPS/WSS still require XYLEM_ENABLE_TLS=ON and OpenSSL >= 3.5.
TLS/DTLS and the runtime retain their existing APIs. SSE and streaming
HTTP uploads/downloads are outside this client scope.

## Tests

test-http.c contains the HTTP client tests and a scripted peer built on
public TCP/TLS connections. It supplies wire responses without exposing
an HTTP server API in the library. With XYLEM_ENABLE_TLS=ON, the same test
also covers HTTPS, including a TCP CONNECT fixture with authenticated
proxy negotiation and end-to-end TLS.
