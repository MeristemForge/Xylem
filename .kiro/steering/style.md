---
inclusion: fileMatch
fileMatchPattern: "**/*.{c,h}"
---

# Code Style

## License Header

Every `.c` and `.h` file must start with the project license block:

```c
/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */
```

## Naming Convention

**Prefix Rule:** Use your project name in lowercase as the namespace prefix. In C identifiers (functions, types, macros), replace hyphens with underscores. In file names, keep hyphens as-is.

| Context | Project `mylib` | Project `hello-lib` |
|---------|----------------|---------------------|
| C identifier prefix | `mylib_` | `hello_lib_` |
| Source file prefix | `mylib-` | `hello-lib-` |

| Category | Pattern | Example (project: `xylem`) |
|----------|---------|---------------------------|
| Public functions | `xylem_<module>_<action>` | `xylem_list_insert` |
| Static functions | `_<module>_<action>` | `_tcp_flush_writes` |
| Static callbacks | `_<module>_<subject>_<event>_cb` | `_tcp_conn_io_cb` |
| Types | `xylem_<module>_t` | `xylem_list_t` |
| Node types | `xylem_<module>_node_t` | `xylem_heap_node_t` |
| Function pointer typedefs | `xylem_<module>_<purpose>_fn_t` | `xylem_rbtree_cmp_fn_t` |
| Internal types (file-scope) | `_<name>_t` | `_node_t` |
| Static variables (file-scope) | `_<name>` | `_echo_loop` |
| Global variables (non-static) | no prefix | `stop_io` |
| Source files | `xylem-<module>.c` | `xylem-list.c` |
| Test files | `test-<module>.c` | `test-list.c` |

### Derived Forms

The `xylem` placeholder is used as-is in file names and CMake identifiers. In C identifiers, hyphens are replaced with underscores:

| Context | `xylem` | `XYLEM` |
|---------|---------|---------|
| File names | `xylem-list.c` | — |
| CMake target | `add_library(xylem ...)` | — |
| CMake options | — | `XYLEM_ENABLE_TESTING` |
| CMake functions | `xylem_apply_sanitizer(...)` | — |
| C function prefix | `xylem_list_insert()` | — |
| C type prefix | `xylem_list_t` | — |
| Include directory | `include/xylem/` | — |
| Umbrella header | `xylem.h` | — |

> When the project name has no hyphens (e.g. `xylem`), all forms are identical — no conversion needed.

> The `_` prefix for file-scope static functions and internal types is technically reserved by C11 (§7.1.3), but is used intentionally here. These symbols are never exported and do not enter the linker symbol table, so conflicts with the implementation are not a practical concern.

### Public Functions

Three logical segments: `xylem`, `<module>`, `<action>`. Module is a single identifier without underscores. Action may be compound (verb + object with `_`), verb first:
- `xylem_list_insert` — module=`list`, action=`insert`
- `xylem_loop_init_timer` — module=`loop`, action=`init_timer`
- `xylem_loop_start_io` — module=`loop`, action=`start_io`
- `xylem_timer_set_time` — module=`timer`, action=`set_time`

### Static Functions

Two logical segments: `<module>`, `<action>`, prefixed with `_`. Same rules as public: module is a single identifier without underscores, action may be compound (verb first):
- `_heap_swap_node` — module=`heap`, action=`swap_node`
- `_tcp_flush_writes` — module=`tcp`, action=`flush_writes`
- `_tcp_setup_conn` — module=`tcp`, action=`setup_conn`

### Static Callbacks

Three logical segments: `<module>`, `<subject>`, `<event>`, with `_cb` suffix. Subject names the resource or concern being monitored (e.g. `conn`, `server`, `reconnect`), not the mechanism (use `reconnect` not `reconnect_timer`). These describe events, not actions, so the verb-first rule does not apply:
- `_tcp_conn_io_cb` — module=`tcp`, subject=`conn`, event=`io`
- `_tcp_conn_connected_cb` — module=`tcp`, subject=`conn`, event=`connected`
- `_tcp_server_io_cb` — module=`tcp`, subject=`server`, event=`io`
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
xylem_list_t list;
xylem_list_init(&list);
/* ... use list ... */
xylem_list_deinit(&list);

/* create/destroy — module owns the memory */
xylem_logger_t* logger = xylem_logger_create(cfg);
/* ... use logger ... */
xylem_logger_destroy(logger);
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
- Header: forward declaration + typedef only (`typedef struct xylem_foo_s xylem_foo_t;`)
- Implementation (.c): full struct definition with fields
- Users allocate via `create()` / module-specific constructors, never `sizeof()`

