# UDS Coroutine Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite xylem-uds from callback/event-loop API to coroutine API, matching TCP/UDP pattern.

**Architecture:** Replace `loop_t`/`loop_io_t`/`loop_timer_t` with `iowait_t`. Remove handler callbacks, write queue, state machine, userdata, heartbeat, CUSTOM framing. New API uses synchronous coroutine-suspending functions (listen/accept/dial/recv/send/close). UDS-specific: `close_listener` unlinks socket file.

**Tech Stack:** C11, iowait/scheduler coroutine runtime, platform-socket Unix helpers.

**Spec:** `docs/superpowers/specs/2026-05-17-uds-coroutine-design.md`

---

## File Map

| Action | File | Responsibility |
|--------|------|---------------|
| Rewrite | `include/xylem/net/xylem-uds.h` | New coroutine public API |
| Rewrite | `src/net/xylem-uds.c` | New coroutine implementation |
| Rewrite | `tests/test-uds.c` | New coroutine-style tests |
| Modify | `tests/CMakeLists.txt` | Register `xylem_add_test(uds)` |

---

### Task 1: Rewrite Public Header

**Files:**
- Rewrite: `include/xylem/net/xylem-uds.h`

- [ ] **Step 1: Replace the entire header with the new coroutine API**

```c
_Pragma("once")

#include "xylem/xylem-error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct xylem_uds_conn_s     xylem_uds_conn_t;
typedef struct xylem_uds_listener_s xylem_uds_listener_t;

typedef enum xylem_uds_frame_type_e {
    XYLEM_UDS_FRAME_NONE,      /*< Raw mode, recv returns available bytes. */
    XYLEM_UDS_FRAME_FIXED,     /*< Fixed-length frames. */
    XYLEM_UDS_FRAME_LENGTH,    /*< Length-prefixed frames. */
    XYLEM_UDS_FRAME_DELIMITER, /*< Delimiter-terminated frames. */
} xylem_uds_frame_type_t;

typedef enum xylem_uds_length_coding_e {
    XYLEM_UDS_LENGTH_FIXEDINT, /*< Fixed-width integer (1-8 bytes). */
    XYLEM_UDS_LENGTH_VARINT,   /*< Variable-length integer (LEB128). */
} xylem_uds_length_coding_t;

typedef struct xylem_uds_frame_opts_s {
    xylem_uds_frame_type_t type;
    union {
        struct {
            size_t len; /*< Fixed frame length in bytes. */
        } fixed;
        struct {
            uint32_t                  header_size;  /*< Total header size in bytes. */
            uint32_t                  field_offset; /*< Byte offset of the length field. */
            uint32_t                  field_size;   /*< Size of the length field (1-8). */
            int32_t                   adjustment;   /*< Added to decoded length for payload size. */
            xylem_uds_length_coding_t coding;       /*< FIXEDINT or VARINT. */
            bool                      big_endian;   /*< true: big-endian length field. */
        } length;
        struct {
            const char* delim;     /*< Delimiter bytes. */
            size_t      delim_len; /*< Delimiter length, 0 = auto strlen. */
        } delimiter;
    };
} xylem_uds_frame_opts_t;

extern xylem_uds_listener_t* xylem_uds_listen(const char* path);

extern xylem_uds_conn_t* xylem_uds_accept(xylem_uds_listener_t* ln);

extern void xylem_uds_close_listener(xylem_uds_listener_t* ln);

extern xylem_uds_conn_t* xylem_uds_dial(
    const char* path, uint64_t connect_timeout_ms);

extern void xylem_uds_set_framing(
    xylem_uds_conn_t* uds, xylem_uds_frame_opts_t* opts);

extern void xylem_uds_set_read_deadline(
    xylem_uds_conn_t* uds, uint64_t deadline_ms);

extern void xylem_uds_set_write_deadline(
    xylem_uds_conn_t* uds, uint64_t deadline_ms);

extern int64_t xylem_uds_recv(
    xylem_uds_conn_t* uds, void* buf, size_t len);

extern int xylem_uds_send(
    xylem_uds_conn_t* uds, const void* data, size_t len);

extern void xylem_uds_close(xylem_uds_conn_t* uds);

extern xylem_err_t xylem_uds_get_error(xylem_uds_conn_t* uds);

extern int xylem_uds_shutdown_wr(xylem_uds_conn_t* uds);

extern int xylem_uds_shutdown_rd(xylem_uds_conn_t* uds);
```

