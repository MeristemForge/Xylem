# 需求文档: network-tcp-udp

## 简介

本文档定义 Xylem C 工具库中非阻塞 TCP 和 UDP 网络模块的功能需求。该模块基于 `xylem_loop_t` 事件循环，提供 TCP 服务端监听/接受、客户端异步连接、库内部读取与帧解析、缓冲写入、读写超时、心跳检测、自动重连，以及 UDP 绑定、sendto/recvfrom 和组播支持。所有需求均从已批准的设计文档派生，遵循 EARS 模式和 INCOSE 质量规则。

## 术语表

- **Event_Loop**: `xylem_loop_t` 事件循环实例，驱动所有异步 IO 和定时器操作
- **TCP_Server**: `xylem_tcp_server_t` TCP 服务端句柄，管理监听套接字和活跃连接
- **TCP_Connection**: `xylem_tcp_conn_t` TCP 连接句柄，表示一条客户端发起或服务端接受的连接
- **UDP_Handle**: `xylem_udp_t` UDP 句柄，管理 UDP 套接字的收发和组播
- **Handler**: 聚合回调结构体 (`xylem_tcp_handler_t` 或 `xylem_udp_handler_t`)，包含用户注册的事件回调函数
- **Frame_Parser**: 帧解析器，根据配置的帧策略从环形缓冲区中提取完整帧
- **Ringbuf**: `xylem_ringbuf_t` 环形缓冲区，用于 TCP 连接的读缓冲
- **Write_Queue**: 基于 `xylem_queue_t` 的写请求队列，管理待发送数据
- **Write_Req**: `xylem_tcp_write_req_t` 写请求节点，包含已复制的数据、总长度和已发送偏移
- **Addr**: `xylem_addr_t` 统一地址结构，封装 `sockaddr_storage`，支持 IPv4 和 IPv6
- **Framing_Config**: `xylem_tcp_framing_t` 帧策略配置，支持 NONE/FIXED/LENGTH/DELIM/CUSTOM 五种模式
- **TCP_Opts**: `xylem_tcp_opts_t` TCP 选项结构，包含帧策略、超时、心跳、重连等配置
- **Connection_State**: TCP 连接状态枚举 (CONNECTING, CONNECTED, CLOSING, CLOSED)

## 需求

### 需求 1: 地址工具

**用户故事:** 作为开发者，我希望使用统一的地址结构在字符串和二进制地址之间转换，以便在 TCP 和 UDP 操作中方便地指定网络地址。

#### 验收标准

1. WHEN 调用 `xylem_addr_pton` 并传入合法的 IPv4 地址字符串和端口, THEN Addr 工具 SHALL 返回 0 并将解析结果写入输出 Addr 结构
2. WHEN 调用 `xylem_addr_pton` 并传入合法的 IPv6 地址字符串和端口, THEN Addr 工具 SHALL 返回 0 并将解析结果写入输出 Addr 结构
3. WHEN 调用 `xylem_addr_pton` 并传入非法地址字符串, THEN Addr 工具 SHALL 返回 -1
4. WHEN 调用 `xylem_addr_ntop` 并传入有效的 Addr 结构, THEN Addr 工具 SHALL 返回 0 并将规范化的主机字符串和端口写入输出参数
5. WHEN 对合法地址执行 `xylem_addr_pton` 后再执行 `xylem_addr_ntop`, THEN Addr 工具 SHALL 产生与原始输入等价的规范化地址字符串和相同的端口号 (往返一致性)

### 需求 2: TCP 服务端监听与接受

**用户故事:** 作为开发者，我希望创建 TCP 服务端来监听指定地址并接受客户端连接，以便构建基于事件循环的网络服务。

#### 验收标准

