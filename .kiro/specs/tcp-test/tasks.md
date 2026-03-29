# 实现计划：TCP 模块测试重写

## 概述

将 `tests/test-tcp.c` 完整重写，消除文件级共享可变状态，为每个测试分配独立端口，通过 userdata 机制传递测试上下文，覆盖需求文档中全部 33 个需求。按功能分组增量实现，每个任务完成后文件可编译运行。

## 任务

- [x] 1. 搭建测试文件骨架与基础设施 + 公共 API 测试
  - [x] 1.1 创建 `tests/test-tcp.c` 骨架：License Header、includes、端口号宏（PORT_LISTEN_CLOSE 19001 到 PORT_DRAIN_WRITE_QUEUE 19040）、Safety Timer 辅助回调 `_safety_timeout_cb`、空 `main()` 函数（含 `xylem_startup`/`xylem_cleanup`）
    - 文件结构遵循 docs/style.md 测试规范
    - 使用 `#include "xylem.h"` 和 `#include "assert.h"`
    - _Requirements: 33.1, 33.2, 33.3_

  - [x] 1.2 实现 `test_listen_and_close`：验证 `xylem_tcp_listen` 返回非 NULL，`xylem_tcp_close_server` 正常关闭
    - 创建 Loop、Safety Timer、局部 context struct
    - listen 后立即 close_server，验证不崩溃
    - _Requirements: 1.1_

  - [x] 1.3 实现 `test_close_server_with_active_conn`：关闭 Server 时活跃连接的 on_close 被触发
    - Server accept 后设置 userdata，close_server 触发所有 conn 的 on_close
    - 通过 context 中的计数器验证 on_close 被调用
    - _Requirements: 1.2_

  - [x] 1.4 实现 `test_close_server_idempotent`：重复调用 `xylem_tcp_close_server` 不崩溃
    - 连续调用两次 close_server，验证无崩溃
    - _Requirements: 1.3_

  - [x] 1.5 实现 `test_dial_connect`：客户端连接成功，on_connect 触发
    - dial 后运行事件循环，验证 on_connect 回调被触发
    - _Requirements: 2.1_

  - [x] 1.6 实现 `test_close_empty_queue`：Write_Queue 为空时关闭立即触发 on_close
    - on_connect 中直接调用 xylem_tcp_close，验证 on_close 触发
    - _Requirements: 2.2_

  - [x] 1.7 实现 `test_send_basic`：发送数据成功，on_write_done 正确触发
    - 发送 "data"，验证 on_write_done 以 status=0 和 len=4 触发
    - _Requirements: 3.1, 29.1_

  - [x] 1.8 实现 `test_send_after_close`：关闭后发送返回 -1
    - on_connect 中 close 后调用 send，验证返回 -1
    - _Requirements: 3.2_

  - [x] 1.9 实现 `test_conn_userdata`：连接级 userdata 存取
    - on_accept 中 set_userdata，get_userdata 验证指针和值一致
    - _Requirements: 4.1_

  - [x] 1.10 实现 `test_server_userdata`：Server 级 userdata 存取
    - listen 后 set_userdata，on_accept 中 get_userdata 验证
    - _Requirements: 5.1_

  - [x] 1.11 实现 `test_peer_addr`：服务端 accept 后获取客户端对端地址
    - on_accept 中调用 xylem_tcp_get_peer_addr，验证非 NULL 且地址族为 AF_INET
    - _Requirements: 6.1_

  - [x] 1.12 实现 `test_get_loop`：获取关联 Loop 句柄正确
    - on_connect 中调用 xylem_tcp_get_loop，验证与创建时的 loop 相同
    - _Requirements: 7.1_

- [x] 2. Checkpoint — 基础设施与公共 API 测试
  - 确保所有测试编译通过并运行成功，如有问题请询问用户。

