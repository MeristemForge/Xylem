# 设计文档：TCP 模块测试重写

## 概述

本设计描述 `tests/test-tcp.c` 的完整重写方案。目标是为 `src/xylem-tcp.c` 中所有公共 API 和关键内部路径提供全面的测试覆盖，同时严格遵循 `docs/style.md` 中的测试规范。

现有测试文件存在以下问题：
- 大量使用文件级静态可变变量在测试间共享状态，违反 "No Global State" 规则
- 所有测试共用同一端口 `TCP_PORT 18080`，顺序执行时存在端口冲突风险
- 缺少对多种分帧错误路径、超时机制、心跳、重连、读缓冲区满等场景的测试

重写后的测试文件将：
- 每个 `test_*` 函数内部声明所有状态（通过局部 struct 或局部变量），不使用文件级共享可变变量
- 每个测试使用独立的端口号宏，避免冲突
- 覆盖需求文档中定义的全部 33 个需求

## 架构

### 测试文件整体结构

```
tests/test-tcp.c
├── License Header
├── #include "xylem.h"
├── #include "assert.h"
├── #include <string.h>
├── 端口号宏定义 (每个测试一个独立端口)
├── Safety Timer 辅助函数 (文件级，无状态)
├── 各测试的回调函数 + test_* 函数 (按需求分组)
└── main() — 顺序调用所有 test_* 函数
```

### 状态管理策略

为遵循 "No Global State" 规则，每个测试使用一个局部 context struct 来持有该测试所需的全部状态。回调函数通过 `xylem_tcp_set_userdata` / `xylem_tcp_server_set_userdata` 或 Safety Timer 的 `ud` 参数获取 context 指针。

```c
typedef struct {
    xylem_loop_t*       loop;
    xylem_tcp_server_t* server;
    xylem_tcp_conn_t*   srv_conn;
    xylem_tcp_conn_t*   cli_conn;
    int                 accept_called;
    int                 connect_called;
    /* ... 测试特定字段 ... */
} _echo_ctx_t;
```

但由于 `xylem_tcp_handler_t` 的回调签名是固定的（没有通用 `void* ud` 参数），回调函数获取 context 的方式受限。实际可行的方案是：

1. 对于 `on_accept` 回调：通过 `xylem_tcp_server_set_userdata(server, &ctx)` 在 listen 后设置，回调中通过 `xylem_tcp_server_get_userdata(server)` 获取
2. 对于 `on_connect/on_read/on_write_done/on_close` 回调：通过 `xylem_tcp_set_userdata(conn, &ctx)` 设置，回调中通过 `xylem_tcp_get_userdata(conn)` 获取
3. 对于 `on_accept` 中新接受的连接：在 `on_accept` 回调中立即调用 `xylem_tcp_set_userdata(conn, &ctx)`

这种方式使每个测试函数完全自包含，回调通过 userdata 机制访问测试上下文，无需文件级变量。

### Safety Timer

每个测试函数创建一个 2 秒超时的 Safety Timer，防止事件循环因测试失败而永久阻塞。Safety Timer 的辅助函数是文件级的无状态工具函数：

```c
static void _safety_timeout_cb(xylem_loop_t* loop,
                                xylem_loop_timer_t* timer,
                                void* ud) {
    (void)timer;
    (void)ud;
    xylem_loop_stop(loop);
}
```

每个测试函数内部创建和销毁 Safety Timer：

```c
xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
xylem_loop_start_timer(safety, _safety_timeout_cb, 2000, 0);
/* ... 测试逻辑 ... */
xylem_loop_destroy_timer(safety);
```

### 端口号分配

使用宏定义为每个测试分配独立端口，避免测试间端口冲突：

```c
#define PORT_LISTEN_CLOSE       19001
#define PORT_DIAL_CONNECT       19002
#define PORT_SEND_BASIC         19003
/* ... 每个测试一个端口 ... */
```

## 组件与接口

