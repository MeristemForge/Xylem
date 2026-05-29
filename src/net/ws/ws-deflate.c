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

#include "ws-deflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sync flush trailer appended/stripped per RFC 7692 section 7.2.1. */
static const uint8_t _ws_deflate_tail[] = {0x00, 0x00, 0xFF, 0xFF};

int ws_deflate_init(ws_deflate_ctx_t* ctx, bool no_context_takeover) {
    if (!ctx) {
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));

    int rc = mz_deflateInit2(
        &ctx->deflate_stream,
        MZ_DEFAULT_COMPRESSION,
        MZ_DEFLATED,
        -MZ_DEFAULT_WINDOW_BITS,
        9,
        MZ_DEFAULT_STRATEGY
    );
    if (rc != MZ_OK) {
        return -1;
    }

    rc = mz_inflateInit2(&ctx->inflate_stream, -MZ_DEFAULT_WINDOW_BITS);
    if (rc != MZ_OK) {
        mz_deflateEnd(&ctx->deflate_stream);
        return -1;
    }

    ctx->active              = true;
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

    /* Estimate output size: deflateBound + some margin. */
    size_t buf_size = mz_deflateBound(&ctx->deflate_stream, (mz_ulong)in_len);
    if (buf_size < 64) {
        buf_size = 64;
    }

    uint8_t* buf = (uint8_t*)malloc(buf_size);
    if (!buf) {
        return -1;
    }

    ctx->deflate_stream.next_in  = (const unsigned char*)in;
    ctx->deflate_stream.avail_in = (mz_uint32)in_len;
    ctx->deflate_stream.next_out  = buf;
    ctx->deflate_stream.avail_out = (mz_uint32)buf_size;

    int rc = mz_deflate(&ctx->deflate_stream, MZ_SYNC_FLUSH);
    if (rc != MZ_OK) {
        free(buf);
        return -1;
    }

    size_t produced = buf_size - (size_t)ctx->deflate_stream.avail_out;

    /* Strip the trailing 0x00 0x00 0xFF 0xFF per RFC 7692. */
    if (produced >= 4 &&
        buf[produced - 4] == 0x00 &&
        buf[produced - 3] == 0x00 &&
        buf[produced - 2] == 0xFF &&
        buf[produced - 1] == 0xFF) {
        produced -= 4;
    }

    /* Reset stream if no context takeover. */
    if (ctx->no_context_takeover) {
        mz_deflateReset(&ctx->deflate_stream);
    }

    *out     = buf;
    *out_len = produced;
    return 0;
}

int ws_deflate_decompress(ws_deflate_ctx_t* ctx,
                          const void* in, size_t in_len,
                          void** out, size_t* out_len,
                          size_t max_size) {
    if (!ctx || !ctx->active || !out || !out_len) {
        return -1;
    }

    /* Build input with appended tail bytes. */
    size_t total_in = in_len + sizeof(_ws_deflate_tail);
    uint8_t* input = (uint8_t*)malloc(total_in);
    if (!input) {
        return -1;
    }
    memcpy(input, in, in_len);
    memcpy(input + in_len, _ws_deflate_tail, sizeof(_ws_deflate_tail));

    /* Start with a buffer 4x input size, grow as needed. */
    size_t buf_cap = total_in * 4;
    if (buf_cap < 256) {
        buf_cap = 256;
    }
    uint8_t* buf = (uint8_t*)malloc(buf_cap);
    if (!buf) {
        free(input);
        return -1;
    }

    ctx->inflate_stream.next_in  = input;
    ctx->inflate_stream.avail_in = (mz_uint32)total_in;

    size_t total_out = 0;

    for (;;) {
        ctx->inflate_stream.next_out  = buf + total_out;
        ctx->inflate_stream.avail_out = (mz_uint32)(buf_cap - total_out);

        int rc = mz_inflate(&ctx->inflate_stream, MZ_SYNC_FLUSH);

        total_out = buf_cap - (size_t)ctx->inflate_stream.avail_out;

        if (rc == MZ_STREAM_END || ctx->inflate_stream.avail_in == 0) {
            break;
        }

        if (rc != MZ_OK && rc != MZ_BUF_ERROR) {
            free(buf);
            free(input);
            return -1;
        }

        /* Need more output space. */
        if (ctx->inflate_stream.avail_out == 0) {
            size_t new_cap = buf_cap * 2;
            if (max_size && new_cap > max_size) {
                new_cap = max_size;
            }
            if (new_cap <= buf_cap) {
                /* Already at max, cannot grow further. */
                free(buf);
                free(input);
                return -1;
            }
            uint8_t* nb = (uint8_t*)realloc(buf, new_cap);
            if (!nb) {
                free(buf);
                free(input);
                return -1;
            }
            buf     = nb;
            buf_cap = new_cap;
        }
    }

    free(input);

    /* Reset stream if no context takeover. */
    if (ctx->no_context_takeover) {
        mz_inflateReset(&ctx->inflate_stream);
    }

    *out     = buf;
    *out_len = total_out;
    return 0;
}

