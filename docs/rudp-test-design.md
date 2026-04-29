# RUDP 模块测试设计文档

## 概述

`tests/test-rudp.c` 包含 16 个测试函数，覆盖 `src/rudp/xylem-rudp.c` 的所有公共 API 和 RUDP 特有的内部分支。

RUDP 模块构建在 UDP 之上，以下 UDP 层功能已由 `test-udp.c` 覆盖，不在本测试中重复：
- UDP listen/dial 基本收发
- UDP 数据报边界保持

设计风格与 `test-dtls.c` 对称：统一 `_test_ctx_t` 上下文结构体、Safety Timer 防挂起。与 DTLS 测试的关键差异：
- 无证书/TLS 上下文测试（RUDP 使用 AES-256-CTR 而非 TLS）
- 无 ALPN/SNI/keylog 测试（RUDP 无 TLS 特性）
- 增加 `test_aes_echo`（覆盖 AES-256-CTR 加密/解密数据路径）
- 增加 `test_aes_with_fec`（覆盖 AES + FEC 组合数据路径）
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
- 跨线程测试使用 `xylem_thrdpool_t` 线程池在工作线程上执行 send/close 操作
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
| `test_handshake_timeout` | 握手超时（无服务端） | handshake_ms=200，SYN 重传后仍无 ACK，on_close 触发（可能由握手超时或 ECONNREFUSED 触发） |

### 多会话多路复用（1 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_multi_session` | 服务端同时处理 2 个客户端会话 | accept_called==2，两个客户端各自收到正确的回显数据（"AAA"/"BBB"），会话间数据不串扰 |

### AES-256-CTR 加密（2 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_aes_echo` | AES-256-CTR 加密 echo | 双方使用相同 32 字节预共享密钥，accept/connect/read/close 全触发，数据 "hello" 往返一致 |
| `test_aes_with_fec` | AES-256-CTR + FEC 组合 echo | 双方使用相同密钥 + fec_data=3/fec_parity=1，accept/connect/read/close 全触发，数据 "hello" 往返一致 |

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

### 跨线程操作（3 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_cross_thread_send` | 跨线程 xylem_rudp_send + acquire/release | on_connect 中 acquire，工作线程发送 "hello" 后 release，客户端收到回显数据一致，received_len==5 |
| `test_cross_thread_close` | 跨线程 xylem_rudp_close + acquire/release | on_connect 中 acquire，工作线程调用 close 后 release，on_close 回调触发，close_called==1 |
| `test_cross_thread_send_stop_on_close` | 跨线程持续 send + 服务端关闭会话 + acquire/release | on_connect 中 acquire，工作线程循环 send（每次间隔 1ms），50ms 后服务端关闭会话，on_close 触发后 send 停止（atomic closed 标志），worker release，worker_done==true |

## 覆盖的公共 API

| API 函数 | 覆盖的测试 |
|---------|-----------|
| `xylem_rudp_dial` | 除 test_server_userdata 外的 15 个 |
| `xylem_rudp_send` | test_handshake_and_echo, test_send_after_close, test_send_before_handshake, test_multi_session, test_aes_echo, test_aes_with_fec, test_cross_thread_send, test_cross_thread_send_stop_on_close |
| `xylem_rudp_close` | test_handshake_and_echo, test_session_userdata, test_peer_addr, test_get_loop, test_send_after_close, test_close_idempotent, test_send_before_handshake, test_multi_session, test_aes_echo, test_aes_with_fec, test_handshake_timeout, test_cross_thread_close, test_cross_thread_send_stop_on_close |
| `xylem_rudp_get_peer_addr` | test_peer_addr |
| `xylem_rudp_get_loop` | test_get_loop |
| `xylem_rudp_get_userdata` | test_session_userdata + 所有回调中通过 userdata 获取 ctx |
| `xylem_rudp_set_userdata` | test_session_userdata + 所有异步测试的 setup |
| `xylem_rudp_listen` | 除 test_handshake_timeout 外的 15 个 |
| `xylem_rudp_close_server` | test_close_server_with_active_session, test_cross_thread_send_stop_on_close + 所有异步测试的清理路径 |
| `xylem_rudp_server_get_userdata` | test_server_userdata + _rudp_srv_accept_cb（共享回调） |
| `xylem_rudp_server_set_userdata` | test_server_userdata + 所有使用 _rudp_srv_accept_cb 的测试 |
| `xylem_rudp_conn_acquire` | test_cross_thread_send, test_cross_thread_close, test_cross_thread_send_stop_on_close |
| `xylem_rudp_conn_release` | test_cross_thread_send, test_cross_thread_close, test_cross_thread_send_stop_on_close |

## 覆盖的内部分支

