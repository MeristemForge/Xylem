# 实现计划：TLS 模块测试重写

## 概述

重写 `tests/test-tls.c`，覆盖 `src/xylem-tls.c` 的所有公共 API 和 TLS 特有的内部分支。按照 design.md 中的测试分类，分阶段实现 21 个测试函数。不修改 `src/xylem-tls.c`、`include/xylem/xylem-tls.h` 或 `CMakeLists.txt`。

## Tasks

- [x] 1. 搭建测试基础设施
  - [x] 1.1 重写 `tests/test-tls.c` 的头部：license header、includes、宏定义 `TLS_PORT 14433`
    - 包含 `assert.h`、`xylem/xylem-tls.h`、OpenSSL 头文件（`evp.h`、`pem.h`、`x509.h`）、`stdio.h`、`string.h`
    - _Requirements: 1.1, 1.2_
  - [x] 1.2 实现 `_test_ctx_t` 结构体和 `_gen_self_signed()` 辅助函数
    - `_test_ctx_t` 包含 design.md 中定义的所有字段
    - `_gen_self_signed` 使用 OpenSSL API 生成 RSA 2048 自签名证书
    - _Requirements: 2.1_
  - [x] 1.3 实现共享回调函数和 `_safety_timeout_cb`
    - `_safety_timeout_cb`：2 秒安全超时，调用 `xylem_loop_stop`
    - _Requirements: 5.1, 5.4_

- [x] 2. 实现上下文管理 API 测试（6 个）
  - [x] 2.1 实现 `test_ctx_create_destroy`
    - 调用 `xylem_tls_ctx_create()` 验证返回非 NULL，`xylem_tls_ctx_destroy()` 不崩溃
    - _Requirements: 1.1, 1.2_
  - [x] 2.2 实现 `test_load_cert_valid`
    - `_gen_self_signed` 生成证书，`xylem_tls_ctx_load_cert` 返回 0，清理文件
    - _Requirements: 2.1_
  - [x] 2.3 实现 `test_load_cert_invalid`
    - 使用不存在的文件路径，`xylem_tls_ctx_load_cert` 返回 -1
    - _Requirements: 2.2_
  - [x] 2.4 实现 `test_set_ca`
    - `_gen_self_signed` 生成 CA 证书，`xylem_tls_ctx_set_ca` 返回 0
    - _Requirements: 3.1_
  - [x] 2.5 实现 `test_set_verify`
    - `xylem_tls_ctx_set_verify(true)` 和 `xylem_tls_ctx_set_verify(false)` 均不崩溃
    - _Requirements: 3.2_
  - [x] 2.6 实现 `test_set_alpn`
    - `xylem_tls_ctx_set_alpn(ctx, {"h2", "http/1.1"}, 2)` 返回 0
    - _Requirements: 4.1_

- [x] 3. Checkpoint - 确保上下文测试编译通过
  - 确保所有测试编译通过并运行成功，如有问题请询问用户。

- [x] 4. 实现握手与数据传输测试（3 个）
  - [x] 4.1 实现 `test_handshake_and_echo` 的回调函数
    - 服务端回调：`_tls_srv_accept_cb`、`_tls_srv_read_echo_cb`、`_tls_srv_close_cb`
    - 客户端回调：`_tls_cli_connect_cb`（发送 "hello"）、`_tls_cli_read_cb`（验证回显）、`_tls_cli_write_done_cb`、`_tls_cli_close_cb`
    - 回调中使用 `_test_ctx_t` 字段记录状态，不使用文件作用域全局变量
    - _Requirements: 5.1, 5.2, 5.3, 5.4_
  - [x] 4.2 实现 `test_handshake_and_echo` 测试函数
    - 创建 loop + safety timer + 服务端/客户端 TLS 上下文 + listen + dial
    - 验证 accept_called、connect_called、wd_called、received == "hello"、close 触发
    - 完整清理：destroy ctx、destroy timer、destroy loop、remove 证书文件
    - _Requirements: 5.1, 5.2, 5.3, 5.4_
  - [x] 4.3 实现 `test_handshake_failure_wrong_ca`
    - 生成两套独立证书，服务端加载 cert1 禁用验证，客户端启用验证设置 cert2 为 CA
    - 验证握手失败触发 on_close
    - _Requirements: 6.1_
  - [x] 4.4 实现 `test_alpn_negotiation`
    - 服务端和客户端均设置 ALPN `{"h2", "http/1.1"}`
    - 握手完成后在 `on_connect` 中调用 `xylem_tls_get_alpn()` 验证返回 "h2"
    - _Requirements: 4.2, 4.3_

- [x] 5. Checkpoint - 确保握手测试通过
  - 确保所有测试编译通过并运行成功，如有问题请询问用户。

