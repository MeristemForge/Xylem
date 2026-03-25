# Requirements Document

## Introduction

为 Xylem C 库添加 WebSocket（RFC 6455）支持。该模块提供客户端和服务端两种角色，支持 ws://（基于 TCP）和 wss://（基于 TLS）两种传输方式。WebSocket 模块构建在现有的 TCP、TLS、HTTP、SHA-1、Base64 和事件循环模块之上，遵循 Xylem 的异步回调驱动架构，不引入任何外部依赖。

## Glossary

- **WS_Module**: WebSocket 模块，Xylem 库中实现 RFC 6455 协议的组件，公共 API 前缀为 `xylem_ws_*`
- **WS_Client**: WebSocket 客户端连接句柄，由 `xylem_ws_dial` 创建，负责发起握手和数据通信
- **WS_Server**: WebSocket 服务端句柄，由 `xylem_ws_listen` 创建，负责监听并接受 WebSocket 连接
- **WS_Conn**: WebSocket 服务端接受的单个连接句柄，在 `on_accept` 回调中交付给用户
- **Frame**: RFC 6455 定义的 WebSocket 数据帧，包含 opcode、payload length、masking key 和 payload data
- **Opcode**: 帧类型标识符，包括 text (0x1)、binary (0x2)、close (0x8)、ping (0x9)、pong (0xA)
- **Masking**: RFC 6455 要求客户端发送的每一帧必须使用 4 字节随机掩码对 payload 进行异或变换
- **Opening_Handshake**: 客户端发送 HTTP Upgrade 请求、服务端回复 101 Switching Protocols 的过程
- **Closing_Handshake**: 一端发送 close 帧、另一端回复 close 帧后关闭 TCP 连接的过程
- **Fragmentation**: 将一条逻辑消息拆分为多个帧传输的机制，首帧 FIN=0，续帧 opcode=0x0，末帧 FIN=1
- **Event_Loop**: Xylem 的事件循环 (`xylem_loop_t`)，所有异步 I/O 操作在其上调度
- **Transport**: 底层传输层，TCP (`xylem_tcp_conn_t`) 或 TLS (`xylem_tls_t`)

## Requirements

### Requirement 1: Opening Handshake — 客户端发起

**User Story:** 作为开发者，我希望通过一个异步 API 发起 WebSocket 连接，以便在事件循环中建立 ws:// 或 wss:// 通信。

#### Acceptance Criteria

1. WHEN the caller invokes `xylem_ws_dial` with a ws:// URL, THE WS_Client SHALL establish a TCP connection to the target host and port, then send an HTTP/1.1 Upgrade request containing the headers `Upgrade: websocket`, `Connection: Upgrade`, `Sec-WebSocket-Key` (16 bytes random, Base64-encoded), and `Sec-WebSocket-Version: 13`
2. WHEN the caller invokes `xylem_ws_dial` with a wss:// URL and XYLEM_ENABLE_TLS is enabled, THE WS_Client SHALL establish a TLS connection to the target host and port, then send the same HTTP/1.1 Upgrade request as for ws://
3. WHEN the server responds with HTTP 101 and a valid `Sec-WebSocket-Accept` header, THE WS_Client SHALL verify the accept value by computing SHA-1 of the concatenation of the sent `Sec-WebSocket-Key` and the GUID `258EAFA5-E914-47DA-95CA-5AB9DC63B5E0`, then Base64-encoding the result, and invoke the `on_open` callback
4. IF the server responds with a status code other than 101, THEN THE WS_Client SHALL invoke the `on_close` callback with an error code indicating handshake failure
5. IF the `Sec-WebSocket-Accept` value does not match the expected value, THEN THE WS_Client SHALL close the underlying transport and invoke the `on_close` callback with an error code indicating handshake validation failure

### Requirement 2: Opening Handshake — 服务端接受

**User Story:** 作为开发者，我希望创建一个 WebSocket 服务端来接受客户端连接，以便构建实时通信服务。

#### Acceptance Criteria

1. WHEN `xylem_ws_listen` is called with a bind address, THE WS_Server SHALL start a TCP server (or TLS server when cert and key paths are provided) on the specified address and begin accepting incoming connections
2. WHEN a client sends a valid HTTP/1.1 Upgrade request with `Upgrade: websocket`, `Connection: Upgrade`, `Sec-WebSocket-Version: 13`, and a valid `Sec-WebSocket-Key`, THE WS_Server SHALL respond with HTTP 101 Switching Protocols, compute the `Sec-WebSocket-Accept` value, and invoke the `on_accept` callback with the new WS_Conn handle
3. IF a client sends an Upgrade request missing any required header (`Upgrade`, `Connection`, `Sec-WebSocket-Key`, or `Sec-WebSocket-Version`), THEN THE WS_Server SHALL respond with HTTP 400 Bad Request and close the connection
4. IF a client sends `Sec-WebSocket-Version` with a value other than 13, THEN THE WS_Server SHALL respond with HTTP 426 Upgrade Required, include a `Sec-WebSocket-Version: 13` header, and close the connection