- [x] 3. 分帧测试 — NONE / FIXED / LENGTH
  - [x] 3.1 实现 `test_frame_none`：FRAME_NONE echo 模式累积验证
    - 客户端发送 "hello"，服务端 on_read 回显数据，客户端累积收到的总数据验证与发送内容一致（TCP 字节流单次 on_read 交付量不确定）
    - _Requirements: 8.1_

  - [x] 3.2 实现 `test_frame_fixed`：FRAME_FIXED 按固定大小分帧
    - 服务端配置 frame_size=4，客户端发送 "ABCDEFGH"（8 字节）
    - 验证 on_read 触发 2 次，每次 4 字节，内容分别为 "ABCD" 和 "EFGH"
    - _Requirements: 9.1_

  - [x] 3.3 实现 `test_frame_fixed_zero`：frame_size=0 导致连接关闭
    - 服务端配置 frame_size=0，客户端发送数据后验证 on_close 触发
    - _Requirements: 9.2_

  - [x] 3.4 实现 `test_frame_length_be`：FRAME_LENGTH fixedint 大端
    - 服务端配置 2 字节大端 fixedint，客户端发送 [0x00, 0x05, "HELLO"]
    - 验证 on_read 收到 "HELLO"（5 字节）
    - _Requirements: 10.1_

  - [x] 3.5 实现 `test_frame_length_le`：FRAME_LENGTH fixedint 小端
    - 服务端配置 2 字节小端 fixedint（field_big_endian=false）
    - 客户端发送 [0x05, 0x00, "HELLO"]，验证 on_read 收到 "HELLO"
    - _Requirements: 11.1_

  - [x] 3.6 实现 `test_frame_length_field_size_zero`：field_size=0 导致连接关闭
    - 服务端配置 field_size=0，客户端发送数据后验证 on_close 触发
    - _Requirements: 12.1_

  - [x] 3.7 实现 `test_frame_length_field_size_over8`：field_size>8 导致连接关闭
    - 服务端配置 field_size=9，客户端发送数据后验证 on_close 触发
    - _Requirements: 12.2_

  - [x] 3.8 实现 `test_frame_length_varint`：FRAME_LENGTH varint
    - 服务端配置 varint 编码，客户端发送 [varint(5), "WORLD"]
    - 验证 on_read 收到 "WORLD"（5 字节）
    - _Requirements: 13.1_

  - [x] 3.9 实现 `test_frame_length_adjustment`：adjustment 非零
    - 服务端配置 adjustment=-2（长度字段值包含头部大小）
    - 验证 on_read 收到的 payload 长度正确
    - _Requirements: 14.1_

  - [x] 3.10 实现 `test_frame_length_empty_payload`：frame_size<=0 导致连接关闭
    - 配置 adjustment 使 frame_size <= 0，验证 on_close 触发
    - _Requirements: 15.1_

- [x] 4. 分帧测试 — DELIM / CUSTOM
  - [ ] 4.1 实现 `test_frame_delim_multi`：FRAME_DELIM 多字节分隔符 "\r\n"
    - 客户端发送 "hello\r\nworld\r\n"，验证 on_read 触发 2 次
    - 分别收到 "hello" 和 "world"
    - _Requirements: 16.1_

  - [ ] 4.2 实现 `test_frame_delim_single`：FRAME_DELIM 单字节分隔符 "\n"
    - 客户端发送 "abc\ndef\n"，验证 on_read 触发 2 次
    - 分别收到 "abc" 和 "def"（走 memchr 优化路径）
    - _Requirements: 17.1_

  - [ ] 4.3 实现 `test_frame_delim_null`：delim=NULL 导致连接关闭
    - 服务端配置 delim=NULL，客户端发送数据后验证 on_close 触发
    - _Requirements: 18.1_

  - [ ] 4.4 实现 `test_frame_custom_positive`：FRAME_CUSTOM parse 返回正值
    - 自定义 parse 函数返回固定正值，验证 on_read 收到正确长度帧
    - _Requirements: 19.1_

  - [ ] 4.5 实现 `test_frame_custom_zero`：FRAME_CUSTOM parse 返回 0
    - parse 始终返回 0（数据不足），验证 on_read 不被触发
    - 通过定时器延迟后停止循环，检查 read_count == 0
    - _Requirements: 19.2_

  - [ ] 4.6 实现 `test_frame_custom_negative`：FRAME_CUSTOM parse 返回负值
    - parse 返回 -1，验证连接因帧提取错误被关闭
    - _Requirements: 19.3_

  - [ ] 4.7 实现 `test_frame_custom_null_parse`：parse=NULL 导致连接关闭
    - 服务端配置 FRAME_CUSTOM 但 parse=NULL，验证 on_close 触发
    - _Requirements: 19.4_

- [x] 5. Checkpoint — 分帧测试
  - 确保所有分帧测试编译通过并运行成功，如有问题请询问用户。

