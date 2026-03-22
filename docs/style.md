# Code Style

## License Header

Every `.c` and `.h` file must start with the project license text as a `/** ... */` comment. Read the content from the project root `LICENSE` file.

## Naming Convention

### Prefix Rule

`<project>` is the project name from `CMakeLists.txt`.

| Category | Pattern |
|----------|---------|
| Public functions | `<project>_<module>_<action>` |
| Static functions | `_<module>_<action>` |
| Static callbacks | `_<module>_<subject>_<event>_cb` |
| Types | `<project>_<module>_t` |
| Node types | `<project>_<module>_node_t` |
| Function pointer typedefs | `<project>_<module>_<purpose>_fn_t` |
| Internal types (file-scope) | `_<name>_t` |
| Static variables (file-scope) | `_<name>` |
| Global variables (non-static, in `.c` files) | `snake_case`, no prefix |
| Source files | `<project>-<module>.c` |
| Test files | `test-<module>.c` |
| Include directory | `include/<project>/` |
| Umbrella header | `include/<project>.h` |

> The `_` prefix for file-scope static functions and internal types is technically reserved by C11 (§7.1.3), but is used intentionally here. These symbols are never exported and do not enter the linker symbol table, so conflicts with the implementation are not a practical concern.

### Public Functions

Three segments: `<project>`, `<module>`, `<action>`. Module is a single identifier without underscores. Action may be compound (verb + object with `_`), verb first:
- `<project>_list_insert` — module=`list`, action=`insert`
- `<project>_loop_init_timer` — module=`loop`, action=`init_timer`
- `<project>_timer_set_time` — module=`timer`, action=`set_time`

### Static Functions

Two segments: `<module>`, `<action>`, prefixed with `_`. Same rules as public:
- `_heap_swap_node` — module=`heap`, action=`swap_node`
- `_tcp_flush_writes` — module=`tcp`, action=`flush_writes`

### Static Callbacks

Three segments: `<module>`, `<subject>`, `<event>`, with `_cb` suffix. Subject names the resource being monitored, not the mechanism. These describe events, not actions, so the verb-first rule does not apply:
- `_tcp_conn_io_cb` — module=`tcp`, subject=`conn`, event=`io`
- `_tcp_write_timeout_cb` — module=`tcp`, subject=`write`, event=`timeout`
- `_tcp_reconnect_timeout_cb` — module=`tcp`, subject=`reconnect`, event=`timeout`

## Types

- Use fixed-width integer types (`int8_t`, `uint8_t`, etc.) instead of plain `int`/`unsigned`
- Exception: function return values, loop counters, and parameters may use `int` for error codes, comparator results, and boolean-like flags
- Use `size_t` for sizes and counts, `bool` for flags
- For printf: use `<inttypes.h>` macros (`PRIu64`, `PRId32`, `PRIx32`) instead of `%lu`, `%llu`
  - Example: `printf("value: %" PRIu64 "\n", my_uint64);`
  - Reason: `long` size varies across platforms (Windows 64-bit: 4 bytes, Linux 64-bit: 8 bytes)

## Lifecycle Naming

Memory ownership determines the naming pattern:

| Owner | Create | Destroy | Usage |
|-------|--------|---------|-------|
| Caller (stack/embedded) | `init` | `deinit` | Intrusive types, short-lived objects |
| Module (malloc inside) | `create` | `destroy` | Opaque types, dynamic lifetime |

- `init`/`deinit` — caller provides memory, module only initializes/cleans up fields
- `create`/`destroy` — module allocates and frees the struct internally