Add the standard copyright header and doxygen comments following the TCP header style. Each function gets a brief `@brief`, parameter list, and return doc.

- [ ] **Step 2: Verify header compiles**

Run: `cmake --build build --target xylem 2>&1 | head -30`
Expected: may have linker errors (unresolved symbols from old .c), but no syntax errors in the header itself.

- [ ] **Step 3: Commit**

```bash
git add include/xylem/net/xylem-uds.h
git commit -m "feat(uds): rewrite public header for coroutine API"
```

---

### Task 2: Rewrite Implementation

**Files:**
- Rewrite: `src/net/xylem-uds.c`

The new implementation follows `src/net/xylem-tcp.c` structurally. Key differences from TCP:
- No DNS resolution (AF_UNIX paths only)
- No `addr_t` / `peer_addr` (no address queries)
- `close_listener` calls `remove(path)` to unlink the socket file
- LENGTH framing supports varint via `xylem_varint_decode`/`xylem_varint_encode`
- Uses `platform_socket_listen_unix`/`platform_socket_dial_unix`/`platform_socket_accept_unix`

- [ ] **Step 1: Write the internal structs and helpers**

```c
#include "xylem/net/xylem-uds.h"

#include "xylem/encoding/xylem-varint.h"
#include "xylem/xylem-error.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "platform/platform-socket.h"
#include "runtime/iowait.h"
#include "runtime/runtime.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_READ_BUF_SIZE 65536
#define UDS_MAX_PATH          104

struct xylem_uds_conn_s {
    iowait_t*              waiter;
    platform_sock_t        fd;
    xylem_uds_frame_opts_t frame_opts;
    char*                  read_buf;
    size_t                 read_buf_cap;
    size_t                 read_buf_pos;
    size_t                 read_buf_len;
    xylem_err_t            err;
    _Atomic int32_t        refcnt;
    _Atomic bool           closed;
};

struct xylem_uds_listener_s {
    iowait_t*       waiter;
    platform_sock_t fd;
    char            path[UDS_MAX_PATH];
    _Atomic int32_t refcnt;
    _Atomic bool    closing;
};
```

Ref/unref helpers follow TCP pattern exactly:

```c
static void _uds_conn_ref(xylem_uds_conn_t* uds) {
    atomic_fetch_add_explicit(&uds->refcnt, 1, memory_order_relaxed);
}

static void _uds_conn_unref(xylem_uds_conn_t* uds) {
    if (atomic_fetch_sub_explicit(&uds->refcnt, 1, memory_order_acq_rel) != 1)
        return;
    if (uds->waiter)
        iowait_destroy(uds->waiter);
    if (uds->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        shutdown(uds->fd, PLATFORM_SHUT_WR);
        platform_socket_close(uds->fd);
    }
    free(uds->read_buf);
    free(uds);
}

static void _uds_listener_ref(xylem_uds_listener_t* ln) {
    atomic_fetch_add_explicit(&ln->refcnt, 1, memory_order_relaxed);
}

static void _uds_listener_unref(xylem_uds_listener_t* ln) {
    if (atomic_fetch_sub_explicit(&ln->refcnt, 1, memory_order_acq_rel) != 1)
        return;
    if (ln->waiter)
        iowait_destroy(ln->waiter);
    if (ln->fd != PLATFORM_SO_ERROR_INVALID_SOCKET)
        platform_socket_close(ln->fd);
    free(ln);
}
```

Conn alloc helper:

```c
static xylem_uds_conn_t* _uds_conn_alloc(platform_sock_t fd) {
    xylem_uds_conn_t* uds = (xylem_uds_conn_t*)calloc(1, sizeof(*uds));
    if (!uds)
        return NULL;

    uds->fd     = fd;
    uds->waiter = iowait_create(fd);
    if (!uds->waiter) {
        free(uds);
        return NULL;
    }

    uds->read_buf = (char*)malloc(DEFAULT_READ_BUF_SIZE);
    if (!uds->read_buf) {
        iowait_destroy(uds->waiter);
        free(uds);
        return NULL;
    }
    uds->read_buf_cap = DEFAULT_READ_BUF_SIZE;

    _uds_conn_ref(uds);
    return uds;
}
```

- [ ] **Step 2: Write raw recv/send (copy from TCP, remove addr-specific code)**

`_uds_raw_recv` - identical to `_tcp_raw_recv` but operates on `xylem_uds_conn_t*`:

