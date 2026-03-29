# 需求文档

## 简介

本文档定义了 Xylem 项目 TCP 模块（`src/xylem-tcp.c`）测试文件 `tests/test-tcp.c` 的完整重写需求。目标是为 TCP 模块的所有公共 API、分帧逻辑、读写流程、超时/心跳机制、重连逻辑和连接生命周期提供全面的测试覆盖。测试必须遵循项目 `docs/style.md` 中定义的测试规范。

## 术语表

- **Test_Runner**: `tests/test-tcp.c` 中的 `main()` 函数，顺序调用所有 `test_*` 测试函数
- **Loop**: `xylem_loop_t` 事件循环实例，驱动所有异步 TCP 操作
- **Server**: `xylem_tcp_server_t` TCP 服务端句柄，由 `xylem_tcp_listen` 创建
- **Conn**: `xylem_tcp_conn_t` TCP 连接句柄，由 `xylem_tcp_dial`（客户端）或 `on_accept`（服务端）产生
- **Handler**: `xylem_tcp_handler_t` 回调集合，包含 on_connect、on_accept、on_read、on_write_done、on_timeout、on_close、on_heartbeat_miss
- **Opts**: `xylem_tcp_opts_t` TCP 选项结构体，包含分帧配置、超时、心跳、重连等参数
- **Safety_Timer**: 每个测试函数中创建的超时定时器，防止事件循环因测试失败而永久阻塞
- **Frame**: 根据分帧策略从 TCP 字节流中提取的一个完整数据单元
- **Write_Queue**: Conn 内部的写请求队列，`xylem_tcp_send` 将数据入队，`_tcp_flush_writes` 负责发送

## 需求

### 需求 1：Server 监听与关闭

**用户故事：** 作为开发者，我想测试 TCP 服务端的创建和关闭流程，以确保 `xylem_tcp_listen` 和 `xylem_tcp_close_server` 在正常和边界条件下行为正确。

#### 验收标准

1. WHEN `xylem_tcp_listen` 以有效地址和 Handler 被调用, THE Test_Runner SHALL 验证返回的 Server 句柄非 NULL
2. WHEN `xylem_tcp_close_server` 在存在活跃 Conn 的 Server 上被调用, THE Test_Runner SHALL 验证所有活跃 Conn 的 on_close 回调被触发
3. WHEN `xylem_tcp_close_server` 在同一 Server 上被重复调用, THE Test_Runner SHALL 验证第二次调用不会导致崩溃或重复关闭

### 需求 2：客户端连接与关闭

**用户故事：** 作为开发者，我想测试 TCP 客户端的连接建立和关闭流程，以确保 `xylem_tcp_dial` 和 `xylem_tcp_close` 在各种场景下行为正确。

#### 验收标准

1. WHEN `xylem_tcp_dial` 以有效地址被调用, THE Test_Runner SHALL 验证返回的 Conn 句柄非 NULL 且 on_connect 回调被触发
2. WHEN `xylem_tcp_close` 在 Write_Queue 为空时被调用, THE Test_Runner SHALL 验证连接立即执行 shutdown 并触发 on_close 回调
3. WHEN `xylem_tcp_close` 在 Write_Queue 非空时被调用, THE Test_Runner SHALL 验证连接等待 Write_Queue 排空后再执行 shutdown 并触发 on_close 回调

### 需求 3：数据发送

**用户故事：** 作为开发者，我想测试 TCP 数据发送功能，以确保 `xylem_tcp_send` 在正常和错误条件下行为正确。

#### 验收标准

1. WHEN `xylem_tcp_send` 在已连接的 Conn 上被调用, THE Test_Runner SHALL 验证返回值为 0 且 on_write_done 回调以 status=0 和正确的 len 被触发
2. WHEN `xylem_tcp_send` 在已关闭或正在关闭的 Conn 上被调用, THE Test_Runner SHALL 验证返回值为 -1


### 需求 4：连接级 Userdata

**用户故事：** 作为开发者，我想测试连接级用户数据的存取功能，以确保 `xylem_tcp_set_userdata` 和 `xylem_tcp_get_userdata` 正确工作。

#### 验收标准

1. WHEN `xylem_tcp_set_userdata` 在 Conn 上设置一个指针后调用 `xylem_tcp_get_userdata`, THE Test_Runner SHALL 验证返回的指针与设置的指针相同且指向的值正确

### 需求 5：Server 级 Userdata

