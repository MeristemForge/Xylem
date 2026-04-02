# 需求文档

## 简介

为 `xylem-dtls` 模块编写完整的测试文件 `tests/test-dtls.c`，覆盖所有公共 API 的核心流程和关键内部分支。现有测试存在文件作用域可变状态、缺少安全超时定时器、ALPN 端到端协商未验证、多项 API 未覆盖等问题。本需求定义重写后的测试应满足的全部验收标准。

DTLS 模块构建在 UDP 之上，以下 UDP 层功能已由 `test-udp.c` 覆盖，不在本测试中重复：
- UDP listen/dial 基本收发
- UDP 数据报边界保持
- UDP on_error 回调

## 术语表

- **Test_File**：`tests/test-dtls.c` 测试源文件
- **DTLS_Context**：`xylem_dtls_ctx_t` 不透明类型，封装 OpenSSL SSL_CTX
- **DTLS_Session**：`xylem_dtls_t` 不透明类型，表示一个 DTLS 会话（客户端或服务端）
- **DTLS_Server**：`xylem_dtls_server_t` 不透明类型，表示 DTLS 服务器
- **Event_Loop**：`xylem_loop_t` 事件循环
- **Safety_Timer**：防止测试挂起的安全超时定时器
- **Self_Signed_Cert**：运行时通过 OpenSSL API 生成的 RSA 2048 自签名证书

## 需求

### 需求 1：测试基础设施

**用户故事：** 作为开发者，我希望测试基础设施遵循项目代码风格规范，以便测试可靠、可维护且与现有测试一致。

#### 验收标准

1. THE Test_File SHALL 使用统一的上下文结构体 `_test_ctx_t` 通过 userdata 传递状态，无文件作用域可变变量，无全局变量
2. THE Test_File SHALL 为每个异步测试独立创建 Event_Loop 和 Safety_Timer（10 秒超时），测试间无共享状态
3. THE Test_File SHALL 提供 `_gen_self_signed` 辅助函数在运行时生成 Self_Signed_Cert，测试结束后调用 `remove` 清理临时文件
4. THE Test_File SHALL 使用单一端口 `DTLS_PORT 15433` 用于所有测试，测试顺序执行不冲突
5. THE Test_File SHALL 在 `main` 函数首尾调用 `xylem_startup` 和 `xylem_cleanup`
6. WHEN 每个测试函数结束时，THE Test_File SHALL 调用 `xylem_dtls_ctx_destroy`、`xylem_loop_destroy_timer`、`xylem_loop_destroy` 释放所有资源

### 需求 2：上下文管理 API 测试

**用户故事：** 作为开发者，我希望验证 DTLS_Context 的创建、销毁和配置 API 的正确性，以便确保上下文生命周期管理无内存泄漏。

#### 验收标准

1. WHEN `xylem_dtls_ctx_create` 被调用时，THE Test_File SHALL 验证返回值非 NULL，且 `xylem_dtls_ctx_destroy` 不崩溃
2. WHEN 有效的 Self_Signed_Cert 路径传入 `xylem_dtls_ctx_load_cert` 时，THE Test_File SHALL 验证返回值为 0
3. WHEN 不存在的文件路径传入 `xylem_dtls_ctx_load_cert` 时，THE Test_File SHALL 验证返回值为 -1
4. WHEN 有效的 CA 文件传入 `xylem_dtls_ctx_set_ca` 时，THE Test_File SHALL 验证返回值为 0
5. WHEN `xylem_dtls_ctx_set_verify` 分别以 true 和 false 调用时，THE Test_File SHALL 验证调用不崩溃
6. WHEN 协议列表传入 `xylem_dtls_ctx_set_alpn` 时，THE Test_File SHALL 验证返回值为 0

### 需求 3：握手与数据传输测试

**用户故事：** 作为开发者，我希望验证 DTLS 握手和数据回显的完整流程，以便确保客户端-服务端通信正确。

#### 验收标准

1. WHEN 客户端通过 `xylem_dtls_dial` 连接到服务端时，THE Test_File SHALL 验证 `on_connect` 回调被触发
2. WHEN 服务端收到客户端握手时，THE Test_File SHALL 验证 `on_accept` 回调被触发
3. WHEN 客户端在 `on_connect` 中发送 "hello" 且服务端回显时，THE Test_File SHALL 验证客户端 `on_read` 收到的数据为 "hello"，长度为 5
4. WHEN 客户端调用 `xylem_dtls_close` 时，THE Test_File SHALL 验证 `on_close` 回调被触发
5. WHEN 客户端启用证书验证并使用错误的 CA 证书连接服务端时，THE Test_File SHALL 验证 `on_close` 回调被触发（握手失败）

### 需求 4：ALPN 协商测试

**用户故事：** 作为开发者，我希望验证 DTLS ALPN 协议协商的端到端流程，以便确保应用层协议选择正确。

#### 验收标准

1. WHEN 客户端和服务端均设置 ALPN 协议列表 ["h2", "http/1.1"] 时，THE Test_File SHALL 验证握手完成后 `xylem_dtls_get_alpn` 返回 "h2"

### 需求 5：会话 Userdata 测试

**用户故事：** 作为开发者，我希望验证 DTLS_Session 的 userdata 存取 API，以便确保用户数据指针往返一致。

#### 验收标准

1. WHEN `xylem_dtls_set_userdata` 设置一个指针后，THE Test_File SHALL 验证 `xylem_dtls_get_userdata` 返回相同的指针，且解引用值正确

### 需求 6：关闭行为测试

**用户故事：** 作为开发者，我希望验证 DTLS 会话和服务器的关闭行为，以便确保资源正确释放且无崩溃。

#### 验收标准

1. WHEN `xylem_dtls_send` 在会话关闭后被调用时，THE Test_File SHALL 验证返回值为 -1
2. WHEN `xylem_dtls_close_server` 在有活跃会话时被调用，THE Test_File SHALL 验证活跃会话的 `on_close` 回调被触发

### 需求 7：Keylog 测试

**用户故事：** 作为开发者，我希望验证 DTLS keylog 功能，以便确保密钥材料正确写入文件。

#### 验收标准

1. WHEN `xylem_dtls_ctx_set_keylog` 启用后完成一次握手时，THE Test_File SHALL 验证 keylog 文件非空（文件大小 > 0）