### 测试函数列表

下表列出所有 `test_*` 函数及其对应的需求：

| 测试函数 | 对应需求 | 描述 |
|---------|---------|------|
| `test_listen_and_close` | 需求 1.1 | Server 创建返回非 NULL |
| `test_close_server_with_active_conn` | 需求 1.2 | 关闭 Server 时活跃连接的 on_close 被触发 |
| `test_close_server_idempotent` | 需求 1.3 | 重复关闭 Server 不崩溃 |
| `test_dial_connect` | 需求 2.1 | 客户端连接成功，on_connect 触发 |
| `test_close_empty_queue` | 需求 2.2 | Write_Queue 为空时关闭立即触发 on_close |
| `test_close_pending_writes` | 需求 2.3, 30.1, 30.2 | Write_Queue 非空时等待排空后关闭 |
| `test_send_basic` | 需求 3.1, 29.1 | 发送数据成功，on_write_done 正确触发 |
| `test_send_after_close` | 需求 3.2 | 关闭后发送返回 -1 |
| `test_conn_userdata` | 需求 4.1 | 连接级 userdata 存取 |
| `test_server_userdata` | 需求 5.1 | Server 级 userdata 存取 |
| `test_peer_addr` | 需求 6.1 | 服务端 accept 后获取客户端对端地址非 NULL 且地址族正确 |
| `test_get_loop` | 需求 7.1 | 获取关联 Loop 句柄正确 |
| `test_frame_none` | 需求 8.1 | FRAME_NONE echo 模式累积验证数据一致 |
| `test_frame_fixed` | 需求 9.1 | FRAME_FIXED 按固定大小分帧 |
| `test_frame_fixed_zero` | 需求 9.2 | frame_size=0 导致连接关闭 |
| `test_frame_length_be` | 需求 10.1 | FRAME_LENGTH fixedint 大端 |
| `test_frame_length_le` | 需求 11.1 | FRAME_LENGTH fixedint 小端 |
| `test_frame_length_field_size_zero` | 需求 12.1 | field_size=0 导致连接关闭 |
| `test_frame_length_field_size_over8` | 需求 12.2 | field_size>8 导致连接关闭 |
| `test_frame_length_varint` | 需求 13.1 | FRAME_LENGTH varint |
| `test_frame_length_adjustment` | 需求 14.1 | adjustment 非零 |
| `test_frame_length_empty_payload` | 需求 15.1 | frame_size<=0 导致连接关闭 |
| `test_frame_delim_multi` | 需求 16.1 | FRAME_DELIM 多字节分隔符 |
| `test_frame_delim_single` | 需求 17.1 | FRAME_DELIM 单字节分隔符 |
| `test_frame_delim_null` | 需求 18.1 | delim=NULL 导致连接关闭 |
| `test_frame_custom_positive` | 需求 19.1 | FRAME_CUSTOM parse 返回正值 |
| `test_frame_custom_zero` | 需求 19.2 | FRAME_CUSTOM parse 返回 0 |
| `test_frame_custom_negative` | 需求 19.3 | FRAME_CUSTOM parse 返回负值 |
| `test_frame_custom_null_parse` | 需求 19.4 | parse=NULL 导致连接关闭 |
| `test_read_timeout` | 需求 20.1 | 读超时触发 on_timeout |
| `test_write_timeout` | 需求 21.1 | 写超时触发 on_timeout |
| `test_connect_timeout` | 需求 22.1 | 连接超时触发 on_timeout |
| `test_heartbeat_miss` | 需求 23.1 | 心跳超时触发 on_heartbeat_miss |
| `test_heartbeat_reset_on_data` | 需求 24.1 | 收到数据后心跳定时器重置 |
| `test_reconnect_success` | 需求 25.1 | 重连成功 |
| `test_reconnect_limit` | 需求 26.1 | 重连达到上限后关闭 |
| `test_read_buf_full` | 需求 27.1 | 读缓冲区满导致连接关闭 |
| `test_peer_close_eof` | 需求 28.1 | 对端关闭触发 on_close |
| `test_lifecycle_full` | 需求 31.1 | 完整生命周期 |
| `test_drain_write_queue_on_error` | 需求 32.1 | 错误关闭时 drain Write_Queue |

