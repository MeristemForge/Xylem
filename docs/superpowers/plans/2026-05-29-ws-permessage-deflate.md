# WebSocket permessage-deflate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add RFC 7692 permessage-deflate compression to Xylem's WebSocket module using the bundled miniz library.

**Architecture:** New `ws-deflate.c` module handles compress/decompress via miniz's zlib-style API (raw deflate, windowBits=-15). Handshake extension parsing is added to `ws-handshake.c`. The send/recv paths in `ws.c` gain deflate/inflate calls gated on `deflate_ctx.active`. Two new bool fields in `xylem_ws_opts_t` control opt-in.

**Tech Stack:** C11, miniz (bundled at `src/encoding/gzip/miniz/`), CMake/Ninja build

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `include/xylem/net/xylem-ws.h` | Modify | Add `permessage_deflate` and `deflate_context_takeover` to opts |
| `src/net/ws/ws-deflate.h` | Create | Internal deflate context type + function declarations |
| `src/net/ws/ws-deflate.c` | Create | Init/cleanup/compress/decompress + extension string parse/build |
| `src/net/ws/ws.h` | Modify | Embed `ws_deflate_ctx_t` in `xylem_ws_conn_s` |
| `src/net/ws/ws.c` | Modify | Wire deflate into send/recv + accept/dial |
| `src/net/ws/ws-handshake.h` | Modify | Add extension-aware handshake builders |
| `src/net/ws/ws-handshake.c` | Modify | Emit/parse `Sec-WebSocket-Extensions` header |
| `CMakeLists.txt` | Modify | Add `ws-deflate.c` to sources |
| `tests/test-ws.c` | Modify | Add deflate integration tests |

---

### Task 1: Public API — Add opts fields

**Files:**
- Modify: `include/xylem/net/xylem-ws.h:43-48`

- [ ] **Step 1: Add the two new fields to `xylem_ws_opts_t`**

```c
typedef struct {
    size_t   max_msg_size;
    size_t   fragment_threshold;
    uint64_t handshake_timeout_ms;
    uint64_t close_timeout_ms;
    bool     permessage_deflate;
    bool     deflate_context_takeover;
} xylem_ws_opts_t;
```

Add `#include <stdbool.h>` if not already present (it is not — `xylem-ws.h` only has `stddef.h` and `stdint.h`).

- [ ] **Step 2: Verify build still compiles**

Run: `cmake --build build --config Debug 2>&1 | head -20`
Expected: Clean compile (new fields are zero-initialized by existing callsites using `= {0}` or partial init).

- [ ] **Step 3: Commit**

```bash
git add include/xylem/net/xylem-ws.h
git commit -m "feat(ws): add permessage_deflate opts fields to public API"
```

---

### Task 2: Internal deflate module — header and core functions

**Files:**
- Create: `src/net/ws/ws-deflate.h`
- Create: `src/net/ws/ws-deflate.c`
- Modify: `CMakeLists.txt:137-142`

- [ ] **Step 1: Create `ws-deflate.h`**

```c
_Pragma("once")

#include "encoding/gzip/miniz/miniz.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    mz_stream deflate_stream;
    mz_stream inflate_stream;
    bool      active;
    bool      no_context_takeover;
} ws_deflate_ctx_t;

/**
 * @brief Initialize deflate/inflate streams.
 *
 * @param ctx                    Deflate context (must be zeroed).
 * @param no_context_takeover    Reset streams after each message.
 *
 * @return 0 on success, -1 on failure.
 */
extern int ws_deflate_init(ws_deflate_ctx_t* ctx, bool no_context_takeover);

/**
 * @brief Release deflate/inflate stream resources.
 *
 * @param ctx  Deflate context. Safe to call on inactive ctx.
 */
extern void ws_deflate_cleanup(ws_deflate_ctx_t* ctx);

/**
 * @brief Compress a message payload for sending.
 *
 * Applies DEFLATE with MZ_SYNC_FLUSH and strips the trailing
 * 4-byte tail (0x00 0x00 0xFF 0xFF). Resets the deflate stream
 * if no_context_takeover is set.
 *
 * @param ctx       Active deflate context.
 * @param in        Input payload.
 * @param in_len    Input length.
 * @param out       Output: malloc'd compressed buffer (caller frees).
 * @param out_len   Output: compressed length.
 *
 * @return 0 on success, -1 on failure.
 */
extern int ws_deflate_compress(ws_deflate_ctx_t* ctx,
                               const void* in, size_t in_len,
                               void** out, size_t* out_len);

/**
 * @brief Decompress a received message payload.
 *
 * Appends the 4-byte sync tail and inflates. Resets the inflate
 * stream if no_context_takeover is set.
 *
 * @param ctx       Active deflate context.
 * @param in        Compressed input (without trailing 4 bytes).
 * @param in_len    Input length.
 * @param out       Output: malloc'd decompressed buffer (caller frees).
 * @param out_len   Output: decompressed length.
 * @param max_size  Maximum allowed output size (0 = no limit).
 *
 * @return 0 on success, -1 on failure.
 */
extern int ws_deflate_decompress(ws_deflate_ctx_t* ctx,
                                 const void* in, size_t in_len,
                                 void** out, size_t* out_len,
                                 size_t max_size);
```