| 内部函数/路径 | 覆盖的分支 | 触发测试 |
|-------------|-----------|---------|
| `_rudp_client_read_cb` | 握手阶段：decode ACK + conv 匹配 → handshake_done + on_connect | `test_handshake_and_echo` |
| `_rudp_client_read_cb` | 数据阶段：ikcp_input + _rudp_input_complete | `test_handshake_and_echo`（数据阶段） |
| `_rudp_client_read_cb` | closing 检查：提前返回 | `test_handshake_and_echo`（on_read 中 close） |
| `_rudp_server_read_cb` | SYN 新会话：decode → 回复加密 ACK → 创建 KCP + 初始化 FEC + 插入红黑树 + on_accept | `test_handshake_and_echo`, `test_aes_echo`, `test_aes_with_fec` |
| `_rudp_server_read_cb` | SYN 已有会话：回复 ACK，不重复创建 | `test_handshake_and_echo`（客户端 SYN 重传可能在服务端已创建会话后到达） |
| `_rudp_server_read_cb` | FEC DATA 分发：FEC 头后提取 conv → find_session → _rudp_recv_input | `test_aes_with_fec` |
| `_rudp_server_read_cb` | FEC PARITY 分发：遍历同地址 FEC 会话 → _rudp_recv_input | `test_aes_with_fec` |
| `_rudp_server_read_cb` | 原始 KCP 分发：提取 conv → find_session → ikcp_input | `test_handshake_and_echo`（数据阶段） |
| `_rudp_server_read_cb` | closing 检查：server.closing 提前返回 | `test_close_server_with_active_session` |
| `_rudp_server_read_cb` | 数据包 len < 4：丢弃 | （隐式：正常测试不产生短包） |
| `_rudp_server_read_cb` | find_session 未命中：丢弃 | （隐式：正常测试不产生未知 conv） |
| `_rudp_do_handshake` → `_rudp_encode/decode_handshake` | SYN 编码/解码 | 所有异步测试（客户端发送 SYN） |
| `_rudp_do_handshake` → `_rudp_encode/decode_handshake` | ACK 编码/解码 | 所有异步测试（服务端回复 ACK） |
| `_rudp_kcp_output_cb` | 客户端路径：AES 上下文从 rudp->aes 获取 | `test_handshake_and_echo`, `test_aes_echo` |
| `_rudp_kcp_output_cb` | 服务端路径：AES 上下文从 server->aes 借用 | `test_handshake_and_echo`（服务端回显）, `test_aes_echo`（服务端回显） |
| `_rudp_kcp_output_cb` | FEC + AES 路径：FEC 编码后逐个加密发送 | `test_aes_with_fec` |
| `_rudp_update_timeout_cb` | 正常路径：ikcp_update + drain_recv + schedule | 所有异步测试 |
| `_rudp_update_timeout_cb` | dead link 检测：kcp->state == -1 → close | （未覆盖：回环网络无丢包，不触发 dead link） |
| `_rudp_update_timeout_cb` | closing 检查：提前返回 | 所有异步测试（close 后定时器可能仍在队列中） |
| `_rudp_schedule_update` | 正常路径：ikcp_check + reset_timer | 所有异步测试 |
| `_rudp_schedule_update` | closing 检查：提前返回 | 所有异步测试 |
| `_rudp_drain_recv` | 正常路径：循环 ikcp_recv + on_read | `test_handshake_and_echo` |
| `_rudp_drain_recv` | closing 检查：on_read 中 close 后返回 false | `test_handshake_and_echo` |
| `_rudp_input_complete` | flush ACK + drain_recv + schedule_update | 所有异步测试（数据阶段） |
| `_rudp_handshake_timeout_cb` | 握手超时 → close | `test_handshake_timeout`（无服务端时） |
| `_rudp_handshake_timeout_cb` | SYN 重传（未超过 deadline） | `test_handshake_timeout`（handshake_ms=200 > SYN 重传间隔，至少触发 1 次重传后超时）；所有正常异步测试（首次 SYN 成功前定时器可能触发但 handshake_done 已为 true） |
| `_rudp_client_close_cb` | 正常路径：stop timers + ikcp_release + on_close + loop_post | `test_handshake_and_echo` |
| `_rudp_client_close_cb` | UDP 错误传播：close_err==0 且 err!=0 时传播 | `test_handshake_timeout`（ECONNREFUSED 路径） |
| `_rudp_free_cb` | 延迟释放 rudp + destroy timers + destroy FEC + destroy AES（仅客户端） | 所有异步测试 |
| `_rudp_server_close_cb` | 释放 server 内存 + destroy server->aes + 清零密钥 | 所有异步测试 |
| `_rudp_apply_opts` | 快速模式（唯一模式）：nodelay=1, interval=10ms, resend=2, nc=1 | `test_handshake_and_echo`（opts=NULL 使用默认）|
| `_rudp_apply_opts` | AES MTU 调整：减去 RUDP_AES_IV_SIZE | `test_aes_echo`, `test_aes_with_fec` |
| `_rudp_apply_opts` | FEC + AES MTU 调整：减去 FEC 头 + AES IV | `test_aes_with_fec` |
| `_rudp_recv_input` | 无 FEC：直接 ikcp_input | `test_handshake_and_echo`, `test_aes_echo` |
| `_rudp_recv_input` | 有 FEC：FEC 解码后逐个 ikcp_input | `test_aes_with_fec` |
| `_rudp_encrypted_send` | AES 加密路径：xylem_aes256_ctr_encrypt + xylem_udp_send | `test_aes_echo`, `test_aes_with_fec` |
| `_rudp_encrypted_send` | 无 AES 路径：直接 xylem_udp_send | `test_handshake_and_echo` |
| `_rudp_decrypt_packet` | AES 解密路径：xylem_aes256_ctr_decrypt | `test_aes_echo`, `test_aes_with_fec` |
| `_rudp_decrypt_packet` | 无 AES 路径：透传原始数据 | `test_handshake_and_echo` |
| `_rudp_init_fec` | FEC 编码器/解码器创建 | `test_aes_with_fec` |
| `_rudp_find_session` | 红黑树查找命中 | `test_handshake_and_echo`（数据阶段）、`test_multi_session` |
| `_rudp_find_session` | 红黑树查找未命中 | `test_handshake_and_echo`（首次 SYN） |
| `_rudp_session_cmp` | 地址 + conv 复合键比较 | `test_multi_session`（两个不同 conv 的会话） |
| `_rudp_addr_cmp` | IPv4 地址比较 | 所有异步测试 |
| `xylem_rudp_send` | 正常路径：ikcp_send + ikcp_flush + schedule | `test_handshake_and_echo`, `test_multi_session`, `test_aes_echo`, `test_aes_with_fec` |
| `xylem_rudp_send` | 失败路径：closing==true 返回 -1 | `test_send_after_close` |
| `xylem_rudp_send` | 失败路径：handshake_done==false 返回 -1 | `test_send_before_handshake` |
| `xylem_rudp_close` | 客户端路径：stop timers + udp_close → _rudp_client_close_cb | `test_handshake_and_echo` |
| `xylem_rudp_close` | 服务端路径：erase from rbtree + ikcp_release + on_close + loop_post | `test_close_server_with_active_session` |
| `xylem_rudp_close` | 幂等：closing==true 提前返回 | `test_close_idempotent` |
| `xylem_rudp_close_server` | 遍历红黑树 + 逐个 close + udp_close | `test_close_server_with_active_session` |
| `xylem_rudp_close_server` | 幂等：closing==true 提前返回 | （隐式：close_server 在清理路径中可能被重复调用） |
| `xylem_rudp_send`（跨线程） | 非事件循环线程调用 → `_rudp_deferred_send_cb` 转发到事件循环线程入队 KCP | `test_cross_thread_send`, `test_cross_thread_send_stop_on_close` |
| `xylem_rudp_conn_acquire` / `xylem_rudp_conn_release` | on_connect 中 acquire 递增引用计数，工作线程完成后 release 递减引用计数 | `test_cross_thread_send`, `test_cross_thread_close`, `test_cross_thread_send_stop_on_close` |
| `xylem_rudp_close`（跨线程） | 非事件循环线程调用 → `_rudp_deferred_close_cb` 转发到事件循环线程执行 | `test_cross_thread_close` |
| `xylem_rudp_send`（跨线程 + 连接关闭） | 工作线程持续 send，连接关闭后 atomic closing 检查拒绝发送 | `test_cross_thread_send_stop_on_close` |

## 未覆盖的路径

| 路径 | 原因 |
|------|------|
| `_rudp_update_timeout_cb` dead link 检测 | 回环网络无丢包，KCP 不会触发 dead link 状态 |
| `_rudp_kcp_output_cb` 返回错误 | 需要 `xylem_udp_send` 失败，回环测试中不会发生 |
| `_rudp_create_kcp` 失败路径（ikcp_create 返回 NULL） | 需要 mock 内存分配失败，不实际 |
| `xylem_rudp_dial` 失败路径（udp_dial 失败） | 需要端口耗尽等极端条件 |
| `xylem_rudp_listen` 失败路径（udp_listen 失败） | 需要端口占用等极端条件 |
| `_rudp_server_read_cb` 短包丢弃（len < 4） | 正常测试不产生短包 |
| IPv6 地址 | 所有测试使用 127.0.0.1 回环地址 |