### 每个测试的通用模式

```c
static void test_xxx(void) {
    /* 1. 创建 Loop */
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    /* 2. 创建 Safety Timer */
    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, 2000, 0);

    /* 3. 初始化局部 context */
    _xxx_ctx_t ctx = {0};
    ctx.loop = loop;

    /* 4. 创建 Server (如需要) */
    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", PORT_XXX, &addr);
    /* ... listen, set userdata ... */

    /* 5. 创建 Client (如需要) */
    /* ... dial, set userdata ... */

    /* 6. 运行事件循环 */
    xylem_loop_run(loop);

    /* 7. 断言验证 */
    ASSERT(ctx.xxx == expected);

    /* 8. 清理资源 */
    xylem_loop_destroy_timer(safety);
    if (ctx.server) { xylem_tcp_close_server(ctx.server); }
    xylem_loop_destroy(loop);
}
```

### 被测公共 API

| 函数 | 覆盖的测试 |
|------|-----------|
| `xylem_tcp_listen` | test_listen_and_close, 以及所有需要 Server 的测试 |
| `xylem_tcp_close_server` | test_close_server_with_active_conn, test_close_server_idempotent |
| `xylem_tcp_dial` | test_dial_connect, 以及所有需要 Client 的测试 |
| `xylem_tcp_send` | test_send_basic, test_send_after_close |
| `xylem_tcp_close` | test_close_empty_queue, test_close_pending_writes |
| `xylem_tcp_get_peer_addr` | test_peer_addr |
| `xylem_tcp_get_loop` | test_get_loop |
| `xylem_tcp_get_userdata` | test_conn_userdata |
| `xylem_tcp_set_userdata` | test_conn_userdata |
| `xylem_tcp_server_get_userdata` | test_server_userdata |
| `xylem_tcp_server_set_userdata` | test_server_userdata |

## 数据模型

### 测试 Context 结构体

每个测试定义自己的局部 context 类型。以下是几个典型示例：

#### Echo 测试 Context（用于分帧测试）

```c
typedef struct {
    xylem_loop_t*       loop;
    xylem_tcp_server_t* server;
    xylem_tcp_conn_t*   srv_conn;
    xylem_tcp_conn_t*   cli_conn;
    int                 read_count;
    char                received[128];
    size_t              received_len;
} _frame_ctx_t;
```

#### 生命周期测试 Context

```c
typedef struct {
    xylem_loop_t*       loop;
    xylem_tcp_server_t* server;
    xylem_tcp_conn_t*   srv_conn;
    int                 accept_called;
    int                 connect_called;
    int                 cli_close_called;
    int                 srv_close_called;
} _life_ctx_t;
```

#### 超时测试 Context

```c
typedef struct {
    xylem_loop_t*              loop;
    xylem_tcp_server_t*        server;
    xylem_tcp_conn_t*          cli_conn;
    int                        timeout_called;
    xylem_tcp_timeout_type_t   timeout_type;
} _timeout_ctx_t;
```

### Handler 配置模式

每个测试根据需要配置不同的 Handler 回调子集：

```c
/* 服务端 Handler — 只需 accept 和 close */
xylem_tcp_handler_t srv_handler = {
    .on_accept = _xxx_srv_accept_cb,
    .on_close  = _xxx_srv_close_cb,
};

/* 客户端 Handler — 需要 connect、read、close */
xylem_tcp_handler_t cli_handler = {
    .on_connect = _xxx_cli_connect_cb,
    .on_read    = _xxx_cli_read_cb,
    .on_close   = _xxx_cli_close_cb,
};
```

### Opts 配置模式

