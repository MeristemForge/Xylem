# Implementation Plan: HTTP 模块

## Overview

为 Xylem 库实现 HTTP/1.1 模块，包含同步客户端 API 和异步服务器 API。按增量顺序实现：先构建基础组件（URL 解析、percent-encoding、请求序列化、响应解析），再实现客户端 API，最后实现服务器 API。每个阶段都包含对应的测试任务。

## Tasks

- [x] 1. 项目结构搭建与 llhttp 构建集成
  - [x] 1.1 创建 `include/xylem/xylem-http.h` 头文件，包含所有公共类型前向声明（`xylem_http_res_t`、`xylem_http_req_t`、`xylem_http_conn_t`、`xylem_http_srv_t`）、`xylem_http_srv_cfg_t` 结构体定义、回调函数类型定义、以及所有公共函数的 `extern` 声明（含 Doxygen 注释）
    - 遵循 `_Pragma("once")`、license header、opaque struct 前向声明模式
    - _Requirements: 7.1-7.6, 8.2, 8.3, 12.1, 12.7_
  - [x] 1.2 创建 `src/xylem-http.c` 骨架文件，包含 license header、必要的 includes（`llhttp.h`、`xylem-tcp.h`、`xylem-tls.h`、`xylem-addr.h`、`xylem-loop.h`、`xylem-thrdpool.h`）、所有内部结构体定义（`_http_url_t`、`_http_header_t`、`xylem_http_res_s`、`xylem_http_req_s`、`xylem_http_conn_s`、`xylem_http_srv_s`、`_http_client_ctx_t`）、以及线程局部全局配置变量
    - _Requirements: 12.2, 14.5, 18.3, 18.5_
  - [x] 1.3 修改根 `CMakeLists.txt`：将 `src/xylem-http.c` 和 llhttp 源文件（`src/http/llhttp/llhttp.c`、`src/http/llhttp/http.c`、`src/http/llhttp/api.c`）添加到 SRCS 列表，将 `src/http/llhttp/` 添加到 include_directories
    - _Requirements: 1.1, 1.2, 1.3, 12.3, 12.4_
  - [x] 1.4 修改 `include/xylem.h`：添加 `#include "xylem/xylem-http.h"`
    - _Requirements: 12.6_
  - [x] 1.5 创建 `tests/test-http.c` 骨架文件（license header、`#include "assert.h"`、`#include "xylem.h"`、空 `main`），并在 `tests/CMakeLists.txt` 中添加 `xylem_add_test(http)`
    - _Requirements: 12.5_

- [x] 2. Checkpoint - 确保项目编译通过
  - 确保 llhttp 源文件和 HTTP 模块骨架编译通过，ask the user if questions arise.

- [x] 3. URL 解析器与 percent-encoding
  - [x] 3.1 在 `src/xylem-http.c` 中实现 `_http_url_parse` 静态函数：从 URL 字符串中提取 scheme（仅 http/https）、host、port（http 默认 80、https 默认 443、支持显式端口）、path（默认 "/"），无效 URL 返回 -1
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6_
  - [x] 3.2 在 `src/xylem-http.c` 中实现 `_http_url_serialize` 静态函数：将 `_http_url_t` 重新拼接为 URL 字符串
    - _Requirements: 2.7_
  - [x] 3.3 在 `src/xylem-http.c` 中实现 `xylem_http_url_encode` 和 `xylem_http_url_decode` 公共函数
    - _Requirements: 21.1, 21.2, 21.3, 21.4_
  - [x] 3.4 在 `tests/test-http.c` 中添加 URL 解析单元测试：`test_url_parse_http_default_port`、`test_url_parse_https_default_port`、`test_url_parse_explicit_port`、`test_url_parse_no_path`、`test_url_parse_invalid`
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6_
  - [ ]* 3.5 在 `tests/test-http.c` 中添加属性测试：URL parse-serialize round-trip
    - **Property 1: URL parse-serialize round-trip**
    - 使用 theft 库生成随机有效 URL 组件（scheme、host、port、path），验证 parse -> serialize -> parse 产生等价结果
    - **Validates: Requirements 2.1, 2.2, 2.3, 2.4, 2.5, 2.7, 2.8**
  - [ ]* 3.6 在 `tests/test-http.c` 中添加属性测试：Invalid URL rejection
    - **Property 2: Invalid URL rejection**
    - 使用 theft 库生成随机无效 URL 字符串（缺少 scheme、不支持的 scheme、空 host），验证 `_http_url_parse` 返回 -1
    - **Validates: Requirements 2.6**
  - [ ]* 3.7 在 `tests/test-http.c` 中添加属性测试：URL percent-encoding round-trip
    - **Property 9: URL percent-encoding round-trip**
    - 使用 theft 库生成随机字节序列，验证 encode -> decode 产生原始字节序列
    - **Validates: Requirements 21.1, 21.2, 21.3, 21.5**

