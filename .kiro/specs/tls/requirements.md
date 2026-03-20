# TLS Module Requirements

## Overview

Add TLS (Transport Layer Security) support to Xylem, built on top of the existing TCP module using OpenSSL. The TLS module provides encrypted communication with an API that mirrors `xylem_tcp_*` for minimal learning curve.

## Dependencies

- OpenSSL (libssl + libcrypto), found via `find_package(OpenSSL)` at build time
- Controlled by CMake option `XYLEM_ENABLE_TLS=ON/OFF` (default OFF)
- When OFF, no TLS code is compiled, zero external dependency
- Users link `-lssl -lcrypto` alongside `-lxylem` when TLS is enabled

## Functional Requirements

### FR-1: TLS Context Management

- `xylem_tls_ctx_create()` — create a reusable TLS context (wraps `SSL_CTX`)
- `xylem_tls_ctx_destroy()` — free context and associated resources
- `xylem_tls_ctx_load_cert(ctx, cert_path, key_path)` — load certificate chain and private key from PEM files
- `xylem_tls_ctx_set_ca(ctx, ca_path)` — set CA certificate file or directory for peer verification
- `xylem_tls_ctx_set_verify(ctx, enable)` — enable/disable peer certificate verification (default: enabled)
- `xylem_tls_ctx_set_alpn(ctx, protocols, count)` — set ALPN protocol list for negotiation
- One context can be shared across multiple connections and servers

### FR-2: TLS Client (Dial)

- `xylem_tls_dial(loop, addr, ctx, handler, opts)` — create outbound TLS connection
- Internally calls `xylem_tcp_dial`, then performs TLS handshake (`SSL_connect`) after TCP connects
- `on_connect` callback fires only after TLS handshake completes successfully
- `xylem_tls_set_hostname(tls, hostname)` — set SNI server name (must be called before dial, or passed via opts)
- Supports all `xylem_tcp_opts_t` options (timeouts, reconnect, framing, heartbeat)
- Reconnect re-does full TLS handshake on new TCP connection

### FR-3: TLS Server (Listen)

- `xylem_tls_listen(loop, addr, ctx, handler, opts)` — create TLS server
- Internally calls `xylem_tcp_listen`, then performs TLS handshake (`SSL_accept`) on each accepted connection
- `on_accept` callback fires only after TLS handshake completes successfully
- Server context must have cert+key loaded

### FR-4: TLS Data Transfer

- `xylem_tls_send(tls, data, len)` — encrypt and send data
- Internally: `SSL_write` → read from write BIO → `xylem_tcp_send` (ciphertext)
- `on_read` callback delivers decrypted plaintext to user
- Internally: TCP `on_read` (ciphertext) → write to read BIO → `SSL_read` → user callback
- `on_write_done` fires when the plaintext data has been fully encrypted and sent
- Framing operates on plaintext (user sees frames, TLS handles encryption transparently)

### FR-5: TLS Connection Lifecycle

- `xylem_tls_close(tls)` — initiate graceful TLS shutdown (`SSL_shutdown`) then close TCP
- `on_close` callback fires after both TLS shutdown and TCP close complete
- `on_timeout` and `on_heartbeat_miss` transparently forwarded from TCP layer
- `xylem_tls_get_userdata` / `xylem_tls_set_userdata` — same pattern as TCP
- `xylem_tls_close_server(server)` — close TLS server and detach all connections

### FR-6: ALPN Negotiation

- `xylem_tls_ctx_set_alpn(ctx, protocols, count)` — set offered protocols (client) or accepted protocols (server)
- `xylem_tls_get_alpn(tls)` — query negotiated protocol after handshake
- Returns NULL if no protocol was negotiated

### FR-7: Async Handshake via Memory BIO

- OpenSSL must NOT directly access the socket fd
- Use memory BIO pair: `BIO_new(BIO_s_mem())` for both read and write
- Handshake data flows through BIO → TCP send/recv, keeping IO control in the event loop
- `SSL_ERROR_WANT_READ` / `SSL_ERROR_WANT_WRITE` drive the async handshake state machine

### FR-8: DTLS Context

- `xylem_dtls_ctx_create()` — create a reusable DTLS context (wraps `SSL_CTX` with `DTLS_method()`)
- `xylem_dtls_ctx_destroy()` — free context
- `xylem_dtls_ctx_load_cert` / `set_ca` / `set_verify` / `set_alpn` — same semantics as TLS ctx
- Automatically configures cookie generation/verification callbacks (`SSL_CTX_set_cookie_generate_cb`, `SSL_CTX_set_cookie_verify_cb`)

### FR-9: DTLS Client

- `xylem_dtls_dial(loop, addr, ctx, handler)` — create outbound DTLS connection over UDP
- Internally creates a connected UDP socket, then performs DTLS handshake (`SSL_connect`)
- `on_connect` fires after DTLS handshake completes
- Uses memory BIO, same async pattern as TLS

### FR-10: DTLS Server

- `xylem_dtls_listen(loop, addr, ctx, handler)` — create DTLS server on a UDP socket
- Handles `DTLSv1_listen` for cookie exchange, then `SSL_accept` for full handshake
- `on_accept` fires after DTLS handshake completes per peer
- Manages multiple DTLS sessions on a single UDP socket (demux by peer address)

### FR-11: DTLS Data Transfer

- `xylem_dtls_send(dtls, data, len)` — encrypt and send datagram
- `on_read` delivers decrypted plaintext datagrams
- No framing needed — DTLS preserves message boundaries (datagram semantics)

### FR-12: DTLS Lifecycle

- `xylem_dtls_close(dtls)` — send close_notify, close session
- `on_close` fires after shutdown
- `xylem_dtls_close_server(server)` — close all sessions and UDP socket
- DTLS retransmission timer: integrate `DTLSv1_get_timeout` / `DTLSv1_handle_timeout` with `xylem_loop_timer`
- `xylem_dtls_get_userdata` / `xylem_dtls_set_userdata`

## Non-Functional Requirements

### NFR-1: API Symmetry

- TLS API mirrors TCP API naming and callback structure
- Switching from TCP to TLS should require minimal code changes

### NFR-2: Opaque Types

- `xylem_tls_t`, `xylem_tls_ctx_t`, `xylem_tls_server_t` are opaque (struct body in `.c` only)

### NFR-3: Style Compliance

- Follow all rules in `style.md` (naming, braces, license header, Doxygen, etc.)

### NFR-4: Build Integration

- CMake option `XYLEM_ENABLE_TLS` controls compilation
- TLS source added via `list(APPEND SRCS ...)` consistent with existing pattern
- Test executable links against OpenSSL
