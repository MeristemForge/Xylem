# TLS Module Tasks

## Task 1: CMake build integration
- [x] Add `XYLEM_ENABLE_TLS` option to root `CMakeLists.txt`
- [x] Add `find_package(OpenSSL REQUIRED)` when enabled
- [x] Append `src/xylem-tls.c` and `src/xylem-dtls.c` to `SRCS` when enabled
- [x] Add `include_directories(${OPENSSL_INCLUDE_DIR})` when enabled
- [x] Add conditional TLS and DTLS tests in `tests/CMakeLists.txt` with OpenSSL link

## Task 2: TLS header — public API
- [x] Create `include/xylem/xylem-tls.h`
- [x] Forward declarations: `xylem_tls_t`, `xylem_tls_ctx_t`, `xylem_tls_server_t`
- [x] Define `xylem_tls_handler_t` callback struct (on_accept, on_connect, on_read, on_write_done, on_close, on_timeout, on_heartbeat_miss)
- [x] Declare ctx API: create, destroy, load_cert, set_ca, set_verify, set_alpn
- [x] Declare connection API: dial, send, close, set_hostname, get_alpn, get_userdata, set_userdata
- [x] Declare server API: listen, close_server
- [x] Full Doxygen comments on all public functions

## Task 3: TLS context implementation
- [x] Create `src/xylem-tls.c` with license header and struct definitions
- [x] Implement `xylem_tls_ctx_create` — allocate struct, create `SSL_CTX` with `TLS_method()`
- [x] Implement `xylem_tls_ctx_destroy` — free `SSL_CTX` and struct
- [x] Implement `xylem_tls_ctx_load_cert` — `SSL_CTX_use_certificate_chain_file` + `SSL_CTX_use_PrivateKey_file`
- [x] Implement `xylem_tls_ctx_set_ca` — `SSL_CTX_load_verify_locations`
- [x] Implement `xylem_tls_ctx_set_verify` — `SSL_CTX_set_verify` with `SSL_VERIFY_PEER` or `SSL_VERIFY_NONE`
- [x] Implement `xylem_tls_ctx_set_alpn` — `SSL_CTX_set_alpn_protos` (client) + `SSL_CTX_set_alpn_select_cb` (server)

## Task 4: Memory BIO helpers and flush logic
- [x] Implement `_tls_flush_write_bio` — read pending data from write BIO, send via `xylem_tcp_send`
- [x] Implement `_tls_feed_read_bio` — write ciphertext into read BIO from TCP on_read

## Task 5: TLS handshake state machine
- [x] Implement `_tls_do_handshake` — call `SSL_do_handshake`, handle WANT_READ/WANT_WRITE, flush BIO
- [x] Implement `_tls_tcp_connect_cb` — TCP connected, create SSL, set BIOs, set SNI, start client handshake
- [x] Implement `_tls_tcp_accept_cb` — TCP accepted, create SSL with accept state, wait for ClientHello
- [x] Implement `_tls_tcp_read_cb` during handshake — feed read BIO, continue handshake
- [x] On handshake success: set `handshake_done`, call user `on_connect` or `on_accept`
- [x] On handshake failure: close with error

## Task 6: TLS read path (post-handshake)
- [x] In `_tls_tcp_read_cb`: feed ciphertext to read BIO
- [x] Loop `SSL_read` to extract all available plaintext
- [x] Deliver plaintext to user `on_read` callback
- [x] Handle `SSL_ERROR_ZERO_RETURN` (peer shutdown)
- [x] Handle read errors → close connection

## Task 7: TLS write path
- [x] Implement `xylem_tls_send` — `SSL_write` plaintext, flush write BIO via TCP send
- [x] Track write requests for `on_write_done` callback
- [x] Bridge TCP `on_write_done` → TLS `on_write_done` with original plaintext pointer/len

## Task 8: TLS connection lifecycle
- [x] Implement `xylem_tls_dial` — allocate `xylem_tls_t`, set up internal TCP handler, call `xylem_tcp_dial`
- [x] Implement `xylem_tls_close` — `SSL_shutdown`, flush BIO, then `xylem_tcp_close`
- [x] Implement `xylem_tls_set_hostname` / `xylem_tls_get_alpn`
- [x] Implement `xylem_tls_get_userdata` / `xylem_tls_set_userdata`
- [x] Bridge timeout and heartbeat callbacks from TCP to TLS handler

## Task 9: TLS server implementation
- [x] Implement `xylem_tls_listen` — allocate `xylem_tls_server_t`, set up internal TCP handler, call `xylem_tcp_listen`
- [x] On TCP accept: create `xylem_tls_t` for accepted connection, start server handshake
- [x] Implement `xylem_tls_close_server` — close all TLS connections, then close TCP server

