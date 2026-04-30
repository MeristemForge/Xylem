# UDS 模块测试设计文档

## 概述

`tests/test-uds.c` 包含 11 个测试函数，覆盖 `src/xylem-uds.c` 的所有公共 API 和核心内部分支。

设计风格与 `test-tcp.c` 对称：统一 `_test_ctx_t` 上下文结构体、Safety Timer 防挂起。与 TCP 测试的关键差异：
- 无重连测试（UDS 不支持自动重连）
- 无连接超时测试（UDS 本地连接几乎瞬时完成）
- 无对端地址测试（UDS 无有意义的对端地址）
- 无 MSS 钳制测试（UDS 无 MSS 概念）
- 仅覆盖 FRAME_FIXED 一种分帧策略（其余策略逻辑与 TCP 完全对称，已由 `test-tcp.c` 充分覆盖）

## 测试基础设施

- 统一上下文结构 `_test_ctx_t`，所有测试共用，按需使用字段
- 共享回调：`_srv_accept_cb`（保存 srv_conn + 设置 userdata）、`_srv_read_echo_cb`（回显）
- 每个测试独立创建 Loop，需要运行事件循环的测试额外创建 Safety Timer（10 秒）防止挂起
- 测试间无共享状态
- 单一路径 `UDS_PATH "/tmp/xylem-test-uds.sock"`，测试顺序执行不冲突
- 每个测试结束后 `remove(UDS_PATH)` 清理 socket 文件
- 跨线程测试使用 `xylem_thrdpool_t` 线程池在工作线程上执行 send/close 操作
- 无文件作用域可变变量，所有状态通过 `_test_ctx_t` 和 userdata 传递
- `main` 函数首尾调用 `xylem_startup` 和 `xylem_cleanup`

## 测试列表

### 连接建立（1 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_dial_connect` | dial 异步连接 | on_connect 回调触发，connect_called==1 |

### 数据传输（1 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_echo` | 完整 echo 往返 | connect/accept 触发，数据 "hello" 往返一致，received_len==5 |

### 访问器（3 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_userdata` | 连接级 set/get_userdata | 指针往返一致，解引用值==42 |
| `test_server_userdata` | server 级 set/get_userdata | 指针往返一致，解引用值==99 |
| `test_get_loop` | 获取关联 loop | 与创建时的 loop 相同 |

### 关闭行为（2 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_send_after_close` | close 后 send | `xylem_uds_send` 返回 -1 |
| `test_close_server_with_active_conn` | close_server 带活跃连接 | 定时器触发关闭，活跃连接的 on_close 被触发，close_called>=1 |

### 分帧（1 个）

| 测试函数 | 分帧策略 | 验证点 |
|---------|---------|--------|
| `test_frame_fixed` | FRAME_FIXED(4) | 8 字节拆成 2×4 帧 "ABCD"+"EFGH"，read_count==2 |

### 跨线程操作（3 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_cross_thread_send` | 跨线程 xylem_uds_send + acquire/release | on_connect 中 acquire，工作线程发送 "hello" 后 release，客户端收到回显数据一致，received_len==5 |
| `test_cross_thread_close` | 跨线程 xylem_uds_close + acquire/release | on_connect 中 acquire，工作线程调用 close 后 release，on_close 回调触发，close_called==1 |
| `test_cross_thread_send_stop_on_close` | 跨线程持续 send + 服务端关闭连接 + acquire/release | on_connect 中 acquire，工作线程循环 send，50ms 后服务端关闭连接，on_close 触发后 send 停止（atomic closed 标志），worker release，worker_done==true |

## 覆盖的公共 API

| API 函数 | 覆盖的测试 |
|---------|-----------|
| `xylem_uds_listen` | 全部 11 个 |
| `xylem_uds_dial` | 除 test_server_userdata 外的 10 个 |
| `xylem_uds_send` | test_echo, test_send_after_close, test_frame_fixed, test_cross_thread_send, test_cross_thread_send_stop_on_close |
| `xylem_uds_close` | test_dial_connect, test_echo, test_send_after_close, test_userdata, test_get_loop, test_close_server_with_active_conn, test_cross_thread_close, test_cross_thread_send_stop_on_close |
| `xylem_uds_close_server` | 全部 11 个（异步测试的清理路径 + test_server_userdata 直接调用） |
| `xylem_uds_get_userdata` | test_userdata + 所有回调中通过 userdata 获取 ctx |
| `xylem_uds_set_userdata` | test_userdata + 所有异步测试的 setup |
| `xylem_uds_get_loop` | test_get_loop |
| `xylem_uds_conn_acquire` | test_cross_thread_send, test_cross_thread_close, test_cross_thread_send_stop_on_close |
| `xylem_uds_conn_release` | test_cross_thread_send, test_cross_thread_close, test_cross_thread_send_stop_on_close |
| `xylem_uds_server_get_userdata` | test_server_userdata + _srv_accept_cb（共享回调） |
| `xylem_uds_server_set_userdata` | test_server_userdata + 所有使用 _srv_accept_cb 的测试 |

## 覆盖的内部分支

