# Code Style

## Naming

| Category | Pattern | Example |
|----------|---------|---------|
| Public functions | `xylem_<module>_<action>` | `xylem_heap_insert` |
| Types | `xylem_<module>_t` | `xylem_list_t` |
| Node types | `xylem_<module>_node_t` | `xylem_heap_node_t` |
| Function pointer typedefs | `xylem_<module>_<purpose>_fn_t` | `xylem_rbtree_cmp_fn_t` |
| Internal/static helpers | `_<name>` prefix | `_heap_node_swap` |
| Internal types (file-scope) | `_<name>_t` prefix | `_xlist_node_t` |

> **Note:** The `_` prefix for file-scope static functions and internal types is technically reserved by C11 (§7.1.3), but is used intentionally here. These symbols are never exported and do not enter the linker symbol table, so conflicts with the implementation are not a practical concern. This convention is consistent with projects like libuv and nginx.
| Source files | `xylem-<module>.c`, `xylem-<module>.h` | `xylem-list.c` |
| Test files | `test-<module>.c` | `test-list.c` |

## Types

- Prefer fixed-width integer types (`int8_t`, `int16_t`, `int32_t`, `int64_t`, `uint8_t`, etc.) over plain `int`/`unsigned`
- Exception: function return values and parameters may use `int` where semantically appropriate (e.g., error codes, comparator results)
- Use `size_t` for sizes and counts, `bool` for flags

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
