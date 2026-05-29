# WebSocket permessage-deflate Extension

## Overview

Add RFC 7692 permessage-deflate support to Xylem's WebSocket module, using the
already-bundled miniz library. The extension compresses message payloads with
DEFLATE, typically reducing text traffic by 60-80%.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Compression library | bundled miniz | Already used by HTTP gzip; zero new deps |
| Default state | Disabled | Opt-in via `opts.permessage_deflate = true` |
| Context takeover | Configurable, default off | `no_context_takeover` is memory-safe; takeover available for users who want better ratio |
| Window bits | Fixed 15 (max) | miniz uses 32KB window internally; negotiating smaller is pointless complexity |
| Unsupported params | Reject extension | If peer demands `*_max_window_bits < 15` or unknown params, fall back to uncompressed |

## Public API Changes

### `xylem_ws_opts_t` (in `xylem-ws.h`)

```c
typedef struct {
    size_t   max_msg_size;
    size_t   fragment_threshold;
    uint64_t handshake_timeout_ms;
    uint64_t close_timeout_ms;
    bool     permessage_deflate;        /* Enable compression negotiation. */
    bool     deflate_context_takeover;  /* Keep deflate context across messages. */
} xylem_ws_opts_t;
```

No new public functions. Compression is transparent once negotiated.

## Internal Architecture

### New Files

- `src/net/ws/ws-deflate.h` — internal header
- `src/net/ws/ws-deflate.c` — implementation

### Data Structures

```c
typedef struct {
    mz_stream deflate_stream;       /* Send-side compressor. */
    mz_stream inflate_stream;       /* Receive-side decompressor. */
    bool      active;               /* Extension negotiated successfully. */
    bool      no_context_takeover;  /* Reset streams after each message. */
} ws_deflate_ctx_t;
```

Embedded in `xylem_ws_conn_s`. When `active == false`, zero overhead on the
data path (no branches in hot loops — the compress/decompress calls are gated
at the entry points in send/recv).

### Lifecycle

| Event | Action |
|-------|--------|
| `ws_conn_create` | If opts request deflate, zero-init the ctx (streams not yet inited) |
| Handshake success | `ws_deflate_init()` — `mz_deflateInit2` + `mz_inflateInit2` with raw deflate (windowBits = -15) |
| Handshake fail/no extension | ctx.active remains false, no streams allocated |
| Each send (if active) | Compress, optionally reset deflate stream |
| Each recv (if active + RSV1) | Decompress, optionally reset inflate stream |
| `ws_conn_free` | `ws_deflate_cleanup()` — `mz_deflateEnd` + `mz_inflateEnd` |

## Protocol Details (RFC 7692)

### Handshake — Client

Add to the Upgrade request:
```
Sec-WebSocket-Extensions: permessage-deflate; client_no_context_takeover; server_no_context_takeover
```

When `deflate_context_takeover == true`, omit the `*_no_context_takeover`
parameters (request context takeover).

### Handshake — Server

Parse incoming `Sec-WebSocket-Extensions`. If `permessage-deflate` is present
and all parameters are acceptable:
- Echo back the agreed parameters in the 101 response.
- Reject (omit from response) if any parameter is unsupported.

Acceptable parameters:
- `server_no_context_takeover` — always accept
- `client_no_context_takeover` — always accept
- `server_max_window_bits=15` — accept (matches our fixed size)
- `client_max_window_bits=15` or bare `client_max_window_bits` — accept

Rejected (causes extension to not be negotiated):
- `server_max_window_bits` < 15
- `client_max_window_bits` < 15 (explicit value)
- Any unknown parameter

### Compression (Send)

1. Feed message payload to `mz_deflate(&stream, MZ_SYNC_FLUSH)`.
2. Strip the trailing 4-byte tail `0x00 0x00 0xFF 0xFF` from output.
3. Set RSV1 bit on the first frame of the message.
4. If `no_context_takeover`: call `mz_deflateReset(&stream)`.

### Decompression (Receive)

1. Detect RSV1=1 on first frame of a message.
2. After reassembling all fragments, append `0x00 0x00 0xFF 0xFF`.
3. Feed to `mz_inflate(&stream, MZ_SYNC_FLUSH)`.
4. If `no_context_takeover`: call `mz_inflateReset(&stream)`.

### Fragmentation Interaction

- RSV1 is set only on the **first fragment** of a compressed message.
- Continuation frames (opcode 0x0) do NOT set RSV1.
- Decompression happens on the **reassembled** message, not per-fragment.

## Code Changes to Existing Files

| File | Change |
|------|--------|
| `xylem-ws.h` | Add two bool fields to `xylem_ws_opts_t` |
| `ws.h` | Add `ws_deflate_ctx_t deflate_ctx` to `xylem_ws_conn_s` |
| `ws.c` — `ws_conn_create` | Zero-init deflate_ctx |
| `ws.c` — `ws_conn_free` | Call `ws_deflate_cleanup()` |
| `ws.c` — `xylem_ws_send` | Before `_ws_write_frame`, if deflate active: compress and set RSV1 |
| `ws.c` — `xylem_ws_recv` | After reassembly, if RSV1 was set: decompress before delivering |
| `ws-handshake.h` | Add `ws_handshake_build_request_ext` or extend existing with extensions param |
| `ws-handshake.c` | Emit/parse `Sec-WebSocket-Extensions` header |
| `ws.c` — `ws_accept_impl` | Parse extension offer, negotiate, init deflate ctx |
| `ws.c` — `ws_dial_impl` | Send extension offer, parse response, init deflate ctx |
| `CMakeLists.txt` | Add `ws-deflate.c` to sources |

## Testing Strategy

1. **Unit tests for ws-deflate.c**: compress → decompress roundtrip, verify tail stripping/appending.
2. **Unit tests for extension negotiation**: various parameter combinations.
3. **Integration tests**: client (with deflate) ↔ server (with deflate), verify messages decoded correctly.
4. **Interop**: test against a browser or `websocat --deflate` to validate real-world compatibility.
5. **Edge cases**: empty messages, messages smaller than compression overhead, context takeover across multiple messages.

## Non-Goals

- `server_max_window_bits` / `client_max_window_bits` negotiation (fixed at 15)
- Multiple extension offers in a single header (take first valid one)
- Per-frame compression (not part of RFC 7692 for permessage-deflate)
