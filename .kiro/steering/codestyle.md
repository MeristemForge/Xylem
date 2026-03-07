---
inclusion: fileMatch
fileMatchPattern: "**/*.{c,h}"
---

# Code Style

## License Header

Every `.c` and `.h` file must start with the project license block:

```c
/** Copyright (c) {YEAR}, {AUTHOR} <{EMAIL}>
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

## Naming

| Category | Pattern | Example |
|----------|---------|---------|
| Public functions | `xylem_<module>_<action>` | `xylem_heap_insert` |
| Types | `xylem_<module>_t` | `xylem_list_t` |
| Node types | `xylem_<module>_node_t` | `xylem_heap_node_t` |
| Function pointer typedefs | `xylem_<module>_<purpose>_fn_t` | `xylem_rbtree_cmp_fn_t` |
| Internal/static helpers | `_<name>` prefix | `_heap_node_swap` |
| Internal types (file-scope) | `_<name>_t` prefix | `_xlist_node_t` |

Action (verb) goes last: `xylem_list_insert`, `xylem_heap_remove`.
Compound actions stay together: `xylem_timer_set_time` (not `xylem_timer_time_set`).

> **Note:** The `_` prefix for file-scope static functions and internal types is technically reserved by C11 (§7.1.3), but is used intentionally here. These symbols are never exported and do not enter the linker symbol table, so conflicts with the implementation are not a practical concern. This convention is consistent with projects like libuv and nginx.
| Source files | `xylem-<module>.c`, `xylem-<module>.h` | `xylem-list.c` |
| Test files | `test-<module>.c` | `test-list.c` |

## Types

- Prefer fixed-width integer types (`int8_t`, `int16_t`, `int32_t`, `int64_t`, `uint8_t`, etc.) over plain `int`/`unsigned`
- Exception: function return values and parameters may use `int` where semantically appropriate (e.g., error codes, comparator results)
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

Order: License → includes → macros → structs → static functions → public functions

Static functions ordered by dependency (no forward declarations).

## Platform Layer

Prefer C11 standard library interfaces (`thrd_sleep`, `timespec_get`, `<threads.h>`, etc.) over platform-specific APIs. Only use the platform layer when C11 has no equivalent (e.g. monotonic clock).

Platform-specific code lives under `src/platform/win/` and `src/platform/unix/`. All platform functions are declared in `src/platform/platform.h` and use the `platform_` prefix.

| Rule | Detail |
|------|--------|
| Prefix | `platform_<module>_<action>` (e.g. `platform_time_monotonic_nsec`) |
| Declaration | All in `src/platform/platform.h` — callers include this single header |
| Implementation | One `.c` per module per platform (`src/platform/win/xylem-<module>.c`, `src/platform/unix/xylem-<module>.c`) |
| Scope | Only the minimal OS-specific logic; everything else stays in `src/xylem-<module>.c` |

Public API functions belong in `src/xylem-<module>.c` and call `platform_*` helpers for OS-dependent parts. Never put public API implementations directly in platform files.

## Project Structure

```
include/xylem/xylem-<module>.h  # Public API
src/xylem-<module>.c            # Implementation
src/platform/win/               # Windows platform code
src/platform/unix/              # Linux/macOS platform code
tests/test-<module>.c           # Unit tests
```

### Adding a Module
1. Create `include/xylem/xylem-<module>.h` with public API
2. Create `src/xylem-<module>.c` with implementation
3. Add to `SRCS` in root `CMakeLists.txt`
4. Include in `include/xylem.h`
5. Create `tests/test-<module>.c`
6. Add `xylem_add_test(<module>)` to `tests/CMakeLists.txt`
7. When adding files to `src/platform/win/` or `src/platform/unix/`, delete `.gitkeep` in that directory if it exists

## Restricted Functions

| Banned | Replacement | Reason |
|--------|-------------|--------|
| `localtime`, `gmtime`, `ctime`, `asctime`, `strtok`, `strerror` | `_r` (POSIX) / `_s` (MSVC) variants | Return static buffer — not thread-safe, successive calls overwrite results |
| `sprintf`, `strcpy`, `strcat` | `snprintf` | No bounds checking — buffer overflow risk |
| `gets` | `fgets` | Removed in C11 — unconditional buffer overflow |
| `atoi`, `atof`, `atol` | `strtol`, `strtod` | No error detection — overflow is undefined behavior |
| `memcpy` with overlapping regions | `memmove` | Overlapping src/dst is undefined behavior |

## Headers

- Use `_Pragma("once")` for header guards

## Formatting

clang-format with LLVM base style:
- 4-space indent, no tabs
- Pointer left-aligned (`int* p`)
- No bin-packing of arguments/parameters
- Aligned consecutive declarations
- No single-line functions

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
- Concise, no filler

### Internal / Static Functions

No Doxygen required. A short `/* ... */` one-liner if the name isn't self-explanatory:

```c
/* Swap two heap nodes and update their positions. */
static inline void _heap_node_swap(...) { ... }
```

### Inline Comments

- `/* ... */` style (C11 compatible)
- Only where code is non-obvious
- Keep short