- [ ] **Step 2: Create `ws-deflate.c`**

```c
#include "ws-deflate.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t _deflate_tail[4] = {0x00, 0x00, 0xFF, 0xFF};

int ws_deflate_init(ws_deflate_ctx_t* ctx, bool no_context_takeover) {
    if (!ctx) {
        return -1;
    }

    memset(&ctx->deflate_stream, 0, sizeof(ctx->deflate_stream));
    memset(&ctx->inflate_stream, 0, sizeof(ctx->inflate_stream));

    int rc = mz_deflateInit2(&ctx->deflate_stream, MZ_DEFAULT_COMPRESSION,
                             MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS,
                             8, MZ_DEFAULT_STRATEGY);
    if (rc != MZ_OK) {
        return -1;
    }

    rc = mz_inflateInit2(&ctx->inflate_stream, -MZ_DEFAULT_WINDOW_BITS);
    if (rc != MZ_OK) {
        mz_deflateEnd(&ctx->deflate_stream);
        return -1;
    }

    ctx->active = true;
    ctx->no_context_takeover = no_context_takeover;
    return 0;
}

void ws_deflate_cleanup(ws_deflate_ctx_t* ctx) {
    if (!ctx || !ctx->active) {
        return;
    }
    mz_deflateEnd(&ctx->deflate_stream);
    mz_inflateEnd(&ctx->inflate_stream);
    ctx->active = false;
}

int ws_deflate_compress(ws_deflate_ctx_t* ctx,
                        const void* in, size_t in_len,
                        void** out, size_t* out_len) {
    if (!ctx || !ctx->active || !out || !out_len) {
        return -1;
    }

    size_t bound = mz_deflateBound(&ctx->deflate_stream, (mz_ulong)in_len);
    uint8_t* buf = (uint8_t*)malloc(bound);
    if (!buf) {
        return -1;
    }

    ctx->deflate_stream.next_in = (const unsigned char*)in;
    ctx->deflate_stream.avail_in = (mz_uint32)in_len;
    ctx->deflate_stream.next_out = buf;
    ctx->deflate_stream.avail_out = (mz_uint32)bound;

    int rc = mz_deflate(&ctx->deflate_stream, MZ_SYNC_FLUSH);
    if (rc != MZ_OK) {
        free(buf);
        return -1;
    }

    size_t produced = bound - ctx->deflate_stream.avail_out;

    /* Strip trailing 0x00 0x00 0xFF 0xFF per RFC 7692 section 7.2.1 */
    if (produced >= 4 &&
        memcmp(buf + produced - 4, _deflate_tail, 4) == 0) {
        produced -= 4;
    }

    *out = buf;
    *out_len = produced;

    if (ctx->no_context_takeover) {
        mz_deflateReset(&ctx->deflate_stream);
    }

    return 0;
}

int ws_deflate_decompress(ws_deflate_ctx_t* ctx,
                          const void* in, size_t in_len,
                          void** out, size_t* out_len,
                          size_t max_size) {
    if (!ctx || !ctx->active || !out || !out_len) {
        return -1;
    }

    /* Append the sync tail that was stripped by the sender */
    size_t total_in = in_len + 4;
    uint8_t* input = (uint8_t*)malloc(total_in);
    if (!input) {
        return -1;
    }
    memcpy(input, in, in_len);
    memcpy(input + in_len, _deflate_tail, 4);

    size_t buf_cap = in_len * 4;
    if (buf_cap < 256) {
        buf_cap = 256;
    }
    uint8_t* buf = (uint8_t*)malloc(buf_cap);
    if (!buf) {
        free(input);
        return -1;
    }

    ctx->inflate_stream.next_in = input;
    ctx->inflate_stream.avail_in = (mz_uint32)total_in;
    ctx->inflate_stream.next_out = buf;
    ctx->inflate_stream.avail_out = (mz_uint32)buf_cap;

    size_t total_out = 0;

    for (;;) {
        int rc = mz_inflate(&ctx->inflate_stream, MZ_SYNC_FLUSH);
        total_out = buf_cap - ctx->inflate_stream.avail_out;

        if (max_size && total_out > max_size) {
            free(buf);
            free(input);
            return -1;
        }

        if (rc == MZ_STREAM_END || ctx->inflate_stream.avail_in == 0) {
            break;
        }

        if (rc != MZ_OK && rc != MZ_BUF_ERROR) {
            free(buf);
            free(input);
            return -1;
        }

        if (ctx->inflate_stream.avail_out == 0) {
            size_t new_cap = buf_cap * 2;
            if (max_size && new_cap > max_size) {
                new_cap = max_size + 1;
            }
            uint8_t* nb = (uint8_t*)realloc(buf, new_cap);
            if (!nb) {
                free(buf);
                free(input);
                return -1;
            }
            buf = nb;
            ctx->inflate_stream.next_out = buf + total_out;
            ctx->inflate_stream.avail_out = (mz_uint32)(new_cap - total_out);
            buf_cap = new_cap;
        }
    }

    free(input);

    *out = buf;
    *out_len = total_out;

    if (ctx->no_context_takeover) {
        mz_inflateReset(&ctx->inflate_stream);
    }

    return 0;
}
```

