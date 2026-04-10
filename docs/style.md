# C Project Code Style Guide

The placeholder `<project>` refers to the project name from the root `CMakeLists.txt` (lowercase); `<PROJECT>` is its uppercase form.

## 1. License Header

Every `.c` and `.h` file must start with the project license text as a `/** ... */` comment. Read the content from the project root `LICENSE` file. The opening `/**` sits on the same line as the first word. Each continuation line starts with ` *  ` (space-star-two spaces). The closing ` */` is on its own line.

## 2. Language Standard

- C11 with extensions (`CMAKE_C_STANDARD 11`, `CMAKE_C_EXTENSIONS ON`).
- On Windows MSVC: `/experimental:c11atomics` for `<stdatomic.h>`.
- `CMAKE_EXPORT_COMPILE_COMMANDS ON` for tooling.

## 3. Header Guards

All `.h` files use `_Pragma("once")` immediately after the license block. No `#ifndef`/`#define` guards.

## 4. Extern Keyword

All non-static function declarations in `.h` files must use the `extern` keyword -- both public API headers (`include/`) and internal headers (`src/`).

## 5. Include Ordering

Includes are grouped with blank lines between groups:

1. The module's own public header
2. Other project public headers
3. Internal project headers (platform layer, sibling modules)
4. Third-party bundled headers
5. Standard library headers

Example (implementation file):

```c
#include "<project>/<project>-udp.h"
#include "<project>/<project>-logger.h"

#include "platform/platform-socket.h"
#include "<project>-loop-io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
```

Example (test file):

```c
#include "<project>.h"
#include "assert.h"

#include <string.h>
```


## 6. File Naming Conventions

### 6.1 Directory Layout

```
include/
  <project>.h                                   umbrella header
  <project>/                                    public headers (single-file modules)
    <project>-<module>.h
    <module>/                                   public headers (multi-file modules)
      <project>-<module>-<sub>.h

src/
  <project>.c                                   library init/cleanup
  <project>-<module>.c                          single-file module implementations
  <project>-<name>.h                            internal headers at src root
  <module>/                                     multi-file module directories
    <project>-<module>-<sub>.c                  public implementation files
    <module>-<name>.c                           internal implementation files
    <module>-<name>.h                           internal headers
    <libname>/                                  bundled third-party libraries
  platform/
    platform.h                                  umbrella (includes per-module headers)
    platform-<module>.h                         per-module platform API headers
    unix/platform-<module>.c                    Unix implementations
    win/platform-<module>.c                     Windows implementations

tests/
  assert.h                                      custom ASSERT macro
  CMakeLists.txt                                test registration
  test-<module>.c                               one test file per module

examples/
  <topic>-<pattern>-<role>.c                    example programs
```

### 6.2 Public vs Internal Distinction

The `<project>-` prefix on a file name means it is part of the public API. Files without this prefix are internal to the module.

| Type | Pattern | Example (module=`http`) |
|------|---------|-------------------------|
| Public header | `<project>-<module>.h` or `<project>-<module>-<sub>.h` | `<project>-http-client.h` |
| Public implementation | `<project>-<module>.c` or `<project>-<module>-<sub>.c` | `<project>-http-client.c` |
| Internal header | `<module>-<name>.h` | `http-common.h` |
| Internal implementation | `<module>-<name>.c` | `http-common.c` |
| Third-party bundled | `<libname>/` | `llhttp/` |

When `<sub>` is generic (`client`, `server`, `common`), the full `<project>-<module>-<sub>` form is required. When `<sub>` is globally unique and self-descriptive (e.g. `gzip`, `fec`), the short `<project>-<sub>` form is used.

### 6.3 Platform Files

Headers are shared; implementations are per-platform. One `.c` per module per platform.

| Pattern | Example |
|---------|---------|
| `platform-<module>.h` | `platform-socket.h`, `platform-io.h` |
| `unix/platform-<module>.c` | `unix/platform-socket.c` |
| `win/platform-<module>.c` | `win/platform-socket.c` |

