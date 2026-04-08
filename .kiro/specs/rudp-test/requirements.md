# Requirements Document

## Introduction

为 Xylem C 库的 RUDP 模块（`src/rudp/xylem-rudp.c`）编写完整的单元测试套件。测试文件为 `tests/test-rudp.c`，包含 13 个测试函数，覆盖所有公共 API 和 RUDP 特有的内部分支。RUDP 模块构建在 UDP 之上，通过 KCP 提供可靠传输。UDP 层基础功能已由 `test-udp.c` 覆盖，本测试聚焦于 RUDP 层特有的行为：上下文管理、SYN/ACK 握手、KCP 数据传输、会话访问器、关闭行为、发送前置条件、多会话隔离和握手超时。

## Glossary

- **Test_Suite**: RUDP 模块测试套件，即 `tests/test-rudp.c` 文件中的全部测试函数
- **RUDP_Ctx**: RUDP 上下文句柄 (`xylem_rudp_ctx_t`)，管理 KCP 会话 ID（conv）生成
- **RUDP_Session**: RUDP 会话句柄 (`xylem_rudp_t`)，代表一个客户端或服务端 KCP 会话
- **RUDP_Server**: RUDP 服务器句柄 (`xylem_rudp_server_t`)，在单个 UDP socket 上多路复用多个 KCP 会话
- **Test_Ctx**: 统一测试上下文结构体 (`_test_ctx_t`)，所有测试共用，按需使用字段
- **Safety_Timer**: 每个异步测试创建的 10 秒一次性定时器，防止事件循环挂起
- **Echo_Server**: 回显服务端模式，服务端在 `on_read` 回调中将收到的数据原样发回
- **Handshake**: RUDP 轻量级 SYN/ACK 握手协议，9 字节包格式 `[magic:4][type:1][conv:4]`
- **Event_Loop**: Xylem 事件循环 (`xylem_loop_t`)，所有异步 I/O 操作在其上调度

## Requirements

### Requirement 1: 测试基础设施

**User Story:** 作为开发者，我希望测试套件具备统一的基础设施，以便所有测试函数共享一致的上下文结构和回调模式。

#### Acceptance Criteria

1. THE Test_Suite SHALL define a unified `_test_ctx_t` struct containing fields for loop, server handle, client/server session handles, ctx, callback counters, verification flags, and receive buffer
2. THE Test_Suite SHALL provide shared callbacks `_rudp_srv_accept_cb` (save session handle and set userdata) and `_rudp_srv_read_echo_cb` (echo received data back)
3. WHEN any asynchronous test starts, THE Test_Suite SHALL create an independent Event_Loop and a 10-second Safety_Timer to prevent indefinite blocking
4. THE Test_Suite SHALL use a single port constant `RUDP_PORT 16433` for all tests, with tests executing sequentially to avoid port conflicts
5. THE Test_Suite SHALL not use file-scope mutable variables; all state SHALL be passed through `_test_ctx_t` and userdata pointers

### Requirement 2: 上下文管理 API 测试

**User Story:** 作为开发者，我希望验证 RUDP 上下文的创建和销毁功能，以确保上下文生命周期管理正确。

#### Acceptance Criteria

1. WHEN `xylem_rudp_ctx_create` is called, THE Test_Suite SHALL verify that the returned pointer is non-NULL
2. WHEN `xylem_rudp_ctx_destroy` is called with a valid context, THE Test_Suite SHALL verify that the call completes without crashing

### Requirement 3: 握手与数据传输测试

**User Story:** 作为开发者，我希望验证 RUDP 的 SYN/ACK 握手和 KCP 数据回显功能，以确保端到端可靠传输正确工作。

#### Acceptance Criteria

1. WHEN a RUDP client connects to a RUDP server via `xylem_rudp_dial`, THE Test_Suite SHALL verify that the `on_accept` callback fires on the server side and the `on_connect` callback fires on the client side
2. WHEN the client sends data "hello" via `xylem_rudp_send` after handshake completion, THE Echo_Server SHALL echo the data back, and THE Test_Suite SHALL verify that the client receives data identical to "hello" (content and length match)
3. WHEN the echo exchange completes and both sides close, THE Test_Suite SHALL verify that the `on_close` callback fires
4. WHEN `xylem_rudp_opts_t` is configured with `mode=XYLEM_RUDP_MODE_FAST`, THE Test_Suite SHALL verify that the same echo round-trip succeeds with identical data integrity as DEFAULT mode

### Requirement 4: 会话访问器测试

**User Story:** 作为开发者，我希望验证 RUDP 会话和服务器的访问器 API，以确保 userdata、peer address 和 loop 查询功能正确。

#### Acceptance Criteria

1. WHEN `xylem_rudp_set_userdata` is called with a pointer and `xylem_rudp_get_userdata` is called on the same RUDP_Session, THE Test_Suite SHALL verify that the returned pointer is identical to the one set
2. WHEN `xylem_rudp_server_set_userdata` is called with a pointer and `xylem_rudp_server_get_userdata` is called on the same RUDP_Server, THE Test_Suite SHALL verify that the returned pointer is identical to the one set
3. WHEN `xylem_rudp_get_peer_addr` is called on a connected client session, THE Test_Suite SHALL verify that the returned pointer is non-NULL, the IP address is "127.0.0.1", and the port equals RUDP_PORT
4. WHEN `xylem_rudp_get_loop` is called on a RUDP_Session, THE Test_Suite SHALL verify that the returned loop pointer is identical to the loop used when creating the session

### Requirement 5: 关闭行为测试

**User Story:** 作为开发者，我希望验证 RUDP 会话和服务器的关闭行为，以确保关闭操作正确、幂等且不泄漏资源。

#### Acceptance Criteria

1. WHEN `xylem_rudp_send` is called on a RUDP_Session after `xylem_rudp_close` has been called, THE Test_Suite SHALL verify that `xylem_rudp_send` returns -1
2. WHEN `xylem_rudp_close` is called twice on the same RUDP_Session, THE Test_Suite SHALL verify that the second call completes without crashing
3. WHEN `xylem_rudp_close_server` is called on a RUDP_Server with an active session, THE Test_Suite SHALL verify that the active session's `on_close` callback fires

### Requirement 6: 发送前置条件测试

**User Story:** 作为开发者，我希望验证在握手完成前调用 send 的行为，以确保 RUDP 模块正确拒绝过早的发送请求。

#### Acceptance Criteria

1. WHEN `xylem_rudp_send` is called on a RUDP_Session before the SYN/ACK handshake completes (handshake_done == false), THE Test_Suite SHALL verify that `xylem_rudp_send` returns -1

### Requirement 7: 多会话隔离测试

**User Story:** 作为开发者，我希望验证同一服务器上多个客户端会话的隔离性，以确保数据不会在会话间串扰。

#### Acceptance Criteria

1. WHEN two RUDP clients connect to the same RUDP_Server, THE Test_Suite SHALL verify that `on_accept` fires twice (accept_called == 2)
2. WHEN each client sends distinct data through the Echo_Server, THE Test_Suite SHALL verify that each client receives back only its own data without cross-contamination

### Requirement 8: 握手超时测试

**User Story:** 作为开发者，我希望验证客户端在无服务端响应时的握手超时行为，以确保超时机制正确触发关闭。

#### Acceptance Criteria

1. WHEN a RUDP client dials an address with no server listening, THE Test_Suite SHALL verify that the `on_close` callback fires after the handshake timeout expires
2. WHEN the handshake timeout triggers `on_close`, THE Test_Suite SHALL verify that the error message equals "handshake timeout"
