# RUDP 模块测试设计文档

## 概述

`tests/test-rudp.c` 包含 11 个测试函数，覆盖 `src/rudp/xylem-rudp.c` 的所有公共 API 和 RUDP 特有的内部分支。

RUDP 模块构建在 UDP 之上，以下 UDP 层功能已由 `test-udp.c` 覆盖，不在本测试中重复：
- UDP listen/dial 基本收发
- UDP 数据报边界保持

设计风格与 `test-dtls.c` 对称：统一 `_test_ctx_t` 上下文结构体、Safety Timer 防挂起。与 DTLS 测试的关键差异：
- 无证书/TLS 上下文测试（RUDP 无加密层）
- 无 ALPN/SNI/keylog 测试（RUDP 无 TLS 特性）
- 增加 `test_multi_session`（覆盖服务端多会话多路复用和红黑树查找）
- 增加 `test_send_before_handshake`（覆盖握手未完成时 send 拒绝路径）
- 增加 `test_handshake_timeout`（覆盖握手超时定时器路径）

## 测试基础设施

- 统一上下文结构 `_test_ctx_t`，所有测试共用，按需使用字段
- 多会话测试使用额外的 `_multi_cli_t` 结构体，每个客户端独立持有发送/接收缓冲区
- 共享回调：`_rudp_srv_accept_cb`（保存 srv_session + 设置 userdata）、`_rudp_srv_read_echo_cb`（回显）
- 每个测试独立创建 Loop，需要运行事件循环的测试额外创建 Safety Timer（10 秒）防止挂起
- 同步测试（test_server_userdata）不运行事件循环，无需 Safety Timer
- 单一端口 `RUDP_PORT 16433`，测试顺序执行不冲突
- 异步测试使用 100ms 延迟定时器触发发送，确保握手完成后数据不与事件循环竞争
- 无文件作用域可变变量，所有状态通过 `_test_ctx_t`、`_multi_cli_t` 和 userdata 传递
- `main` 函数首尾调用 `xylem_startup` 和 `xylem_cleanup`

## 端口分配

| 端口 | 用途 |
|------|------|
| `RUDP_PORT` (16433) | 所有测试的服务端监听地址 |

客户端使用 `xylem_rudp_dial`（底层 `xylem_udp_dial`），由系统分配临时端口。

## 测试列表

### 握手与数据传输（2 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_handshake_and_echo` | 完整 SYN/ACK 握手 + echo | accept/connect/read/close 全触发，数据 "hello" 往返一致 |
| `test_handshake_timeout` | 握手超时（无服务端） | handshake_ms=200，on_close 触发（可能由握手超时或 ECONNREFUSED 触发） |

### 多会话多路复用（1 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_multi_session` | 服务端同时处理 2 个客户端会话 | accept_called==2，两个客户端各自收到正确的回显数据（"AAA"/"BBB"），会话间数据不串扰 |

### 访问器（4 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_session_userdata` | set/get_userdata | 指针往返一致，解引用值==42 |
| `test_server_userdata` | server_set/get_userdata | 指针往返一致，解引用值==99 |
| `test_peer_addr` | get_peer_addr | IP=="127.0.0.1"，端口==RUDP_PORT |
| `test_get_loop` | get_loop | 与创建时的 loop 相同 |

### 关闭与错误行为（4 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_send_after_close` | close 后 send | `xylem_rudp_send` 返回 -1 |
| `test_close_idempotent` | close 重复调用 | 第二次调用不崩溃 |
| `test_close_server_with_active_session` | close_server 带活跃会话 | 定时器触发关闭，活跃会话的 on_close 被触发 |
| `test_send_before_handshake` | 握手前 send | `xylem_rudp_send` 返回 -1（handshake_done==false） |

## 覆盖的公共 API

| API 函数 | 覆盖的测试 |
|---------|-----------|
| `xylem_rudp_dial` | 除 test_server_userdata 外的 10 个 |
| `xylem_rudp_send` | test_handshake_and_echo, test_send_after_close, test_send_before_handshake, test_multi_session |
| `xylem_rudp_close` | test_handshake_and_echo, test_session_userdata, test_peer_addr, test_get_loop, test_send_after_close, test_close_idempotent, test_send_before_handshake, test_multi_session, test_handshake_timeout |
| `xylem_rudp_get_peer_addr` | test_peer_addr |
| `xylem_rudp_get_loop` | test_get_loop |
| `xylem_rudp_get_userdata` | test_session_userdata + 所有回调中通过 userdata 获取 ctx |
| `xylem_rudp_set_userdata` | test_session_userdata + 所有异步测试的 setup |
| `xylem_rudp_listen` | 除 test_handshake_timeout 外的 10 个 |
| `xylem_rudp_close_server` | test_close_server_with_active_session + 所有异步测试的清理路径 |
| `xylem_rudp_server_get_userdata` | test_server_userdata + _rudp_srv_accept_cb（共享回调） |
| `xylem_rudp_server_set_userdata` | test_server_userdata + 所有使用 _rudp_srv_accept_cb 的测试 |

## 覆盖的内部分支