- [ ] **Step 3: Add `ws-deflate.c` to CMakeLists.txt**

In `CMakeLists.txt`, after line 141 (`src/net/ws/ws-handshake.c`), add:
```
	src/net/ws/ws-deflate.c
```

- [ ] **Step 4: Verify build compiles**

Run: `cmake --build build --config Debug 2>&1 | head -20`
Expected: Clean compile.

- [ ] **Step 5: Commit**

```bash
git add src/net/ws/ws-deflate.h src/net/ws/ws-deflate.c CMakeLists.txt
git commit -m "feat(ws): add ws-deflate module with compress/decompress"
```

---

### Task 3: Extension negotiation — parse and build

**Files:**
- Modify: `src/net/ws/ws-deflate.h`
- Modify: `src/net/ws/ws-deflate.c`

- [ ] **Step 1: Add extension negotiation declarations to `ws-deflate.h`**

Append to `ws-deflate.h`:

```c
typedef struct {
    bool offered;
    bool server_no_context_takeover;
    bool client_no_context_takeover;
} ws_deflate_offer_t;

/**
 * @brief Parse a Sec-WebSocket-Extensions header value for permessage-deflate.
 *
 * @param header  The header value string (may contain multiple extensions).
 * @param offer   Output: parsed offer parameters.
 *
 * @return 0 if permessage-deflate found and acceptable, -1 otherwise.
 */
extern int ws_deflate_parse_offer(const char* header, ws_deflate_offer_t* offer);

/**
 * @brief Build the client Sec-WebSocket-Extensions header value.
 *
 * @param context_takeover  If true, omit no_context_takeover params.
 * @param out               Output buffer.
 * @param out_size          Buffer size.
 *
 * @return Number of bytes written (excluding null), or -1 on error.
 */
extern int ws_deflate_build_client_offer(bool context_takeover,
                                         char* out, size_t out_size);

/**
 * @brief Build the server Sec-WebSocket-Extensions response value.
 *
 * @param offer   The parsed client offer.
 * @param out     Output buffer.
 * @param out_size Buffer size.
 *
 * @return Number of bytes written (excluding null), or -1 on error.
 */
extern int ws_deflate_build_server_accept(const ws_deflate_offer_t* offer,
                                          char* out, size_t out_size);
```

- [ ] **Step 2: Implement extension negotiation in `ws-deflate.c`**

Append to `ws-deflate.c`:

```c
int ws_deflate_build_client_offer(bool context_takeover,
                                  char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return -1;
    }

    int len;
    if (context_takeover) {
        len = snprintf(out, out_size, "permessage-deflate");
    } else {
        len = snprintf(out, out_size,
                       "permessage-deflate"
                       "; client_no_context_takeover"
                       "; server_no_context_takeover");
    }

    if (len < 0 || (size_t)len >= out_size) {
        return -1;
    }
    return len;
}

static const char* _skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

int ws_deflate_parse_offer(const char* header, ws_deflate_offer_t* offer) {
    if (!header || !offer) {
        return -1;
    }
    memset(offer, 0, sizeof(*offer));

    const char* ext = strstr(header, "permessage-deflate");
    if (!ext) {
        return -1;
    }

    offer->offered = true;

    /* Parse parameters after "permessage-deflate" */
    const char* p = ext + strlen("permessage-deflate");
    p = _skip_ws(p);

    while (*p == ';') {
        p++;
        p = _skip_ws(p);

        if (strncmp(p, "server_no_context_takeover", 25) == 0) {
            offer->server_no_context_takeover = true;
            p += 25;
        } else if (strncmp(p, "client_no_context_takeover", 25) == 0) {
            offer->client_no_context_takeover = true;
            p += 25;
        } else if (strncmp(p, "server_max_window_bits", 22) == 0) {
            p += 22;
            p = _skip_ws(p);
            if (*p == '=') {
                p++;
                p = _skip_ws(p);
                int val = (int)strtol(p, NULL, 10);
                if (val != 15) {
                    return -1; /* Reject: we only support 15 */
                }
                while (*p >= '0' && *p <= '9') p++;
            }
            /* Bare parameter (no value) = accept, means 15 */
        } else if (strncmp(p, "client_max_window_bits", 22) == 0) {
            p += 22;
            p = _skip_ws(p);
            if (*p == '=') {
                p++;
                p = _skip_ws(p);
                int val = (int)strtol(p, NULL, 10);
                if (val != 15) {
                    return -1; /* Reject */
                }
                while (*p >= '0' && *p <= '9') p++;
            }
            /* Bare parameter = accept */
        } else {
            return -1; /* Unknown parameter → reject */
        }
        p = _skip_ws(p);
    }

    return 0;
}

int ws_deflate_build_server_accept(const ws_deflate_offer_t* offer,
                                   char* out, size_t out_size) {
    if (!offer || !out || out_size == 0) {
        return -1;
    }

    char params[256] = "";
    size_t plen = 0;

    if (offer->server_no_context_takeover) {
        plen += (size_t)snprintf(params + plen, sizeof(params) - plen,
                                 "; server_no_context_takeover");
    }
    if (offer->client_no_context_takeover) {
        plen += (size_t)snprintf(params + plen, sizeof(params) - plen,
                                 "; client_no_context_takeover");
    }

    int len = snprintf(out, out_size, "permessage-deflate%s", params);
    if (len < 0 || (size_t)len >= out_size) {
        return -1;
    }
    return len;
}
```

