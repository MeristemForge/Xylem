# UDS Coroutine Refactor Design

## Goal

Rewrite `xylem-uds` from callback/event-loop style to coroutine style,
matching the pattern established by TCP and UDP.

## Public API

### Types

```c
typedef struct xylem_uds_conn_s     xylem_uds_conn_t;
typedef struct xylem_uds_listener_s xylem_uds_listener_t;

typedef enum xylem_uds_frame_type_e {
    XYLEM_UDS_FRAME_NONE,
    XYLEM_UDS_FRAME_FIXED,
    XYLEM_UDS_FRAME_LENGTH,
    XYLEM_UDS_FRAME_DELIMITER,
} xylem_uds_frame_type_t;

typedef enum xylem_uds_length_coding_e {
    XYLEM_UDS_LENGTH_FIXEDINT,
    XYLEM_UDS_LENGTH_VARINT,
} xylem_uds_length_coding_t;

typedef struct xylem_uds_frame_opts_s {
    xylem_uds_frame_type_t type;
    union {
        struct {
            size_t len;
        } fixed;
        struct {
            uint32_t                  header_size;
            uint32_t                  field_offset;
            uint32_t                  field_size;
            int32_t                   adjustment;
            xylem_uds_length_coding_t coding;
            bool                      big_endian;
        } length;
        struct {
            const char* delim;
            size_t      delim_len;
        } delimiter;
    };
} xylem_uds_frame_opts_t;
```

### Functions

| Function | Signature |
|----------|-----------|
| listen | `xylem_uds_listener_t* xylem_uds_listen(const char* path)` |
| accept | `xylem_uds_conn_t* xylem_uds_accept(xylem_uds_listener_t* ln)` |
| dial | `xylem_uds_conn_t* xylem_uds_dial(const char* path, int64_t timeout_ms)` |
| recv | `ssize_t xylem_uds_recv(xylem_uds_conn_t* uds, void* buf, size_t len)` |
| send | `int xylem_uds_send(xylem_uds_conn_t* uds, const void* data, size_t len)` |
| close | `void xylem_uds_close(xylem_uds_conn_t* uds)` |
| close listener | `void xylem_uds_close_listener(xylem_uds_listener_t* ln)` |
| set framing | `void xylem_uds_set_framing(xylem_uds_conn_t* uds, xylem_uds_frame_opts_t* opts)` |
| set read deadline | `void xylem_uds_set_read_deadline(xylem_uds_conn_t* uds, int64_t ms)` |
| set write deadline | `void xylem_uds_set_write_deadline(xylem_uds_conn_t* uds, int64_t ms)` |
| get error | `xylem_err_t xylem_uds_get_error(xylem_uds_conn_t* uds)` |
| shutdown wr | `int xylem_uds_shutdown_wr(xylem_uds_conn_t* uds)` |
| shutdown rd | `int xylem_uds_shutdown_rd(xylem_uds_conn_t* uds)` |

### Semantics

- **recv**: suspends calling coroutine until data arrives. Returns bytes read,
  0 on orderly shutdown, -1 on error. With framing enabled, returns exactly
  one complete frame.
- **send**: suspends until all bytes are written. Returns 0 on success, -1 on
  error. With framing, automatically prepends length header or appends
  delimiter.
- **accept**: suspends until a client connects. Returns NULL if listener was
  closed (get_error returns XYLEM_ERR_CLOSED).
- **dial**: suspends until connected or timeout. Returns NULL on failure.
  timeout_ms <= 0 means no timeout.
- **close**: synchronous. Sets closed flag, wakes any parked coroutines
  (they see IOWAIT_CLOSED). Idempotent.
- **close_listener**: closes the listener fd, unlinks the socket file from
  the filesystem, wakes any blocked accept calls.
- **deadlines**: absolute monotonic clock values. recv/send return -1 with
  XYLEM_ERR_TIMEOUT when deadline expires. 0 clears the deadline.

## Removed Features

Removed from the old callback API (aligned with TCP/UDP coroutine pattern):

- `xylem_uds_handler_t` callback struct
- `xylem_uds_opts_t` options struct (timeouts via deadline API, framing via set_framing)
- `xylem_uds_conn_ref/unref` (refcount internal only)
- `xylem_uds_get/set_userdata` (coroutine stack replaces userdata)
- `XYLEM_UDS_FRAME_CUSTOM` framing type
- heartbeat detection (`on_heartbeat_miss`)
- `xylem_uds_timeout_type_t` enum
- address query functions (UDS paths are user-supplied, no system-assigned addresses)

## Internal Structure

```c
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
    char            path[104];  /* sun_path max */
    _Atomic bool    closed;
};
```

## UDS-Specific Behavior

- `close_listener` calls `unlink(path)` to remove the socket file.
- No DNS resolution, no MSS, no reconnection.
- Uses `AF_UNIX` / `SOCK_STREAM` sockets.

## Varint in LENGTH Framing

UDS retains the `xylem_uds_length_coding_t` enum with FIXEDINT and VARINT
options. TCP will also get varint support added to its `length` sub-struct
(separate task).

## Implementation Approach

Follow the TCP implementation pattern in `src/net/xylem-tcp.c`:

1. Replace `loop_io_t` with `iowait_t` for all I/O waiting.
2. Replace `loop_timer_t` with iowait deadline API.
3. Remove write queue; send blocks until complete.
4. Remove state machine; use atomic `closed` flag.
5. Remove all callback invocations.
6. Internal refcount for safe concurrent close.
