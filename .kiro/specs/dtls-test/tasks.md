# 实现计划：DTLS 模块测试

## 概述

为 `xylem-dtls` 模块创建完整的测试文件 `tests/test-dtls.c`，覆盖所有 15 个公共 API、11 个测试函数和 2 个正确性属性。实现风格与 `test-tls.c` 对称，遵循 `docs/style.md` 代码规范。

## 任务

- [x] 1. 构建系统注册与测试基础设施
  - [x] 1.1 验证构建系统注册
    - 确认 `tests/CMakeLists.txt` 已包含 `xylem_add_test(dtls)` 和 `target_link_libraries(test-dtls PRIVATE OpenSSL::SSL OpenSSL::Crypto)`
    - 若缺失则添加（当前已存在，仅需验证）
    - _需求: 1.4, 1.5_

  - [x] 1.2 创建测试文件骨架与基础设施
    - 创建 `tests/test-dtls.c`，包含许可证头、必要的 `#include`
    - 定义 `#define DTLS_PORT 15433` 和 `#define SAFETY_TIMEOUT_MS 10000`
    - 实现统一上下文结构体 `_test_ctx_t`（字段参考设计文档）
    - 实现 `_gen_self_signed` 辅助函数（运行时生成 RSA 2048 自签名证书）
    - 实现共享回调：`_safety_timeout_cb`、`_dtls_srv_accept_cb`、`_dtls_srv_read_echo_cb`
    - 实现 `main` 函数骨架，首尾调用 `xylem_startup` / `xylem_cleanup`
    - 无文件作用域可变变量，无全局变量
    - _需求: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6_

- [x] 2. 上下文管理 API 测试（6 个同步测试）
  - [x] 2.1 实现 test_ctx_create_destroy
    - 验证 `xylem_dtls_ctx_create` 返回非 NULL
    - 验证 `xylem_dtls_ctx_destroy` 不崩溃
    - _需求: 2.1_

  - [x] 2.2 实现 test_load_cert_valid
    - 生成自签名证书，验证 `xylem_dtls_ctx_load_cert` 返回 0
    - 测试结束后 `remove` 清理临时文件
    - _需求: 2.2_

  - [x] 2.3 实现 test_load_cert_invalid
    - 传入不存在的文件路径，验证 `xylem_dtls_ctx_load_cert` 返回 -1
    - _需求: 2.3_

  - [x] 2.4 实现 test_set_ca
    - 生成自签名证书作为 CA，验证 `xylem_dtls_ctx_set_ca` 返回 0
    - 测试结束后 `remove` 清理临时文件
    - _需求: 2.4_

  - [x] 2.5 实现 test_set_verify
    - 分别以 true 和 false 调用 `xylem_dtls_ctx_set_verify`，验证不崩溃
    - _需求: 2.5_

  - [x] 2.6 实现 test_set_alpn
    - 设置协议列表 `["h2", "http/1.1"]`，验证 `xylem_dtls_ctx_set_alpn` 返回 0
    - _需求: 2.6_

- [x] 3. 检查点 - 确保上下文管理测试通过
  - 确保所有测试通过，如有问题请询问用户。

- [x] 4. 握手与数据传输测试（2 个异步测试）
  - [x] 4.1 实现 test_handshake_and_echo
    - 创建服务端（加载证书、禁用验证）和客户端（禁用验证）
    - 客户端 `on_connect` 中发送 "hello"，服务端回显
    - 验证：`accept_called == 1`、`connect_called == 1`、`read_count >= 1`、`received_len == 5`、数据为 "hello"
    - 每个测试独立创建 Loop + Safety Timer，测试结束释放所有资源
    - _需求: 3.1, 3.2, 3.3, 3.4_

  - [x] 4.2 实现 test_handshake_failure_wrong_ca
    - 服务端使用证书 A，客户端启用验证并使用证书 B 作为 CA
    - 验证 `close_called >= 1`（握手失败触发 on_close）
    - _需求: 3.5_

  - [ ]* 4.3 编写属性测试：DTLS 数据回显往返一致
    - **Property 1: DTLS 数据回显往返一致**
    - 对于任意非空明文数据报，通过 DTLS 会话发送到回显服务端后，客户端收到的数据应与发送的数据完全一致
    - 循环随机输入至少 100 次迭代
    - 标签：`/* Feature: dtls-test, Property 1: DTLS echo round trip */`
    - **验证: 需求 3.3**

- [x] 5. 检查点 - 确保握手与数据传输测试通过
  - 确保所有测试通过，如有问题请询问用户。

- [x] 6. ALPN 协商测试
  - [x] 6.1 实现 test_alpn_negotiation
    - 客户端和服务端均设置 ALPN 协议列表 `["h2", "http/1.1"]`
    - 握手完成后验证 `xylem_dtls_get_alpn` 返回 "h2"
    - _需求: 4.1_

- [x] 7. 会话 Userdata 测试
  - [x] 7.1 实现 test_session_userdata
    - 在 `on_connect` 回调中调用 `xylem_dtls_set_userdata` 设置指针
    - 验证 `xylem_dtls_get_userdata` 返回相同指针，解引用值为 42
    - _需求: 5.1_

  - [ ]* 7.2 编写属性测试：Userdata 指针往返一致
    - **Property 2: Userdata 指针往返一致**
    - 对于任意指针值，通过 `xylem_dtls_set_userdata` 设置后，`xylem_dtls_get_userdata` 应返回相同的指针
    - 循环随机输入至少 100 次迭代
    - 标签：`/* Feature: dtls-test, Property 2: Userdata pointer round trip */`
    - **验证: 需求 5.1**

- [x] 8. 关闭行为测试（2 个）
  - [x] 8.1 实现 test_send_after_close
    - 客户端 `on_connect` 中先调用 `xylem_dtls_close`，再调用 `xylem_dtls_send`
    - 验证 `xylem_dtls_send` 返回 -1
    - _需求: 6.1_

  - [x] 8.2 实现 test_close_server_with_active_session
    - 定时器触发 `xylem_dtls_close_server`，验证活跃会话的 `on_close` 被触发
    - 验证 `close_called >= 1`
    - _需求: 6.2_

- [x] 9. Keylog 测试
  - [x] 9.1 实现 test_keylog_write
    - 客户端上下文调用 `xylem_dtls_ctx_set_keylog` 启用 keylog
    - 完成一次握手后验证 keylog 文件非空（`fseek/ftell` 验证 `sz > 0`）
    - 测试结束后 `remove` 清理 keylog 文件
    - _需求: 7.1_

- [x] 10. 完善 main 函数并最终验证
  - [x] 10.1 在 main 函数中按顺序注册所有测试函数调用
    - 顺序：上下文管理 6 个 → 握手 2 个 → ALPN → userdata → 关闭 2 个 → keylog
    - 属性测试函数也在 main 中调用（如已实现）
    - _需求: 1.5_

- [x] 11. 最终检查点 - 确保所有测试通过
  - 确保所有测试通过，如有问题请询问用户。

## 备注

- 标记 `*` 的任务为可选，可跳过以加速 MVP
- 每个任务引用具体需求以确保可追溯性
- 检查点确保增量验证
- 属性测试验证通用正确性属性
- 单元测试验证具体示例和边界条件
- 实现语言：C（与设计文档一致）
- 参考文件：`tests/test-tls.c`（实现风格）、`docs/style.md`（代码规范）