Add `#include <stdio.h>` to the top of `ws-deflate.c` (for `snprintf` and `strtol`).

- [ ] **Step 3: Verify build**

Run: `cmake --build build --config Debug 2>&1 | head -20`
Expected: Clean compile.

- [ ] **Step 4: Commit**

```bash
git add src/net/ws/ws-deflate.h src/net/ws/ws-deflate.c
git commit -m "feat(ws): add permessage-deflate extension negotiation"
```

---

### Task 4: Wire deflate into connection struct

**Files:**
- Modify: `src/net/ws/ws.h:24-56`
- Modify: `src/net/ws/ws.c:32-75` (ws_conn_create, ws_conn_free)

- [ ] **Step 1: Add deflate_ctx to `ws.h`**

Add include at top of `ws.h`:
```c
#include "ws-deflate.h"
```

Add field to `xylem_ws_conn_s` struct (after `void* userdata;`):
```c
    ws_deflate_ctx_t deflate_ctx;
    bool             deflate_requested;
    bool             deflate_context_takeover;
```

- [ ] **Step 2: Update `ws_conn_create` in `ws.c`**

After the `_ws_opts_apply(conn, opts);` call, add:
```c
    if (opts && opts->permessage_deflate) {
        conn->deflate_requested = true;
        conn->deflate_context_takeover = opts->deflate_context_takeover;
    }
```

- [ ] **Step 3: Update `ws_conn_free` in `ws.c`**

Before `free(conn->recv_buf);` add:
```c
    ws_deflate_cleanup(&conn->deflate_ctx);
```

Add `#include "ws-deflate.h"` to `ws.c` includes if not already there (it will be via `ws.h`).

- [ ] **Step 4: Verify build**

Run: `cmake --build build --config Debug 2>&1 | head -20`
Expected: Clean compile.

- [ ] **Step 5: Commit**

```bash
git add src/net/ws/ws.h src/net/ws/ws.c
git commit -m "feat(ws): embed deflate context in connection struct"
```

---

### Task 5: Wire deflate into client handshake (dial)

**Files:**
- Modify: `src/net/ws/ws-handshake.h`
- Modify: `src/net/ws/ws-handshake.c`
- Modify: `src/net/ws/ws.c` (ws_dial_impl)

- [ ] **Step 1: Add extension-aware request builder to `ws-handshake.h`**

Add declaration:
```c
/**
 * @brief Build a client HTTP Upgrade request with optional extensions.
 *
 * @param host        Target hostname.
 * @param port        Target port.
 * @param path        Request path.
 * @param key         Base64-encoded Sec-WebSocket-Key.
 * @param extensions  Sec-WebSocket-Extensions value, or NULL.
 * @param out_len     Receives output length. May be NULL.
 *
 * @return Heap-allocated request string, or NULL.
 */
extern char* ws_handshake_build_request_ext(const char* host, uint16_t port,
                                            const char* path, const char* key,
                                            const char* extensions,
                                            size_t* out_len);
```

- [ ] **Step 2: Implement in `ws-handshake.c`**

```c
char* ws_handshake_build_request_ext(const char* host, uint16_t port,
                                     const char* path, const char* key,
                                     const char* extensions,
                                     size_t* out_len) {
    if (host == NULL || path == NULL || key == NULL) {
        return NULL;
    }

    size_t host_len = strlen(host);
    size_t path_len = strlen(path);
    size_t key_len  = strlen(key);
    size_t ext_len  = extensions ? strlen(extensions) : 0;

    size_t buf_size = path_len + host_len + key_len + ext_len + 512;
    char*  buf      = (char*)malloc(buf_size);
    if (buf == NULL) {
        return NULL;
    }

    const char* ext_hdr = "";
    char ext_line[512] = "";
    if (extensions && ext_len > 0) {
        snprintf(ext_line, sizeof(ext_line),
                 "Sec-WebSocket-Extensions: %s\r\n", extensions);
        ext_hdr = ext_line;
    }

    int len;
    if ((port == 80) || (port == 443)) {
        len = snprintf(buf, buf_size,
                       "GET %s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Key: %s\r\n"
                       "Sec-WebSocket-Version: 13\r\n"
                       "%s"
                       "\r\n",
                       path, host, key, ext_hdr);
    } else {
        len = snprintf(buf, buf_size,
                       "GET %s HTTP/1.1\r\n"
                       "Host: %s:%" PRIu16 "\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Key: %s\r\n"
                       "Sec-WebSocket-Version: 13\r\n"
                       "%s"
                       "\r\n",
                       path, host, port, key, ext_hdr);
    }

    if (len < 0 || (size_t)len >= buf_size) {
        free(buf);
        return NULL;
    }

    if (out_len != NULL) {
        *out_len = (size_t)len;
    }
    return buf;
}
```

