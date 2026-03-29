# 需求文档

## 简介

本文档定义了 Xylem 项目 UDP 模块（`src/xylem-udp.c`）测试文件 `tests/test-udp.c` 的完整重写需求。目标是为 UDP 模块的所有公共 API（listen、dial、send、close、userdata、get_loop）和两种工作模式（listen 未连接模式、dial 已连接模式）提供全面的测试覆盖。测试必须遵循项目 `docs/style.md` 中定义的测试规范。不包含 on_error 回调的测试。

## 术语表

- **Test_Runner**: `tests/test-udp.c` 中的 `main()` 函数，顺序调用所有 `test_*` 测试函数
- **Loop**: `xylem_loop_t` 事件循环实例，驱动所有异步 UDP 操作
- **UDP_Handle**: `xylem_udp_t` UDP 句柄，由 `xylem_udp_listen` 或 `xylem_udp_dial` 创建
- **Handler**: `xylem_udp_handler_t` 回调集合，包含 on_read、on_error、on_close
- **Safety_Timer**: 每个异步测试函数中创建的超时定时器（2000ms），防止事件循环因测试失败而永久阻塞
- **Listen_Mode**: 通过 `xylem_udp_listen` 创建的未连接 UDP socket，使用 `recvfrom` 接收并获取发送方地址，使用 `sendto` 发送到指定目标
- **Dial_Mode**: 通过 `xylem_udp_dial` 创建的已连接 UDP socket，使用 `recv` 接收并使用保存的 peer 地址，使用 `send` 发送到已连接的对端
- **Addr**: `xylem_addr_t` 网络地址结构体，封装 `sockaddr_storage`

## 需求

### 需求 1：Listen 模式接收数据报

**用户故事：** 作为开发者，我想测试 listen 模式下接收数据报的功能，以确保 on_read 回调正确交付数据内容和发送方地址。

#### 验收标准

1. WHEN 外部 UDP socket 通过 `sendto` 向 Listen_Mode 的 UDP_Handle 发送数据报, THE Test_Runner SHALL 验证 on_read 回调被触发且 data 和 len 与发送的内容一致
2. WHEN Listen_Mode 的 UDP_Handle 收到数据报, THE Test_Runner SHALL 验证 on_read 回调中的 addr 参数包含发送方的 IP 地址和端口信息

### 需求 2：Listen 模式发送数据报

**用户故事：** 作为开发者，我想测试 listen 模式下通过 `xylem_udp_send` 指定目标地址发送数据报的功能，以确保对方能正确收到回复。

#### 验收标准

1. WHEN Listen_Mode 的 UDP_Handle 通过 `xylem_udp_send(udp, dest, data, len)` 向指定目标地址发送数据报, THE Test_Runner SHALL 验证目标端的 on_read 回调被触发且收到的数据与发送的内容一致

### 需求 3：Dial 模式收发数据报

**用户故事：** 作为开发者，我想测试 dial 模式下的双向数据报收发功能，以确保已连接 UDP socket 能正确发送和接收数据。

#### 验收标准

1. WHEN Dial_Mode 的 UDP_Handle 通过 `xylem_udp_send(udp, NULL, data, len)` 发送数据报, THE Test_Runner SHALL 验证 Listen_Mode 对端的 on_read 回调被触发且收到的数据与发送的内容一致
2. WHEN Listen_Mode 对端通过 `xylem_udp_send(udp, dest, data, len)` 向 Dial_Mode 的 UDP_Handle 回复数据报, THE Test_Runner SHALL 验证 Dial_Mode 端的 on_read 回调被触发且收到的数据与回复的内容一致

### 需求 4：Dial 模式 on_read 地址参数

**用户故事：** 作为开发者，我想测试 dial 模式下 on_read 回调中的 addr 参数，以确保已连接 UDP socket 报告正确的 peer 地址。

#### 验收标准