不同分帧策略的 Opts 配置示例：

```c
/* FRAME_NONE */
xylem_tcp_opts_t opts = {0};

/* FRAME_FIXED */
xylem_tcp_opts_t opts = {0};
opts.framing.type = XYLEM_TCP_FRAME_FIXED;
opts.framing.fixed.frame_size = 4;

/* FRAME_LENGTH fixedint 大端 */
xylem_tcp_opts_t opts = {0};
opts.framing.type = XYLEM_TCP_FRAME_LENGTH;
opts.framing.length.header_size = 2;
opts.framing.length.field_offset = 0;
opts.framing.length.field_size = 2;
opts.framing.length.coding = XYLEM_TCP_LENGTH_FIXEDINT;
opts.framing.length.field_big_endian = true;

/* FRAME_DELIM */
xylem_tcp_opts_t opts = {0};
opts.framing.type = XYLEM_TCP_FRAME_DELIM;
opts.framing.delim.delim = "\r\n";
opts.framing.delim.delim_len = 2;

/* 超时配置 */
xylem_tcp_opts_t opts = {0};
opts.read_timeout_ms = 100;
opts.heartbeat_ms = 100;
```


## 正确性属性

*属性是一种在系统所有有效执行中都应成立的特征或行为——本质上是关于系统应该做什么的形式化陈述。属性是人类可读规范与机器可验证正确性保证之间的桥梁。*

### 属性分析

本测试文件的所有需求均为集成测试场景，涉及事件循环、Socket I/O 和异步回调验证。每个测试需要完整的 server/client 设置，输入是特定的线格式和配置，行为是事件驱动的回调触发。

可以进行属性化测试的候选项是分帧逻辑（`_tcp_extract_frame`），但该函数是 `static` 的，无法从测试文件直接访问。公共 API 表面完全是异步/回调式的，不适合属性化测试。

因此，本测试文件不包含属性化测试。所有验收标准通过具体的示例测试（example-based tests）覆盖。

以下是从需求中提取的关键可验证行为：

### Property 1: Send 返回值与连接状态一致

*For any* TCP 连接，当连接处于 CONNECTED 状态时 `xylem_tcp_send` 返回 0，当连接处于 CLOSING 或 CLOSED 状态时返回 -1。

**Validates: Requirements 3.1, 3.2**

### Property 2: Userdata 存取往返

*For any* 指针值 p，在 Conn 上调用 `xylem_tcp_set_userdata(conn, p)` 后，`xylem_tcp_get_userdata(conn)` 应返回 p。Server 级 userdata 同理。

**Validates: Requirements 4.1, 5.1**

### Property 3: 分帧提取的帧边界正确性

*For any* 使用 FRAME_FIXED(size=N) 配置的连接，收到 K*N 字节数据时，on_read 应被触发恰好 K 次，每次收到 N 字节。

**Validates: Requirements 9.1**

### Property 4: 优雅关闭保证 Write_Queue 排空

*For any* 连接，当 `xylem_tcp_close` 被调用时 Write_Queue 中有 M 个待发送请求，则 on_write_done 应被触发恰好 M 次后 on_close 才被触发。

**Validates: Requirements 2.3, 30.1, 30.2**

### Property 5: close_server 的幂等性

*For any* Server 句柄，连续调用 `xylem_tcp_close_server` 两次不应导致崩溃或未定义行为。

**Validates: Requirements 1.3**

> 注意：由于所有被测 API 都是异步回调式的，且分帧核心逻辑（`_tcp_extract_frame`）是 static 函数，上述属性在实际测试中以具体示例形式实现，而非使用属性化测试框架。

## 错误处理

### 测试中的错误场景覆盖