### 6.4 Test and Example Files

- Test: `test-<module>.c`. One test file per module. Multi-file modules have a single test file.
- Example: `<topic>-<pattern>-<role>.c`. E.g. `tcp-echo-client.c`, `http-static-server.c`.

## 7. Symbol Naming Conventions

### 7.1 Summary

| Category | Pattern | Example |
|----------|---------|---------|
| Public functions | `<project>_<module>_<action>` | `<project>_list_insert_head` |
| Static functions | `_<module>_<action>` | `_tcp_flush_writes` |
| Static callbacks | `_<module>_<subject>_<event>_cb` | `_tcp_conn_io_cb` |
| Public types | `<project>_<module>_t` | `<project>_list_t` |
| Node types (intrusive) | `<project>_<module>_node_t` | `<project>_list_node_t` |
| Function pointer typedefs | `<project>_<module>_<purpose>_fn_t` | `<project>_loop_timer_fn_t` |
| Internal types (file-scope) | `_<name>_t` | `_tcp_state_t` |
| Static variables (file-scope) | `_<name>` | `_tls_ex_data_idx` |
| Global variables (non-static) | `snake_case`, no prefix | |
| Enum tags | `<project>_<module>_<name>_e` | `<project>_tcp_framing_type_e` |
| Enum typedefs | `<project>_<module>_<name>_t` | `<project>_tcp_framing_type_t` |
| Enum values | `<PROJECT>_<MODULE>_<NAME>` | `<PROJECT>_TCP_FRAME_NONE` |
| Struct tags | `<project>_<module>_<name>_s` | `<project>_tcp_conn_s` |
| Entry macros (intrusive) | `<project>_<module>_entry` | `<project>_list_entry` |
| Constants (macros) | `UPPER_SNAKE_CASE` | `DEFAULT_READ_BUF_SIZE` |

> The `_` prefix for file-scope static symbols is technically reserved by C11 (7.1.3), but is used intentionally. These symbols are never exported and do not enter the linker symbol table.

### 7.2 Public Functions

Three segments: `<project>`, `<module>`, `<action>`. Module is a single identifier without underscores. Action may be compound (verb + object with `_`), verb first:

- `<project>_list_insert_head` -- module=`list`, action=`insert_head`
- `<project>_loop_create_timer` -- module=`loop`, action=`create_timer`

### 7.3 Static Functions

Two segments: `<module>`, `<action>`, prefixed with `_`:

- `_tcp_flush_writes`, `_tcp_extract_frame`, `_xlist_alloc_node`

### 7.4 Static Callbacks

Three segments: `<module>`, `<subject>`, `<event>`, with `_cb` suffix. Subject names the resource being monitored, not the mechanism. These describe events, not actions, so the verb-first rule does not apply:

- `_tcp_conn_io_cb` -- module=`tcp`, subject=`conn`, event=`io`
- `_tcp_read_timeout_cb` -- module=`tcp`, subject=`read`, event=`timeout`
- `_tcp_reconnect_timeout_cb` -- module=`tcp`, subject=`reconnect`, event=`timeout`

### 7.5 Enums

Tags use `_e` suffix, typedefs use `_t` suffix. Values are `<PROJECT>_<MODULE>_<NAME>` in UPPER_SNAKE_CASE. Internal (file-scope) enums omit the project prefix.

### 7.6 Structs

Public structs use `_s` tag suffix and `_t` typedef suffix. Forward declaration and typedef appear together in the header:

```c
typedef struct <project>_foo_s <project>_foo_t;
```

Internal structs may use named or anonymous tags:

```c
typedef struct _foo_req_s { ... } _foo_req_t;
typedef struct { ... } _bar_node_t;
```

### 7.7 Multi-File Module Functions

Function names use functional sub-categories, not file names. Do not put file-organization names (like `common`) into function names.

