# TCP 模块测试设计文档

## 概述

`tests/test-tcp.c` 包含 39 个测试函数，覆盖 `src/xylem-tcp.c` 的所有公共 API 和核心内部分支。

## 测试基础设施

- 统一上下文结构 `_test_ctx_t`，所有测试共用，按需使用字段
- 共享回调：`_srv_accept_cb`（设置 userdata）、`_srv_close_cb`（计数+停循环）、`_srv_read_one_cb`（存单帧）、`_srv_read_two_cb`（存两帧）
- 每个测试独立创建 Loop + 2 秒 Safety Timer，测试间无共享状态
- 单一端口 `TCP_PORT 18080`，测试顺序执行不冲突

## 测试列表

### 公共 API（11 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_listen_and_close` | listen + close_server | server 非 NULL，关闭不崩溃 |
| `test_close_server_with_active_conn` | close_server 带活跃连接 | 活跃连接的 on_close 被触发 |
| `test_close_server_idempotent` | close_server 重复调用 | 第二次调用不崩溃 |
| `test_dial_connect` | dial 异步连接 | on_connect 回调触发 |
| `test_close_empty_queue` | close 空写队列 | 立即触发 on_close |
| `test_send_basic` | send + on_write_done | status=0, len=4 |
| `test_send_after_close` | close 后 send | 返回 -1 |
| `test_conn_userdata` | 连接级 userdata | set/get 往返一致 |
| `test_server_userdata` | server 级 userdata | set/get 往返一致 |
| `test_peer_addr` | 服务端获取客户端地址 | 非 NULL，AF_INET |
| `test_get_loop` | 获取关联 loop | 与创建时的 loop 相同 |

### 分帧 — NONE / FIXED / LENGTH（10 个）

| 测试函数 | 分帧策略 | 验证点 |
|---------|---------|--------|
| `test_frame_none` | FRAME_NONE | echo 模式累积验证 "hello" |
| `test_frame_fixed` | FRAME_FIXED(4) | 8 字节拆成 2×4 帧 "ABCD"+"EFGH" |
| `test_frame_fixed_zero` | FRAME_FIXED(0) | 帧提取错误，on_close 触发 |
| `test_frame_length_be` | LENGTH fixedint 大端 | [00 05 HELLO] → "HELLO" |
| `test_frame_length_le` | LENGTH fixedint 小端 | [05 00 HELLO] → "HELLO" |
| `test_frame_length_field_size_zero` | LENGTH field_size=0 | 帧提取错误，on_close 触发 |
| `test_frame_length_field_size_over8` | LENGTH field_size=9 | 帧提取错误，on_close 触发 |
| `test_frame_length_varint` | LENGTH varint | [varint(5) WORLD] → "WORLD" |
| `test_frame_length_adjustment` | LENGTH adjustment=-2 | 长度含头部，正确提取 payload |
| `test_frame_length_empty_payload` | LENGTH adjustment=-3 | frame_size≤0，on_close 触发 |

### 分帧 — DELIM / CUSTOM（7 个）

| 测试函数 | 分帧策略 | 验证点 |
|---------|---------|--------|
| `test_frame_delim_multi` | DELIM "\r\n" | "hello\r\nworld\r\n" → 2 帧 |
| `test_frame_delim_single` | DELIM "\n" | "abc\ndef\n" → 2 帧（memchr 路径）|
| `test_frame_delim_null` | DELIM NULL | 帧提取错误，on_close 触发 |
| `test_frame_custom_positive` | CUSTOM parse→4 | 8 字节拆成 2×4 帧 |
| `test_frame_custom_zero` | CUSTOM parse→0 | on_read 不触发（数据不足）|
| `test_frame_custom_negative` | CUSTOM parse→-1 | 帧提取错误，on_close 触发 |
| `test_frame_custom_null_parse` | CUSTOM parse=NULL | 帧提取错误，on_close 触发 |

### 超时和心跳（4 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_read_timeout` | read_timeout_ms=100 | on_timeout(READ) 触发 |
| `test_write_timeout` | write_timeout_ms=1 | 填满发送缓冲区后 on_timeout(WRITE) 触发 |
| `test_heartbeat_miss` | heartbeat_ms=100 | 无数据时 on_heartbeat_miss 触发 |
| `test_heartbeat_reset_on_data` | heartbeat_ms=200 | 持续发数据，heartbeat_miss 不触发 |

> `test_connect_timeout` 已移除。该测试依赖 RFC 5737 TEST-NET 地址（192.0.2.1）不可达来触发连接超时，但在 VPN/全局代理环境下这些地址可达（`connect` 返回 `SO_ERROR=0`），导致超时永远不会触发。由于无法找到一个在所有网络环境下都保证不可达的 IPv4 地址，该测试不具备可移植性，因此移除。`_tcp_connect_timeout_cb` 的逻辑仍通过 `test_reconnect_limit`（连接超时 → 重连耗尽）间接覆盖。

### 重连（2 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_reconnect_success` | reconnect_max=3 | 延迟启动 server，重连后 on_connect 触发 |
| `test_reconnect_limit` | reconnect_max=1, connect_timeout_ms=1000 | 无 server，重连耗尽后 on_close 触发 |

### 读写边界与生命周期（5 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_read_buf_full` | read_buf_size=8 | 缓冲区满，on_close 触发 |
| `test_peer_close_eof` | 对端关闭 | 服务端 conn 的 on_close 触发 |
| `test_close_pending_writes` | close 带 pending writes | 3 个 on_write_done 均触发，然后 on_close |
| `test_drain_write_queue_on_error` | 错误关闭 | 对端立即关闭，客户端 on_close 触发 |
| `test_lifecycle_full` | 完整生命周期 | accept/connect/read/close 6 个事件全触发（bitmask 0x3F）|

## 覆盖的内部分支

| 内部函数 | 覆盖的分支 |
|---------|-----------|
| `_tcp_extract_frame` | NONE/FIXED/LENGTH(BE/LE/varint/adj)/DELIM(1/2字节)/CUSTOM(>0/0/<0/NULL) + 错误路径 |
| `_tcp_conn_readable_cb` | 正常读取、缓冲区满(space==0)、peer EOF(nread==0)、心跳/读超时 reset |
| `_tcp_flush_writes` | 正常完成、写超时 |
| `_tcp_close_conn` | 写队列 drain |
| `_tcp_start_reconnect_timer` | 指数退避、达到上限 |
| `_tcp_read/write/connect_timeout_cb` | 读超时、写超时回调（连接超时通过 `test_reconnect_limit` 间接覆盖） |
| `_tcp_heartbeat_timeout_cb` | 心跳超时 |
| `xylem_tcp_close` | 空队列立即 shutdown、非空队列等 flush |
| `xylem_tcp_close_server` | 带活跃连接关闭、幂等 |