- [ ] **Step 3: Update `ws_dial_impl` in `ws.c` to use extensions**

Replace the `ws_handshake_build_request` call with extension-aware version. In `ws_dial_impl`, after generating the key:

```c
    char ext_buf[256] = "";
    const char* ext_param = NULL;
    if (opts && opts->permessage_deflate) {
        ws_deflate_build_client_offer(
            opts->deflate_context_takeover, ext_buf, sizeof(ext_buf));
        ext_param = ext_buf;
    }

    size_t req_len;
    char* req = ws_handshake_build_request_ext(host, port, path, key,
                                               ext_param, &req_len);
```

After validating the server handshake response (after `ws_handshake_validate_accept`), parse extension acceptance:

```c
    /* Check if server accepted permessage-deflate */
    bool deflate_active = false;
    bool no_ctx_takeover = true;
    if (opts && opts->permessage_deflate) {
        const char* ext_resp = strstr(resp_buf, "Sec-WebSocket-Extensions:");
        if (!ext_resp) {
            ext_resp = strstr(resp_buf, "sec-websocket-extensions:");
        }
        if (ext_resp) {
            ext_resp += strlen("Sec-WebSocket-Extensions:");
            const char* ext_end = strstr(ext_resp, "\r\n");
            if (ext_end) {
                char ext_val[512];
                size_t elen = (size_t)(ext_end - ext_resp);
                if (elen < sizeof(ext_val)) {
                    memcpy(ext_val, ext_resp, elen);
                    ext_val[elen] = '\0';
                    ws_deflate_offer_t offer;
                    if (ws_deflate_parse_offer(ext_val, &offer) == 0) {
                        deflate_active = true;
                        no_ctx_takeover = !opts->deflate_context_takeover ||
                                          offer.server_no_context_takeover;
                    }
                }
            }
        }
    }
```

After creating the conn (`ws_conn_create`), activate deflate:

```c
    if (deflate_active) {
        if (ws_deflate_init(&conn->deflate_ctx, no_ctx_takeover) != 0) {
            /* Non-fatal: fall back to uncompressed */
            conn->deflate_ctx.active = false;
        }
    }
```

- [ ] **Step 4: Verify build**

Run: `cmake --build build --config Debug 2>&1 | head -20`
Expected: Clean compile.

- [ ] **Step 5: Commit**

```bash
git add src/net/ws/ws-handshake.h src/net/ws/ws-handshake.c src/net/ws/ws.c
git commit -m "feat(ws): wire deflate extension into client handshake"
```

---

### Task 6: Wire deflate into server handshake (accept)

**Files:**
- Modify: `src/net/ws/ws.c` (ws_accept_impl)

- [ ] **Step 1: Update `ws_accept_impl` to negotiate deflate**

After validating `ws_key`/`ws_ver` and before calling `xylem_http_res_upgrade`, add extension negotiation:

```c
    /* Negotiate permessage-deflate */
    bool deflate_active = false;
    bool no_ctx_takeover = true;
    ws_deflate_offer_t deflate_offer = {0};

    if (opts && opts->permessage_deflate) {
        const char* ext_hdr = xylem_http_req_header(req, "Sec-WebSocket-Extensions");
        if (ext_hdr && ws_deflate_parse_offer(ext_hdr, &deflate_offer) == 0) {
            deflate_active = true;
            no_ctx_takeover = !opts->deflate_context_takeover ||
                              deflate_offer.server_no_context_takeover ||
                              deflate_offer.client_no_context_takeover;

            /* Force our preference into the offer for the response */
            if (!opts->deflate_context_takeover) {
                deflate_offer.server_no_context_takeover = true;
                deflate_offer.client_no_context_takeover = true;
            }

            char ext_resp[256];
            if (ws_deflate_build_server_accept(&deflate_offer, ext_resp,
                                               sizeof(ext_resp)) > 0) {
                xylem_http_res_set_header(res, "Sec-WebSocket-Extensions", ext_resp);
            }
        }
    }
```

After creating the conn (`ws_conn_create`), activate deflate:

```c
    if (deflate_active) {
        if (ws_deflate_init(&conn->deflate_ctx, no_ctx_takeover) != 0) {
            conn->deflate_ctx.active = false;
        }
    }
```

- [ ] **Step 2: Verify build**

Run: `cmake --build build --config Debug 2>&1 | head -20`
Expected: Clean compile.

- [ ] **Step 3: Commit**

```bash
git add src/net/ws/ws.c
git commit -m "feat(ws): wire deflate extension into server handshake"
```

---

### Task 7: Wire deflate into send path

**Files:**
- Modify: `src/net/ws/ws.c` (xylem_ws_send)

- [ ] **Step 1: Add compression before frame write in `xylem_ws_send`**

In `xylem_ws_send`, after the validation checks and before the fragmentation logic, add:

```c
    const uint8_t* send_data = p;
    size_t send_len = len;
    void* compressed = NULL;
    bool rsv1 = false;

    if (conn->deflate_ctx.active) {
        if (ws_deflate_compress(&conn->deflate_ctx, p, len,
                                &compressed, &send_len) == 0) {
            send_data = (const uint8_t*)compressed;
            rsv1 = true;
        } else {
            /* Compression failed, send uncompressed */
            send_data = p;
            send_len = len;
        }
    }
```

Update the frame writing to use `send_data`/`send_len` instead of `p`/`len`, and pass `rsv1` to set the RSV1 bit.

Modify `_ws_write_frame` to accept an `rsv1` parameter:

```c
static int _ws_write_frame(xylem_ws_conn_t* conn, bool fin, uint8_t opcode,
                           const void* data, size_t len, bool rsv1) {
```

In the header encoding line, OR in RSV1:
```c
    size_t hdr_len = ws_frame_encode_header(hdr_buf, fin,
                                            rsv1 ? (opcode | 0x40) : opcode,
                                            conn->is_client, mask_key, len);
```

Wait — RSV1 is bit 6 of byte 0 (0x40), but `ws_frame_encode_header` constructs byte 0 as `(fin ? 0x80 : 0) | (opcode & 0x0F)`. We need to pass RSV1 separately. Simpler approach: add RSV1 to the opcode byte before passing to encode, since encode only masks `& 0x0F`:

Actually the cleanest change is to update `_ws_write_frame` signature and set RSV1 directly in the header buffer after encode:

```c
static int _ws_write_frame(xylem_ws_conn_t* conn, bool fin, uint8_t opcode,
                           const void* data, size_t len, bool rsv1) {
    uint8_t hdr_buf[14];
    uint8_t mask_key[4] = {0};

    if (conn->is_client) {
        uint32_t r = (uint32_t)rand();
        memcpy(mask_key, &r, 4);
    }

    size_t hdr_len = ws_frame_encode_header(hdr_buf, fin, opcode,
                                            conn->is_client, mask_key, len);
    if (rsv1) {
        hdr_buf[0] |= 0x40;
    }
    /* ... rest unchanged ... */
```

Update all existing callers of `_ws_write_frame` to pass `false` for rsv1.

The send function becomes:

```c
int xylem_ws_send(xylem_ws_conn_t* conn, xylem_ws_opcode_t opcode,
                  const void* data, size_t len) {
    if (!conn || conn->close_sent || conn->close_received) {
        return -1;
    }
    if (opcode != XYLEM_WS_TEXT && opcode != XYLEM_WS_BINARY) {
        return -1;
    }

    const uint8_t* p = (const uint8_t*)data;
    const uint8_t* send_data = p;
    size_t send_len = len;
    void* compressed = NULL;
    bool rsv1 = false;

    if (conn->deflate_ctx.active) {
        if (ws_deflate_compress(&conn->deflate_ctx, p, len,
                                &compressed, &send_len) == 0) {
            send_data = (const uint8_t*)compressed;
            rsv1 = true;
        } else {
            send_data = p;
            send_len = len;
        }
    }

    size_t threshold = conn->fragment_threshold;
    int result;

    if (send_len <= threshold) {
        result = _ws_write_frame(conn, true, (uint8_t)opcode,
                                 send_data, send_len, rsv1);
    } else {
        size_t offset = 0;
        bool first = true;
        result = 0;
        while (offset < send_len) {
            size_t chunk = send_len - offset;
            if (chunk > threshold) {
                chunk = threshold;
            }
            bool fin_f = (offset + chunk >= send_len);
            uint8_t op = first ? (uint8_t)opcode : 0x0;
            bool frame_rsv1 = first ? rsv1 : false;

            if (_ws_write_frame(conn, fin_f, op,
                                send_data + offset, chunk, frame_rsv1) != 0) {
                result = -1;
                break;
            }
            offset += chunk;
            first = false;
        }
    }

    free(compressed);
    return result;
}
```

- [ ] **Step 2: Update all other callers of `_ws_write_frame`**

Search for all calls to `_ws_write_frame` in `ws.c` and add `, false` as the last argument:
- `xylem_ws_ping`: `_ws_write_frame(conn, true, 0x9, data, len, false);`
- `_ws_send_close_frame`: `_ws_write_frame(conn, true, 0x8, payload, (size_t)plen, false);`
- Auto-pong in recv: `_ws_write_frame(conn, true, 0xA, payload, (size_t)fh.payload_len, false);`

- [ ] **Step 3: Verify build**

Run: `cmake --build build --config Debug 2>&1 | head -20`
Expected: Clean compile.

- [ ] **Step 4: Commit**

```bash
git add src/net/ws/ws.c
git commit -m "feat(ws): compress outgoing messages when deflate active"
```

---

### Task 8: Wire deflate into receive path

**Files:**
- Modify: `src/net/ws/ws.c` (xylem_ws_recv)

- [ ] **Step 1: Track RSV1 bit and decompress after reassembly**