int ws_deflate_parse_offer(const char* header, ws_deflate_offer_t* offer) {
    if (!header || !offer) {
        return -1;
    }

    memset(offer, 0, sizeof(*offer));

    /* Find "permessage-deflate" in the header value. */
    const char* ext = strstr(header, "permessage-deflate");
    if (!ext) {
        return -1;
    }

    offer->offered = true;

    /* Find the end of this extension (next comma or end of string). */
    const char* end = strchr(ext, ',');
    if (!end) {
        end = ext + strlen(ext);
    }

    /* Parse parameters within the extension token. */
    const char* p = ext + strlen("permessage-deflate");

    while (p < end) {
        /* Skip whitespace and semicolons. */
        while (p < end && (*p == ' ' || *p == ';' || *p == '\t')) {
            p++;
        }
        if (p >= end) {
            break;
        }

        if (strncmp(p, "server_no_context_takeover", 25) == 0 &&
            (p[25] == '\0' || p[25] == ';' || p[25] == ' ' || p[25] == ',')) {
            offer->server_no_context_takeover = true;
            p += 25;
        } else if (strncmp(p, "client_no_context_takeover", 26) == 0 &&
                   (p[26] == '\0' || p[26] == ';' || p[26] == ' ' ||
                    p[26] == ',')) {
            offer->client_no_context_takeover = true;
            p += 26;
        } else if (strncmp(p, "server_max_window_bits", 22) == 0 &&
                   (p[22] == '\0' || p[22] == ';' || p[22] == ' ' ||
                    p[22] == ',' || p[22] == '=')) {
            p += 22;
            /* Skip '=' if present. */
            while (p < end && (*p == ' ' || *p == '=')) {
                p++;
            }
            if (p < end && *p >= '0' && *p <= '9') {
                long val = strtol(p, NULL, 10);
                if (val != 15) {
                    return -1;
                }
                while (p < end && *p >= '0' && *p <= '9') {
                    p++;
                }
            }
            /* Bare param (no value) is acceptable -- means 15. */
        } else if (strncmp(p, "client_max_window_bits", 22) == 0 &&
                   (p[22] == '\0' || p[22] == ';' || p[22] == ' ' ||
                    p[22] == ',' || p[22] == '=')) {
            p += 22;
            /* Skip '=' if present. */
            while (p < end && (*p == ' ' || *p == '=')) {
                p++;
            }
            if (p < end && *p >= '0' && *p <= '9') {
                long val = strtol(p, NULL, 10);
                if (val != 15) {
                    return -1;
                }
                while (p < end && *p >= '0' && *p <= '9') {
                    p++;
                }
            }
            /* Bare param is acceptable. */
        } else {
            /* Unknown parameter -- reject. */
            return -1;
        }
    }

    return 0;
}

int ws_deflate_build_client_offer(bool context_takeover,
                                  char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return -1;
    }

    int len;
    if (context_takeover) {
        len = snprintf(out, out_size, "permessage-deflate");
    } else {
        len = snprintf(
            out, out_size,
            "permessage-deflate; client_no_context_takeover; "
            "server_no_context_takeover"
        );
    }

    if (len < 0 || (size_t)len >= out_size) {
        return -1;
    }
    return 0;
}

int ws_deflate_build_server_accept(const ws_deflate_offer_t* offer,
                                   char* out, size_t out_size) {
    if (!offer || !out || out_size == 0) {
        return -1;
    }

    /* Start building the response. */
    size_t pos = 0;
    int    len;

    len = snprintf(out + pos, out_size - pos, "permessage-deflate");
    if (len < 0 || (size_t)len >= out_size - pos) {
        return -1;
    }
    pos += (size_t)len;

    if (offer->server_no_context_takeover) {
        len = snprintf(
            out + pos, out_size - pos, "; server_no_context_takeover"
        );
        if (len < 0 || (size_t)len >= out_size - pos) {
            return -1;
        }
        pos += (size_t)len;
    }

    if (offer->client_no_context_takeover) {
        len = snprintf(
            out + pos, out_size - pos, "; client_no_context_takeover"
        );
        if (len < 0 || (size_t)len >= out_size - pos) {
            return -1;
        }
        pos += (size_t)len;
    }

    return 0;
}