### Requirement 3: 数据帧发送

**User Story:** 作为开发者，我希望通过 WebSocket 连接发送 text 和 binary 消息，以便进行双向数据通信。

#### Acceptance Criteria

1. WHEN the caller invokes `xylem_ws_send` with opcode text (0x1) and a payload, THE WS_Module SHALL construct a WebSocket frame with FIN=1, the specified opcode, and the payload, then write the frame to the underlying transport
2. WHEN the caller invokes `xylem_ws_send` with opcode binary (0x2) and a payload, THE WS_Module SHALL construct a WebSocket frame with FIN=1, the specified opcode, and the payload, then write the frame to the underlying transport
3. WHILE the connection role is client, THE WS_Module SHALL generate a 4-byte random masking key for each outgoing frame and apply the mask to the payload before transmission
4. WHILE the connection role is server, THE WS_Module SHALL send frames without masking (mask bit = 0)
5. WHEN the payload length is 125 bytes or fewer, THE WS_Module SHALL encode the length in the 7-bit payload length field of the frame header
6. WHEN the payload length is between 126 and 65535 bytes, THE WS_Module SHALL set the 7-bit payload length field to 126 and encode the actual length as a 16-bit unsigned integer in network byte order
7. WHEN the payload length exceeds 65535 bytes, THE WS_Module SHALL set the 7-bit payload length field to 127 and encode the actual length as a 64-bit unsigned integer in network byte order

### Requirement 4: 数据帧接收与解析

**User Story:** 作为开发者，我希望接收并解析 WebSocket 帧，以便处理对端发送的消息。

#### Acceptance Criteria

1. WHEN a complete data frame (text or binary) with FIN=1 is received, THE WS_Module SHALL invoke the `on_message` callback with the opcode and the unmasked payload
2. WHEN a masked frame is received, THE WS_Module SHALL unmask the payload by XOR-ing each byte with the corresponding byte of the 4-byte masking key (cyclic)
3. WHILE the connection role is server, IF a client frame is received without the mask bit set, THEN THE WS_Module SHALL close the connection with status code 1002 (Protocol Error)
4. WHILE the connection role is client, IF a server frame is received with the mask bit set, THEN THE WS_Module SHALL close the connection with status code 1002 (Protocol Error)
5. IF a frame with a reserved opcode (3-7 or 0xB-0xF) is received, THEN THE WS_Module SHALL close the connection with status code 1002 (Protocol Error)
6. IF a control frame (opcode >= 0x8) with payload length exceeding 125 bytes is received, THEN THE WS_Module SHALL close the connection with status code 1002 (Protocol Error)
7. IF a control frame with FIN=0 is received, THEN THE WS_Module SHALL close the connection with status code 1002 (Protocol Error)

### Requirement 5: 消息分片（Fragmentation）

**User Story:** 作为开发者，我希望 WebSocket 模块自动处理大消息的分片发送和接收端重组，以便无需关心底层帧拆分细节。

#### Acceptance Criteria

1. WHEN the caller invokes `xylem_ws_send` with a payload exceeding an internal fragment threshold, THE WS_Module SHALL automatically split the payload into multiple frames: the first frame with FIN=0 and the original opcode, intermediate frames with FIN=0 and opcode 0x0, and the final frame with FIN=1 and opcode 0x0
2. WHEN the caller invokes `xylem_ws_send` with a payload not exceeding the fragment threshold, THE WS_Module SHALL send a single frame with FIN=1 and the specified opcode (no fragmentation)
3. WHEN a frame with FIN=0 is received, THE WS_Module SHALL buffer the payload and wait for continuation frames until a frame with FIN=1 and opcode 0x0 is received, then invoke the `on_message` callback with the reassembled payload and the original opcode
4. WHILE receiving a fragmented message, WHEN a control frame (ping, pong, or close) is received between fragments, THE WS_Module SHALL process the control frame immediately without disrupting the fragmented message reassembly
5. IF a new data frame (opcode 0x1 or 0x2) is received while a fragmented message is in progress, THEN THE WS_Module SHALL close the connection with status code 1002 (Protocol Error)

### Requirement 6: Ping/Pong 心跳

**User Story:** 作为开发者，我希望使用 ping/pong 机制检测连接活性，以便及时发现断开的连接。

#### Acceptance Criteria

