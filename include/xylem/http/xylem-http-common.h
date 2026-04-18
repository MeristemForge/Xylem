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

/**
 * @brief HTTP header name-value pair for public API use.
 *
 * Non-owning pointers: the caller must ensure the strings remain
 * valid for the duration of the API call that receives them.
 */
typedef struct {
    const char* name;  /*< Header name (e.g. "Authorization"). */
    const char* value; /*< Header value (e.g. "Bearer token"). */
} xylem_http_hdr_t;

/**
 * @brief Percent-encode a string for use in URL path or query.
 *
 * Encodes reserved and non-ASCII bytes as %XX sequences per RFC 3986.
 *
 * @param src      Source bytes.
 * @param src_len  Source length in bytes.
 * @param out_len  Output: encoded length excluding NUL terminator.
 *
 * @return Newly allocated encoded string, or NULL on failure.
 *
 * @note The caller must free the returned string with free().
 */
extern char* xylem_http_url_encode(const char* src, size_t src_len,
                                    size_t* out_len);

/**
 * @brief Decode a percent-encoded string.
 *
 * Decodes %XX sequences back to their original byte values.
 *
 * @param src      Source string.
 * @param src_len  Source length in bytes.
 * @param out_len  Output: decoded length.
 *
 * @return Newly allocated decoded string, or NULL on failure.
 *
 * @note The caller must free the returned string with free().
 */
extern char* xylem_http_url_decode(const char* src, size_t src_len,
                                    size_t* out_len);

/**
 * @brief CORS configuration for generating Access-Control-* headers.
 *
 * Pass to xylem_http_cors_headers() to produce the appropriate
 * response headers for a given request origin.
 */
typedef struct {
    const char* allowed_origins;   /*< Comma-separated origins or "*". */
    const char* allowed_methods;   /*< Comma-separated methods (e.g. "GET,POST"). */
    const char* allowed_headers;   /*< Comma-separated headers (e.g. "Content-Type,Authorization"). */
    const char* expose_headers;    /*< Comma-separated headers to expose to the client. */
    int         max_age;           /*< Preflight cache duration in seconds, 0 to omit. */
    bool        allow_credentials; /*< If true, emit Access-Control-Allow-Credentials: true. */
} xylem_http_cors_t;

/**
 * @brief Generate CORS response headers from a configuration.
 *
 * Checks whether the request origin matches allowed_origins
 * ("*" matches all, otherwise comma-separated exact list).
 * Writes matching Access-Control-* headers into the output array.
 *
 * When is_preflight is true, additionally emits Allow-Methods,
 * Allow-Headers, and Max-Age headers for OPTIONS preflight requests.
 *
 * When allow_credentials is true, Access-Control-Allow-Origin is
 * set to the actual origin value (never "*") and
 * Access-Control-Allow-Credentials: true is emitted.
 *
 * @param cors          CORS configuration, or NULL (returns 0).
 * @param origin        Request Origin header value, or NULL (returns 0).
 * @param is_preflight  True if this is an OPTIONS preflight request.
 * @param out           Output array of headers. Caller provides storage.
 *                      Must have room for at least 7 entries.
 * @param out_cap       Capacity of the out array.
 *
 * @return Number of headers written to out, or 0 if origin does
 *         not match or cors/origin is NULL.
 *
 * @note The returned header name/value pointers reference either
 *       static strings or fields within the cors struct and the
 *       origin parameter. They remain valid as long as those
 *       inputs remain valid.
 */
extern size_t xylem_http_cors_headers(const xylem_http_cors_t* cors,
                                      const char* origin,
                                      bool is_preflight,
                                      xylem_http_hdr_t* out,
                                      size_t out_cap);

/* Opaque multipart type */
typedef struct xylem_http_multipart_s xylem_http_multipart_t;

/**
 * @brief Parse a multipart/form-data request body.
 *
 * Extracts the boundary from content_type, then splits the body
 * into individual parts, parsing Content-Disposition (name, filename)
 * and Content-Type for each part.
 *
 * @param content_type  Content-Type header value (must contain boundary).
 * @param body          Request body data.
 * @param body_len      Body length in bytes.
 *
 * @return Parsed multipart handle on success, NULL on invalid input.
 *
 * @note The caller must free with xylem_http_multipart_destroy().
 */
extern xylem_http_multipart_t* xylem_http_multipart_parse(
    const char* content_type, const void* body, size_t body_len);

/**
 * @brief Get the number of parts in a multipart message.
 *
 * @param mp  Multipart handle.
 *
 * @return Number of parts, or 0 if mp is NULL.
 */
extern size_t xylem_http_multipart_count(const xylem_http_multipart_t* mp);

/**
 * @brief Get the name field of a part.
 *
 * @param mp     Multipart handle.
 * @param index  Part index (0-based).
 *
 * @return Name string, or NULL if not present or index out of range.
 */
extern const char* xylem_http_multipart_name(
    const xylem_http_multipart_t* mp, size_t index);

/**
 * @brief Get the filename field of a part.
 *
 * @param mp     Multipart handle.
 * @param index  Part index (0-based).
 *
 * @return Filename string, or NULL if not present or index out of range.
 */
extern const char* xylem_http_multipart_filename(
    const xylem_http_multipart_t* mp, size_t index);

/**
 * @brief Get the Content-Type of a part.
 *
 * @param mp     Multipart handle.
 * @param index  Part index (0-based).
 *
 * @return Content-Type string, or NULL if not present or index out of range.
 */
extern const char* xylem_http_multipart_content_type(
    const xylem_http_multipart_t* mp, size_t index);

/**
 * @brief Get the body data of a part.
 *
 * @param mp     Multipart handle.
 * @param index  Part index (0-based).
 *
 * @return Pointer to part body data, or NULL if index out of range.
 */
extern const void* xylem_http_multipart_data(
    const xylem_http_multipart_t* mp, size_t index);

/**
 * @brief Get the body data length of a part.
 *
 * @param mp     Multipart handle.
 * @param index  Part index (0-based).
 *
 * @return Part body length in bytes, or 0 if index out of range.
 */
extern size_t xylem_http_multipart_data_len(
    const xylem_http_multipart_t* mp, size_t index);

/**
 * @brief Destroy a multipart handle and free all associated memory.
 *
 * @param mp  Multipart handle, or NULL (no-op).
 */
extern void xylem_http_multipart_destroy(xylem_http_multipart_t* mp);