| 内部函数/路径 | 覆盖的分支 | 触发测试 |
|-------------|-----------|---------|
| `_uds_conn_readable_cb` | 正常读取 + 帧提取循环 | `test_echo`, `test_frame_fixed` |
| `_uds_conn_readable_cb` | 心跳/读超时 reset | （隐式：未配置超时的测试不触发） |
| `_uds_conn_readable_cb` | peer EOF（nread==0） | `test_close_server_with_active_conn`（服务端关闭后客户端收到 EOF） |
| `_uds_conn_readable_cb` | state==CLOSING/CLOSED 退出 | `test_echo`（on_read 中 close 后退出循环） |
| `_uds_extract_frame` | FRAME_NONE | `test_echo` |
| `_uds_extract_frame` | FRAME_FIXED 正常路径 | `test_frame_fixed` |
| `_uds_flush_writes` | 正常完成 + on_write_done | `test_echo`, `test_frame_fixed` |
| `_uds_flush_writes` | state==CLOSING 排空 + shutdown | `test_close_server_with_active_conn` |
| `_uds_conn_io_cb` | 可读事件分发 | 所有异步测试 |
| `_uds_conn_io_cb` | 可写事件分发 + 队列空后切回仅读 | `test_echo`, `test_frame_fixed` |
| `_uds_try_connect` | SO_ERROR==0 成功路径 | `test_dial_connect`（非阻塞 connect 路径） |
| `_uds_setup_conn` | 分配读缓冲区 + 启动 IO | 所有异步测试 |
| `_uds_server_io_cb` | accept 循环 + 创建 conn + 插入链表 | 所有异步测试 |
| `_uds_server_io_cb` | server.closing 提前退出 | `test_close_server_with_active_conn` |
| `_uds_destroy_conn` | 销毁定时器 + IO + fd + 读缓冲区 + on_close + loop_post | 所有异步测试 |
| `_uds_destroy_conn` | 从 server 连接链表移除 | `test_close_server_with_active_conn` |
| `_uds_close_conn` | 排空写队列 + destroy | `test_close_server_with_active_conn`（peer EOF 路径） |
| `xylem_uds_close` | 空队列立即 shutdown + destroy | `test_dial_connect`, `test_echo`, `test_userdata`, `test_get_loop` |
| `xylem_uds_close` | 幂等：CLOSING/CLOSED 提前返回 | `test_send_after_close` |
| `xylem_uds_send` | 正常入队 + 切换读写模式 | `test_echo`, `test_frame_fixed` |
| `xylem_uds_send` | CLOSING/CLOSED 返回 -1 | `test_send_after_close` |
| `xylem_uds_close_server` | 幂等 closing 标志 | （隐式：close_server 在清理路径中可能被重复调用） |
| `xylem_uds_close_server` | 遍历连接链表 + 逐个 close + remove(path) | `test_close_server_with_active_conn` |
| `_uds_conn_free_cb` | 延迟释放连接内存 | 所有异步测试 |
| `_uds_server_free_cb` | 延迟释放 server 内存 | 所有测试 |
| `xylem_uds_send`（跨线程） | 非事件循环线程调用 → `_uds_deferred_send_cb` 转发到事件循环线程入队 | `test_cross_thread_send`, `test_cross_thread_send_stop_on_close` |
| `xylem_uds_conn_acquire` / `xylem_uds_conn_release` | on_connect 中 acquire 递增引用计数，工作线程完成后 release 递减引用计数 | `test_cross_thread_send`, `test_cross_thread_close`, `test_cross_thread_send_stop_on_close` |
| `xylem_uds_close`（跨线程） | 非事件循环线程调用 → `_uds_deferred_close_cb` 转发到事件循环线程执行 | `test_cross_thread_close` |
| `xylem_uds_send`（跨线程 + 连接关闭） | 工作线程持续 send，连接关闭后 atomic state 检查拒绝发送 | `test_cross_thread_send_stop_on_close` |

## 未覆盖的路径

| 路径 | 原因 |
|------|------|
| `_uds_conn_readable_cb` 缓冲区满（space==0） | 需要配置极小 read_buf_size 并发送超量数据 |
| `_uds_conn_readable_cb` recv 错误（非 EAGAIN） | 回环测试中难以触发 |
| `_uds_flush_writes` 发送错误（非 EAGAIN） | 回环测试中难以触发 |
| `_uds_flush_writes` 部分写入（partial write） | 回环测试中本地 socket 通常一次写完 |
| `_uds_flush_writes` 写超时 | 未配置 write_timeout_ms |
| `_uds_try_connect` SO_ERROR!=0 失败路径 | 需要连接到不存在的路径且 connect 返回 EINPROGRESS |
| `_uds_extract_frame` FRAME_LENGTH 所有子路径 | 逻辑与 TCP 完全对称，已由 test-tcp.c 覆盖 |
| `_uds_extract_frame` FRAME_DELIM 所有子路径 | 逻辑与 TCP 完全对称，已由 test-tcp.c 覆盖 |
| `_uds_extract_frame` FRAME_CUSTOM 所有子路径 | 逻辑与 TCP 完全对称，已由 test-tcp.c 覆盖 |
| `_uds_extract_frame` FRAME_FIXED frame_size==0 错误路径 | 逻辑与 TCP 完全对称，已由 test-tcp.c 覆盖 |
| `_uds_read_timeout_cb` | 未配置 read_timeout_ms |
| `_uds_write_timeout_cb` | 未配置 write_timeout_ms |
| `_uds_heartbeat_timeout_cb` | 未配置 heartbeat_ms |
| `xylem_uds_dial` 立即连接成功路径 | 平台相关，macOS 可能走 EINPROGRESS |
| `xylem_uds_dial` 失败路径（socket 创建失败） | 需要极端条件 |
| `xylem_uds_listen` 失败路径（bind 失败） | 需要路径不可写等极端条件 |
