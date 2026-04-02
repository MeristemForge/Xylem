# UDP 模块测试设计文档

## 概述

`tests/test-udp.c` 包含 10 个测试函数，覆盖 `src/xylem-udp.c` 的所有公共 API 和两种工作模式（listen 未连接模式、dial 已连接模式）。不包含 `on_error` 回调的测试。

## 测试基础设施

- 每个异步测试定义独立的 context 结构体（如 `_lr_ctx_t`、`_de_ctx_t`），通过 `xylem_udp_set_userdata` 传递给回调，无文件作用域可变状态
- 共享回调：`_safety_timeout_cb`（2000ms 安全超时停循环）、`_stop_cb`（同步测试排空延迟释放）
- 每个测试独立创建 Loop，测试间无共享状态
- 两个端口 `PORT_A 19001` / `PORT_B 19002`，所有测试复用（UDP 无 TIME_WAIT，close 后端口立即可重新 bind）
- 异步测试使用 10ms 延迟定时器触发发送，确保接收端已注册到事件循环
- 同步测试（close、userdata、get_loop）使用 50ms drain 定时器让 deferred free 完成

## 端口分配

| 端口 | 用途 |
|------|------|
| `PORT_A` (19001) | 接收端 / 服务端 / 单 socket 测试 |
| `PORT_B` (19002) | 发送端（listen-to-listen 通信需要两个不同端口避免竞争收包） |

Dial 模式测试只需 PORT_A（client 由系统分配临时端口）。

## 测试列表

### 数据路径 — Listen 模式（3 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_listen_recv` | listen 接收 + 发送方地址 | data=="hello", len==5, addr 端口==PORT_B, IP=="127.0.0.1" |
| `test_listen_send` | listen 发送到指定目标 | 目标端收到 "reply", len==5 |
| `test_datagram_boundary` | 数据报边界保持 | 3 个数据报独立交付，sizes=={1,2,3}，内容匹配 |

### 数据路径 — Dial 模式（2 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_dial_echo` | dial 双向收发 | server 收到 "ping"，client 收到 "pong" |
| `test_dial_addr` | dial on_read addr 参数 | addr IP=="127.0.0.1", 端口==PORT_A |

### Close 生命周期（3 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_close_idempotent` | close 重复调用 | 第二次调用不崩溃 |
| `test_close_callback` | close 触发 on_close | on_close 被调用 |
| `test_send_after_close` | close 后 send | 返回 -1 |

### 访问器（2 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_userdata` | set/get userdata | 指针往返一致，解引用值==42 |
| `test_get_loop` | 获取关联 loop | 与创建时的 loop 相同 |

## 覆盖的公共 API

| API 函数 | 覆盖的测试 |
|---------|-----------|
| `xylem_udp_listen` | 全部 10 个（listen 模式创建 + dial 模式服务端） |
| `xylem_udp_dial` | test_dial_echo, test_dial_addr |
| `xylem_udp_send` | test_listen_recv, test_listen_send, test_dial_echo, test_datagram_boundary, test_send_after_close |
| `xylem_udp_close` | test_close_idempotent, test_close_callback, test_send_after_close + 所有异步测试的清理路径 |
| `xylem_udp_get_userdata` | test_userdata + 所有 context 回调（通过 userdata 传递 ctx） |
| `xylem_udp_set_userdata` | test_userdata + 所有异步测试的 setup |
| `xylem_udp_get_loop` | test_get_loop |

## 覆盖的内部分支

| 内部路径 | 覆盖的测试 |
|---------|-----------|
| `_udp_io_cb` recv 循环（connected=false, recvfrom） | test_listen_recv, test_listen_send, test_datagram_boundary |
| `_udp_io_cb` recv 循环（connected=true, recv） | test_dial_echo, test_dial_addr |
| `_udp_io_cb` EAGAIN 退出 | 所有异步测试（drain 循环正常退出） |
| `xylem_udp_send` connected 路径（send） | test_dial_echo, test_dial_addr |
| `xylem_udp_send` unconnected 路径（sendto） | test_listen_recv, test_listen_send, test_datagram_boundary |
| `xylem_udp_send` closing 拒绝 | test_send_after_close |
| `xylem_udp_close` 幂等（closing 标志） | test_close_idempotent |
| `xylem_udp_close` on_close 回调 | test_close_callback |
| `xylem_udp_close` deferred free | 所有测试（loop_run 后资源释放） |

## 未覆盖的路径

| 路径 | 原因 |
|------|------|
| `on_error` 回调 | 需求排除：难以可靠触发非 EAGAIN 的 recv 错误 |
| IPv6 地址 | 所有测试使用 127.0.0.1 回环地址 |
| 大数据报（接近 64KB） | 回环接口可靠，不需要专门测试截断 |