```c
static int64_t _uds_raw_recv(xylem_uds_conn_t* uds, void* buf, size_t len) {
    if (atomic_load_explicit(&uds->closed, memory_order_acquire)) {
        uds->err = XYLEM_ERR_CLOSED;
        return -1;
    }
    for (;;) {
        ssize_t n = platform_socket_recv(uds->fd, buf, (int)len);
        if (n > 0)
            return n;
        if (n == 0) {
            uds->err = XYLEM_ERR_PEER_CLOSED;
            return 0;
        }
        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN &&
            err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            uds->err = XYLEM_ERR_UNKNOWN;
            return -1;
        }
        iowait_result_t r = iowait_read(uds->waiter);
        if (r != IOWAIT_READY ||
            atomic_load_explicit(&uds->closed, memory_order_acquire)) {
            uds->err = (r == IOWAIT_TIMEOUT)
                ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
            return -1;
        }
    }
}
```

`_uds_raw_send` - identical pattern:

```c
static int _uds_raw_send(xylem_uds_conn_t* uds, const void* data, size_t len) {
    if (atomic_load_explicit(&uds->closed, memory_order_acquire)) {
        uds->err = XYLEM_ERR_CLOSED;
        return -1;
    }
    const char* ptr = (const char*)data;
    size_t      rem = len;
    while (rem > 0) {
        ssize_t n = platform_socket_send(uds->fd, ptr, (int)rem);
        if (n > 0) {
            ptr += n;
            rem -= (size_t)n;
            continue;
        }
        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN &&
            err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            uds->err = XYLEM_ERR_UNKNOWN;
            return -1;
        }
        iowait_result_t r = iowait_write(uds->waiter);
        if (r != IOWAIT_READY ||
            atomic_load_explicit(&uds->closed, memory_order_acquire)) {
            uds->err = (r == IOWAIT_TIMEOUT)
                ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
            return -1;
        }
    }
    return 0;
}
```

- [ ] **Step 3: Write buffered read helpers (copy from TCP)**

Copy `_tcp_read_exact` → `_uds_read_exact`, `_tcp_buffered_read` → `_uds_buffered_read`. Same logic, different struct type.

- [ ] **Step 4: Write framing recv functions**

`_uds_recv_fixed` - same as TCP.

`_uds_recv_length` - like TCP but with varint support:

```c
static int64_t _uds_recv_length(xylem_uds_conn_t* uds, void* buf, size_t len) {
    uint8_t  hdr[16];
    uint32_t hdr_sz = uds->frame_opts.length.header_size;
    if (hdr_sz > sizeof(hdr)) {
        uds->err = XYLEM_ERR_UNKNOWN;
        return -1;
    }
    if (_uds_read_exact(uds, hdr, hdr_sz) != 0)
        return -1;

    uint64_t body_len = 0;

    if (uds->frame_opts.length.coding == XYLEM_UDS_LENGTH_VARINT) {
        size_t pos = (size_t)uds->frame_opts.length.field_offset;
        if (!xylem_varint_decode(hdr, hdr_sz, &pos, &body_len)) {
            uds->err = XYLEM_ERR_UNKNOWN;
            return -1;
        }
    } else {
        uint8_t* field = hdr + uds->frame_opts.length.field_offset;
        if (uds->frame_opts.length.big_endian) {
            for (uint32_t i = 0; i < uds->frame_opts.length.field_size; i++)
                body_len = (body_len << 8) | field[i];
        } else {
            for (uint32_t i = 0; i < uds->frame_opts.length.field_size; i++)
                body_len |= (uint64_t)field[i] << (i * 8);
        }
    }

    int64_t adjusted = (int64_t)body_len + uds->frame_opts.length.adjustment;
    if (adjusted < 0) {
        uds->err = XYLEM_ERR_UNKNOWN;
        return -1;
    }
    size_t payload_len = (size_t)adjusted;
    if (payload_len > len) {
        uds->err = XYLEM_ERR_UNKNOWN;
        return -1;
    }
    if (payload_len > 0 && _uds_read_exact(uds, buf, payload_len) != 0)
        return -1;
    return (int64_t)payload_len;
}
```

`_uds_recv_delimiter` - same as TCP.

- [ ] **Step 5: Write framing send function**

`_uds_send_length` - like TCP but with varint support:

```c
static int _uds_send_length(
    xylem_uds_conn_t* uds, const void* data, size_t len) {
    uint8_t  hdr[16];
    uint32_t hdr_sz = uds->frame_opts.length.header_size;
    if (hdr_sz > sizeof(hdr)) {
        uds->err = XYLEM_ERR_UNKNOWN;
        return -1;
    }

    int64_t wire_len = (int64_t)len - uds->frame_opts.length.adjustment;
    if (wire_len < 0) {
        uds->err = XYLEM_ERR_UNKNOWN;
        return -1;
    }

    memset(hdr, 0, hdr_sz);

    if (uds->frame_opts.length.coding == XYLEM_UDS_LENGTH_VARINT) {
        size_t pos = (size_t)uds->frame_opts.length.field_offset;
        if (!xylem_varint_encode((uint64_t)wire_len, hdr, hdr_sz, &pos)) {
            uds->err = XYLEM_ERR_UNKNOWN;
            return -1;
        }
        if (_uds_raw_send(uds, hdr, pos) != 0)
            return -1;
    } else {
        uint8_t* field = hdr + uds->frame_opts.length.field_offset;
        uint64_t val   = (uint64_t)wire_len;
        if (uds->frame_opts.length.big_endian) {
            for (int32_t i = (int32_t)uds->frame_opts.length.field_size - 1;
                 i >= 0; i--) {
                field[i] = (uint8_t)(val & 0xFF);
                val >>= 8;
            }
        } else {
            for (uint32_t i = 0; i < uds->frame_opts.length.field_size; i++) {
                field[i] = (uint8_t)(val & 0xFF);
                val >>= 8;
            }
        }
        if (_uds_raw_send(uds, hdr, hdr_sz) != 0)
            return -1;
    }
    return _uds_raw_send(uds, data, len);
}
```

- [ ] **Step 6: Write public API functions**

`xylem_uds_listen`:

```c
xylem_uds_listener_t* xylem_uds_listen(const char* path) {
    if (!path || strlen(path) >= UDS_MAX_PATH) {
        xylem_loge("uds listen: path is NULL or too long (max %d)",
                   UDS_MAX_PATH - 1);
        return NULL;
    }

    platform_sock_t fd = platform_socket_listen_unix(path, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("uds listen: socket creation failed for %s", path);
        return NULL;
    }

    xylem_uds_listener_t* ln = (xylem_uds_listener_t*)calloc(1, sizeof(*ln));
    if (!ln) {
        platform_socket_close(fd);
        return NULL;
    }
    ln->fd = fd;
    snprintf(ln->path, UDS_MAX_PATH, "%s", path);
    ln->waiter = iowait_create(fd);
    if (!ln->waiter) {
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }
    _uds_listener_ref(ln);
    return ln;
}
```

`xylem_uds_accept` - follows TCP pattern with backoff:

```c
xylem_uds_conn_t* xylem_uds_accept(xylem_uds_listener_t* ln) {
    _uds_listener_ref(ln);
    xylem_uds_conn_t* result = NULL;
    uint64_t backoff_ms = 5;

    for (;;) {
        if (atomic_load_explicit(&ln->closing, memory_order_acquire))
            break;

        platform_sock_t fd = platform_socket_accept_unix(ln->fd, true);
        if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                if (iowait_read(ln->waiter) != IOWAIT_READY)
                    break;
                continue;
            }
            xylem_logw("uds listener fd=%d accept error=%d (%s)",
                       (int)ln->fd, err, platform_socket_tostring(err));
            runtime_sleep(backoff_ms);
            if (backoff_ms < 1000)
                backoff_ms *= 2;
            continue;
        }

        backoff_ms = 5;
        result = _uds_conn_alloc(fd);
        if (!result) {
            platform_socket_close(fd);
            continue;
        }
        break;
    }

    _uds_listener_unref(ln);
    return result;
}
```

`xylem_uds_close_listener` - like TCP but also unlinks socket file:

```c
void xylem_uds_close_listener(xylem_uds_listener_t* ln) {
    if (atomic_exchange(&ln->closing, true))
        return;
    iowait_close(ln->waiter);
    if (ln->path[0] != '\0')
        remove(ln->path);
    _uds_listener_unref(ln);
}
```

`xylem_uds_dial` - simpler than TCP (no DNS, no MSS):