| 错误场景 | 对应测试 | 预期行为 |
|---------|---------|---------|
| FRAME_FIXED frame_size=0 | test_frame_fixed_zero | 连接关闭，on_close 触发 |
| FRAME_LENGTH field_size=0 | test_frame_length_field_size_zero | 连接关闭，on_close 触发 |
| FRAME_LENGTH field_size>8 | test_frame_length_field_size_over8 | 连接关闭，on_close 触发 |
| FRAME_LENGTH adjustment 导致 frame_size<=0 | test_frame_length_empty_payload | 连接关闭，on_close 触发 |
| FRAME_DELIM delim=NULL | test_frame_delim_null | 连接关闭，on_close 触发 |
| FRAME_CUSTOM parse=NULL | test_frame_custom_null_parse | 连接关闭，on_close 触发 |
| FRAME_CUSTOM parse 返回负值 | test_frame_custom_negative | 连接关闭，on_close 触发 |
| Send after close | test_send_after_close | 返回 -1 |
| 读缓冲区满 | test_read_buf_full | 连接关闭，on_close 触发 |
| 对端 EOF | test_peer_close_eof | on_close 触发 |
| 读超时 | test_read_timeout | on_timeout(READ) 触发 |
| 写超时 | test_write_timeout | on_timeout(WRITE) 触发 |
| 连接超时 | test_connect_timeout | on_timeout(CONNECT) 触发 |
| 重连达到上限 | test_reconnect_limit | on_close 触发 |
| 错误关闭时 drain Write_Queue | test_drain_write_queue_on_error | on_write_done 以非零 status 触发 |

### Safety Timer 兜底

每个测试函数都配置 2 秒 Safety Timer。如果事件循环因任何原因未能正常退出（如回调未触发 `xylem_loop_stop`），Safety Timer 会强制停止循环，防止测试进程永久挂起。

## 测试策略

### 测试方法

本测试文件采用纯示例测试（example-based testing）方法：

- **单元测试**：每个 `test_*` 函数测试一个具体场景，使用特定输入和预期输出
- **集成测试**：大部分测试涉及完整的 server/client 交互，属于集成测试性质
- **边界测试**：专门的测试函数覆盖无效配置和错误条件

### 不使用属性化测试的理由

本测试文件不使用属性化测试（property-based testing），原因如下：

1. 所有被测 API 都是异步回调式的，需要完整的事件循环设置
2. 核心分帧逻辑 `_tcp_extract_frame` 是 `static` 函数，无法从测试文件直接调用
3. 测试涉及真实的 Socket I/O，每次测试需要绑定端口、建立连接，不适合大量随机化运行
4. C 语言生态中缺乏成熟的属性化测试框架

### 测试执行

```bash
# 构建并运行 TCP 测试
cmake -B out -DCMAKE_BUILD_TYPE=Debug
cmake --build out -j 8
ctest --test-dir out -R tcp --output-on-failure
```

### 覆盖目标

| 类别 | 覆盖范围 |
|------|---------|
| 公共 API | 所有 11 个公共函数均有测试 |
| 分帧策略 | NONE、FIXED、LENGTH(fixedint BE/LE, varint, adjustment)、DELIM(单/多字节)、CUSTOM |
| 错误路径 | 无效 frame_size、无效 field_size、NULL delim、NULL parse、send after close、读缓冲区满 |
| 超时机制 | 读超时、写超时、连接超时、心跳超时、心跳重置 |
| 重连 | 重连成功、重连达到上限 |
| 生命周期 | 完整 accept->read/write->close 流程、优雅关闭 flush、错误关闭 drain |

### 关键约束

1. 所有测试在单个文件 `tests/test-tcp.c` 中
2. 使用项目自定义 `ASSERT` 宏（`tests/assert.h`）
3. 每个测试函数独立创建/销毁 Loop 和所有资源
4. 使用 Safety Timer（2 秒）防止事件循环阻塞
5. 每个测试使用独立端口号宏，避免冲突
6. 回调通过 userdata 机制访问测试上下文，不使用文件级共享可变变量
7. `main()` 中 `xylem_startup()` / `xylem_cleanup()` 包裹所有测试调用