Rule: opaque types must use `create`/`destroy` (caller can't `sizeof`). Intrusive types must use `init`/`deinit` (memory belongs to caller).

```c
/* init/deinit — caller owns the memory */
<project>_list_t list;
<project>_list_init(&list);
/* ... use list ... */
<project>_list_deinit(&list);

/* create/destroy — module owns the memory */
<project>_logger_t* logger = <project>_logger_create(cfg);
/* ... use logger ... */
<project>_logger_destroy(logger);
```

### Related Verb Pairs

| Pair | Semantics | When to use |
|------|-----------|-------------|
| `open` / `close` | Access an external resource (file, connection, device) | The resource already exists; you're obtaining a handle to it |
| `alloc` / `free` | Allocate raw memory, minimal or no initialization | Low-level allocators, memory pools |
| `start` / `stop` | Control running state (repeatable) | Timers, workers, polling — can start/stop multiple times within one init/deinit lifecycle |
| `acquire` / `release` | Obtain/return ownership or reference count | Reference-counted resources, shared handles |
| `register` / `unregister` | Add/remove a callback or handler to a system | Event handlers, hooks, observers |
| `attach` / `detach` | Associate/dissociate with a parent object | Mounting a child handle onto a loop or manager |
| `bind` / `unbind` | Associate with an address, port, or resource | Network sockets, device endpoints |
| `flush` | Push buffered data to its destination | Write buffers, log buffers — data is sent out, buffer may refill |
| `drain` | Process/consume all pending items until empty | Work queues, event queues — items are consumed, queue ends up empty |

## Opaque Structs

Non-intrusive types that users interact with only through pointers (handles) must use the opaque pattern:
- Header: forward declaration + typedef only (`typedef struct <project>_foo_s <project>_foo_t;`)
- Implementation (.c): full struct definition with fields
- Users allocate via `create()` / module-specific constructors, never `sizeof()`

Intrusive data structures (list, queue, stack, heap, rbtree, etc.) where users embed nodes into their own structs are exempt — their struct bodies must remain in headers.

## File Organization

Order: License -> includes -> macros -> structs -> variables -> static functions -> public functions

Static functions ordered by dependency (no forward declarations).

## Platform Layer

All platform-specific code lives exclusively under `src/platform/`. Source files outside `platform/` must not contain platform conditionals (`#ifdef _WIN32`, `#ifdef __linux__`, etc.).

Prefer cross-platform APIs wherever possible. Use the platform layer only when no cross-platform equivalent exists.

| Rule | Detail |
|------|--------|
| Umbrella header | `src/platform/platform.h` only includes per-module headers — no declarations directly in it |
| Per-module header | `src/platform/platform-<module>.h` declares that module's `platform_*` functions |
| Prefix | `platform_<module>_<action>` (e.g. `platform_time_monotonic_nsec`) |
| Implementation | One `.c` per module per platform (`src/platform/win/platform-<module>.c`, `src/platform/unix/platform-<module>.c`) |
| Scope | Only the minimal OS-specific logic; everything else stays in `src/<project>-<module>.c` |

Public API functions belong in `src/<project>-<module>.c` and call `platform_*` helpers for OS-dependent parts. Never put public API implementations directly in platform files.

## Adding a Module

### Library
1. If `include/<project>/` does not exist, create it
2. Create `include/<project>/<project>-<module>.h` with public API
3. Create `src/<project>-<module>.c` with implementation
4. Add to `SRCS` in root `CMakeLists.txt`
5. Include in `include/<project>.h`
6. Create `tests/test-<module>.c`
7. Add `<project>_add_test(<module>)` to `tests/CMakeLists.txt`
8. When adding files to `src/platform/win/` or `src/platform/unix/`, delete `.gitkeep` in that directory if it exists

### Executable
1. Create `src/<project>-<module>.c` and `src/<project>-<module>.h`
2. Add to `SRCS` in root `CMakeLists.txt`
3. Create `tests/test-<module>.c`
4. Add `<project>_add_test(<module>)` to `tests/CMakeLists.txt`
5. When adding files to `src/platform/win/` or `src/platform/unix/`, delete `.gitkeep` in that directory if it exists

## Restricted Functions

Applies to all `.c` and `.h` files in the project (including tests).

| Banned | Replacement | Reason |
|--------|-------------|--------|
| `localtime`, `gmtime`, `ctime`, `asctime`, `strtok`, `strerror` | `_r` (POSIX) / `_s` (MSVC) variants | Return static buffer — not thread-safe, successive calls overwrite results |
| `sprintf`, `strcpy`, `strcat` | `snprintf` | No bounds checking — buffer overflow risk |
| `gets` | `fgets` | Removed in C11 — unconditional buffer overflow |
| `atoi`, `atof`, `atol` | `strtol`, `strtod` | No error detection — overflow is undefined behavior |
| `memcpy` with overlapping regions | `memmove` | Overlapping src/dst is undefined behavior |

## Headers

- Use `_Pragma("once")` for header guards

## Test Code

### File Scope

Applies to `tests/test-<module>.c` files and `tests/CMakeLists.txt`.

### Framework

No external test framework. Tests use the custom `ASSERT(expr)` macro from `tests/assert.h`. Do not use standard `<assert.h>` or any third-party test library.

### Test File Structure

One test file per module: `tests/test-<module>.c`. Each file follows this structure:

```c
#include "assert.h"
#include "<project>-<module>.h"  /* or the relevant header */

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
| No test runner | Each test executable is self-contained — no shared `main()` across modules |

### Naming

| Category | Pattern | Example |
|----------|---------|---------|
| Test file | `test-<module>.c` | `test-list.c` |
| Test function | `static void test_<case>(void)` | `test_insert`, `test_remove_empty` |
| Test executable | `test-<module>` (created by CMake) | `test-list` |

Test function names use `snake_case`. The `<case>` part describes the scenario, not the function under test:
- `test_insert` ✓ — tests the insert operation
- `test_insert_duplicate` ✓ — tests inserting a duplicate
- `test_mylib_list_insert` ✗ — do not repeat the module/project prefix

### Registration

Add each test module in `tests/CMakeLists.txt` using the helper function:

```cmake
<project>_add_test(<module>)
```

This creates executable `test-<module>` from `tests/test-<module>.c`, linked against `<project>`, with sanitizers and coverage applied automatically.

Do not create test executables manually with `add_executable` in `tests/CMakeLists.txt`.

### Test Organization

#### One Concern Per Function

Each `test_*` function tests one behavior. Do not combine multiple unrelated assertions in a single function.

```c
/* Correct — one concern */
static void test_insert(void) {
    <project>_list_t list;
    <project>_list_init(&list);
    int32_t rc = <project>_list_insert(&list, 42);
    ASSERT(rc == 0);
    ASSERT(<project>_list_size(&list) == 1);
    <project>_list_deinit(&list);
}

/* Correct — separate concern */
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

### Prohibited Test Patterns

| Pattern | Why | Fix |
|---------|-----|-----|
| `#include <assert.h>` | Standard assert has no file:line output and may be disabled by `NDEBUG` | Use `#include "assert.h"` (project macro) |
| `sleep()` / `thrd_sleep()` in tests | Flaky timing-dependent tests | Use deterministic synchronization or callbacks |
| `printf` as the only validation | Output is not checked automatically | Use `ASSERT()` to validate results |
| Shared mutable file-scope variables | Creates order-dependent tests | Initialize all state inside each `test_*` function |

### Coverage Requirements

| Rule | Detail |
|------|--------|
| Target | Every public function must have at least one test that exercises its success path |
| Error paths | Functions with error returns must have tests that trigger each distinct error condition |
| Platform code | Platform-specific code is tested on its native platform only — do not mock platform functions |

## Formatting

clang-format with LLVM base style (see `.clang-format` for full config). Additional rule beyond the formatter:
- All `if`/`else`/`for`/`while` bodies must use braces, even single statements

## Comments

### Public API (Header Files)

All `extern` function declarations must have a Doxygen `/** ... */` block:

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
- Pure ASCII only — use `->` not `→`, `>=` not `≥`
- Blank comment lines between sections

### Internal / Static Functions

No Doxygen tags required. Use `/** ... */` for multi-line comments, `/* ... */` for single-line:

```c
/**
 * Common setup for a newly connected socket: init ringbuf, start IO,
 * start heartbeat/read timers. Does NOT call any handler callback.
 */
static void _tcp_setup_conn(...) { ... }

/* Swap two heap nodes and update their positions. */
static inline void _heap_swap_node(...) { ... }
```

### Inline Comments

Applies to comments inside function bodies in `.c` files.

- `/* ... */` style (C11 compatible)
- `/** ... */` when spanning multiple lines (block format: `/**` and `*/` on own lines)
- A comment must explain *why* the code does something, not *what* it does. If the comment restates the code, remove it.
- No decorative dividers (lines of `===`, `---`, `***`, etc.)

```c
/* Correct — explains why */
/* Reserve extra byte for null terminator required by snprintf. */
uint8_t buf[len + 1];

/* Incorrect — restates the code */
/* Add 1 to len. */
uint8_t buf[len + 1];
```