1. WHEN the caller invokes `xylem_ws_ping` with optional payload data (up to 125 bytes), THE WS_Module SHALL send a ping frame (opcode 0x9) with the specified payload
2. WHEN a ping frame is received, THE WS_Module SHALL automatically send a pong frame (opcode 0xA) with the same payload as the received ping, and invoke the `on_ping` callback
3. WHEN a pong frame is received, THE WS_Module SHALL invoke the `on_pong` callback with the pong payload
4. IF the caller invokes `xylem_ws_ping` with a payload exceeding 125 bytes, THEN THE WS_Module SHALL return an error code without sending the frame

### Requirement 7: 关闭握手（Closing Handshake）

**User Story:** 作为开发者，我希望优雅地关闭 WebSocket 连接，以便双方都能正确释放资源。

#### Acceptance Criteria

1. WHEN the caller invokes `xylem_ws_close` with a status code and optional reason string, THE WS_Module SHALL send a close frame (opcode 0x8) containing the 2-byte status code in network byte order followed by the reason string (up to 123 bytes), then enter the closing state
2. WHILE in the closing state, THE WS_Module SHALL continue to process incoming frames but discard outgoing data frames
3. WHEN a close frame is received and the connection is not in the closing state, THE WS_Module SHALL send a close frame echoing the received status code, then close the underlying transport and invoke the `on_close` callback with the received status code and reason
4. WHEN a close frame is received and the connection is already in the closing state, THE WS_Module SHALL close the underlying transport and invoke the `on_close` callback with the received status code and reason
5. IF the remote end closes the underlying transport without sending a close frame, THEN THE WS_Module SHALL invoke the `on_close` callback with status code 1006 (Abnormal Closure)
6. IF no close frame is received within a configurable timeout after sending a close frame, THEN THE WS_Module SHALL forcibly close the underlying transport and invoke the `on_close` callback

### Requirement 8: 事件循环集成

**User Story:** 作为开发者，我希望 WebSocket 模块与 Xylem 事件循环无缝集成，以便在同一个事件循环中管理多种 I/O 资源。

#### Acceptance Criteria

1. THE WS_Client SHALL accept a `xylem_loop_t*` parameter and perform all I/O operations on the specified event loop
2. THE WS_Server SHALL accept a `xylem_loop_t*` parameter and perform all I/O operations on the specified event loop
3. THE WS_Module SHALL invoke all user callbacks (on_open, on_accept, on_message, on_ping, on_pong, on_close) on the event loop thread
4. THE WS_Module SHALL use `xylem_loop_timer_t` for close handshake timeout management

### Requirement 9: 传输层抽象

**User Story:** 作为开发者，我希望 ws:// 和 wss:// 使用统一的 API，以便无需关心底层传输细节。

#### Acceptance Criteria

1. THE WS_Module SHALL provide identical public API signatures for ws:// and wss:// connections, with传输方式由 URL scheme 或配置参数决定
2. WHEN the URL scheme is ws://, THE WS_Module SHALL use `xylem_tcp_dial` or `xylem_tcp_listen` as the underlying transport
3. WHEN the URL scheme is wss:// and XYLEM_ENABLE_TLS is enabled, THE WS_Module SHALL use `xylem_tls_dial` or `xylem_tls_listen` as the underlying transport
4. IF the URL scheme is wss:// and XYLEM_ENABLE_TLS is not enabled, THEN THE WS_Module SHALL return NULL from `xylem_ws_dial` and `xylem_ws_listen`

### Requirement 10: 回调接口

**User Story:** 作为开发者，我希望通过回调函数处理 WebSocket 事件，以便与 Xylem 的异步编程模型保持一致。

#### Acceptance Criteria

1. THE WS_Module SHALL define a handler struct (`xylem_ws_handler_t`) containing function pointers: `on_open`, `on_accept`, `on_message`, `on_ping`, `on_pong`, and `on_close`
2. THE WS_Module SHALL allow the user to attach and retrieve a `void*` user data pointer on each WS_Client and WS_Conn handle via `xylem_ws_set_userdata` and `xylem_ws_get_userdata`
3. WHEN `on_message` is invoked, THE WS_Module SHALL provide the opcode (text or binary) and a pointer to the payload data along with the payload length
4. WHEN `on_close` is invoked, THE WS_Module SHALL provide the close status code and the reason string (which may be empty)

### Requirement 11: 帧编码/解码往返一致性

**User Story:** 作为开发者，我希望帧的编码和解码过程互为逆操作，以确保数据传输的正确性。

#### Acceptance Criteria

1. FOR ALL valid payload data and opcodes, encoding a frame then decoding the resulting bytes SHALL produce a payload identical to the original input
2. FOR ALL valid masked frames, unmasking the payload then re-masking with the same key SHALL produce bytes identical to the original masked payload
3. FOR ALL three payload length encodings (7-bit, 16-bit extended, 64-bit extended), encoding a length value then decoding the frame header SHALL recover the original length value

### Requirement 12: 资源管理