1. WHEN 调用 `xylem_tcp_listen` 并传入有效的 Event_Loop、Addr、Handler 和 TCP_Opts, THEN TCP_Server SHALL 创建非阻塞监听套接字、绑定到指定地址、注册到 Event_Loop 并返回有效的 TCP_Server 句柄
2. WHEN 客户端连接到达监听套接字, THEN TCP_Server SHALL 接受连接、为新连接分配并初始化 TCP_Connection (包括 Ringbuf 和 IO 句柄)、并调用 Handler 的 `on_accept` 回调
3. WHEN `platform_socket_accept` 返回 EAGAIN, THEN TCP_Server SHALL 停止本轮接受循环并等待下次 IO 事件
4. IF `platform_socket_accept` 返回非 EAGAIN 错误, THEN TCP_Server SHALL 记录日志并继续监听
5. IF 接受连接时内存分配失败, THEN TCP_Server SHALL 关闭已接受的文件描述符并记录日志
6. WHEN 调用 `xylem_tcp_server_close`, THEN TCP_Server SHALL 停止接受新连接，已有 TCP_Connection 不受影响

### 需求 3: TCP 客户端异步连接

**用户故事:** 作为开发者，我希望发起异步 TCP 连接并在连接完成后收到通知，以便在事件循环中实现非阻塞的客户端逻辑。

#### 验收标准

1. WHEN 调用 `xylem_tcp_dial` 并传入有效的 Event_Loop、目标 Addr、Handler 和 TCP_Opts, THEN TCP_Connection SHALL 创建非阻塞套接字、发起异步连接、注册到 Event_Loop 并返回有效的 TCP_Connection 句柄
2. WHEN 异步连接成功完成 (`getsockopt(SO_ERROR) == 0`), THEN TCP_Connection SHALL 将 Connection_State 转换为 CONNECTED、停止连接超时定时器、启动读轮询、并调用 Handler 的 `on_connect` 回调
3. IF 调用 `xylem_tcp_dial` 时内存分配失败, THEN `xylem_tcp_dial` SHALL 返回 NULL

### 需求 4: TCP 连接超时

**用户故事:** 作为开发者，我希望为 TCP 连接设置连接超时，以便在连接长时间未完成时收到通知并自行决定处理方式。

#### 验收标准

1. WHERE TCP_Opts 的 `connect_timeout_ms` 大于 0, WHEN 调用 `xylem_tcp_dial`, THEN TCP_Connection SHALL 启动连接超时定时器
2. WHEN 连接超时定时器到期且连接尚未完成, THEN TCP_Connection SHALL 调用 Handler 的 `on_timeout` 回调并传入 `XYLEM_TCP_TIMEOUT_CONNECT` 类型
3. WHEN 连接超时触发后, THE TCP_Connection SHALL 保持当前状态不自动关闭，由用户在回调中决定是否调用 `xylem_tcp_close`

### 需求 5: TCP 自动重连

**用户故事:** 作为开发者，我希望客户端连接在失败后自动重连并使用指数退避策略，以便提高连接的可靠性。

#### 验收标准

1. WHERE TCP_Opts 的 `reconnect_max` 大于 0, WHEN TCP 连接失败, THEN TCP_Connection SHALL 关闭旧套接字并启动重连定时器
2. THE TCP_Connection SHALL 使用指数退避策略计算重连延迟: `delay = min(500 * 2^retry_count, 30000)` 毫秒
3. WHEN 重连次数达到 `reconnect_max`, THEN TCP_Connection SHALL 停止重连并进入 CLOSED 状态，调用 Handler 的 `on_close` 回调
4. WHEN 重连成功, THEN TCP_Connection SHALL 将 Connection_State 转换为 CONNECTED 并调用 Handler 的 `on_connect` 回调

### 需求 6: TCP 帧解析

**用户故事:** 作为开发者，我希望库自动从 TCP 字节流中提取完整帧并通过回调传递，以便我只需处理完整的应用层消息。

#### 验收标准

