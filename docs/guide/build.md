# 构建与安装

## 依赖

| 依赖 | 必需 | 说明 |
|------|------|------|
| CMake ≥ 3.25 | ✅ | 构建系统 |
| C11 编译器 | ✅ | GCC、Clang 或 MSVC |
| OpenSSL ≥ 3.5 | ❌ | 仅 TLS/DTLS 模块需要 |

核心模块（TCP、UDP、UDS、HTTP、WebSocket、RUDP、串口）无外部依赖。

## 构建

```bash
# 基础构建（不含 TLS）
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 启用 TLS/DTLS
cmake -B build -DCMAKE_BUILD_TYPE=Release -DXYLEM_ENABLE_TLS=ON
cmake --build build

# 启用测试
cmake -B build -DXYLEM_ENABLE_TESTING=ON
cmake --build build
ctest --test-dir build
```

## 构建选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `XYLEM_ENABLE_TLS` | OFF | 启用 TLS/DTLS（需要 OpenSSL 3.5+） |
| `XYLEM_ENABLE_TESTING` | OFF | 构建单元测试 |
| `XYLEM_ENABLE_DYNAMIC_LIBRARY` | OFF | 构建动态库（默认静态库） |
| `XYLEM_ENABLE_ASAN` | OFF | 启用 AddressSanitizer |
| `XYLEM_ENABLE_TSAN` | OFF | 启用 ThreadSanitizer |
| `XYLEM_ENABLE_UBSAN` | OFF | 启用 UndefinedBehaviorSanitizer |
| `XYLEM_ENABLE_COVERAGE` | OFF | 启用代码覆盖率（仅 Unix） |

## 集成到你的项目

### 方式一：CMake 子目录

```cmake
add_subdirectory(vendor/xylem)
target_link_libraries(your_app PRIVATE xylem)
```

### 方式二：安装后链接

```bash
cmake --install build
```

```cmake
find_package(xylem REQUIRED)
target_link_libraries(your_app PRIVATE xylem)
```

### 方式三：直接复制源码

将 `src/` 和 `include/` 目录复制到你的项目中，添加到你的构建系统即可。
