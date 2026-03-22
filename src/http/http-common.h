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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char     scheme[8];
    char     host[256];
    uint16_t port;
    char     path[2048];
} http_url_t;


typedef struct {
    char* name;
    char* value;
} http_header_t;

/**
 * @brief Convert a hex character to its integer value.
 *
 * @param c  Hex character ('0'-'9', 'A'-'F', 'a'-'f').
 *
 * @return 0-15 on success, -1 if c is not a valid hex digit.
 */
extern int http_hex_digit(char c);

/**
 * @brief Check if a byte is an RFC 3986 unreserved character.
 *
 * @param c  Byte value to check.
 *
 * @return true if unreserved (A-Z, a-z, 0-9, '-', '.', '_', '~').
 */
extern bool http_is_unreserved(uint8_t c);

/**
 * @brief Parse a URL string into its components.
 *
 * Extracts scheme (http/https only), host, port (defaults 80/443),
 * and path (defaults "/").
 *
 * @param url  Null-terminated URL string.
 * @param out  Parsed result.
 *
 * @return 0 on success, -1 on invalid URL.
 */
extern int http_url_parse(const char* url, http_url_t* out);

/**
 * @brief Serialize a parsed URL back into a string.
 *
 * Omits the port when it matches the scheme default (80/443).
 *
 * @param url       Parsed URL.
 * @param buf       Output buffer.
 * @param buf_size  Buffer capacity in bytes.
 *
 * @return 0 on success, -1 on truncation.
 */
extern int http_url_serialize(const http_url_t* url, char* buf,
                              size_t buf_size);

/**
 * @brief Build an HTTP/1.1 request into a malloc'd buffer.
 *
 * Fills Host, Content-Length, Connection, Content-Type, and
 * optionally Expect headers. When expect_continue is true the
 * body is NOT appended.
 *
 * @param method           HTTP method string (e.g. "GET").
 * @param url              Parsed URL.
 * @param body             Request body, or NULL.
 * @param body_len         Body length in bytes.
 * @param content_type     Content-Type value, or NULL.
 * @param expect_continue  If true, add Expect: 100-continue and omit body.
 * @param out_len          Output: total serialized length.
 *
 * @return Allocated buffer on success, NULL on failure. Caller frees.
 */
extern char* http_req_serialize(const char* method, const http_url_t* url,
                                const void* body, size_t body_len,
                                const char* content_type,
                                bool expect_continue, size_t* out_len);

/**
 * @brief Case-insensitive ASCII comparison of two strings.
 *
 * @param a  First string.
 * @param b  Second string.
 *
 * @return true if equal (ignoring case).
 */
extern bool http_header_eq(const char* a, const char* b);

/**
 * @brief Find a header value by name (case-insensitive).
 *
 * @param headers  Header array.
 * @param count    Number of headers.
 * @param name     Header name to look up.
 *
 * @return Header value string, or NULL if not found.
 */
extern const char* http_header_find(const http_header_t* headers,
                                    size_t count, const char* name);

/**
 * @brief Append a header to a growable array.
 *
 * Copies name and value into newly allocated strings.
 *
 * @param headers    Pointer to header array (may be reallocated).
 * @param count      Pointer to current header count.
 * @param cap        Pointer to current array capacity.
 * @param name       Header name.
 * @param name_len   Header name length.
 * @param value      Header value.
 * @param value_len  Header value length.
 *
 * @return 0 on success, -1 on allocation failure.
 */
extern int http_header_add(http_header_t** headers, size_t* count,
                           size_t* cap, const char* name, size_t name_len,
                           const char* value, size_t value_len);

/**
 * @brief Free all name/value strings and the header array itself.
 *
 * @param headers  Header array (may be NULL).
 * @param count    Number of headers.
 */
extern void http_headers_free(http_header_t* headers, size_t count);

/**
 * @brief Map an HTTP status code to its standard reason phrase.
 *
 * @param status  HTTP status code (e.g. 200, 404).
 *
 * @return Reason phrase string, or "" for unknown codes.
 */
extern const char* http_reason_phrase(int status);