| 内部函数/路径 | 覆盖的分支 | 触发测试 |
|-------------|-----------|---------|
| `_rudp_client_read_cb` | 握手阶段：decode ACK + conv 匹配 → handshake_done + on_connect | `test_handshake_and_echo` |
| `_rudp_client_read_cb` | 数据阶段：ikcp_input + _rudp_input_complete | `test_handshake_and_echo`（数据阶段） |
| `_rudp_client_read_cb` | closing 检查：提前返回 | `test_handshake_and_echo`（on_read 中 close） |
| `_rudp_server_read_cb` | SYN 新会话：decode → 回复 ACK → 创建 KCP + 插入红黑树 + on_accept | `test_handshake_and_echo` |
| `_rudp_server_read_cb` | SYN 已有会话：回复 ACK，不重复创建 | （隐式：客户端可能重发 SYN） |
| `_rudp_server_read_cb` | KCP 数据分发：提取 conv → find_session → ikcp_input | `test_handshake_and_echo`（数据阶段） |
| `_rudp_server_read_cb` | closing 检查：server.closing 提前返回 | `test_close_server_with_active_session` |
| `_rudp_server_read_cb` | 数据包 len < 4：丢弃 | （隐式：正常测试不产生短包） |
| `_rudp_server_read_cb` | find_session 未命中：丢弃 | （隐式：正常测试不产生未知 conv） |
| `_rudp_do_handshake` → `_rudp_encode/decode_handshake` | SYN 编码/解码 | 所有异步测试（客户端发送 SYN） |
| `_rudp_do_handshake` → `_rudp_encode/decode_handshake` | ACK 编码/解码 | 所有异步测试（服务端回复 ACK） |
| `_rudp_kcp_output_cb` | 客户端路径：dest=NULL（已连接 socket） | `test_handshake_and_echo` |
| `_rudp_kcp_output_cb` | 服务端路径：dest=peer_addr | `test_handshake_and_echo`（服务端回显） |
| `_rudp_update_timeout_cb` | 正常路径：ikcp_update + drain_recv + schedule | 所有异步测试 |
| `_rudp_update_timeout_cb` | dead link 检测：kcp->state == -1 → close | （未覆盖：回环网络无丢包，不触发 dead link） |
| `_rudp_update_timeout_cb` | closing 检查：提前返回 | 所有异步测试（close 后定时器可能仍在队列中） |
| `_rudp_schedule_update` | 正常路径：ikcp_check + reset_timer | 所有异步测试 |
| `_rudp_schedule_update` | closing 检查：提前返回 | 所有异步测试 |
| `_rudp_drain_recv` | 正常路径：循环 ikcp_recv + on_read | `test_handshake_and_echo` |
| `_rudp_drain_recv` | closing 检查：on_read 中 close 后返回 false | `test_handshake_and_echo` |
| `_rudp_input_complete` | flush ACK + drain_recv + schedule_update | 所有异步测试（数据阶段） |
| `_rudp_handshake_timeout_cb` | 握手超时 → close | `test_handshake_timeout`（无服务端时） |
| `_rudp_client_close_cb` | 正常路径：stop timers + ikcp_release + on_close + loop_post | `test_handshake_and_echo` |
| `_rudp_client_close_cb` | UDP 错误传播：close_err==0 且 err!=0 时传播 | `test_handshake_timeout`（ECONNREFUSED 路径） |
| `_rudp_free_cb` | 延迟释放 rudp + destroy timers | 所有异步测试 |
| `_rudp_server_close_cb` | 释放 server 内存 | 所有异步测试 |
| `_rudp_apply_opts` | 快速模式（唯一模式）：nodelay=1, interval=10ms, resend=2, nc=1 | `test_handshake_and_echo`（opts=NULL 使用默认）|
| `_rudp_find_session` | 红黑树查找命中 | `test_handshake_and_echo`（数据阶段）、`test_multi_session` |
| `_rudp_find_session` | 红黑树查找未命中 | `test_handshake_and_echo`（首次 SYN） |
| `_rudp_session_cmp` | 地址 + conv 复合键比较 | `test_multi_session`（两个不同 conv 的会话） |
| `_rudp_addr_cmp` | IPv4 地址比较 | 所有异步测试 |
| `xylem_rudp_send` | 正常路径：ikcp_send + ikcp_flush + schedule | `test_handshake_and_echo`, `test_multi_session` |
| `xylem_rudp_send` | 失败路径：closing==true 返回 -1 | `test_send_after_close` |
| `xylem_rudp_send` | 失败路径：handshake_done==false 返回 -1 | `test_send_before_handshake` |
| `xylem_rudp_close` | 客户端路径：stop timers + udp_close → _rudp_client_close_cb | `test_handshake_and_echo` |
| `xylem_rudp_close` | 服务端路径：erase from rbtree + ikcp_release + on_close + loop_post | `test_close_server_with_active_session` |
| `xylem_rudp_close` | 幂等：closing==true 提前返回 | `test_close_idempotent` |
| `xylem_rudp_close_server` | 遍历红黑树 + 逐个 close + udp_close | `test_close_server_with_active_session` |
| `xylem_rudp_close_server` | 幂等：closing==true 提前返回 | （隐式：close_server 在清理路径中可能被重复调用） |

## 未覆盖的路径

| 路径 | 原因 |
|------|------|
| `_rudp_update_timeout_cb` dead link 检测 | 回环网络无丢包，KCP 不会触发 dead link 状态 |
| `_rudp_kcp_output_cb` 返回错误 | 需要 `xylem_udp_send` 失败，回环测试中不会发生 |
| `_rudp_create_kcp` 失败路径（ikcp_create 返回 NULL） | 需要 mock 内存分配失败，不实际 |
| `xylem_rudp_dial` 失败路径（udp_dial 失败） | 需要端口耗尽等极端条件 |
| `xylem_rudp_listen` 失败路径（udp_listen 失败） | 需要端口占用等极端条件 |
| `_rudp_server_read_cb` SYN 重复（已有会话仅回复 ACK） | 回环网络不丢包，客户端不会重发 SYN |
| `_rudp_server_read_cb` 短包丢弃（len < 4） | 正常测试不产生短包 |
| IPv6 地址 | 所有测试使用 127.0.0.1 回环地址 |