In `xylem_ws_recv`, the frame decode section needs to track whether RSV1 was set on the first frame. Add to the frame header parsing logic:

When a new data frame arrives (opcode 0x1 or 0x2), capture RSV1:
```c
} else { /* Text or Binary */
    if (conn->frag_active) {
        conn->close_code = 1002;
        return -1;
    }
    bool msg_rsv1 = (conn->recv_buf[0] & 0x40) != 0;
```

Wait — by the time we're in this branch, the header bytes have already been consumed (we're working with `fh` struct). We need to capture RSV1 from the raw byte before `memmove`. The simplest approach: add a `rsv1` field to `ws_frame_header_t`.

Actually, let's read RSV1 from the first byte of the frame **before** consuming it. In `ws_frame_decode_header`, RSV1 is bit 6 of byte 0. We need to add it to the `ws_frame_header_t` struct.

In `ws-frame.h`, add to `ws_frame_header_t`:
```c
typedef struct {
    bool     fin;
    bool     rsv1;       /* NEW: RSV1 bit (used by permessage-deflate). */
    uint8_t  opcode;
    bool     masked;
    uint64_t payload_len;
    uint8_t  mask_key[4];
    size_t   header_size;
} ws_frame_header_t;
```

In `ws-frame.c` `ws_frame_decode_header`, after extracting `fin`:
```c
    out->fin    = (b0 & 0x80) != 0;
    out->rsv1   = (b0 & 0x40) != 0;
    out->opcode = b0 & 0x0F;
```

Now in `xylem_ws_recv`, handle decompression:

For **single-frame complete messages** (the `if (fh.fin)` branch inside the text/binary case):

```c
if (fh.fin) {
    if (conn->max_msg_size && fh.payload_len > conn->max_msg_size) {
        conn->close_code = 1009;
        return -1;
    }

    void* msg_data;
    size_t msg_len;

    if (fh.rsv1 && conn->deflate_ctx.active) {
        if (ws_deflate_decompress(&conn->deflate_ctx,
                                  payload, (size_t)fh.payload_len,
                                  &msg_data, &msg_len,
                                  conn->max_msg_size) != 0) {
            conn->close_code = 1007;
            return -1;
        }
    } else {
        msg_data = malloc(fh.payload_len ? (size_t)fh.payload_len : 1);
        if (!msg_data) {
            return -1;
        }
        if (fh.payload_len) {
            memcpy(msg_data, payload, (size_t)fh.payload_len);
        }
        msg_len = (size_t)fh.payload_len;
    }

    if (fh.opcode == 0x1) {
        if (ws_utf8_validate(msg_data, msg_len) != 0) {
            free(msg_data);
            conn->close_code = 1007;
            return -1;
        }
    }
    msg->opcode = (xylem_ws_opcode_t)fh.opcode;
    msg->data   = msg_data;
    msg->len    = msg_len;
    return 0;
}
```

For **fragmented messages**, track RSV1 on the first fragment. Add a `bool frag_compressed;` field to `xylem_ws_conn_s` in `ws.h`:

```c
    bool             frag_compressed;
```

When starting fragmentation:
```c
} else {
    conn->frag_active = true;
    conn->frag_opcode = fh.opcode;
    conn->frag_compressed = fh.rsv1;
    conn->frag_len = 0;
    /* ... append ... */
}
```

When the final continuation frame arrives (the `if (fh.fin)` branch inside opcode 0x0):

```c
if (fh.fin) {
    void* msg_data;
    size_t msg_len;

    if (conn->frag_compressed && conn->deflate_ctx.active) {
        if (ws_deflate_decompress(&conn->deflate_ctx,
                                  conn->frag_buf, conn->frag_len,
                                  &msg_data, &msg_len,
                                  conn->max_msg_size) != 0) {
            free(conn->frag_buf);
            conn->frag_buf = NULL;
            conn->frag_len = 0;
            conn->frag_cap = 0;
            conn->frag_active = false;
            conn->close_code = 1007;
            return -1;
        }
        free(conn->frag_buf);
        conn->frag_buf = NULL;
        conn->frag_len = 0;
        conn->frag_cap = 0;
    } else {
        msg_data = conn->frag_buf;
        msg_len  = conn->frag_len;
        conn->frag_buf = NULL;
        conn->frag_len = 0;
        conn->frag_cap = 0;
    }

    if (conn->frag_opcode == 0x1) {
        if (ws_utf8_validate(msg_data, msg_len) != 0) {
            free(msg_data);
            conn->close_code = 1007;
            return -1;
        }
    }
    msg->opcode = (xylem_ws_opcode_t)conn->frag_opcode;
    msg->data   = msg_data;
    msg->len    = msg_len;
    conn->frag_active = false;
    return 0;
}
```

- [ ] **Step 2: Verify build**

Run: `cmake --build build --config Debug 2>&1 | head -20`
Expected: Clean compile.

- [ ] **Step 3: Commit**

```bash
git add src/net/ws/ws-frame.h src/net/ws/ws-frame.c src/net/ws/ws.h src/net/ws/ws.c
git commit -m "feat(ws): decompress incoming messages when deflate active"
```

---