**用户故事：** 作为开发者，我想测试服务端级用户数据的存取功能，以确保 `xylem_tcp_server_set_userdata` 和 `xylem_tcp_server_get_userdata` 正确工作。

#### 验收标准

1. WHEN `xylem_tcp_server_set_userdata` 在 Server 上设置一个指针后调用 `xylem_tcp_server_get_userdata`, THE Test_Runner SHALL 验证返回的指针与设置的指针相同且指向的值正确

### 需求 6：获取对端地址

**用户故事：** 作为开发者，我想测试获取对端地址功能，以确保服务端 accept 后通过 `xylem_tcp_get_peer_addr` 能获取到客户端的地址信息。

#### 验收标准

1. WHEN 服务端 accept 一个新连接后在 on_accept 回调中调用 `xylem_tcp_get_peer_addr`, THE Test_Runner SHALL 验证返回的地址非 NULL 且地址族为 AF_INET

### 需求 7：获取事件循环

**用户故事：** 作为开发者，我想测试获取关联事件循环功能，以确保 `xylem_tcp_get_loop` 返回正确的 Loop 句柄。

#### 验收标准

1. WHEN 在已连接的 Conn 上调用 `xylem_tcp_get_loop`, THE Test_Runner SHALL 验证返回的 Loop 句柄与创建 Conn 时使用的 Loop 句柄相同

### 需求 8：FRAME_NONE 分帧

**用户故事：** 作为开发者，我想测试 FRAME_NONE 分帧策略，以确保收到的数据被直接交付而不做任何分帧处理。

#### 验收标准

1. WHEN 使用 FRAME_NONE 配置的 Conn 通过 echo 模式交换数据, THE Test_Runner SHALL 验证客户端累积收到的总数据内容与发送的原始数据一致（TCP 是字节流，单次 on_read 交付的数据量不确定，需累积验证）

### 需求 9：FRAME_FIXED 分帧

**用户故事：** 作为开发者，我想测试 FRAME_FIXED 分帧策略，以确保数据按固定大小正确分帧。

#### 验收标准

1. WHEN 使用 frame_size=4 的 FRAME_FIXED 配置的 Conn 收到 8 字节数据, THE Test_Runner SHALL 验证 on_read 回调被触发 2 次，每次收到 4 字节且内容正确
2. WHEN FRAME_FIXED 配置的 frame_size 为 0, THE Test_Runner SHALL 验证连接因帧提取错误而被关闭

### 需求 10：FRAME_LENGTH fixedint 大端分帧

**用户故事：** 作为开发者，我想测试 FRAME_LENGTH fixedint 大端编码的分帧策略，以确保长度字段按大端字节序正确解码。

#### 验收标准

1. WHEN 使用 2 字节大端 fixedint 长度字段配置的 Conn 收到 [0x00, 0x05, "HELLO"] 数据, THE Test_Runner SHALL 验证 on_read 回调收到 "HELLO"（5 字节）

### 需求 11：FRAME_LENGTH fixedint 小端分帧

**用户故事：** 作为开发者，我想测试 FRAME_LENGTH fixedint 小端编码的分帧策略，以确保 field_big_endian=false 路径正确工作。

#### 验收标准

1. WHEN 使用 2 字节小端 fixedint 长度字段配置的 Conn 收到 [0x05, 0x00, "HELLO"] 数据, THE Test_Runner SHALL 验证 on_read 回调收到 "HELLO"（5 字节）

### 需求 12：FRAME_LENGTH fixedint 错误路径

**用户故事：** 作为开发者，我想测试 FRAME_LENGTH fixedint 的错误条件，以确保无效的 field_size 被正确拒绝。

#### 验收标准

1. WHEN FRAME_LENGTH fixedint 配置的 field_size 为 0, THE Test_Runner SHALL 验证连接因帧提取错误而被关闭
2. WHEN FRAME_LENGTH fixedint 配置的 field_size 大于 8, THE Test_Runner SHALL 验证连接因帧提取错误而被关闭

### 需求 13：FRAME_LENGTH varint 分帧

**用户故事：** 作为开发者，我想测试 FRAME_LENGTH varint 编码的分帧策略，以确保 varint 长度字段被正确解码。

#### 验收标准

1. WHEN 使用 varint 长度字段配置的 Conn 收到 [varint(5), "WORLD"] 数据, THE Test_Runner SHALL 验证 on_read 回调收到 "WORLD"（5 字节）

### 需求 14：FRAME_LENGTH adjustment 非零

**用户故事：** 作为开发者，我想测试 FRAME_LENGTH 的 adjustment 参数，以确保正值 adjustment 正确调整帧大小计算。