## Task 10: Tests — context and basic setup
- [x] Create `tests/test-tls.c`
- [x] Test ctx create/destroy
- [x] Test load_cert with valid/invalid paths
- [x] Test set_ca, set_verify, set_alpn

## Task 11: Tests — client/server handshake and data transfer
- [x] Generate self-signed test certificates (embed in test or use temp files)
- [x] Test TLS dial + listen: full handshake, on_connect/on_accept fires
- [x] Test send/recv: plaintext round-trip through TLS
- [x] Test on_write_done fires after send
- [x] Test close: graceful shutdown, on_close fires

## Task 12: Tests — error cases and ALPN
- [x] Test handshake failure (e.g., cert verification with wrong CA)
- [x] Test ALPN negotiation: matching and non-matching protocols
- [x] Test SNI hostname setting
- [x] Test close_server detaches connections

## Task 13: DTLS header — public API
- [x] Create `include/xylem/xylem-dtls.h`
- [x] Forward declarations: `xylem_dtls_t`, `xylem_dtls_ctx_t`, `xylem_dtls_server_t`
- [x] Define `xylem_dtls_handler_t` callback struct (on_accept, on_connect, on_read, on_write_done, on_close)
- [x] Declare ctx API: create, destroy, load_cert, set_ca, set_verify, set_alpn
- [x] Declare connection API: dial, send, close, get_alpn, get_userdata, set_userdata
- [x] Declare server API: listen, close_server
- [x] Full Doxygen comments on all public functions

## Task 14: DTLS context implementation
- [x] Create `src/xylem-dtls.c` with license header and struct definitions
- [ ] Implement `xylem_dtls_ctx_create` — `SSL_CTX` with `DTLS_method()`, set cookie callbacks
- [ ] Implement `xylem_dtls_ctx_destroy`
- [ ] Implement `xylem_dtls_ctx_load_cert` / `set_ca` / `set_verify` / `set_alpn`

## Task 15: DTLS memory BIO helpers and retransmission timer
- [ ] Implement `_dtls_flush_write_bio` — read from write BIO, send via UDP
- [ ] Implement `_dtls_feed_read_bio` — write ciphertext into read BIO
- [ ] Implement retransmit timer: `DTLSv1_get_timeout` → `xylem_loop_start_timer` / `xylem_loop_reset_timer`
- [ ] Implement retransmit timer callback: `DTLSv1_handle_timeout` → flush write BIO

## Task 16: DTLS client handshake and dial
- [ ] Implement `xylem_dtls_dial` — create connected UDP socket, create SSL, set BIOs, start handshake
- [ ] Implement async handshake via `SSL_connect` + WANT_READ/WANT_WRITE + BIO flush
- [ ] On handshake success: set `handshake_done`, call user `on_connect`
- [ ] On failure: close with error

## Task 17: DTLS server — listen and accept
- [ ] Implement `xylem_dtls_listen` — bind UDP socket, register on_read
- [ ] Implement peer demux: lookup session by peer address in server's session list
- [ ] New peer: `DTLSv1_listen` for cookie exchange, then `SSL_accept`
- [ ] On handshake success: call user `on_accept`
- [ ] Implement `xylem_dtls_close_server`

## Task 18: DTLS read/write paths
- [ ] Read: UDP on_read → feed read BIO → `SSL_read` → user `on_read`
- [ ] Write: `xylem_dtls_send` → `SSL_write` → flush write BIO → UDP send
- [ ] Handle `SSL_ERROR_ZERO_RETURN` (peer shutdown)

## Task 19: DTLS connection lifecycle
- [ ] Implement `xylem_dtls_close` — `SSL_shutdown`, flush BIO, close UDP
- [ ] Implement `xylem_dtls_get_userdata` / `xylem_dtls_set_userdata`
- [ ] Implement `xylem_dtls_get_alpn`

## Task 20: Tests — DTLS context and basic setup
- [ ] Create `tests/test-dtls.c`
- [ ] Test ctx create/destroy
- [ ] Test load_cert, set_ca, set_verify, set_alpn

## Task 21: Tests — DTLS client/server handshake and data transfer
- [ ] Test DTLS dial + listen: full handshake, on_connect/on_accept fires
- [ ] Test send/recv: plaintext datagram round-trip
- [ ] Test close: graceful shutdown, on_close fires

## Task 22: Tests — DTLS error cases
- [ ] Test handshake failure (wrong CA)
- [ ] Test ALPN negotiation
- [ ] Test close_server detaches sessions
