# 实现计划：UDP 模块测试重写

## 概述

将 `tests/test-udp.c` 完整重写，为 `xylem-udp` 模块的所有公共 API 提供测试覆盖。共 10 个测试函数，覆盖 listen/dial 两种模式的收发、地址验证、数据报边界、close 生命周期、userdata 和 get_loop。每个异步测试使用独立 Loop、Safety Timer（2000ms）和唯一端口，文件作用域状态变量按测试分组。

## 任务

- [x] 1. 搭建测试文件骨架与基础设施
  - [x] 1.1 创建 `tests/test-udp.c` 骨架：License Header、includes（`xylem.h`、`assert.h`、`<string.h>`）、共享安全定时器回调 `_safety_timeout_cb`、空 `main()` 函数（含 `xylem_startup`/`xylem_cleanup`）
    - 文件结构遵循 docs/style.md 测试规范
    - _Requirements: 11.1, 11.2, 11.4_

- [x] 2. Listen 模式数据路径测试
  - [x] 2.1 实现 `test_listen_recv`：验证 listen 模式接收数据报，on_read 交付正确的 data/len 和发送方 addr
    - 接收端：`xylem_udp_listen` 绑定 19001，发送端：`xylem_udp_listen` 绑定 19002
    - 定时器触发 `xylem_udp_send(sender, &dest, "hello", 5)`
    - on_read 回调中复制 data/len，提取 addr 的 IP 和端口，`xylem_loop_stop`
    - 断言：data == "hello"，len == 5，addr 端口 == 19002，addr IP == "127.0.0.1"
    - 文件作用域状态：`_lr_loop`, `_lr_receiver`, `_lr_sender`, `_lr_read_called`, `_lr_data[64]`, `_lr_data_len`, `_lr_sender_port`, `_lr_sender_ip[INET6_ADDRSTRLEN]`
    - **Property 1: Listen 模式接收数据完整性与发送方地址**
    - **Validates: Requirements 1.1, 1.2**

  - [x] 2.2 实现 `test_listen_send`：验证 listen 模式发送数据报到指定目标
    - A 端：`xylem_udp_listen` 绑定 19011（发送端），B 端：`xylem_udp_listen` 绑定 19012（接收端）
    - 定时器触发 A 端 `xylem_udp_send(a, &dest_b, "reply", 5)`
    - B 端 on_read 回调中验证收到的数据
    - 断言：B 端收到 "reply"，len == 5
    - 文件作用域状态：`_ls_loop`, `_ls_sender`, `_ls_receiver`, `_ls_read_called`, `_ls_data[64]`, `_ls_data_len`
    - **Property 2: Listen 模式发送数据完整性**
    - **Validates: Requirements 2.1**

  - [x] 2.3 实现 `test_datagram_boundary`：验证连续发送的多个不同大小数据报在接收端被独立交付
    - 接收端：`xylem_udp_listen` 绑定 19041，发送端：`xylem_udp_listen` 绑定 19042
    - 定时器触发连续发送 "A"(1B)、"BB"(2B)、"CCC"(3B)
    - on_read 回调计数并记录每次的 len 和 data，收到 3 次后 stop
    - 断言：read_count == 3，sizes == {1, 2, 3}，data 内容匹配
    - 文件作用域状态：`_db_loop`, `_db_receiver`, `_db_sender`, `_db_read_count`, `_db_sizes[3]`, `_db_bufs[3][4]`
    - **Property 4: 数据报边界保持**
    - **Validates: Requirements 5.1, 5.2**

- [x] 3. Checkpoint — Listen 模式与数据报边界测试
  - 确保所有测试编译通过并运行成功，如有问题请询问用户。

- [x] 4. Dial 模式测试
  - [x] 4.1 实现 `test_dial_echo`：验证 dial 模式双向收发
    - 服务端：`xylem_udp_listen` 绑定 19021，on_read 中回送数据到发送方地址
    - 客户端：`xylem_udp_dial` 连接到 19021
    - 定时器触发客户端 `xylem_udp_send(client, NULL, "ping", 4)`
    - 服务端 on_read 收到后通过 `xylem_udp_send(server, addr, "pong", 4)` 回复
    - 客户端 on_read 收到回复后 stop
    - 断言：服务端收到 "ping"，客户端收到 "pong"
    - 文件作用域状态：`_de_loop`, `_de_server`, `_de_client`, `_de_srv_data[64]`, `_de_srv_data_len`, `_de_cli_data[64]`, `_de_cli_data_len`
    - **Property 3: Dial 模式回声往返**
    - **Validates: Requirements 3.1, 3.2**

  - [x] 4.2 实现 `test_dial_addr`：验证 dial 模式 on_read 中 addr 参数与 dial 目标一致
    - 服务端：`xylem_udp_listen` 绑定 19031，收到数据后回复
    - 客户端：`xylem_udp_dial` 连接到 127.0.0.1:19031
    - 客户端 on_read 中提取 addr 的 IP 和端口
    - 断言：addr IP == "127.0.0.1"，端口 == 19031
    - 文件作用域状态：`_da_loop`, `_da_server`, `_da_client`, `_da_addr_port`, `_da_addr_ip[INET6_ADDRSTRLEN]`, `_da_read_called`
    - **Validates: Requirements 4.1**

