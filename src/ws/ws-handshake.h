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

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Generate a random Sec-WebSocket-Key (Base64-encoded).
 *
 * Produces 16 random bytes and Base64-encodes them into out.
 * The result is null-terminated.
 *
 * @param out       Destination buffer (must be at least 25 bytes).
 * @param out_size  Size of the destination buffer.
 *
 * @return 0 on success, -1 on error (buffer too small).
 */
extern int ws_handshake_gen_key(char* out, size_t out_size);

/**
 * @brief Compute the Sec-WebSocket-Accept value for a given key.
 *
 * Concatenates key with the RFC 6455 GUID, computes SHA-1, then
 * Base64-encodes the 20-byte digest. The result is null-terminated.
 *
 * @param key       Null-terminated Sec-WebSocket-Key string.
 * @param out       Destination buffer (must be at least 29 bytes).
 * @param out_size  Size of the destination buffer.
 *
 * @return 0 on success, -1 on error.
 */
extern int ws_handshake_compute_accept(const char* key,
                                       char* out, size_t out_size);

/**
 * @brief Build a client HTTP Upgrade request string.
 *
 * Constructs the full HTTP/1.1 Upgrade request for the WebSocket
 * opening handshake. The returned string is malloc'd; the caller
 * must free it.
 *
 * @param host     Target hostname.
 * @param port     Target port number.
 * @param path     Request path (e.g. "/ws").
 * @param key      Base64-encoded Sec-WebSocket-Key.
 * @param out_len  Receives the length of the returned string (excluding
 *                 null terminator). May be NULL.
 *
 * @return Heap-allocated request string, or NULL on allocation failure.
 */
extern char* ws_handshake_build_request(const char* host, uint16_t port,
                                        const char* path, const char* key,
                                        size_t* out_len);

/**
 * @brief Validate a server's Sec-WebSocket-Accept value.
 *
 * Computes the expected accept value from expected_key and compares
 * it with accept_value.
 *
 * @param expected_key   The Sec-WebSocket-Key that was sent.
 * @param accept_value   The Sec-WebSocket-Accept from the server.
 *
 * @return 0 if the accept value matches, -1 otherwise.
 */
extern int ws_handshake_validate_accept(const char* expected_key,
                                        const char* accept_value);

/**
 * @brief Build a server HTTP 101 Switching Protocols response.
 *
 * Constructs the full HTTP/1.1 101 response for the WebSocket
 * opening handshake. The returned string is malloc'd; the caller
 * must free it.
 *
 * @param accept_value  The computed Sec-WebSocket-Accept value.
 * @param out_len       Receives the length of the returned string
 *                      (excluding null terminator). May be NULL.
 *
 * @return Heap-allocated response string, or NULL on allocation failure.
 */
extern char* ws_handshake_build_response(const char* accept_value,
                                         size_t* out_len);