- [x] 4. HTTP 请求序列化器
  - [x] 4.1 在 `src/xylem-http.c` 中实现 `_http_req_serialize` 静态函数：将 method、URL、headers、body 组装为 HTTP/1.1 请求报文，自动填充 Host、Content-Length、Connection: keep-alive，body > 1024 字节时添加 Expect: 100-continue，返回 `xylem_buf_t`
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 20.1_
  - [ ]* 4.2 在 `tests/test-http.c` 中添加属性测试：HTTP request serialize-parse round-trip
    - **Property 3: HTTP request serialize-parse round-trip**
    - 使用 theft 库生成随机有效请求组件（method、host、port、path、body），序列化后通过 llhttp 解析，验证 method、URL path、body 等价
    - **Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 3.6**
  - [ ]* 4.3 在 `tests/test-http.c` 中添加属性测试：100-Continue header for large bodies
    - **Property 8: 100-Continue header for large bodies**
    - 使用 theft 库生成随机大小的 body，验证 > 1024 字节时序列化结果包含 Expect: 100-continue，<= 1024 字节时不包含
    - **Validates: Requirements 20.1**

- [x] 5. HTTP 响应解析与 Header 管理
  - [x] 5.1 在 `src/xylem-http.c` 中实现 llhttp 响应解析回调集（`on_header_field`、`on_header_value`、`on_header_value_complete`、`on_headers_complete`、`on_body`、`on_message_complete`），支持 Content-Length 和 chunked transfer encoding，支持 max_body_size 检查
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 18.3, 18.4_
  - [x] 5.2 在 `src/xylem-http.c` 中实现 Header 大小写不敏感查找辅助函数 `_http_header_find`，使用逐字符 ASCII tolower 比较
    - _Requirements: 11.1, 11.2, 11.3_
  - [x] 5.3 在 `src/xylem-http.c` 中实现响应访问器公共函数：`xylem_http_res_status`、`xylem_http_res_header`、`xylem_http_res_body`、`xylem_http_res_body_len`、`xylem_http_res_destroy`（NULL 安全）
    - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7_
  - [x] 5.4 在 `tests/test-http.c` 中添加单元测试：`test_res_destroy_null`、`test_header_lookup_missing`
    - _Requirements: 7.3, 7.7_
  - [ ]* 5.5 在 `tests/test-http.c` 中添加属性测试：Duplicate header preservation
    - **Property 4: Duplicate header preservation**
    - 构造包含重复 header 的 HTTP 响应字节流，通过解析器解析后验证所有 header 值均被保留
    - **Validates: Requirements 4.4**
  - [ ]* 5.6 在 `tests/test-http.c` 中添加属性测试：Malformed response rejection
    - **Property 5: Malformed response rejection**
    - 使用 theft 库生成随机非法 HTTP 响应字节流，验证解析器返回错误
    - **Validates: Requirements 4.5**
  - [ ]* 5.7 在 `tests/test-http.c` 中添加属性测试：Case-insensitive header lookup
    - **Property 6: Case-insensitive header lookup**
    - 使用 theft 库生成随机 header name 及其大小写变体，验证查找返回相同值
    - **Validates: Requirements 7.2, 9.4, 11.1, 11.2, 11.3**