- [x] 5. Checkpoint — Dial 模式测试
  - 确保所有测试编译通过并运行成功，如有问题请询问用户。

- [x] 6. Close 生命周期测试
  - [x] 6.1 实现 `test_close_idempotent`：验证 `xylem_udp_close` 连续调用两次不崩溃
    - `xylem_udp_listen` 绑定 19051
    - 连续调用 `xylem_udp_close` 两次
    - 运行事件循环让 deferred free 完成
    - 断言：程序正常继续（不崩溃即通过）
    - _Requirements: 6.1_

  - [x] 6.2 实现 `test_close_callback`：验证 `xylem_udp_close` 触发 on_close 回调且 err == 0
    - `xylem_udp_listen` 绑定 19061，handler 设置 on_close
    - on_close 回调中记录 err 值和调用标志
    - 调用 `xylem_udp_close`，然后运行事件循环
    - 断言：on_close 被调用，err == 0
    - 文件作用域状态：`_cc_close_called`, `_cc_close_err`
    - **Property 5: Close 触发 on_close 回调且 err 为 0**
    - **Validates: Requirements 7.1**

  - [x] 6.3 实现 `test_send_after_close`：验证关闭后 `xylem_udp_send` 返回 -1
    - 发送端：`xylem_udp_listen` 绑定 19071
    - 调用 `xylem_udp_close`，然后调用 `xylem_udp_send` 并记录返回值
    - 运行事件循环让 deferred free 完成
    - 断言：send 返回值 == -1
    - **Property 6: Close 后 Send 返回 -1**
    - **Validates: Requirements 8.1**

- [x] 7. Checkpoint — Close 生命周期测试
  - 确保所有测试编译通过并运行成功，如有问题请询问用户。

- [x] 8. 访问器测试
  - [x] 8.1 实现 `test_userdata`：验证 userdata 存取往返
    - `xylem_udp_listen` 绑定 19081
    - `xylem_udp_set_userdata(udp, &value)`，`xylem_udp_get_userdata(udp)` 获取指针
    - 断言：返回指针 == &value，解引用值正确
    - **Property 7: Userdata 存取往返**
    - **Validates: Requirements 9.1**

  - [x] 8.2 实现 `test_get_loop`：验证 `xylem_udp_get_loop` 返回创建时传入的 loop
    - 创建 loop，`xylem_udp_listen` 绑定 19091
    - 调用 `xylem_udp_get_loop(udp)`
    - 断言：返回值 == loop
    - **Property 8: Get Loop 返回创建时的 Loop**
    - **Validates: Requirements 10.1**

- [x] 9. Final Checkpoint — 全部测试通过
  - 确保所有测试编译通过并运行成功，如有问题请询问用户。
  - 运行命令：`cmake -B out -DCMAKE_BUILD_TYPE=Debug && cmake --build out -j 8 && ctest --test-dir out -R udp --output-on-failure`

## 备注

- 所有测试在单个文件 `tests/test-udp.c` 中实现
- 每个异步测试使用文件作用域状态变量（按测试分组，命名前缀标识所属测试），同步测试使用局部变量
- 每个测试使用独立端口范围（19001–19099，每个测试间隔 10），避免端口冲突
- 每个异步测试创建独立的 Loop 和 2000ms Safety Timer
- 使用项目自定义 `ASSERT` 宏，不使用标准 `<assert.h>`
- `main()` 中 `xylem_startup()` / `xylem_cleanup()` 包裹所有测试调用
- 构建通过 `tests/CMakeLists.txt` 中已有的 `xylem_add_test(udp)` 注册，无需修改 CMake
- 本测试文件不包含属性化测试库（C 语言生态限制），属性通过精心选择的具体示例验证