Intrusive data structures (list, queue, stack, heap, rbtree, etc.) where users embed nodes into their own structs are exempt — their struct bodies must remain in headers.

## File Organization

Order: License -> includes -> macros -> structs -> variables -> static functions -> public functions

Static functions ordered by dependency (no forward declarations).

## Platform Layer

Use C11 standard library interfaces (`thrd_sleep`, `timespec_get`, `<threads.h>`, etc.) instead of platform-specific APIs. Only use the platform layer when C11 has no equivalent (e.g. monotonic clock).

Platform-specific code lives under `src/platform/win/` and `src/platform/unix/`.

| Rule | Detail |
|------|--------|
| No `#ifdef` in `src/` | Source files outside `platform/` must not contain platform conditionals (`#ifdef _WIN32`, `#ifdef __linux__`, etc.). All platform differences go through `platform/` |
| Umbrella header | `src/platform/platform.h` only includes per-module headers — no declarations directly in it |
| Per-module header | `src/platform/platform-<module>.h` declares that module's `platform_*` functions |
| Prefix | `platform_<module>_<action>` (e.g. `platform_time_monotonic_nsec`) |
| Implementation | One `.c` per module per platform (`src/platform/win/platform-<module>.c`, `src/platform/unix/platform-<module>.c`) |
| Scope | Only the minimal OS-specific logic; everything else stays in `src/xylem-<module>.c` |

Public API functions belong in `src/xylem-<module>.c` and call `platform_*` helpers for OS-dependent parts. Never put public API implementations directly in platform files.

## Project Structure (Library)

```
include/xylem/xylem-<module>.h  # Public API
src/xylem-<module>.c            # Implementation
src/platform/win/               # Windows platform code
src/platform/unix/              # Linux/macOS platform code
tests/test-<module>.c           # Unit tests
examples/                       # Example programs
```

### Adding a Module (Library)
1. Create `include/xylem/xylem-<module>.h` with public API
2. Create `src/xylem-<module>.c` with implementation
3. Add to `SRCS` in root `CMakeLists.txt`
4. Include in `include/xylem.h`
5. Create `tests/test-<module>.c`
6. Add `xylem_add_test(<module>)` to `tests/CMakeLists.txt`
7. When adding files to `src/platform/win/` or `src/platform/unix/`, delete `.gitkeep` in that directory if it exists

## Restricted Functions

Applies to all `.c` and `.h` files in the project (including tests).

| Banned | Replacement | Reason |
|--------|-------------|--------|
| `localtime`, `gmtime`, `ctime`, `asctime`, `strtok`, `strerror` | `_r` (POSIX) / `_s` (MSVC) variants | Return static buffer — not thread-safe, successive calls overwrite results |
| `sprintf`, `strcpy`, `strcat` | `snprintf` | No bounds checking — buffer overflow risk |
| `gets` | `fgets` | Removed in C11 — unconditional buffer overflow |
| `atoi`, `atof`, `atol` | `strtol`, `strtod` | No error detection — overflow is undefined behavior |
| `memcpy` with overlapping regions | `memmove` | Overlapping src/dst is undefined behavior |

## Cross-Platform Pitfalls (Reference)

> This section is informational reference, not enforceable rules.

- `long`: 4 bytes on Windows x64, 8 bytes on Linux/macOS x64
- Stack size: Windows 1MB, Linux 8MB
- Uninitialized memory may be zeroed in debug but not release — use ASAN
- Signals: Windows uses SEH, Unix uses POSIX signals
- Paths: `\` on Windows, `/` on Unix

## Headers

- Use `_Pragma("once")` for header guards

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

- `/* ... */` style (C11 compatible)
- `/** ... */` when spanning multiple lines (block format: `/**` and `*/` on own lines)
- Only to explain why, not what — if the comment restates the code, remove it
- No decorative dividers

## Test Convention

Custom `ASSERT(expr)` macro in `tests/assert.h` — prints `file:line` and aborts on failure. Do not use standard `<assert.h>`.

Tests are plain C executables: `main()` calls `static void test_*()` functions. One test file per module: `tests/test-<module>.c`.

```c
#include "assert.h"

static void test_insert(void) {
    /* ... */
    ASSERT(result == 0);
}

int main(void) {
    test_insert();
    return 0;
}
```
