# TLS 模块测试需求

## 概述

为 `xylem-tls` 模块编写全面的单元测试，覆盖所有公共 API 和 TLS 特有的内部分支。测试文件为 `tests/test-tls.c`。

TLS 模块构建在 TCP 模块之上，因此以下 TCP 层功能不需要在 TLS 测试中重复覆盖：
- 所有分帧策略（NONE/FIXED/LENGTH/DELIM/CUSTOM）
- TCP 层重连机制
- TCP 层写队列 drain
- TCP 层读缓冲区满

---

## Requirement 1

**User Story:** 作为开发者，我需要创建和销毁 TLS 上下文，以便管理 TLS 配置的生命周期。

### Acceptance Criteria

1. WHEN 调用 `xylem_tls_ctx_create()` THEN 系统 SHALL 返回非 NULL 的上下文句柄
2. WHEN 调用 `xylem_tls_ctx_destroy()` THEN 系统 SHALL 释放上下文资源且不崩溃

## Requirement 2

**User Story:** 作为开发者，我需要加载证书和私钥到 TLS 上下文，以便服务端能进行 TLS 握手。

### Acceptance Criteria

1. WHEN 使用有效的自签名证书和私钥调用 `xylem_tls_ctx_load_cert()` THEN 系统 SHALL 返回 0
2. WHEN 使用不存在的文件路径调用 `xylem_tls_ctx_load_cert()` THEN 系统 SHALL 返回 -1

## Requirement 3

**User Story:** 作为开发者，我需要配置 CA 证书和对端验证，以便控制 TLS 握手的证书验证行为。

### Acceptance Criteria

1. WHEN 使用有效的 CA 证书调用 `xylem_tls_ctx_set_ca()` THEN 系统 SHALL 返回 0
2. WHEN 调用 `xylem_tls_ctx_set_verify()` 启用或禁用验证 THEN 系统 SHALL 正常完成且不崩溃

## Requirement 4

**User Story:** 作为开发者，我需要配置 ALPN 协议列表，以便在 TLS 握手中协商应用层协议。

### Acceptance Criteria

1. WHEN 调用 `xylem_tls_ctx_set_alpn()` 设置协议列表 THEN 系统 SHALL 返回 0
2. WHEN 客户端和服务端均设置 ALPN 并完成握手 THEN 系统 SHALL 协商出双方共同支持的协议
3. WHEN 调用 `xylem_tls_get_alpn()` THEN 系统 SHALL 返回协商结果或 NULL

## Requirement 5

**User Story:** 作为开发者，我需要通过 TLS 连接进行握手和数据传输，以便验证端到端加密通信。

### Acceptance Criteria

1. WHEN 客户端 dial 服务端且握手成功 THEN 系统 SHALL 触发服务端 `on_accept` 和客户端 `on_connect` 回调
2. WHEN 客户端通过 TLS 连接发送数据 THEN 系统 SHALL 触发 `on_write_done` 回调
3. WHEN 服务端收到数据并回显 THEN 客户端 SHALL 通过 `on_read` 回调收到相同数据
4. WHEN 关闭 TLS 连接 THEN 系统 SHALL 触发 `on_close` 回调

## Requirement 6

**User Story:** 作为开发者，我需要验证 TLS 握手失败场景，以便确保错误的证书配置被正确拒绝。

### Acceptance Criteria

1. WHEN 客户端启用验证但使用错误的 CA 证书 THEN 系统 SHALL 导致握手失败并触发 `on_close` 回调

## Requirement 7

**User Story:** 作为开发者，我需要设置 SNI 主机名，以便支持虚拟主机和主机名验证。

### Acceptance Criteria

1. WHEN 调用 `xylem_tls_set_hostname()` THEN 系统 SHALL 返回 0 且不崩溃

## Requirement 8

**User Story:** 作为开发者，我需要在 TLS 连接上使用 userdata、peer_addr、get_loop 等辅助 API。

### Acceptance Criteria

1. WHEN 调用 `xylem_tls_set_userdata()` 和 `xylem_tls_get_userdata()` THEN 系统 SHALL 返回相同的指针
2. WHEN 调用 `xylem_tls_get_peer_addr()` THEN 系统 SHALL 返回非 NULL 的对端地址
3. WHEN 调用 `xylem_tls_get_loop()` THEN 系统 SHALL 返回与创建时相同的 loop
4. WHEN 调用 `xylem_tls_server_set_userdata()` 和 `xylem_tls_server_get_userdata()` THEN 系统 SHALL 返回相同的指针

## Requirement 9

**User Story:** 作为开发者，我需要测试 TLS 服务器的关闭行为，以便确保资源正确释放。

### Acceptance Criteria

1. WHEN 调用 `xylem_tls_close_server()` 带活跃连接 THEN 系统 SHALL 关闭所有连接并触发 `on_close`
2. WHEN 重复调用 `xylem_tls_close_server()` THEN 系统 SHALL 幂等处理且不崩溃

## Requirement 10

**User Story:** 作为开发者，我需要测试 TLS 连接的 close 后 send 行为。

### Acceptance Criteria

1. WHEN 在 TLS 连接关闭后调用 `xylem_tls_send()` THEN 系统 SHALL 返回 -1

## Requirement 11

**User Story:** 作为开发者，我需要测试 TLS keylog 功能，以便支持流量分析调试。

### Acceptance Criteria

1. WHEN 调用 `xylem_tls_ctx_set_keylog()` 设置有效路径 THEN 系统 SHALL 返回 0
2. WHEN TLS 握手完成后 THEN keylog 文件 SHALL 包含密钥材料
3. WHEN 调用 `xylem_tls_ctx_set_keylog(ctx, NULL)` THEN 系统 SHALL 禁用 keylog 并返回 0

## Requirement 12

**User Story:** 作为开发者，我需要测试 TLS 层的超时和心跳透传，以便确保 TCP 层事件正确桥接到 TLS 层。

### Acceptance Criteria

1. WHEN TCP 层触发读超时 THEN TLS 层 SHALL 通过 `on_timeout` 回调通知用户
2. WHEN TCP 层触发心跳丢失 THEN TLS 层 SHALL 通过 `on_heartbeat_miss` 回调通知用户