1. WHERE Framing_Config 的类型为 FRAME_NONE, WHEN 数据到达, THEN Frame_Parser SHALL 将所有可用字节作为一个帧通过 `on_read` 回调传递
2. WHERE Framing_Config 的类型为 FRAME_FIXED, WHEN Ringbuf 中累积的数据达到 `frame_size` 字节, THEN Frame_Parser SHALL 提取恰好 `frame_size` 字节作为一帧通过 `on_read` 回调传递
3. WHERE Framing_Config 的类型为 FRAME_FIXED, WHEN Ringbuf 中数据不足 `frame_size` 字节, THEN Frame_Parser SHALL 保留数据在 Ringbuf 中等待更多数据到达
4. WHERE Framing_Config 的类型为 FRAME_LENGTH 且 `header_bytes` 为 1、2 或 4, WHEN Ringbuf 中包含完整的头部和载荷, THEN Frame_Parser SHALL 按指定字节序解码头部获取载荷长度，提取载荷数据通过 `on_read` 回调传递
5. WHERE Framing_Config 的类型为 FRAME_DELIM, WHEN Ringbuf 中数据包含分隔符序列, THEN Frame_Parser SHALL 提取分隔符之前的数据作为一帧通过 `on_read` 回调传递 (不含分隔符本身)
6. WHERE Framing_Config 的类型为 FRAME_CUSTOM, WHEN 自定义解析器 `parse(data, len)` 返回值大于 0, THEN Frame_Parser SHALL 提取返回值指定长度的数据作为一帧通过 `on_read` 回调传递
7. WHERE Framing_Config 的类型为 FRAME_CUSTOM, WHEN 自定义解析器 `parse(data, len)` 返回值等于 0, THEN Frame_Parser SHALL 保留数据在 Ringbuf 中等待更多数据到达
8. IF 自定义解析器 `parse(data, len)` 返回值小于 0, THEN TCP_Connection SHALL 关闭连接并调用 Handler 的 `on_close` 回调
9. THE Frame_Parser SHALL 在单次 IO 回调中循环提取所有可用的完整帧，直到数据不足或发生错误

### 需求 7: TCP 数据发送

**用户故事:** 作为开发者，我希望通过非阻塞方式发送数据并在每次发送完成后收到通知，以便跟踪每个写操作的状态。

#### 验收标准

1. WHEN 调用 `xylem_tcp_send(conn, data, len)`, THEN TCP_Connection SHALL 复制数据到内部 Write_Req、将 Write_Req 加入 Write_Queue、并立即返回 0
2. WHEN Write_Queue 中的 Write_Req 数据全部发送完成, THEN TCP_Connection SHALL 调用 Handler 的 `on_write_done(conn, data, len, 0)` 回调 (若回调非 NULL)，然后释放 Write_Req
3. WHILE Write_Queue 非空, THE TCP_Connection SHALL 在 Event_Loop 中注册写兴趣 (RW 模式)
4. WHEN Write_Queue 变为空, THEN TCP_Connection SHALL 将 IO 兴趣切回只读模式 (RD)
5. IF `platform_socket_send` 返回 EAGAIN, THEN TCP_Connection SHALL 保留当前 Write_Req 并等待下次写事件
6. IF `platform_socket_send` 返回非 EAGAIN 错误, THEN TCP_Connection SHALL 对所有未完成的 Write_Req 调用 `on_write_done(conn, data, len, err)` 回调 (若回调非 NULL)、释放所有 Write_Req、并关闭连接
7. IF 调用 `xylem_tcp_send` 时连接已关闭, THEN `xylem_tcp_send` SHALL 返回 -1
8. THE TCP_Connection SHALL 保证对端按序收到的字节流等于所有 `xylem_tcp_send` 调用的数据按调用顺序拼接

### 需求 8: TCP 读写超时

**用户故事:** 作为开发者，我希望为 TCP 连接配置读超时和写超时，以便在数据传输停滞时收到通知并自行决定处理方式。

#### 验收标准

