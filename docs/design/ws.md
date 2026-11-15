# WebSocket

Xylem provides WebSocket clients and standalone servers. General-purpose
HTTP/HTTPS clients and servers are outside its scope; use a dedicated HTTP
library for REST requests, redirects, proxy negotiation, and connection pools.

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

## Module boundaries

The public API is `include/xylem/net/xylem-ws.h`; all implementation and
private dependencies live in `src/net/ws/`. `ws-tcp.c` and `ws-tls.c` adapt
the public TCP/TLS APIs to the WS transport interface. `ws-utils.c` contains
only ws/wss URL parsing and opening-handshake header helpers. The bundled
`llhttp/` parser is used only for the opening handshake. HTTP/1.1 Upgrade is
retained for interoperability; HTTP/2 and HTTP/3 extended CONNECT are not
implemented. Ordinary HTTP request/response bodies are not exposed.

## Connection and listener lifetime

Create a listener with xylem_ws_listen(host, port, opts), then call
xylem_ws_accept. Each returned connection belongs to the caller, who must
eventually call xylem_ws_destroy. xylem_ws_close attempts a bounded close
handshake when I/O is idle; with active I/O it cancels communication and
wakes the operations. Destroy only after every operation has returned.
Listener close and destroy follow the same two-phase lifetime as TCP.
Listener and connection local/remote address queries are available.

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

WSS requires XYLEM_ENABLE_TLS=ON and OpenSSL >= 3.5.

## API migration

The xylem_http_* API and include/xylem/net/http/xylem-http.h have been removed,
along with the HTTP transports, proxies, redirects, response-body decoding,
and connection pool. Use a dedicated HTTP library for these features.
The WebSocket public API is unchanged by this removal.

## Tests

test-ws.c covers URL validation, opening-handshake parsing and rejection,
coalesced handshake/frame reads, message I/O, compression, deadlines, and
connection/listener lifetimes. With XYLEM_ENABLE_TLS=ON, test-wss.c covers
secure message I/O, compression, deadlines, and TLS handshake cancellation.