#### 验收标准

1. WHEN 使用 adjustment=-2 的 FRAME_LENGTH fixedint 配置（长度字段值包含头部大小）的 Conn 收到数据, THE Test_Runner SHALL 验证 on_read 回调收到的 payload 长度等于长度字段值减去 header_size 再加上 adjustment 的结果

### 需求 15：FRAME_LENGTH 空 payload

**用户故事：** 作为开发者，我想测试 FRAME_LENGTH 在 payload 长度为零时的行为，以确保 total <= effective_hdr 路径被正确处理。

#### 验收标准

1. WHEN 使用 FRAME_LENGTH fixedint 配置的 Conn 收到长度字段值为 0 且 adjustment 使 frame_size <= 0 的数据, THE Test_Runner SHALL 验证连接因帧提取错误而被关闭

### 需求 16：FRAME_DELIM 多字节分隔符

**用户故事：** 作为开发者，我想测试 FRAME_DELIM 多字节分隔符的分帧策略，以确保 memcmp 路径正确工作。

#### 验收标准

1. WHEN 使用 "\r\n" 作为分隔符的 FRAME_DELIM 配置的 Conn 收到 "hello\r\nworld\r\n" 数据, THE Test_Runner SHALL 验证 on_read 回调被触发 2 次，分别收到 "hello" 和 "world"

### 需求 17：FRAME_DELIM 单字节分隔符

**用户故事：** 作为开发者，我想测试 FRAME_DELIM 单字节分隔符的分帧策略，以确保 memchr 优化路径正确工作。

#### 验收标准

1. WHEN 使用 "\n" 作为分隔符的 FRAME_DELIM 配置的 Conn 收到 "abc\ndef\n" 数据, THE Test_Runner SHALL 验证 on_read 回调被触发 2 次，分别收到 "abc" 和 "def"

### 需求 18：FRAME_DELIM 错误路径

**用户故事：** 作为开发者，我想测试 FRAME_DELIM 在分隔符为 NULL 时的错误处理。

#### 验收标准

1. WHEN FRAME_DELIM 配置的 delim 为 NULL, THE Test_Runner SHALL 验证连接因帧提取错误而被关闭

### 需求 19：FRAME_CUSTOM 分帧

**用户故事：** 作为开发者，我想测试 FRAME_CUSTOM 自定义分帧策略的所有返回值路径。

#### 验收标准

1. WHEN FRAME_CUSTOM 的 parse 函数返回正值, THE Test_Runner SHALL 验证 on_read 回调收到正确长度的帧数据
2. WHEN FRAME_CUSTOM 的 parse 函数返回 0, THE Test_Runner SHALL 验证 on_read 回调不被触发（数据不足，等待更多数据）
3. WHEN FRAME_CUSTOM 的 parse 函数返回负值, THE Test_Runner SHALL 验证连接因帧提取错误而被关闭
4. WHEN FRAME_CUSTOM 配置的 parse 为 NULL, THE Test_Runner SHALL 验证连接因帧提取错误而被关闭

### 需求 20：读超时

**用户故事：** 作为开发者，我想测试读超时机制，以确保 `read_timeout_ms` 到期后正确触发 on_timeout 回调。

#### 验收标准

1. WHEN Conn 配置了 read_timeout_ms 且在超时时间内未收到数据, THE Test_Runner SHALL 验证 on_timeout 回调以 XYLEM_TCP_TIMEOUT_READ 类型被触发

### 需求 21：写超时

**用户故事：** 作为开发者，我想测试写超时机制，以确保 `write_timeout_ms` 到期后正确触发 on_timeout 回调。

#### 验收标准

1. WHEN Conn 配置了 write_timeout_ms 且写操作在超时时间内未完成, THE Test_Runner SHALL 验证 on_timeout 回调以 XYLEM_TCP_TIMEOUT_WRITE 类型被触发

### 需求 22：连接超时

**用户故事：** 作为开发者，我想测试连接超时机制，以确保 `connect_timeout_ms` 到期后正确触发 on_timeout 回调。

#### 验收标准

1. WHEN Conn 配置了 connect_timeout_ms 且连接在超时时间内未建立, THE Test_Runner SHALL 验证 on_timeout 回调以 XYLEM_TCP_TIMEOUT_CONNECT 类型被触发

### 需求 23：心跳超时

**用户故事：** 作为开发者，我想测试心跳机制，以确保 `heartbeat_ms` 到期后正确触发 on_heartbeat_miss 回调。