1. WHERE TCP_Opts 的 `read_timeout_ms` 大于 0, WHEN 在 `read_timeout_ms` 毫秒内未收到任何数据, THEN TCP_Connection SHALL 调用 Handler 的 `on_timeout` 回调并传入 `XYLEM_TCP_TIMEOUT_READ` 类型
2. WHERE TCP_Opts 的 `read_timeout_ms` 大于 0, WHEN 收到数据, THEN TCP_Connection SHALL 重置读超时定时器
3. WHERE TCP_Opts 的 `write_timeout_ms` 大于 0, WHEN Write_Queue 在 `write_timeout_ms` 毫秒内未排空, THEN TCP_Connection SHALL 调用 Handler 的 `on_timeout` 回调并传入 `XYLEM_TCP_TIMEOUT_WRITE` 类型
4. WHEN 超时回调触发后, THE TCP_Connection SHALL 保持当前状态不自动关闭，由用户在回调中决定是否调用 `xylem_tcp_close`

### 需求 9: TCP 心跳检测

**用户故事:** 作为开发者，我希望检测对端是否存活，以便在对端无响应时及时发现并处理。

#### 验收标准

1. WHERE TCP_Opts 的 `heartbeat_ms` 大于 0, WHEN 在 `heartbeat_ms` 毫秒内未收到任何数据, THEN TCP_Connection SHALL 调用 Handler 的 `on_heartbeat_miss` 回调恰好一次
2. WHERE TCP_Opts 的 `heartbeat_ms` 大于 0, WHEN 收到数据, THEN TCP_Connection SHALL 重置心跳定时器

### 需求 10: TCP 连接状态机

**用户故事:** 作为开发者，我希望 TCP 连接遵循明确的状态转换规则，以便连接行为可预测且不会出现非法状态。

#### 验收标准

1. THE TCP_Connection SHALL 仅支持以下 Connection_State 转换路径: CONNECTING → CONNECTED, CONNECTING → CLOSED (无重连), CONNECTING → CONNECTING (重连), CONNECTED → CLOSING, CLOSING → CLOSED
2. THE TCP_Connection SHALL 拒绝所有不在上述路径中的状态转换

### 需求 11: TCP 优雅关闭

**用户故事:** 作为开发者，我希望关闭连接时所有待发送数据被发送完毕后再关闭套接字，以便不丢失已提交的数据。

#### 验收标准

1. WHEN 调用 `xylem_tcp_close(conn)`, THEN TCP_Connection SHALL 将 Connection_State 转换为 CLOSING
2. WHILE Connection_State 为 CLOSING 且 Write_Queue 非空, THE TCP_Connection SHALL 继续发送 Write_Queue 中的数据
3. WHEN Write_Queue 排空且 Connection_State 为 CLOSING, THEN TCP_Connection SHALL 执行 `shutdown(fd, SHUT_WR)`、调用 Handler 的 `on_close(conn, 0)` 回调、释放所有资源 (Ringbuf、定时器、Write_Req)、并将 Connection_State 转换为 CLOSED

### 需求 12: TCP 连接错误处理

**用户故事:** 作为开发者，我希望连接在遇到网络错误时正确清理并通知我，以便我能做出相应处理。

#### 验收标准

1. IF `platform_socket_recv` 返回 0 (对端关闭), THEN TCP_Connection SHALL 进入 CLOSING 状态并最终调用 Handler 的 `on_close(conn, 0)` 回调
2. IF `platform_socket_recv` 返回 -1 且错误码非 EAGAIN, THEN TCP_Connection SHALL 进入 CLOSING 状态并最终调用 Handler 的 `on_close(conn, err)` 回调
3. IF `platform_socket_recv` 返回 -1 且错误码为 EAGAIN, THEN TCP_Connection SHALL 保持当前状态并等待下次 IO 事件

### 需求 13: TCP 用户数据

**用户故事:** 作为开发者，我希望在 TCP 连接上附加自定义数据，以便在回调中访问应用层上下文。

#### 验收标准

