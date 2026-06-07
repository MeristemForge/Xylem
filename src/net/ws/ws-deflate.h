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

_Pragma("once")

#include "encoding/gzip/miniz/miniz.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ws_deflate_ctx_s {
    mz_stream deflate_stream;
    mz_stream inflate_stream;
    bool      active;
    bool      no_context_takeover;
} ws_deflate_ctx_t;

typedef struct ws_deflate_offer_s {
    bool offered;
    bool server_no_context_takeover;
    bool client_no_context_takeover;
} ws_deflate_offer_t;

/**
 * @brief Initialize the permessage-deflate compression context.
 *
 * Sets up both deflate and inflate streams using raw deflate mode
 * (negative window bits).
 *
 * @param ctx                  Context to initialize.
 * @param no_context_takeover  If true, reset streams after each message.
 *
 * @return 0 on success, -1 on error.
 */
extern int ws_deflate_init(ws_deflate_ctx_t* ctx, bool no_context_takeover);

/**
 * @brief Clean up the permessage-deflate compression context.
 *
 * Ends both deflate and inflate streams and marks the context inactive.
 *
 * @param ctx  Context to clean up. Safe to call on an inactive context.
 */
extern void ws_deflate_cleanup(ws_deflate_ctx_t* ctx);

/**
 * @brief Compress a WebSocket message payload using deflate.
 *
 * Compresses the input data with MZ_SYNC_FLUSH and strips the trailing
 * 4-byte sync marker (0x00 0x00 0xFF 0xFF). Optionally resets the
 * deflate stream if no_context_takeover is set.
 *
 * @param ctx      Active deflate context.
 * @param in       Input payload data.
 * @param in_len   Length of input data in bytes.
 * @param out      Receives pointer to malloc'd compressed data.
 * @param out_len  Receives length of compressed data.
 *
 * @return 0 on success, -1 on error. Caller must free *out on success.
 */
extern int ws_deflate_compress(ws_deflate_ctx_t* ctx,
                               const void* in, size_t in_len,
                               void** out, size_t* out_len);

/**
 * @brief Decompress a WebSocket message payload using inflate.
 *
 * Appends the 4-byte sync marker (0x00 0x00 0xFF 0xFF) and inflates
 * into a growing buffer. Optionally resets the inflate stream if
 * no_context_takeover is set.
 *
 * @param ctx       Active deflate context.
 * @param in        Compressed payload data.
 * @param in_len    Length of compressed data in bytes.
 * @param out       Receives pointer to malloc'd decompressed data.
 * @param out_len   Receives length of decompressed data.
 * @param max_size  Maximum allowed output size (0 for unlimited).
 *
 * @return 0 on success, -1 on error. Caller must free *out on success.
 */
extern int ws_deflate_decompress(ws_deflate_ctx_t* ctx,
                                 const void* in, size_t in_len,
                                 void** out, size_t* out_len,
                                 size_t max_size);

/**
 * @brief Parse a Sec-WebSocket-Extensions header for permessage-deflate.
 *
 * Extracts the permessage-deflate offer parameters from the extension
 * header value. Accepts server_no_context_takeover,
 * client_no_context_takeover, and *_max_window_bits=15 or bare.
 * Rejects unknown parameters or window bits < 15.
 *
 * @param header  The Sec-WebSocket-Extensions header value string.
 * @param offer   Receives the parsed offer parameters.
 *
 * @return 0 on success, -1 if not offered or parameters unacceptable.
 */
extern int ws_deflate_parse_offer(const char* header, ws_deflate_offer_t* offer);

/**
 * @brief Build a client Sec-WebSocket-Extensions offer string.
 *
 * Constructs the extension offer for the client handshake request.
 * When context_takeover is false, includes both
 * client_no_context_takeover and server_no_context_takeover params.
 *
 * @param context_takeover  Whether context takeover is desired.
 * @param out               Destination buffer for the offer string.
 * @param out_size          Size of the destination buffer.
 *
 * @return 0 on success, -1 on error (buffer too small).
 */
extern int ws_deflate_build_client_offer(bool context_takeover,
                                         char* out, size_t out_size);

/**
 * @brief Build a server Sec-WebSocket-Extensions response string.
 *
 * Constructs the extension response echoing the agreed-upon
 * parameters for the server handshake response.
 *
 * @param offer     The parsed offer from the client.
 * @param out       Destination buffer for the response string.
 * @param out_size  Size of the destination buffer.
 *
 * @return 0 on success, -1 on error (buffer too small).
 */
extern int ws_deflate_build_server_accept(const ws_deflate_offer_t* offer,
                                          char* out, size_t out_size);