- [x] 6. Checkpoint - 确保基础组件测试通过
  - 确保 URL 解析、请求序列化、响应解析的所有测试通过，ask the user if questions arise.

- [x] 7. Reason phrase 映射与全局配置函数
  - [x] 7.1 在 `src/xylem-http.c` 中实现 `_http_reason_phrase` 静态函数：switch-case 将状态码映射到标准 reason phrase（1xx-5xx），未知状态码返回空字符串
    - _Requirements: 15.1, 15.2, 15.3_
  - [x] 7.2 在 `src/xylem-http.c` 中实现全局配置公共函数：`xylem_http_set_timeout`、`xylem_http_set_follow_redirects`、`xylem_http_set_max_body_size`
    - _Requirements: 14.1, 14.3, 16.2, 18.6_
  - [x] 7.3 在 `tests/test-http.c` 中添加单元测试：`test_reason_phrase_known`、`test_reason_phrase_unknown`、`test_default_timeout`、`test_default_max_body_client`
    - _Requirements: 14.5, 15.1, 15.3, 18.3_

- [x] 8. 同步客户端核心执行引擎
  - [x] 8.1 在 `src/xylem-http.c` 中实现 `_http_client_exec` 静态函数：创建临时 xylem_loop 和 thrdpool，执行 DNS 解析、TCP/TLS 连接（根据 scheme 自动选择）、请求发送、响应接收，支持超时定时器覆盖全流程，使用 goto cleanup 模式确保资源释放
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 5.7, 14.2, 14.4_
  - [x] 8.2 在 `_http_client_exec` 中实现 100-Continue 处理逻辑：body > 1024 字节时先发送 headers，等待 100 响应或 1 秒超时后发送 body；收到非 100 响应时直接返回该响应
    - _Requirements: 20.1, 20.2, 20.3, 20.5_
  - [x] 8.3 在 `_http_client_exec` 中实现重定向处理逻辑：当 `_http_max_redirects > 0` 且响应为 301/302/307/308 时，提取 Location header 发起新请求；301/302 改用 GET 丢弃 body，307/308 保留原方法和 body；超过最大次数返回最后响应
    - _Requirements: 16.1, 16.3, 16.4, 16.5, 16.6_
  - [x] 8.4 在 `_http_client_exec` 中实现 Connection: close 处理：收到 `Connection: close` 响应头后关闭连接
    - _Requirements: 13.1_

- [x] 9. 客户端公共请求函数
  - [x] 9.1 在 `src/xylem-http.c` 中实现 `xylem_http_get`：调用 `_http_client_exec` 发送 GET 请求
    - _Requirements: 5.1_
  - [x] 9.2 在 `src/xylem-http.c` 中实现 `xylem_http_post` 和 `xylem_http_post_json`：调用 `_http_client_exec` 发送 POST 请求，`post_json` 自动设置 Content-Type 为 "application/json"
    - _Requirements: 6.1, 6.2, 6.3, 6.4_
  - [x] 9.3 在 `src/xylem-http.c` 中实现 `xylem_http_put`、`xylem_http_delete`、`xylem_http_patch`
    - _Requirements: 17.1, 17.2, 17.3, 17.4_
  - [ ] 9.4 在 `tests/test-http.c` 中添加单元测试：`test_empty_body_post`、`test_redirect_301_changes_method`、`test_redirect_307_preserves_method`
    - _Requirements: 6.4, 16.5, 16.6_

- [x] 10. Checkpoint - 确保客户端编译通过
  - 确保客户端所有代码编译通过，ask the user if questions arise.