1. WHEN 调用 `xylem_tcp_conn_set_userdata(conn, ud)`, THEN TCP_Connection SHALL 存储用户数据指针
2. WHEN 调用 `xylem_tcp_conn_get_userdata(conn)`, THEN TCP_Connection SHALL 返回最近一次通过 `xylem_tcp_conn_set_userdata` 设置的指针

### 需求 14: UDP 绑定与接收

**用户故事:** 作为开发者，我希望绑定 UDP 套接字并接收数据报，以便实现基于事件循环的 UDP 服务。

#### 验收标准

1. WHEN 调用 `xylem_udp_bind` 并传入有效的 Event_Loop、Addr 和 Handler, THEN UDP_Handle SHALL 创建非阻塞 UDP 套接字、绑定到指定地址、注册到 Event_Loop 并返回有效的 UDP_Handle
2. WHEN 数据报到达绑定的 UDP 套接字, THEN UDP_Handle SHALL 调用 Handler 的 `on_read(udp, data, len, &sender_addr)` 回调，其中 data 和 len 为完整数据报内容，sender_addr 为发送方地址
3. THE UDP_Handle SHALL 保持数据报边界: 每次 `on_read` 回调对应一个完整的数据报，不合并也不拆分

### 需求 15: UDP 发送

**用户故事:** 作为开发者，我希望通过 UDP 句柄向指定目标发送数据报，以便实现无连接的数据传输。

#### 验收标准

1. WHEN 调用 `xylem_udp_send(udp, dest, data, len)`, THEN UDP_Handle SHALL 通过 `platform_socket_sendto` 发送数据报到目标地址并返回发送字节数
2. IF `platform_socket_sendto` 失败, THEN `xylem_udp_send` SHALL 返回 -1

### 需求 16: UDP 组播

**用户故事:** 作为开发者，我希望加入和离开组播组，以便接收组播数据。

#### 验收标准

1. WHEN 调用 `xylem_udp_mcast_join(udp, group)` 并传入有效的组播地址字符串, THEN UDP_Handle SHALL 通过 `setsockopt(IP_ADD_MEMBERSHIP)` 加入组播组并返回 0
2. WHEN 调用 `xylem_udp_mcast_leave(udp, group)`, THEN UDP_Handle SHALL 通过 `setsockopt(IP_DROP_MEMBERSHIP)` 离开组播组并返回 0
3. IF `setsockopt` 调用失败 (地址无效、接口不支持等), THEN `xylem_udp_mcast_join` 或 `xylem_udp_mcast_leave` SHALL 返回 -1

### 需求 17: UDP 关闭

**用户故事:** 作为开发者，我希望关闭 UDP 句柄并释放资源，以便在不再需要时清理。

#### 验收标准

1. WHEN 调用 `xylem_udp_close(udp)`, THEN UDP_Handle SHALL 关闭套接字、从 Event_Loop 注销 IO 句柄、停止所有定时器、并调用 Handler 的 `on_close(udp, 0)` 回调

### 需求 18: Ringbuf 溢出处理

**用户故事:** 作为开发者，我希望在读缓冲区满时库能安全处理，以便不会导致崩溃或未定义行为。

#### 验收标准

1. IF `platform_socket_recv` 返回的数据超过 Ringbuf 剩余空间, THEN TCP_Connection SHALL 写入尽可能多的数据到 Ringbuf 并丢弃溢出部分

### 需求 19: 内存分配失败处理

**用户故事:** 作为开发者，我希望在内存分配失败时库能安全地返回错误，以便我能处理资源不足的情况。

#### 验收标准

1. IF `xylem_tcp_listen` 中内存分配失败, THEN `xylem_tcp_listen` SHALL 返回 NULL
2. IF `xylem_tcp_dial` 中内存分配失败, THEN `xylem_tcp_dial` SHALL 返回 NULL
3. IF `xylem_tcp_send` 中内存分配失败, THEN `xylem_tcp_send` SHALL 返回 -1