- [x] 6. 实现连接辅助 API 测试（5 个）
  - [x] 6.1 实现 `test_sni_hostname`
    - 创建客户端 TLS 连接，`xylem_tls_set_hostname(tls, "example.com")` 返回 0
    - 立即关闭连接，清理资源
    - _Requirements: 7.1_
  - [x] 6.2 实现 `test_conn_userdata`
    - 在 `on_connect` 回调中 set/get userdata，验证往返一致
    - _Requirements: 8.1_
  - [x] 6.3 实现 `test_server_userdata`
    - `xylem_tls_server_set/get_userdata` 往返一致，在 `on_accept` 中再次验证
    - _Requirements: 8.4_
  - [x] 6.4 实现 `test_peer_addr`
    - 在服务端 `on_accept` 中调用 `xylem_tls_get_peer_addr`，验证非 NULL 且 AF_INET
    - _Requirements: 8.2_
  - [x] 6.5 实现 `test_get_loop`
    - 在客户端 `on_connect` 中调用 `xylem_tls_get_loop`，验证与创建时的 loop 相同
    - _Requirements: 8.3_

- [x] 7. 实现关闭行为测试（3 个）
  - [x] 7.1 实现 `test_close_server_with_active_conn`
    - 服务端接受连接后，定时器触发 `xylem_tls_close_server`，验证活跃连接的 on_close 被触发
    - _Requirements: 9.1_
  - [x] 7.2 实现 `test_close_server_idempotent`
    - `xylem_tls_close_server` 调用两次，第二次不崩溃
    - _Requirements: 9.2_
  - [x] 7.3 实现 `test_send_after_close`
    - 在 `on_connect` 中先 `tls_close` 再 `tls_send`，验证返回 -1
    - _Requirements: 10.1_

- [x] 8. Checkpoint - 确保辅助 API 和关闭测试通过
  - 确保所有测试编译通过并运行成功，如有问题请询问用户。

- [x] 9. 实现 Keylog 测试（2 个）
  - [x] 9.1 实现 `test_keylog_write`
    - `xylem_tls_ctx_set_keylog(ctx, "test_keylog.txt")` 返回 0
    - 完成一次完整握手，读取 keylog 文件验证非空
    - 清理：remove keylog 文件和证书文件
    - _Requirements: 11.1, 11.2_
  - [x] 9.2 实现 `test_keylog_disable`
    - `xylem_tls_ctx_set_keylog(ctx, "test_keylog2.txt")` 返回 0
    - `xylem_tls_ctx_set_keylog(ctx, NULL)` 返回 0
    - _Requirements: 11.3_

- [x] 10. 实现超时与心跳透传测试（2 个）
  - [x] 10.1 实现 `test_read_timeout`
    - 使用 `xylem_tcp_opts_t` 设置 `read_timeout_ms = 100`
    - 客户端连接后不发送数据，验证 TLS `on_timeout` 回调触发且 type == `XYLEM_TCP_TIMEOUT_READ`
    - _Requirements: 12.1_
  - [x] 10.2 实现 `test_heartbeat_miss`
    - 使用 `xylem_tcp_opts_t` 设置 `heartbeat_ms = 100`
    - 客户端连接后不发送数据，验证 TLS `on_heartbeat_miss` 回调触发
    - _Requirements: 12.2_

- [x] 11. 组装 `main()` 函数并最终验证
  - [x] 11.1 实现 `main()` 函数，按顺序调用所有 21 个测试函数
    - 包含 `xylem_startup()` 和 `xylem_cleanup()` 调用
    - 按分类分组：上下文 API → 握手与数据传输 → 辅助 API → 关闭行为 → Keylog → 超时与心跳
    - _Requirements: 1.1-12.2_

- [ ]* 11.2 编写属性测试：TLS echo 数据往返完整性
    - **Property 1: TLS echo 数据往返完整性**
    - **Validates: Requirements 5.1, 5.2, 5.3, 5.4**
    - 循环生成随机长度（1-16384）数据，通过 TLS 连接发送并回显，验证数据一致性

- [ ]* 11.3 编写属性测试：关闭后发送拒绝
    - **Property 3: 关闭后发送拒绝**
    - **Validates: Requirements 10.1**
    - 循环验证关闭后 `xylem_tls_send` 始终返回 -1

- [x] 12. Final checkpoint - 确保所有测试通过
  - 确保所有测试编译通过并运行成功，如有问题请询问用户。

## Notes

- 标记 `*` 的任务为可选，可跳过以加速 MVP
- 每个任务引用具体的 requirements 编号以确保可追溯性
- Checkpoint 确保增量验证
- 所有测试函数内部状态独立（`_test_ctx_t` 栈上零初始化），不使用文件作用域全局变量
- 证书文件在每个测试结束后 `remove()` 清理
