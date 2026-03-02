# Xylem

Xylem is a pure C utility library that supplements (not replaces) the C11 standard. It provides cross-platform, reusable data structures and algorithms as a static or shared library.

## Current Modules

### Data Structures

| Module | Description | Style |
|--------|-------------|-------|
| **list** | Doubly-linked list | Intrusive |
| **stack** | LIFO stack | Intrusive |
| **queue** | FIFO queue (built on list) | Intrusive |
| **heap** | Binary min-heap | Intrusive |
| **rbtree** | Red-black tree (key & node-node comparators) | Intrusive |
| **ringbuf** | Ring buffer | Contiguous |

### Encoding & Hashing

| Module | Description |
|--------|-------------|
| **base64** | Standard and URL-safe Base64 (RFC 4648) |
| **bswap** | Byte-swap utilities |
| **varint** | Variable-length integer encoding |
| **sha1** | SHA-1 hash |
| **sha256** | SHA-256 hash |

### Concurrency

| Module | Description | Status |
|--------|-------------|--------|
| **waitgroup** | Thread synchronization primitive | ✅ |
| **thrdpool** | Thread pool | ✅ |

## Design Philosophy

- Zero external dependencies beyond C11 stdlib
- Intrusive data structures — embed `xylem_<module>_node_t`, recover container via `xylem_*_entry()`
- Error codes (typically `-1`) over exceptions or global state
- Cross-platform: Windows (MSVC) + Unix (GCC/Clang)