| Scope | Pattern | Example |
|-------|---------|---------|
| Public | `<project>_<module>_<subcategory>_<action>` | `<project>_http_url_encode` |
| Internal shared | `<module>_<action>` (no `<project>_` prefix) | `http_url_parse`, `http_header_eq` |
| Static | `_<module>_<subcategory>_<action>` | `_http_cli_abort` |
| Static callback | `_<module>_<subcategory>_<subject>_<event>_cb` | `_http_cli_conn_read_cb` |

Sub-categories are functional groupings (`cli`, `srv`, `url`, `header`), not file groupings (`common`, `client`).


## 8. Types

- Use fixed-width integer types (`int8_t`, `uint8_t`, `int16_t`, `uint16_t`, `int32_t`, `uint32_t`, `int64_t`, `uint64_t`) instead of plain `int`/`unsigned` for struct fields and data manipulation.
- Exception: function return values, loop counters, comparator results, and boolean-like flags may use `int`.
- Use `size_t` for sizes and counts, `bool` for flags.
- For printf: use `<inttypes.h>` macros (`PRIu64`, `PRId32`, `PRIx32`) instead of `%lu`, `%llu`. Use `%zu` for `size_t`, `%zd` for `ssize_t`.
  - Reason: `long` size varies across platforms (Windows 64-bit: 4 bytes, Linux 64-bit: 8 bytes).

## 9. Opaque Structs

Non-intrusive types that users interact with only through pointers (handles) must use the opaque pattern:
- Header: forward declaration + typedef only (`typedef struct <project>_foo_s <project>_foo_t;`)
- Implementation (`.c`): full struct definition with fields
- Users allocate via `create()` / module-specific constructors, never `sizeof()`

Intrusive data structures (list, queue, stack, heap, rbtree, etc.) where users embed nodes into their own structs are exempt -- their struct bodies must remain in headers.

## 10. Lifecycle Naming

Memory ownership determines the naming pattern:

| Owner | Create | Destroy | Usage |
|-------|--------|---------|-------|
| Caller (stack/embedded) | `init` | `deinit` | Intrusive types, short-lived objects |
| Module (malloc inside) | `create` | `destroy` | Opaque types, dynamic lifetime |

- `init`/`deinit` -- caller provides memory, module only initializes/cleans up fields
- `create`/`destroy` -- module allocates and frees the struct internally