**User Story:** 作为开发者，我希望 WebSocket 模块正确管理内存和连接资源，以避免泄漏。

#### Acceptance Criteria

1. WHEN `xylem_ws_close` completes and the `on_close` callback has been invoked, THE WS_Module SHALL free all internal buffers and state associated with the connection
2. WHEN `xylem_ws_close_server` is called, THE WS_Server SHALL stop accepting new connections and free the server handle resources; existing connections remain unaffected and must be closed individually
3. THE WS_Module SHALL not access user-provided callback pointers or user data after the `on_close` callback returns

### Requirement 13: UTF-8 校验

**User Story:** 作为开发者，我希望 WebSocket 模块对 text 帧执行 UTF-8 校验，以确保符合 RFC 6455 §5.6 的要求并拒绝非法数据。

#### Acceptance Criteria

1. WHEN a complete text message (single frame with FIN=1, or reassembled from fragments) is received, THE WS_Module SHALL validate that the payload is well-formed UTF-8 before invoking the `on_message` callback
2. IF the payload of a text message fails UTF-8 validation, THEN THE WS_Module SHALL close the connection with status code 1007 (Invalid Frame Payload Data)
3. WHEN a close frame contains a reason string (bytes following the 2-byte status code), THE WS_Module SHALL validate that the reason string is well-formed UTF-8; IF validation fails, THEN THE WS_Module SHALL close the connection with status code 1007

### Requirement 14: 最大消息大小限制

**User Story:** 作为开发者，我希望能够配置接收消息的最大大小，以防止恶意或异常的大消息耗尽内存。

#### Acceptance Criteria

1. THE WS_Module SHALL support a configurable maximum incoming message size (in bytes) on both WS_Client and WS_Conn, with a sensible default value
2. WHEN a single non-fragmented frame is received with a payload length exceeding the configured maximum, THE WS_Module SHALL close the connection with status code 1009 (Message Too Big) without allocating a buffer for the payload
3. WHEN reassembling a fragmented message, IF the accumulated payload size exceeds the configured maximum, THEN THE WS_Module SHALL close the connection with status code 1009 (Message Too Big) and discard the partially reassembled buffer
4. THE WS_Module SHALL allow the maximum message size to be set to 0 to indicate no limit

### Requirement 15: Close 状态码校验

**User Story:** 作为开发者，我希望 WebSocket 模块校验 close 帧中的状态码合法性，以确保符合 RFC 6455 §7.4 的要求。

#### Acceptance Criteria

1. WHEN a close frame is received with a payload of exactly 1 byte, THE WS_Module SHALL close the connection with status code 1002 (Protocol Error), since a close frame payload must be either 0 bytes or at least 2 bytes
2. WHEN a close frame is received with a status code in the reserved range (0-999, 1004-1006, 1015, or 1016-2999), THE WS_Module SHALL close the connection with status code 1002 (Protocol Error)
3. WHEN the caller invokes `xylem_ws_close` with a status code outside the valid application range (1000-1003, 1007-1011, or 3000-4999), THE WS_Module SHALL return an error code without sending the close frame
4. WHEN a close frame is received with no payload (0 bytes), THE WS_Module SHALL treat it as a valid close frame with no status code and invoke the `on_close` callback with status code 1005 (No Status Received)

### Requirement 16: 握手超时

**User Story:** 作为开发者，我希望能够配置 WebSocket 握手阶段的超时时间，以避免连接在握手阶段无限期挂起。

#### Acceptance Criteria

1. THE WS_Client SHALL support a configurable handshake timeout (in milliseconds), controlling the maximum time allowed from TCP/TLS connection established to receiving the HTTP 101 Upgrade response
2. IF the handshake timeout expires before the server responds with HTTP 101, THEN THE WS_Client SHALL close the underlying transport and invoke the `on_close` callback with an error code indicating handshake timeout
3. THE WS_Server SHALL support a configurable handshake timeout (in milliseconds), controlling the maximum time allowed from accepting a TCP/TLS connection to receiving a valid HTTP Upgrade request
4. IF the server-side handshake timeout expires before a valid Upgrade request is received, THEN THE WS_Server SHALL close the connection without sending an HTTP response

### Requirement 17: 背压处理

**User Story:** 作为开发者，我希望在发送缓冲区积压时得到通知，以便实现流量控制避免内存无限增长。

#### Acceptance Criteria

1. THE WS_Module SHALL define an `on_drain` callback in the handler struct, invoked when the underlying transport's write buffer has been fully flushed after a previous write was buffered
2. WHEN `xylem_ws_send` or `xylem_ws_send_fragment` is called, THE function SHALL return 0 if the data was immediately written to the socket, or 1 if the data was queued in the write buffer
3. THE WS_Module SHALL allow the user to query the current pending write buffer size via `xylem_ws_get_buffered_amount` on both WS_Client and WS_Conn
