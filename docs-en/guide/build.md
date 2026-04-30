# Build & Install

## Dependencies

| Dependency | Required | Notes |
|-----------|----------|-------|
| CMake ≥ 3.25 | ✅ | Build system |
| C11 compiler | ✅ | GCC, Clang, or MSVC |
| OpenSSL ≥ 3.5 | ❌ | Only for TLS/DTLS modules |

Core modules (TCP, UDP, UDS, HTTP, WebSocket, RUDP, Serial) have no external dependencies.

## Build

```bash
# Basic build (no TLS)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# With TLS/DTLS
cmake -B build -DCMAKE_BUILD_TYPE=Release -DXYLEM_ENABLE_TLS=ON
cmake --build build

# With tests
cmake -B build -DXYLEM_ENABLE_TESTING=ON
cmake --build build
ctest --test-dir build
```

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `XYLEM_ENABLE_TLS` | OFF | Enable TLS/DTLS (requires OpenSSL 3.5+) |
| `XYLEM_ENABLE_TESTING` | OFF | Build unit tests |
| `XYLEM_ENABLE_DYNAMIC_LIBRARY` | OFF | Build shared library (default: static) |
| `XYLEM_ENABLE_ASAN` | OFF | Enable AddressSanitizer |
| `XYLEM_ENABLE_TSAN` | OFF | Enable ThreadSanitizer |
| `XYLEM_ENABLE_UBSAN` | OFF | Enable UndefinedBehaviorSanitizer |
| `XYLEM_ENABLE_COVERAGE` | OFF | Enable code coverage (Unix only) |

## Integration

### Option 1: CMake subdirectory

```cmake
add_subdirectory(vendor/xylem)
target_link_libraries(your_app PRIVATE xylem)
```

### Option 2: Install and link

```bash
cmake --install build
```

```cmake
find_package(xylem REQUIRED)
target_link_libraries(your_app PRIVATE xylem)
```

### Option 3: Copy source files

Copy `src/` and `include/` into your project and add them to your build system.