Rule: opaque types must use `create`/`destroy` (caller can't `sizeof`). Intrusive types must use `init`/`deinit` (memory belongs to caller).

```c
/* init/deinit -- caller owns the memory */
<project>_list_t list;
<project>_list_init(&list);
/* ... use list ... */
<project>_list_deinit(&list);

/* create/destroy -- module owns the memory */
<project>_logger_t* logger = <project>_logger_create(cfg);
/* ... use logger ... */
<project>_logger_destroy(logger);
```

### Related Verb Pairs

| Pair | Semantics | When to use |
|------|-----------|-------------|
| `open` / `close` | Access an external resource | Files, connections, devices |
| `alloc` / `free` | Raw memory, minimal init | Memory pools, allocators |
| `start` / `stop` | Control running state (repeatable) | Timers, workers, polling |
| `reset` | Restart with new parameters | Timers, state machines |
| `get` / `set` | Accessor pairs | Userdata, configuration |
| `acquire` / `release` | Obtain/return ownership or refcount | Shared handles |
| `register` / `unregister` | Add/remove a callback or handler | Event handlers, hooks |
| `attach` / `detach` | Associate/dissociate with a parent | Mounting child onto manager |
| `bind` / `unbind` | Associate with an address or resource | Sockets, device endpoints |
| `flush` | Push buffered data to destination | Write buffers, log buffers |
| `drain` | Consume all pending items until empty | Work queues, event queues |

## 11. File Organization

Order within a `.c` file:

1. License header
2. Includes (grouped per section 5)
3. Macros / constants (`#define`)
4. Internal type definitions (structs, enums, typedefs)
5. File-scope static variables
6. Static functions (ordered by dependency -- callees before callers, no forward declarations when possible)
7. Public functions

## 12. Platform Layer

All platform-specific code lives exclusively under `src/platform/`. Source files outside `platform/` must not contain platform conditionals (`#ifdef _WIN32`, `#ifdef __linux__`, etc.).

Prefer cross-platform APIs wherever possible. Use the platform layer only when no cross-platform equivalent exists.

| Rule | Detail |
|------|--------|
| Umbrella header | `platform.h` only includes per-module headers -- no declarations directly in it |
| Per-module header | `platform-<module>.h` declares that module's `platform_*` functions |
| Prefix | `platform_<module>_<action>` |
| Implementation | One `.c` per module per platform |
| Scope | Only the minimal OS-specific logic; everything else stays in `src/<project>-<module>.c` |

Public API functions belong in `src/<project>-<module>.c` and call `platform_*` helpers for OS-dependent parts. Never put public API implementations directly in platform files.

## 13. Formatting

clang-format with LLVM base style. Key settings:

```yaml
BasedOnStyle: LLVM
UseTab: Never
IndentWidth: 4
TabWidth: 4
PointerAlignment: Left
BinPackArguments: false
BinPackParameters: false
AlignConsecutiveDeclarations: true
AlignAfterOpenBracket: AlwaysBreak
AllowShortFunctionsOnASingleLine: None
```

Additional rules beyond the formatter:

- All `if`/`else`/`for`/`while` bodies must use braces, even single statements.
- Pointer `*` attaches to the type: `my_type_t* ptr`, not `my_type_t *ptr`.
- Function parameters that don't fit on one line break after the opening parenthesis (one parameter per line).
- Struct field declarations are column-aligned (`AlignConsecutiveDeclarations`).
- Opening brace on the same line as the function signature (K&R style).

## 14. Comments

### 14.1 ASCII Only

All `.c` and `.h` files (including tests) must contain only ASCII characters (bytes 0x00-0x7F). Non-ASCII characters such as em dashes, arrows, or CJK text are forbidden in source code, comments, and string literals. Use ASCII equivalents (`--`, `->`, `>=`). This prevents MSVC C4819 warnings on non-Unicode code pages.

### 14.2 Function Declarations (Header Files)

All `extern` function declarations in `.h` files -- both public API headers (`include/`) and internal headers (`src/`) -- must have a Doxygen `/** ... */` block:

```c
/**
 * @brief One-line summary.
 *
 * Optional details about behavior or algorithm.
 *
 * @param name   Description of the parameter.
 * @param name2  Aligned with other @param entries.
 *
 * @return Return value and error conditions.
 *
 * @note Caller responsibilities, buffer sizing, etc.
 */
```

Rules:
- Use `@brief`, `@param`, `@return`, `@note` tags
- Align `@param` descriptions
- Pure ASCII only
- Blank comment lines between sections

### 14.3 Internal / Static Functions

No Doxygen tags required. Use `/** ... */` for multi-line, `/* ... */` for single-line:

```c
/**
 * Shared setup logic: allocate buffer, start timers.
 * Does NOT call any user callback.
 */
static int _foo_setup(...) { ... }

/* Swap two heap nodes and update their positions. */
static inline void _heap_swap_node(...) { ... }
```

### 14.4 Inline Comments

- `/* ... */` style only (no `//` comments).
- `/** ... */` when spanning multiple lines (block format: `/**` and `*/` on own lines).
- A comment must explain *why* the code does something, not *what* it does. If the comment restates the code, remove it.
- No decorative dividers (lines of `===`, `---`, `***`, etc.).

```c
/* Reserve extra byte for null terminator required by snprintf. */
uint8_t buf[len + 1];
```

### 14.5 Struct Field Comments

Trailing `/**< ... */` for inline Doxygen on struct fields in public headers:

```c
typedef struct <project>_opts_s {
    int      mtu;           /**< MTU size, 0 for default (1400). */
    bool     stream;        /**< true: byte-stream mode. */
    uint64_t timeout_ms;    /**< Timeout in ms, 0 to disable. */
} <project>_opts_t;
```


## 15. Unused Parameters

Silenced with `(void)param;` at the top of the function body:

```c
static void _my_callback(loop_t* loop, timer_t* timer, void* ud) {
    (void)loop;
    (void)timer;
    my_type_t* obj = (my_type_t*)ud;
    /* ... */
}
```

## 16. Memory Management

### 16.1 Allocation

- Use `calloc(1, sizeof(type))` for zero-initialized allocations. Explicitly cast the `void*` return.
- Use `malloc(size)` when zero-initialization is not needed.
- Always check allocation results: `if (!ptr) { return NULL; }` or `return -1`.
- On allocation failure in constructors, free any partially allocated resources before returning NULL:

```c
my_type_t* my_create(int n) {
    my_type_t* obj = (my_type_t*)calloc(1, sizeof(my_type_t));
    if (!obj) {
        return NULL;
    }
    obj->buf = (uint8_t*)calloc((size_t)n, sizeof(uint8_t));
    obj->ptrs = (uint8_t**)calloc((size_t)n, sizeof(uint8_t*));
    if (!obj->buf || !obj->ptrs) {
        free(obj->buf);
        free(obj->ptrs);
        free(obj);
        return NULL;
    }
    return obj;
}
```

### 16.2 NULL-safe Destroy

Destroy functions accept NULL gracefully:

```c
void my_destroy(my_type_t* obj) {
    if (!obj) {
        return;
    }
    free(obj->buf);
    free(obj->ptrs);
    free(obj);
}
```

### 16.3 Idempotent Close

Close functions that may be called multiple times use a boolean flag to prevent re-entry:

```c
void <project>_foo_close(<project>_foo_t* foo) {
    if (foo->closing) {
        return;
    }
    foo->closing = true;
    /* ... cleanup ... */
}
```

### 16.4 Deferred Free

When an object may still be referenced in the current callback chain, defer its `free()` to the next iteration (or equivalent mechanism) to avoid use-after-free.

## 17. Error Handling

### 17.1 Return Conventions

| Return type | Success | Failure |
|-------------|---------|---------|
| `int` | `0` | `-1` |
| Pointer | Non-NULL | `NULL` |
| `ssize_t` | `>= 0` (bytes) | `-1` |

### 17.2 Logging

Use leveled log macros: `<project>_logd` (debug), `<project>_logi` (info), `<project>_logw` (warning), `<project>_loge` (error). Log messages include relevant context identifiers.

## 18. Restricted Functions

Applies to all `.c` and `.h` files in the project (including tests).

| Banned | Replacement | Reason |
|--------|-------------|--------|
| `localtime`, `gmtime`, `ctime`, `asctime`, `strtok`, `strerror` | `_r` (POSIX) / `_s` (MSVC) variants via platform layer | Return static buffer -- not thread-safe, successive calls overwrite results |
| `sprintf`, `strcpy`, `strcat` | `snprintf` | No bounds checking -- buffer overflow risk |
| `gets` | `fgets` | Removed in C11 -- unconditional buffer overflow |
| `atoi`, `atof`, `atol` | `strtol`, `strtod` | No error detection -- overflow is undefined behavior |
| `memcpy` with overlapping regions | `memmove` | Overlapping src/dst is undefined behavior |

## 19. Intrusive Data Structures

Intrusive types expose their node struct in the header so users can embed them:

```c
struct <project>_list_node_s {
    struct <project>_list_node_s* prev;
    struct <project>_list_node_s* next;
};
```

Container recovery uses the `_entry` macro pattern (pointer arithmetic via `offsetof`):

```c
#define <project>_list_entry(x, t, m) ((t*)((char*)(x) - offsetof(t, m)))
```

Non-intrusive wrappers (prefixed with `x` in this project) internally allocate nodes that embed the intrusive node plus a `void* data` pointer.

## 20. C11 Generic Selection

Type-generic macros use `_Generic` for compile-time dispatch:

```c
#define <project>_bswap(x)                    \
    _Generic((x), uint16_t                    \
             : <project>_bswap_u16, uint32_t  \
             : <project>_bswap_u32, uint64_t  \
             : <project>_bswap_u64, int16_t   \
             : <project>_bswap_i16, int32_t   \
             : <project>_bswap_i32, int64_t   \
             : <project>_bswap_i64, float     \
             : <project>_bswap_f32, double    \
             : <project>_bswap_f64)(x)
```

## 21. Test Code

### 21.1 Framework

No external test framework. Tests use the custom `ASSERT(expr)` macro from `tests/assert.h`. Do not use standard `<assert.h>` or any third-party test library.

### 21.2 Test File Structure

One test file per module: `tests/test-<module>.c`. Each file follows this structure:

```c
#include "<project>.h"
#include "assert.h"

static void test_<case>(void) {
    /* setup */
    /* action */
    /* assert */
    ASSERT(result == expected);
}

int main(void) {
    test_<case>();
    return 0;
}
```

| Rule | Detail |
|------|--------|
| Entry point | `main()` calls `static void test_*()` functions sequentially |
| Return value | `main()` returns `0` on success; `ASSERT` calls `abort()` on failure |
| No test runner | Each test executable is self-contained -- no shared `main()` across modules |

### 21.3 Test Naming

Test function names use `snake_case`. The `<case>` part describes the scenario, not the function under test:

- `test_insert` -- tests the insert operation
- `test_insert_duplicate` -- tests inserting a duplicate
- `test_remove_empty` -- tests removing from an empty container

Do not repeat the module/project prefix in test function names.

### 21.4 Test Registration

Add each test module in `tests/CMakeLists.txt` using the helper function:

```cmake
<project>_add_test(<module>)
```

This creates executable `test-<module>` from `tests/test-<module>.c`, linked against `<project>`, with sanitizers and coverage applied automatically.

Do not create test executables manually with `add_executable` in `tests/CMakeLists.txt`.

### 21.5 Test Organization

#### One Concern Per Function

Each `test_*` function tests one behavior. Do not combine multiple unrelated assertions in a single function.

```c
/* Correct -- one concern */
static void test_insert(void) {
    <project>_list_t list;
    <project>_list_init(&list);
    int32_t rc = <project>_list_insert(&list, 42);
    ASSERT(rc == 0);
    ASSERT(<project>_list_size(&list) == 1);
    <project>_list_deinit(&list);
}

/* Correct -- separate concern */
static void test_insert_duplicate(void) {
    <project>_list_t list;
    <project>_list_init(&list);
    <project>_list_insert(&list, 42);
    int32_t rc = <project>_list_insert(&list, 42);
    ASSERT(rc == -1);
    <project>_list_deinit(&list);
}
```

#### Cleanup on Every Path

Every `test_*` function must call `deinit`/`destroy`/`close` for all resources it creates, even if the test is expected to pass. This ensures sanitizers (ASAN) report accurate leak information.

#### No Global State

Test functions must not depend on execution order. Each function sets up its own state from scratch. Do not use file-scope variables to share state between test functions.

#### Designated Initializers

Use C99/C11 designated initializers for struct setup:

```c
test_item_t a = {.value = 1};
my_handler_t handler = {.on_read = my_read_cb};
my_ctx_t ctx = {0};
```

### 21.6 Prohibited Test Patterns

| Pattern | Why | Fix |
|---------|-----|-----|
| `#include <assert.h>` | Standard assert has no file:line output and may be disabled by `NDEBUG` | Use `#include "assert.h"` (project macro) |
| `sleep()` / `thrd_sleep()` in tests | Flaky timing-dependent tests | Use deterministic synchronization or callbacks |
| `printf` as the only validation | Output is not checked automatically | Use `ASSERT()` to validate results |
| Shared mutable file-scope variables | Creates order-dependent tests | Initialize all state inside each `test_*` function |

### 21.7 Coverage Requirements

| Rule | Detail |
|------|--------|
| Target | Every public function must have at least one test that exercises its success path |
| Error paths | Functions with error returns must have tests that trigger each distinct error condition |
| Platform code | Platform-specific code is tested on its native platform only -- do not mock platform functions |

## 22. Build System

### 22.1 CMake Structure

- Root `CMakeLists.txt`: library target, `SRCS` list, platform conditionals, optional feature flags.
- `tests/CMakeLists.txt`: `<project>_add_test(<module>)`.
- `cmake/<project>-utils.cmake`: `<project>_apply_sanitizer` and `<project>_add_test` helpers.
- `examples/CMakeLists.txt`: example executables.

### 22.2 Options

| Option | Default | Description |
|--------|---------|-------------|
| `<PROJECT>_ENABLE_TESTING` | OFF | Enable unit tests |
| `<PROJECT>_ENABLE_ASAN` | OFF | Address sanitizer |
| `<PROJECT>_ENABLE_TSAN` | OFF | Thread sanitizer |
| `<PROJECT>_ENABLE_UBSAN` | OFF | Undefined behavior sanitizer |
| `<PROJECT>_ENABLE_DYNAMIC_LIBRARY` | OFF | Build shared library |
| `<PROJECT>_ENABLE_COVERAGE` | OFF | Code coverage |

ASAN and TSAN cannot be enabled simultaneously. Run them in separate builds.

### 22.3 Adding a Single-File Module

1. If `include/<project>/` does not exist, create it
2. Create `include/<project>/<project>-<module>.h` with public API
3. Create `src/<project>-<module>.c` with implementation
4. Add to `SRCS` in root `CMakeLists.txt`
5. Include in `include/<project>.h`
6. Create `tests/test-<module>.c`
7. Add `<project>_add_test(<module>)` to `tests/CMakeLists.txt`
8. When adding files to `src/platform/win/` or `src/platform/unix/`, delete `.gitkeep` in that directory if it exists

### 22.4 Adding a Multi-File Module

1. If `include/<project>/` does not exist, create it
2. Create `include/<project>/<module>/` directory for public headers
3. Create public headers under `include/<project>/<module>/` (see naming rules in section 6)
4. Create `src/<module>/` directory for implementation files
5. Create implementation `.c` files in `src/<module>/` (see naming rules in section 6)
6. Third-party libraries bundled with the module go in `src/<module>/<libname>/`
7. Add all `.c` files to `SRCS` in root `CMakeLists.txt`; add `src/<module>/<libname>/` to `include_directories` if needed
8. Include all public headers in `include/<project>.h`
9. Create `tests/test-<module>.c`
10. Add `<project>_add_test(<module>)` to `tests/CMakeLists.txt`
11. When adding files to `src/platform/win/` or `src/platform/unix/`, delete `.gitkeep` in that directory if it exists

### 22.5 Adding an Executable Module

1. Create `src/<project>-<module>.c` and `src/<project>-<module>.h`
2. Add to `SRCS` in root `CMakeLists.txt`
3. Create `tests/test-<module>.c`
4. Add `<project>_add_test(<module>)` to `tests/CMakeLists.txt`
5. When adding files to `src/platform/win/` or `src/platform/unix/`, delete `.gitkeep` in that directory if it exists

## 23. Third-Party Code

Bundled third-party libraries live under `src/<module>/<libname>/`. They are excluded from code coverage reports. The project's coding style rules do not apply to third-party files.