```c
xylem_uds_conn_t* xylem_uds_dial(
    const char* path, uint64_t connect_timeout_ms) {
    if (!path || strlen(path) >= UDS_MAX_PATH) {
        xylem_loge("uds dial: path is NULL or too long (max %d)",
                   UDS_MAX_PATH - 1);
        return NULL;
    }

    bool            connected = false;
    platform_sock_t fd = platform_socket_dial_unix(path, &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("uds dial: socket creation failed for %s", path);
        return NULL;
    }

    xylem_uds_conn_t* uds = _uds_conn_alloc(fd);
    if (!uds) {
        platform_socket_close(fd);
        return NULL;
    }

    if (!connected) {
        if (connect_timeout_ms > 0) {
            uint64_t deadline =
                xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) +
                connect_timeout_ms;
            iowait_set_wr_deadline(uds->waiter, deadline);
        }
        iowait_result_t r = iowait_write(uds->waiter);
        iowait_set_wr_deadline(uds->waiter, 0);

        if (r != IOWAIT_READY) {
            uds->err = XYLEM_ERR_TIMEOUT;
            xylem_uds_close(uds);
            return NULL;
        }

        int32_t   err    = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen);
        if (err != 0) {
            uds->err = XYLEM_ERR_UNKNOWN;
            xylem_loge("uds dial fd=%d connect error=%d (%s)",
                       (int)fd, err, platform_socket_tostring(err));
            xylem_uds_close(uds);
            return NULL;
        }
    }

    return uds;
}
```

Remaining public functions:

```c
void xylem_uds_set_framing(
    xylem_uds_conn_t* uds, xylem_uds_frame_opts_t* opts) {
    if (opts)
        uds->frame_opts = *opts;
    else
        memset(&uds->frame_opts, 0, sizeof(uds->frame_opts));
}

void xylem_uds_set_read_deadline(
    xylem_uds_conn_t* uds, uint64_t deadline_ms) {
    iowait_set_rd_deadline(uds->waiter, deadline_ms);
}

void xylem_uds_set_write_deadline(
    xylem_uds_conn_t* uds, uint64_t deadline_ms) {
    iowait_set_wr_deadline(uds->waiter, deadline_ms);
}

int64_t xylem_uds_recv(xylem_uds_conn_t* uds, void* buf, size_t len) {
    _uds_conn_ref(uds);
    int64_t ret;
    switch (uds->frame_opts.type) {
    case XYLEM_UDS_FRAME_NONE:      ret = _uds_buffered_read(uds, buf, len); break;
    case XYLEM_UDS_FRAME_FIXED:     ret = _uds_recv_fixed(uds, buf, len);    break;
    case XYLEM_UDS_FRAME_LENGTH:    ret = _uds_recv_length(uds, buf, len);   break;
    case XYLEM_UDS_FRAME_DELIMITER: ret = _uds_recv_delimiter(uds, buf, len);break;
    default:                        ret = -1;                                 break;
    }
    _uds_conn_unref(uds);
    return ret;
}

int xylem_uds_send(xylem_uds_conn_t* uds, const void* data, size_t len) {
    _uds_conn_ref(uds);
    int ret;
    switch (uds->frame_opts.type) {
    case XYLEM_UDS_FRAME_LENGTH: ret = _uds_send_length(uds, data, len); break;
    default:                     ret = _uds_raw_send(uds, data, len);    break;
    }
    _uds_conn_unref(uds);
    return ret;
}

void xylem_uds_close(xylem_uds_conn_t* uds) {
    if (atomic_exchange(&uds->closed, true))
        return;
    iowait_close(uds->waiter);
    _uds_conn_unref(uds);
}

xylem_err_t xylem_uds_get_error(xylem_uds_conn_t* uds) {
    return uds->err;
}

int xylem_uds_shutdown_wr(xylem_uds_conn_t* uds) {
    return shutdown(uds->fd, PLATFORM_SHUT_WR) == 0 ? 0 : -1;
}

int xylem_uds_shutdown_rd(xylem_uds_conn_t* uds) {
    return shutdown(uds->fd, PLATFORM_SHUT_RD) == 0 ? 0 : -1;
}
```

- [ ] **Step 7: Build the library**

Run: `cmake --build build --target xylem 2>&1 | tail -5`
Expected: compiles without errors.

- [ ] **Step 8: Commit**

```bash
git add src/net/xylem-uds.c
git commit -m "feat(uds): rewrite implementation for coroutine API"
```

---

### Task 3: Rewrite Tests

**Files:**
- Rewrite: `tests/test-uds.c`
- Modify: `tests/CMakeLists.txt`