- [x] 6. 超时和心跳测试
  - [x] 6.1 实现 `test_read_timeout`：读超时触发 on_timeout
    - 客户端配置 read_timeout_ms=100，连接后不发送数据
    - 验证 on_timeout 以 XYLEM_TCP_TIMEOUT_READ 类型触发
    - _Requirements: 20.1_

  - [x] 6.2 实现 `test_write_timeout`：写超时触发 on_timeout
    - 配置 write_timeout_ms=100，发送数据但对端不读取（模拟写阻塞）
    - 验证 on_timeout 以 XYLEM_TCP_TIMEOUT_WRITE 类型触发
    - _Requirements: 21.1_

  - [x] 6.3 实现 `test_connect_timeout`：连接超时触发 on_timeout
    - 配置 connect_timeout_ms=100，dial 到一个不可达地址（如 192.0.2.1 RFC 5737 TEST-NET）
    - 验证 on_timeout 以 XYLEM_TCP_TIMEOUT_CONNECT 类型触发
    - _Requirements: 22.1_

  - [x] 6.4 实现 `test_heartbeat_miss`：心跳超时触发 on_heartbeat_miss
    - 服务端配置 heartbeat_ms=100，连接后不发送数据
    - 验证 on_heartbeat_miss 回调被触发
    - _Requirements: 23.1_

  - [x] 6.5 实现 `test_heartbeat_reset_on_data`：收到数据后心跳定时器重置
    - 服务端配置 heartbeat_ms=200，客户端每 100ms 发送数据
    - 通过定时器在 500ms 后停止循环，验证 on_heartbeat_miss 未被触发
    - _Requirements: 24.1_

- [x] 7. 重连测试
  - [x] 7.1 实现 `test_reconnect_success`：重连成功
    - 配置 reconnect_max > 0，先 dial 到无监听端口，延迟后启动 server
    - 验证重连后 on_connect 被触发
    - _Requirements: 25.1_

  - [x] 7.2 实现 `test_reconnect_limit`：重连达到上限后关闭
    - 配置 reconnect_max=1，dial 到无监听端口
    - 验证重连次数耗尽后 on_close 被触发
    - _Requirements: 26.1_

- [x] 8. Checkpoint — 超时、心跳与重连测试
  - 确保所有超时、心跳和重连测试编译通过并运行成功，如有问题请询问用户。

- [x] 9. 读写边界与生命周期测试
  - [x] 9.1 实现 `test_read_buf_full`：读缓冲区满导致连接关闭
    - 服务端配置 read_buf_size 为极小值（如 8），使用 FRAME_NONE
    - 客户端发送超过缓冲区大小的数据，验证 on_close 触发
    - _Requirements: 27.1_

  - [x] 9.2 实现 `test_peer_close_eof`：对端关闭触发 on_close
    - 客户端连接后立即 close，验证服务端 conn 的 on_close 触发
    - _Requirements: 28.1_

  - [x] 9.3 实现 `test_close_pending_writes`：Write_Queue 非空时等待排空后关闭
    - on_connect 中连续 send 多条数据后调用 close
    - 验证所有 on_write_done 回调均被触发，最后 on_close 触发
    - _Requirements: 2.3, 30.1, 30.2_

  - [x] 9.4 实现 `test_drain_write_queue_on_error`：错误关闭时 drain Write_Queue
    - 在 Write_Queue 非空时模拟错误关闭（如对端 reset）
    - 验证每个未发送写请求的 on_write_done 以非零 status 触发
    - _Requirements: 32.1_

  - [x] 9.5 实现 `test_lifecycle_full`：完整生命周期 accept->read/write->close
    - 客户端连接、发送数据、服务端回显、双方关闭
    - 验证 on_accept、on_connect、on_read、on_close 均被触发
    - _Requirements: 31.1_

- [x] 10. Final Checkpoint — 全部测试通过
  - 确保所有测试编译通过并运行成功，如有问题请询问用户。
  - 运行命令：`cmake -B out -DCMAKE_BUILD_TYPE=Debug && cmake --build out -j 8 && ctest --test-dir out -R tcp --output-on-failure`

## 备注

- 所有测试在单个文件 `tests/test-tcp.c` 中实现
- 每个测试函数使用局部 context struct，通过 userdata 机制传递给回调，不使用文件级共享可变变量
- 每个测试使用独立端口号宏，避免端口冲突
- 每个测试创建独立的 Loop 和 2 秒 Safety Timer
- 使用项目自定义 `ASSERT` 宏，不使用标准 `<assert.h>`
- `main()` 中 `xylem_startup()` / `xylem_cleanup()` 包裹所有测试调用
- 本测试文件不包含属性化测试（原因见设计文档）