#### 验收标准

1. WHEN Conn 配置了 heartbeat_ms 且在心跳间隔内未收到数据, THE Test_Runner SHALL 验证 on_heartbeat_miss 回调被触发

### 需求 24：心跳定时器重置

**用户故事：** 作为开发者，我想测试收到数据后心跳定时器被重置的行为，以确保活跃连接不会误触发心跳超时。

#### 验收标准

1. WHEN Conn 配置了 heartbeat_ms 且在心跳到期前持续收到数据, THE Test_Runner SHALL 验证 on_heartbeat_miss 回调在数据持续到达期间不被触发

### 需求 25：指数退避重连

**用户故事：** 作为开发者，我想测试重连机制的指数退避行为，以确保连接失败后按指数退避策略重试。

#### 验收标准

1. WHEN Conn 配置了 reconnect_max > 0 且连接失败, THE Test_Runner SHALL 验证重连被发起且最终成功连接后 on_connect 回调被触发

### 需求 26：重连达到上限

**用户故事：** 作为开发者，我想测试重连达到最大次数后的行为，以确保连接被正确关闭。

#### 验收标准

1. WHEN Conn 配置了 reconnect_max 且重连次数达到上限仍未成功, THE Test_Runner SHALL 验证连接被关闭且 on_close 回调被触发

### 需求 27：读缓冲区满

**用户故事：** 作为开发者，我想测试读缓冲区满时的行为，以确保连接被正确关闭。

#### 验收标准

1. WHEN Conn 的读缓冲区已满（space==0）且有新数据到达, THE Test_Runner SHALL 验证连接因缓冲区满而被关闭且 on_close 回调被触发

### 需求 28：对端关闭连接（EOF）

**用户故事：** 作为开发者，我想测试对端关闭连接时的行为，以确保本端正确检测 EOF 并关闭连接。

#### 验收标准

1. WHEN 对端关闭连接导致 recv 返回 0, THE Test_Runner SHALL 验证本端 Conn 的 on_close 回调被触发

### 需求 29：Write Done 回调

**用户故事：** 作为开发者，我想测试写完成回调，以确保数据成功发送后 on_write_done 以正确的参数被触发。

#### 验收标准

1. WHEN 数据通过 `xylem_tcp_send` 发送并成功完成, THE Test_Runner SHALL 验证 on_write_done 回调以 status=0 和正确的 data/len 被触发

### 需求 30：优雅关闭时 flush 后 shutdown

**用户故事：** 作为开发者，我想测试优雅关闭流程中 Write_Queue 排空后的 shutdown 行为。

#### 验收标准

1. WHEN `xylem_tcp_close` 在 Write_Queue 非空时被调用, THE Test_Runner SHALL 验证 Write_Queue 中的数据被发送完毕后连接执行 shutdown 并触发 on_close 回调
2. WHEN `xylem_tcp_close` 在 Write_Queue 非空时被调用, THE Test_Runner SHALL 验证队列中所有写请求的 on_write_done 回调均被触发

### 需求 31：连接生命周期完整流程

**用户故事：** 作为开发者，我想测试完整的连接生命周期（accept -> read/write -> close），以确保所有回调按正确顺序触发。

#### 验收标准

1. WHEN 客户端连接到服务端并交换数据后双方关闭, THE Test_Runner SHALL 验证 on_accept、on_connect、on_read、on_close 回调均被触发

### 需求 32：关闭连接时 drain Write_Queue

**用户故事：** 作为开发者，我想测试 `_tcp_close_conn` 在关闭连接时正确 drain Write_Queue 的行为。

#### 验收标准

1. WHEN 连接因错误被关闭且 Write_Queue 中有未发送的写请求, THE Test_Runner SHALL 验证每个未发送写请求的 on_write_done 回调以非零 status 被触发

### 需求 33：测试基础设施

**用户故事：** 作为开发者，我想确保测试文件遵循项目测试规范，以保证测试的可靠性和可维护性。

#### 验收标准

1. THE Test_Runner SHALL 在每个测试函数中创建独立的 Loop 和 Safety_Timer，不依赖文件级共享可变状态
2. THE Test_Runner SHALL 在每个测试函数结束时销毁所有创建的资源（Loop、Server、Timer）
3. THE Test_Runner SHALL 使用项目自定义的 `ASSERT` 宏进行断言，不使用标准 `<assert.h>`
4. THE Test_Runner SHALL 确保每个测试函数只测试一个关注点