Tests follow the TCP test pattern: `xylem_run` → root coroutine spawns server+client coroutines, synchronized via `xylem_channel_t` and `xylem_waitgroup_t`, with a watchdog timer.

- [ ] **Step 1: Register UDS test in CMake**

In `tests/CMakeLists.txt`, add after the `xylem_add_test(udp)` line:

```cmake
xylem_add_test(uds)
```

- [ ] **Step 2: Write test scaffolding and echo test**

```c
#include "xylem.h"
#include "assert.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#define UDS_PATH "xylem-test-uds.sock"
#else
#define UDS_PATH "/tmp/xylem-test-uds.sock"
#endif

#define SAFETY_TIMEOUT_MS 10000

typedef struct {
    xylem_channel_t*   ready;
    xylem_waitgroup_t* wg;
} _ctx_t;

static void _watchdog_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    (void)ud;
    ASSERT(0 && "test timed out");
}

/* ---------- test_echo ---------- */

static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_uds_conn_t* uds = xylem_uds_accept(ln);
    ASSERT(uds != NULL);

    char buf[256];
    int64_t n = xylem_uds_recv(uds, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_uds_send(uds, buf, (size_t)n) == 0);

    xylem_uds_close(uds);
    xylem_uds_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0);
    ASSERT(uds != NULL);

    const char* msg = "hello xylem uds";
    ASSERT(xylem_uds_send(uds, msg, strlen(msg)) == 0);

    char buf[64];
    int64_t n = xylem_uds_recv(uds, buf, sizeof(buf));
    ASSERT(n == (int64_t)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_uds_close(uds);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_echo_server, &ctx);
    xylem_spawn(_echo_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_echo(void) {
    xylem_run(_echo_main, NULL, NULL);
    remove(UDS_PATH);
}
```

- [ ] **Step 3: Write fixed framing test**

```c
static void _fixed_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_uds_conn_t* uds = xylem_uds_accept(ln);
    ASSERT(uds != NULL);

    ASSERT(xylem_uds_send(uds, "ABCD", 4) == 0);
    xylem_sleep(30);
    ASSERT(xylem_uds_send(uds, "EFGH", 4) == 0);

    xylem_uds_close(uds);
    xylem_uds_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _fixed_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0);
    ASSERT(uds != NULL);

    xylem_uds_frame_opts_t frame = {
        .type  = XYLEM_UDS_FRAME_FIXED,
        .fixed = { .len = 8 },
    };
    xylem_uds_set_framing(uds, &frame);

    char buf[16];
    int64_t n = xylem_uds_recv(uds, buf, sizeof(buf));
    ASSERT(n == 8);
    ASSERT(memcmp(buf, "ABCDEFGH", 8) == 0);

    xylem_uds_close(uds);
    xylem_waitgroup_done(ctx->wg);
}

static void _fixed_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_fixed_server, &ctx);
    xylem_spawn(_fixed_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_fixed(void) {
    xylem_run(_fixed_main, NULL, NULL);
    remove(UDS_PATH);
}
```

- [ ] **Step 4: Write delimiter framing test**

```c
static void _delim_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_uds_conn_t* uds = xylem_uds_accept(ln);
    ASSERT(uds != NULL);

    ASSERT(xylem_uds_send(uds, "hello\r\nworld\r\n", 14) == 0);

    xylem_uds_close(uds);
    xylem_uds_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _delim_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0);
    ASSERT(uds != NULL);

    xylem_uds_frame_opts_t frame = {
        .type      = XYLEM_UDS_FRAME_DELIMITER,
        .delimiter = { .delim = "\r\n", .delim_len = 2 },
    };
    xylem_uds_set_framing(uds, &frame);

    char line[64];
    int64_t n = xylem_uds_recv(uds, line, sizeof(line));
    ASSERT(n == 5);
    ASSERT(memcmp(line, "hello", 5) == 0);

    n = xylem_uds_recv(uds, line, sizeof(line));
    ASSERT(n == 5);
    ASSERT(memcmp(line, "world", 5) == 0);

    xylem_uds_close(uds);
    xylem_waitgroup_done(ctx->wg);
}

static void _delim_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_delim_server, &ctx);
    xylem_spawn(_delim_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_delimiter(void) {
    xylem_run(_delim_main, NULL, NULL);
    remove(UDS_PATH);
}
```

- [ ] **Step 5: Write length-prefixed framing test (fixedint)**

