# Implementation Plan: RUDP 模块测试套件

## 概述

为 `src/rudp/xylem-rudp.c` 编写完整的单元测试文件 `tests/test-rudp.c`，包含 13 个测试函数，覆盖所有公共 API 和 RUDP 特有的内部分支。测试风格与 `test-dtls.c` 对称：统一 `_test_ctx_t` 上下文结构体、Safety Timer 防挂起、无文件作用域可变状态。

## Tasks

- [x] 1. 搭建测试基础设施
  - [x] 1.1 在 `tests/CMakeLists.txt` 中注册 RUDP 测试，添加 `xylem_add_test(rudp)`
    - _Requirements: 1.4_
  - [x] 1.2 创建 `tests/test-rudp.c`，编写 license header、includes、`_test_ctx_t` 结构体、共享回调（`_rudp_srv_accept_cb`、`_rudp_srv_read_echo_cb`）、`_safety_timeout_cb`、端口常量 `RUDP_PORT 16433`，以及空的 `main` 函数
    - 包含 `assert.h`（项目自定义宏）和 `xylem/xylem-rudp.h`
    - `_test_ctx_t` 包含 loop、rudp_server、srv_session、cli_session、ctx、accept_called、connect_called、close_called、read_count、verified、value、send_result、received[256]、received_len 字段
    - `_rudp_srv_accept_cb`：保存 srv_session 句柄、递增 accept_called、设置 userdata
    - `_rudp_srv_read_echo_cb`：将收到的数据原样发回
    - `_safety_timeout_cb`：10 秒后调用 `xylem_loop_stop` 防止挂起
    - 无文件作用域可变变量，所有状态通过 `_test_ctx_t` 和 userdata 传递
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5_

- [x] 2. 实现上下文管理 API 测试
  - [x] 2.1 实现 `test_ctx_create_destroy`：调用 `xylem_rudp_ctx_create` 验证返回非 NULL，调用 `xylem_rudp_ctx_destroy` 验证不崩溃
    - _Requirements: 2.1, 2.2_

- [x] 3. 实现握手与数据传输测试
  - [x] 3.1 实现 `test_handshake_and_echo`：创建 loop + safety timer + server + client，运行事件循环完成 SYN/ACK 握手，客户端发送 "hello"，服务端回显，验证 accept_called==1、connect_called==1、received=="hello"、close_called>=1
    - 客户端回调：`_echo_cli_connect_cb`（发送 "hello"、递增 connect_called）、`_echo_cli_read_cb`（保存数据、关闭客户端和服务器）、`_echo_cli_close_cb`（递增 close_called、停循环）
    - _Requirements: 3.1, 3.2, 3.3_
  - [x] 3.2 实现 `test_fast_mode_echo`：与 `test_handshake_and_echo` 相同流程，但 `xylem_rudp_opts_t` 设置 `mode=XYLEM_RUDP_MODE_FAST`，验证数据回显一致
    - _Requirements: 3.4_
  - [ ]* 3.3 编写属性测试：RUDP 数据回显往返一致
    - **Property 1: RUDP 数据回显往返一致**
    - **Validates: Requirement 3.2**

- [x] 4. Checkpoint - 确保所有测试通过
  - 确保所有测试通过，如有问题请询问用户。

- [x] 5. 实现会话访问器测试
  - [x] 5.1 实现 `test_session_userdata`：建立连接后，在 on_connect 中调用 `xylem_rudp_set_userdata` 设置指向 `_test_ctx_t.value=42` 的指针，再调用 `xylem_rudp_get_userdata` 验证返回指针一致且解引用值==42
    - _Requirements: 4.1_
  - [x] 5.2 实现 `test_server_userdata`：创建 server 后调用 `xylem_rudp_server_set_userdata` 设置指向 `_test_ctx_t.value=99` 的指针，再调用 `xylem_rudp_server_get_userdata` 验证返回指针一致且解引用值==99
    - _Requirements: 4.2_
  - [x] 5.3 实现 `test_peer_addr`：建立连接后，在 on_connect 中调用 `xylem_rudp_get_peer_addr` 验证返回非 NULL、IP=="127.0.0.1"、端口==RUDP_PORT
    - _Requirements: 4.3_
  - [x] 5.4 实现 `test_get_loop`：建立连接后，在 on_connect 中调用 `xylem_rudp_get_loop` 验证返回的 loop 指针与创建时的 loop 相同
    - _Requirements: 4.4_
  - [ ]* 5.5 编写属性测试：Userdata 指针往返一致
    - **Property 2: Userdata 指针往返一致**
    - **Validates: Requirements 4.1, 4.2**

- [x] 6. 实现关闭行为测试
  - [x] 6.1 实现 `test_send_after_close`：建立连接后，在 on_connect 中先调用 `xylem_rudp_close`，再调用 `xylem_rudp_send` 验证返回 -1
    - _Requirements: 5.1_
  - [x] 6.2 实现 `test_close_idempotent`：建立连接后，在 on_connect 中连续调用两次 `xylem_rudp_close`，验证第二次调用不崩溃
    - _Requirements: 5.2_
  - [ ]* 6.3 编写属性测试：Close 幂等性
    - **Property 3: Close 幂等性**
    - **Validates: Requirement 5.2**
  - [x] 6.4 实现 `test_close_server_with_active_session`：建立连接后，在 on_connect 中通过定时器延迟调用 `xylem_rudp_close_server`，验证活跃会话的 on_close 被触发
    - _Requirements: 5.3_

- [x] 7. 实现发送前置条件测试
  - [x] 7.1 实现 `test_send_before_handshake`：调用 `xylem_rudp_dial` 后立即调用 `xylem_rudp_send`（握手尚未完成），验证返回 -1，然后关闭客户端和服务器
    - _Requirements: 6.1_

- [x] 8. 实现多会话隔离测试
  - [x] 8.1 实现 `test_multi_session`：创建一个 server 和两个 client，每个 client 发送不同数据（如 "AAA" 和 "BBB"），验证 accept_called==2，每个 client 收到的回显数据与自己发送的数据一致，无串扰
    - _Requirements: 7.1, 7.2_

- [x] 9. 实现握手超时测试
  - [x] 9.1 实现 `test_handshake_timeout`：不创建 server，仅调用 `xylem_rudp_dial` 连接到无人监听的地址，验证 on_close 触发且 errmsg=="handshake timeout"
    - _Requirements: 8.1, 8.2_

- [x] 10. 完善 main 函数并最终验证
  - 在 `main` 中按顺序调用所有 13 个测试函数
  - 确保所有测试通过，如有问题请询问用户。

## 备注

- 标记 `*` 的任务为可选任务，可跳过以加快 MVP 进度
- 每个任务引用了具体的需求编号以确保可追溯性
- Checkpoint 任务确保增量验证
- 属性测试验证通用正确性属性
- 单元测试验证具体示例和边界条件
