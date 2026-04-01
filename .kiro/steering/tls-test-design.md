# TLS 模块测试设计文档

## 概述

`tests/test-tls.c` 包含 19 个测试函数，覆盖 `src/xylem-tls.c` 的所有公共 API 和 TLS 特有的内部分支。

TLS 模块构建在 TCP 之上，以下 TCP 层功能已由 `test-tcp.c` 覆盖，不在本测试中重复：
- 所有分帧策略（NONE/FIXED/LENGTH/DELIM/CUSTOM）
- TCP 层重连机制
- TCP 层写队列 drain
- TCP 层读缓冲区满

## 测试基础设施

- 统一上下文结构 `_test_ctx_t`，所有测试共用，按需使用字段
- 共享回调：`_tls_srv_accept_cb`（保存 srv_conn + 设置 userdata）、`_tls_srv_read_echo_cb`（回显）
- 每个测试独立创建 Loop + 10 秒 Safety Timer，测试间无共享状态
- 单一端口 `TLS_PORT 14433`，测试顺序执行不冲突
- `_gen_self_signed` 辅助函数：运行时生成 RSA 2048 自签名证书，测试结束后 `remove` 清理
- 无文件作用域可变变量，所有状态通过 `_test_ctx_t` 和 userdata 传递

## 测试列表

### 上下文管理 API（6 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_ctx_create_destroy` | ctx_create + ctx_destroy | 返回非 NULL，销毁不崩溃 |
| `test_load_cert_valid` | ctx_load_cert 成功路径 | 自签名证书加载返回 0 |
| `test_load_cert_invalid` | ctx_load_cert 失败路径 | 不存在的文件返回 -1 |
| `test_set_ca` | ctx_set_ca | 有效 CA 文件返回 0 |
| `test_set_verify` | ctx_set_verify | 启用/禁用均不崩溃 |
| `test_set_alpn` | ctx_set_alpn | 设置协议列表返回 0 |

### 握手与数据传输（3 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_handshake_and_echo` | 完整握手 + echo | accept/connect/write_done/read/close 全触发，数据 "hello" 往返一致 |
| `test_handshake_failure_wrong_ca` | 证书验证失败 | 客户端启用验证 + 错误 CA → on_close 触发 |
| `test_alpn_negotiation` | ALPN 端到端协商 | 双方设置 ALPN，握手后 `xylem_tls_get_alpn` 返回 "h2" |

### 连接辅助 API（5 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_sni_hostname` | tls_set_hostname | 返回 0，关闭不崩溃 |
| `test_conn_userdata` | tls_set/get_userdata | set/get 往返一致（value=42）|
| `test_server_userdata` | tls_server_set/get_userdata | set/get 往返一致（value=99），on_accept 中再次验证 |
| `test_peer_addr` | tls_get_peer_addr | 返回非 NULL，AF_INET |
| `test_get_loop` | tls_get_loop | 与创建时的 loop 相同 |

### 关闭行为（2 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_close_server_with_active_conn` | tls_close_server 带活跃连接 | 定时器触发关闭，活跃连接的 on_close 被触发 |
| `test_send_after_close` | close 后 send | `xylem_tls_send` 返回 -1（closing 标志检查）|

### Keylog（1 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_keylog_write` | ctx_set_keylog + 握手 | keylog 文件非空（fseek/ftell 验证 sz > 0）|

### 超时与心跳透传（2 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_read_timeout` | TCP read_timeout → TLS on_timeout | on_timeout 触发，type == XYLEM_TCP_TIMEOUT_READ |
| `test_heartbeat_miss` | TCP heartbeat → TLS on_heartbeat_miss | on_heartbeat_miss 触发 |

## 覆盖的内部分支

| 内部函数/路径 | 覆盖的分支 | 触发测试 |
|-------------|-----------|---------|
| `_tls_tcp_connect_cb` | 正常路径：init SSL + set_connect_state + do_handshake | `test_handshake_and_echo` |
| `_tls_tcp_connect_cb` | SNI 路径：hostname 非 NULL 时设置 SNI | `test_sni_hostname` |
| `_tls_tcp_accept_cb` | 正常路径：创建 TLS conn + set_accept_state + do_handshake | `test_handshake_and_echo` |
| `_tls_do_handshake` | 成功路径：rc==1，触发 on_accept/on_connect | `test_handshake_and_echo` |
| `_tls_do_handshake` | WANT_READ/WANT_WRITE：flush 后返回 | `test_handshake_and_echo`（多轮握手）|
| `_tls_do_handshake` | 失败路径：flush alert + tcp_close | `test_handshake_failure_wrong_ca` |
| `_tls_tcp_read_cb` | 握手未完成时：调用 do_handshake | `test_handshake_and_echo`（握手阶段）|
| `_tls_tcp_read_cb` | 握手完成后：SSL_read 循环 + on_read | `test_handshake_and_echo`（数据阶段）|
| `_tls_tcp_read_cb` | SSL_ERROR_ZERO_RETURN：对端 close_notify | `test_handshake_and_echo`（关闭阶段）|
| `_tls_tcp_read_cb` | closing 检查：on_read 中触发 close 后退出循环 | `test_handshake_and_echo` |
| `_tls_tcp_close_cb` | 正常路径：SSL_free + on_close + loop_post | `test_handshake_and_echo` |
| `_tls_tcp_close_cb` | server 连接：从链表移除 | `test_handshake_and_echo` |
| `_tls_tcp_timeout_cb` | 透传 on_timeout | `test_read_timeout` |
| `_tls_tcp_heartbeat_cb` | 透传 on_heartbeat_miss | `test_heartbeat_miss` |
| `xylem_tls_send` | 正常路径：SSL_write + flush + on_write_done | `test_handshake_and_echo` |
| `xylem_tls_send` | 失败路径：closing==true 返回 -1 | `test_send_after_close` |
| `xylem_tls_close` | 正常路径：SSL_shutdown + flush + tcp_close | `test_handshake_and_echo` |
| `xylem_tls_close` | 幂等：closing==true 提前返回 | `test_send_after_close` |
| `xylem_tls_close_server` | 正常路径：遍历连接 + 置 NULL + close | `test_close_server_with_active_conn` |
| `_tls_alpn_select_cb` | 协商成功 | `test_alpn_negotiation` |
| `_tls_keylog_cb` | keylog 写入 | `test_keylog_write` |
| `xylem_tls_ctx_set_keylog` | 启用 | `test_keylog_write` |
