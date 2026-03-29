# 需求文档: udp

## 简介

本文档定义 Xylem C 工具库中非阻塞 UDP 网络模块的功能需求。该模块基于 `xylem_loop_t` 事件循环，提供 UDP 绑定、sendto/recvfrom 和组播支持。所有需求均从已批准的设计文档派生，遵循 EARS 模式和 INCOSE 质量规则。

## 术语表

- **Event_Loop**: `xylem_loop_t` 事件循环实例，驱动所有异步 IO 和定时器操作
- **UDP_Handle**: `xylem_udp_t` UDP 句柄，管理 UDP 套接字的收发和组播
- **Handler**: 聚合回调结构体 (`xylem_udp_handler_t`)，包含用户注册的事件回调函数
- **Addr**: `xylem_addr_t` 统一地址结构，封装 `sockaddr_storage`，支持 IPv4 和 IPv6

## 需求

### 需求 1: 地址工具 (共享)

地址工具 (`xylem_addr_t`, `xylem_addr_pton`, `xylem_addr_ntop`) 的需求定义在 TCP 规格文档中。UDP 模块共享使用该地址工具。

### 需求 2: UDP 绑定与接收

**用户故事:** 作为开发者，我希望绑定 UDP 套接字并接收数据报，以便实现基于事件循环的 UDP 服务。

#### 验收标准

1. WHEN 调用 `xylem_udp_bind` 并传入有效的 Event_Loop、Addr 和 Handler, THEN UDP_Handle SHALL 创建非阻塞 UDP 套接字、绑定到指定地址、注册到 Event_Loop 并返回有效的 UDP_Handle
2. WHEN 数据报到达绑定的 UDP 套接字, THEN UDP_Handle SHALL 调用 Handler 的 `on_read(udp, data, len, &sender_addr)` 回调，其中 data 和 len 为完整数据报内容，sender_addr 为发送方地址
3. THE UDP_Handle SHALL 保持数据报边界: 每次 `on_read` 回调对应一个完整的数据报，不合并也不拆分

### 需求 3: UDP 发送

**用户故事:** 作为开发者，我希望通过 UDP 句柄向指定目标发送数据报，以便实现无连接的数据传输。

#### 验收标准

1. WHEN 调用 `xylem_udp_send(udp, dest, data, len)`, THEN UDP_Handle SHALL 通过 `platform_socket_sendto` 发送数据报到目标地址并返回发送字节数
2. IF `platform_socket_sendto` 失败, THEN `xylem_udp_send` SHALL 返回 -1

### 需求 4: UDP 组播

**用户故事:** 作为开发者，我希望加入和离开组播组，以便接收组播数据。

#### 验收标准

1. WHEN 调用 `xylem_udp_mcast_join(udp, group)` 并传入有效的组播地址字符串, THEN UDP_Handle SHALL 通过 `setsockopt(IP_ADD_MEMBERSHIP)` 加入组播组并返回 0
2. WHEN 调用 `xylem_udp_mcast_leave(udp, group)`, THEN UDP_Handle SHALL 通过 `setsockopt(IP_DROP_MEMBERSHIP)` 离开组播组并返回 0
3. IF `setsockopt` 调用失败 (地址无效、接口不支持等), THEN `xylem_udp_mcast_join` 或 `xylem_udp_mcast_leave` SHALL 返回 -1

### 需求 5: UDP 关闭

**用户故事:** 作为开发者，我希望关闭 UDP 句柄并释放资源，以便在不再需要时清理。

#### 验收标准

1. WHEN 调用 `xylem_udp_close(udp)`, THEN UDP_Handle SHALL 关闭套接字、从 Event_Loop 注销 IO 句柄、停止所有定时器、并调用 Handler 的 `on_close(udp, 0)` 回调