### Task 9: Integration tests

**Files:**
- Modify: `tests/test-ws.c`

- [ ] **Step 1: Add deflate echo test**

Add a new echo test with deflate enabled. Insert before the `tests[]` array:

```c
/* --- Test: permessage-deflate text echo --- */
static void test_deflate_text_echo(void* arg) {
    (void)arg;
    xylem_ws_opts_t opts = { .permessage_deflate = true };
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                              echo_handler, NULL, &opts);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%u/", port);
    xylem_ws_conn_t* c = xylem_ws_dial(url, &opts);
    ASSERT(c != NULL);

    const char* text = "hello permessage-deflate compression test!";
    ASSERT(xylem_ws_send(c, XYLEM_WS_TEXT, text, strlen(text)) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == 0);
    ASSERT(msg.opcode == XYLEM_WS_TEXT);
    ASSERT(msg.len == strlen(text));
    ASSERT(memcmp(msg.data, text, msg.len) == 0);
    xylem_ws_msg_free(&msg);

    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    xylem_shutdown();
}

/* --- Test: permessage-deflate multiple messages (context takeover) --- */
static void test_deflate_context_takeover(void* arg) {
    (void)arg;
    xylem_ws_opts_t opts = {
        .permessage_deflate = true,
        .deflate_context_takeover = true,
    };
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                              echo_handler, NULL, &opts);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%u/", port);
    xylem_ws_conn_t* c = xylem_ws_dial(url, &opts);
    ASSERT(c != NULL);

    for (int i = 0; i < 10; i++) {
        char buf[128];
        int len = snprintf(buf, sizeof(buf),
                           "message number %d with repeated content for compression", i);
        ASSERT(xylem_ws_send(c, XYLEM_WS_TEXT, buf, (size_t)len) == 0);

        xylem_ws_msg_t msg;
        ASSERT(xylem_ws_recv(c, &msg) == 0);
        ASSERT(msg.len == (size_t)len);
        ASSERT(memcmp(msg.data, buf, msg.len) == 0);
        xylem_ws_msg_free(&msg);
    }

    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    xylem_shutdown();
}

/* --- Test: permessage-deflate large binary message --- */
static void test_deflate_large_binary(void* arg) {
    (void)arg;
    xylem_ws_opts_t opts = {
        .permessage_deflate = true,
        .fragment_threshold = 4096,
    };
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                              echo_handler, NULL, &opts);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%u/", port);
    xylem_ws_conn_t* c = xylem_ws_dial(url, &opts);
    ASSERT(c != NULL);

    /* Highly compressible data */
    size_t big_len = 32768;
    uint8_t* big = (uint8_t*)malloc(big_len);
    ASSERT(big != NULL);
    memset(big, 'A', big_len);

    ASSERT(xylem_ws_send(c, XYLEM_WS_BINARY, big, big_len) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == 0);
    ASSERT(msg.opcode == XYLEM_WS_BINARY);
    ASSERT(msg.len == big_len);
    ASSERT(memcmp(msg.data, big, big_len) == 0);
    xylem_ws_msg_free(&msg);

    free(big);
    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    xylem_shutdown();
}

/* --- Test: deflate disabled (no negotiation) still works --- */
static void test_deflate_disabled_fallback(void* arg) {
    (void)arg;
    /* Server has deflate OFF, client has deflate ON → should fall back */
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                              echo_handler, NULL, NULL);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%u/", port);
    xylem_ws_opts_t client_opts = { .permessage_deflate = true };
    xylem_ws_conn_t* c = xylem_ws_dial(url, &client_opts);
    ASSERT(c != NULL);

    const char* text = "no compression here";
    ASSERT(xylem_ws_send(c, XYLEM_WS_TEXT, text, strlen(text)) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == 0);
    ASSERT(msg.opcode == XYLEM_WS_TEXT);
    ASSERT(msg.len == strlen(text));
    ASSERT(memcmp(msg.data, text, msg.len) == 0);
    xylem_ws_msg_free(&msg);

    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    xylem_shutdown();
}
```

- [ ] **Step 2: Register new tests in the test array**

Update the `tests[]` array:
```c
static test_fn_t tests[] = {
    test_null_guards,
    test_text_echo,
    test_binary_echo,
    test_multiple_messages,
    test_large_message,
    test_server_close,
    test_deflate_text_echo,
    test_deflate_context_takeover,
    test_deflate_large_binary,
    test_deflate_disabled_fallback,
};
```

- [ ] **Step 3: Build and run tests**

Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug -R test-ws --output-on-failure`
Expected: All tests pass (including new deflate tests).

- [ ] **Step 4: Commit**

```bash
git add tests/test-ws.c
git commit -m "test(ws): add permessage-deflate integration tests"
```

---

### Task 10: Final verification

- [ ] **Step 1: Run full test suite**

Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: All tests pass, no regressions.

- [ ] **Step 2: Run with ASAN (if available)**

Run: `cmake -B build -DXYLEM_ENABLE_ASAN=ON && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: No memory errors reported.

- [ ] **Step 3: Final commit (if any fixups needed)**

Only if previous steps required fixes.