- [ ] 11. HTTP 服务器实现
  - [ ] 11.1 在 `src/xylem-http.c` 中实现 `xylem_http_srv_create`（NULL loop/cfg 返回 NULL）和 `xylem_http_srv_destroy`
    - _Requirements: 8.1, 8.2, 8.8, 8.9_
  - [ ] 11.2 在 `src/xylem-http.c` 中实现 `xylem_http_srv_start`：根据 cfg 中 tls_cert/tls_key 选择 xylem_tcp_listen 或 xylem_tls_listen 绑定端口，设置 on_accept 回调，绑定失败返回 -1
    - _Requirements: 8.4, 8.5, 8.6, 8.10_
  - [ ] 11.3 在 `src/xylem-http.c` 中实现 `xylem_http_srv_stop`
    - _Requirements: 8.7_
  - [ ] 11.4 在 `src/xylem-http.c` 中实现服务器连接管理：on_accept 中分配 `xylem_http_conn_t`，初始化 llhttp（HTTP_REQUEST 类型）和空闲定时器，实现 llhttp 请求解析回调，解析完成后调用用户 on_request
    - _Requirements: 9.1, 13.3, 13.4, 19.1, 19.4_
  - [ ] 11.5 在 `src/xylem-http.c` 中实现请求访问器公共函数：`xylem_http_req_method`、`xylem_http_req_url`、`xylem_http_req_header`、`xylem_http_req_body`、`xylem_http_req_body_len`
    - _Requirements: 9.2, 9.3, 9.4, 9.5, 9.6, 9.7, 9.8_
  - [ ] 11.6 在 `src/xylem-http.c` 中实现 `xylem_http_conn_send`：序列化 HTTP/1.1 响应（状态行含 reason phrase、Content-Type、Content-Length、body），连接已关闭返回 -1；以及 `xylem_http_conn_close`
    - _Requirements: 10.1, 10.2, 10.3, 10.4, 10.5, 15.1_
  - [ ] 11.7 在 `src/xylem-http.c` 中实现服务器端 keep-alive 和 Connection: close 处理：响应发送后检查 keep-alive 标志，是则重置解析器和空闲定时器继续读取，否则关闭连接
    - _Requirements: 13.2, 13.3, 13.4, 13.5_
  - [ ] 11.8 在 `src/xylem-http.c` 中实现服务器端 body 大小限制检查（默认 1 MiB）和 100-Continue 自动响应
    - _Requirements: 18.1, 18.2, 18.5, 19.2, 19.3, 20.4_
  - [ ] 11.9 在 `tests/test-http.c` 中添加单元测试：`test_srv_create_null_loop`、`test_srv_create_null_cfg`、`test_default_max_body_server`、`test_idle_timeout_default`
    - _Requirements: 8.9, 18.1, 19.2_
  - [ ]* 11.10 在 `tests/test-http.c` 中添加属性测试：Server response serialization validity
    - **Property 7: Server response serialization validity**
    - 使用 theft 库生成随机状态码、content type、body，验证 `xylem_http_conn_send` 产生的响应包含正确的状态行（含 reason phrase）、Content-Type、Content-Length 和 body
    - **Validates: Requirements 10.1, 15.1, 15.2, 15.3**

- [ ] 12. Checkpoint - 确保所有测试通过
  - 确保所有单元测试和属性测试通过，ask the user if questions arise.

- [ ] 13. 集成测试
  - [ ]* 13.1 在 `tests/test-http.c` 中添加集成测试：在同一进程中启动 HTTP 服务器和客户端，验证 GET/POST 请求-响应周期、keep-alive 多请求复用、Connection: close 语义
    - _Requirements: 5.1, 6.1, 9.1, 10.1, 13.1, 13.2, 13.3, 13.4_

- [ ] 14. Final checkpoint - 确保所有测试通过
  - 确保所有测试通过，ask the user if questions arise.

## Notes

- 标记 `*` 的任务为可选任务，可跳过以加速 MVP 开发
- 属性测试使用 theft 库（通过 CMake FetchContent 集成），每个属性至少运行 100 次迭代
- 每个任务引用具体的 requirements 编号以确保可追溯性
- Checkpoints 确保增量验证，避免问题累积
- 所有 .c 和 .h 文件必须包含 license header
- 遵循 docs/style.md 中的命名规范、文件组织规范和测试规范