```c
static const xylem_uds_frame_opts_t _len_frame = {
    .type   = XYLEM_UDS_FRAME_LENGTH,
    .length = {
        .header_size  = 2,
        .field_offset = 0,
        .field_size   = 2,
        .adjustment   = 0,
        .coding       = XYLEM_UDS_LENGTH_FIXEDINT,
        .big_endian   = true,
    },
};

static void _frame_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_uds_conn_t* uds = xylem_uds_accept(ln);
    ASSERT(uds != NULL);

    xylem_uds_frame_opts_t frame = _len_frame;
    xylem_uds_set_framing(uds, &frame);

    ASSERT(xylem_uds_send(uds, "FRAME1", 6) == 0);

    xylem_uds_close(uds);
    xylem_uds_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _frame_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0);
    ASSERT(uds != NULL);

    xylem_uds_frame_opts_t frame = _len_frame;
    xylem_uds_set_framing(uds, &frame);

    char buf[64];
    int64_t n = xylem_uds_recv(uds, buf, sizeof(buf));
    ASSERT(n == 6);
    ASSERT(memcmp(buf, "FRAME1", 6) == 0);

    xylem_uds_close(uds);
    xylem_waitgroup_done(ctx->wg);
}

static void _frame_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_frame_server, &ctx);
    xylem_spawn(_frame_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_frame(void) {
    xylem_run(_frame_main, NULL, NULL);
    remove(UDS_PATH);
}
```

- [ ] **Step 6: Write varint length framing test**

```c
static void _varint_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_uds_conn_t* uds = xylem_uds_accept(ln);
    ASSERT(uds != NULL);

    xylem_uds_frame_opts_t frame = {
        .type   = XYLEM_UDS_FRAME_LENGTH,
        .length = {
            .header_size  = 1,
            .field_offset = 0,
            .field_size   = 1,
            .adjustment   = 0,
            .coding       = XYLEM_UDS_LENGTH_VARINT,
            .big_endian   = false,
        },
    };
    xylem_uds_set_framing(uds, &frame);

    ASSERT(xylem_uds_send(uds, "VARINTmsg", 9) == 0);

    xylem_uds_close(uds);
    xylem_uds_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _varint_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0);
    ASSERT(uds != NULL);

    xylem_uds_frame_opts_t frame = {
        .type   = XYLEM_UDS_FRAME_LENGTH,
        .length = {
            .header_size  = 1,
            .field_offset = 0,
            .field_size   = 1,
            .adjustment   = 0,
            .coding       = XYLEM_UDS_LENGTH_VARINT,
            .big_endian   = false,
        },
    };
    xylem_uds_set_framing(uds, &frame);

    char buf[64];
    int64_t n = xylem_uds_recv(uds, buf, sizeof(buf));
    ASSERT(n == 9);
    ASSERT(memcmp(buf, "VARINTmsg", 9) == 0);

    xylem_uds_close(uds);
    xylem_waitgroup_done(ctx->wg);
}

static void _varint_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_varint_server, &ctx);
    xylem_spawn(_varint_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_varint(void) {
    xylem_run(_varint_main, NULL, NULL);
    remove(UDS_PATH);
}
```

- [ ] **Step 7: Write main and build**

```c
int main(void) {
    test_echo();
    test_fixed();
    test_delimiter();
    test_frame();
    test_varint();
    return 0;
}
```

Run: `cmake --build build && ctest --test-dir build -R test-uds -V`
Expected: all 5 tests pass.

- [ ] **Step 8: Commit**

```bash
git add tests/test-uds.c tests/CMakeLists.txt
git commit -m "test(uds): rewrite tests for coroutine API"
```

---

### Task 4: Build, Run All Tests, Fix Issues

- [ ] **Step 1: Full rebuild**

Run: `cmake --build build 2>&1 | tail -10`
Expected: clean build, no warnings.

- [ ] **Step 2: Run UDS test**

Run: `ctest --test-dir build -R test-uds -V`
Expected: PASS

- [ ] **Step 3: Run full test suite to check for regressions**

Run: `ctest --test-dir build -V 2>&1 | tail -20`
Expected: all existing tests still pass.

- [ ] **Step 4: Fix any issues found**

If compile errors or test failures, fix and re-run.

- [ ] **Step 5: Final commit if fixes were needed**

```bash
git add -u
git commit -m "fix(uds): address build/test issues from coroutine rewrite"
```