1. WHEN Dial_Mode 的 UDP_Handle 收到数据报, THE Test_Runner SHALL 验证 on_read 回调中的 addr 参数包含的 IP 地址和端口与 `xylem_udp_dial` 时指定的对端地址一致

### 需求 5：数据报边界保持

**用户故事：** 作为开发者，我想测试 UDP 数据报边界保持特性，以确保连续发送的多个不同大小的数据报在接收端被独立交付。

#### 验收标准

1. WHEN 发送端连续发送 3 个不同大小的数据报（1 字节、2 字节、3 字节）, THE Test_Runner SHALL 验证接收端的 on_read 回调被触发 3 次
2. WHEN 接收端收到 3 个数据报, THE Test_Runner SHALL 验证每次 on_read 回调的 len 参数分别为 1、2、3 且 data 内容与发送的数据一致

### 需求 6：Close 幂等性

**用户故事：** 作为开发者，我想测试 `xylem_udp_close` 的幂等性，以确保对同一 UDP_Handle 连续调用两次 close 不会导致崩溃或未定义行为。

#### 验收标准

1. WHEN `xylem_udp_close` 在同一 UDP_Handle 上被连续调用两次, THE Test_Runner SHALL 验证第二次调用不会导致崩溃且程序正常继续执行

### 需求 7：Close 回调触发

**用户故事：** 作为开发者，我想测试 `xylem_udp_close` 触发 on_close 回调的行为，以确保关闭时回调以 err=0 被正确调用。

#### 验收标准

1. WHEN `xylem_udp_close` 在 UDP_Handle 上被调用, THE Test_Runner SHALL 验证 Handler 的 on_close 回调被触发且 err 参数为 0

### 需求 8：Close 后 Send 返回错误

**用户故事：** 作为开发者，我想测试关闭后调用 `xylem_udp_send` 的行为，以确保 closing 状态下发送操作被正确拒绝。

#### 验收标准

1. WHEN `xylem_udp_send` 在已关闭的 UDP_Handle 上被调用, THE Test_Runner SHALL 验证返回值为 -1

### 需求 9：Userdata 存取

**用户故事：** 作为开发者，我想测试 UDP_Handle 的用户数据存取功能，以确保 `xylem_udp_set_userdata` 和 `xylem_udp_get_userdata` 正确工作。

#### 验收标准

1. WHEN `xylem_udp_set_userdata` 在 UDP_Handle 上设置一个指针后调用 `xylem_udp_get_userdata`, THE Test_Runner SHALL 验证返回的指针与设置的指针相同

### 需求 10：获取事件循环

**用户故事：** 作为开发者，我想测试获取关联事件循环功能，以确保 `xylem_udp_get_loop` 返回创建 UDP_Handle 时传入的 Loop 句柄。

#### 验收标准

1. WHEN 在 UDP_Handle 上调用 `xylem_udp_get_loop`, THE Test_Runner SHALL 验证返回的 Loop 句柄与创建 UDP_Handle 时传入的 Loop 句柄相同

### 需求 11：测试基础设施

**用户故事：** 作为开发者，我想确保测试文件遵循项目测试规范，以保证测试的可靠性和可维护性。

#### 验收标准

1. THE Test_Runner SHALL 在每个测试函数中创建独立的 Loop，不依赖文件级共享可变状态
2. THE Test_Runner SHALL 在每个异步测试函数中创建 Safety_Timer（2000ms 超时），防止事件循环永久阻塞
3. THE Test_Runner SHALL 在每个测试函数结束时对所有创建的 UDP_Handle 调用 `xylem_udp_close` 并销毁所有资源（Loop、Timer）
4. THE Test_Runner SHALL 使用项目自定义的 `ASSERT` 宏进行断言，不使用标准 `<assert.h>`
5. THE Test_Runner SHALL 为每个测试函数分配唯一的端口号（使用 127.0.0.1 回环地址），避免测试间端口冲突
6. THE Test_Runner SHALL 确保每个测试函数只测试一个关注点
