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

/* JSON value type. */
typedef enum xylem_json_type_e {
    XYLEM_JSON_TYPE_NONE = 0,
    XYLEM_JSON_TYPE_NULL,
    XYLEM_JSON_TYPE_BOOL,
    XYLEM_JSON_TYPE_NUM,
    XYLEM_JSON_TYPE_STR,
    XYLEM_JSON_TYPE_ARR,
    XYLEM_JSON_TYPE_OBJ
} xylem_json_type_t;

typedef struct xylem_json_s xylem_json_t;

/**
 * @brief Parse a JSON string.
 *
 * Creates a read-only JSON handle from a null-terminated string.
 * The input string is not modified.
 *
 * @param str  Null-terminated JSON string.
 *
 * @return JSON handle on success, NULL on parse error.
 *
 * @note The returned handle must be freed with xylem_json_destroy().
 */
extern xylem_json_t* xylem_json_parse(const char* str);

/**
 * @brief Destroy a JSON handle and free all associated memory.
 *
 * @param j  JSON handle, or NULL (no-op).
 */
extern void xylem_json_destroy(xylem_json_t* j);

/**
 * @brief Serialize a JSON handle to a minified string.
 *
 * @param j  JSON handle.
 *
 * @return Newly allocated null-terminated string, or NULL on error.
 *
 * @note The caller must free the returned string with free().
 */
extern char* xylem_json_print(const xylem_json_t* j);

/**
 * @brief Serialize a JSON handle to a pretty-printed string.
 *
 * @param j  JSON handle.
 *
 * @return Newly allocated null-terminated string, or NULL on error.
 *
 * @note The caller must free the returned string with free().
 */
extern char* xylem_json_print_pretty(const xylem_json_t* j);

/**
 * @brief Get the type of a JSON handle.
 *
 * @param j  JSON handle.
 *
 * @return The JSON value type, or XYLEM_JSON_TYPE_NONE if j is NULL.
 */
extern xylem_json_type_t xylem_json_type(const xylem_json_t* j);

/**
 * @brief Get a string value by key.
 *
 * @param j    JSON object handle.
 * @param key  Key name.
 *
 * @return The string value, or NULL if key is missing or not a string.
 *
 * @note The returned pointer is valid until the handle is destroyed.
 */
extern const char* xylem_json_str(const xylem_json_t* j, const char* key);

/**
 * @brief Get a 32-bit signed integer value by key.
 *
 * @param j    JSON object handle.
 * @param key  Key name.
 *
 * @return The integer value, or 0 if key is missing or not a number.
 */
extern int32_t xylem_json_i32(const xylem_json_t* j, const char* key);

/**
 * @brief Get a 64-bit signed integer value by key.
 *
 * @param j    JSON object handle.
 * @param key  Key name.
 *
 * @return The integer value, or 0 if key is missing or not a number.
 */
extern int64_t xylem_json_i64(const xylem_json_t* j, const char* key);

/**
 * @brief Get a double-precision floating-point value by key.
 *
 * @param j    JSON object handle.
 * @param key  Key name.
 *
 * @return The double value, or 0.0 if key is missing or not a number.
 */
extern double xylem_json_f64(const xylem_json_t* j, const char* key);

/**
 * @brief Get a boolean value by key.
 *
 * @param j    JSON object handle.
 * @param key  Key name.
 *
 * @return The boolean value, or false if key is missing or not a bool.
 */
extern bool xylem_json_bool(const xylem_json_t* j, const char* key);

/**
 * @brief Get a nested object by key.
 *
 * @param j    JSON object handle.
 * @param key  Key name.
 *
 * @return A JSON handle to the nested object, or NULL if not found.
 *
 * @note The returned handle shares ownership with the parent. Do not
 *       call xylem_json_destroy() on it; destroy the root handle only.
 */
extern xylem_json_t* xylem_json_obj(const xylem_json_t* j, const char* key);

/**
 * @brief Get a nested array by key.
 *
 * @param j    JSON object handle.
 * @param key  Key name.
 *
 * @return A JSON handle to the nested array, or NULL if not found.
 *
 * @note The returned handle shares ownership with the parent. Do not
 *       call xylem_json_destroy() on it; destroy the root handle only.
 */
extern xylem_json_t* xylem_json_arr(const xylem_json_t* j, const char* key);

/**
 * @brief Get the number of elements in a JSON array.
 *
 * @param j  JSON array handle.
 *
 * @return Element count, or 0 if j is NULL or not an array.
 */
extern size_t xylem_json_arr_len(const xylem_json_t* j);

/**
 * @brief Get an element from a JSON array by index.
 *
 * @param j      JSON array handle.
 * @param index  Zero-based index.
 *
 * @return A JSON handle to the element, or NULL if out of bounds.
 *
 * @note The returned handle shares ownership with the parent. Do not
 *       call xylem_json_destroy() on it; destroy the root handle only.
 */
extern xylem_json_t* xylem_json_arr_get(const xylem_json_t* j, size_t index);

/**
 * @brief Create a new empty JSON object for building.
 *
 * @return A mutable JSON object handle, or NULL on allocation failure.
 *
 * @note The returned handle must be freed with xylem_json_destroy().
 */
extern xylem_json_t* xylem_json_new_obj(void);

/**
 * @brief Create a new empty JSON array for building.
 *
 * @return A mutable JSON array handle, or NULL on allocation failure.
 *
 * @note The returned handle must be freed with xylem_json_destroy().
 */
extern xylem_json_t* xylem_json_new_arr(void);

/**
 * @brief Set a string value on a JSON object.
 *
 * @param j    JSON object handle (must be mutable).
 * @param key  Key name.
 * @param val  String value.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_json_set_str(xylem_json_t* j, const char* key,
                              const char* val);

/**
 * @brief Set a 32-bit signed integer value on a JSON object.
 *
 * @param j    JSON object handle (must be mutable).
 * @param key  Key name.
 * @param val  Integer value.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_json_set_i32(xylem_json_t* j, const char* key, int32_t val);

/**
 * @brief Set a 64-bit signed integer value on a JSON object.
 *
 * @param j    JSON object handle (must be mutable).
 * @param key  Key name.
 * @param val  Integer value.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_json_set_i64(xylem_json_t* j, const char* key, int64_t val);

/**
 * @brief Set a double-precision floating-point value on a JSON object.
 *
 * @param j    JSON object handle (must be mutable).
 * @param key  Key name.
 * @param val  Double value.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_json_set_f64(xylem_json_t* j, const char* key, double val);

/**
 * @brief Set a boolean value on a JSON object.
 *
 * @param j    JSON object handle (must be mutable).
 * @param key  Key name.
 * @param val  Boolean value.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_json_set_bool(xylem_json_t* j, const char* key, bool val);

/**
 * @brief Set a null value on a JSON object.
 *
 * @param j    JSON object handle (must be mutable).
 * @param key  Key name.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_json_set_null(xylem_json_t* j, const char* key);

/**
 * @brief Set a nested object value on a JSON object.
 *
 * The child handle is consumed by this call. The caller must not use
 * or destroy it after a successful call.
 *
 * @param j      JSON object handle (must be mutable).
 * @param key    Key name.
 * @param child  JSON object handle to attach.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_json_set_obj(xylem_json_t* j, const char* key,
                              xylem_json_t* child);

/**
 * @brief Set a nested array value on a JSON object.
 *
 * The child handle is consumed by this call. The caller must not use
 * or destroy it after a successful call.
 *
 * @param j      JSON object handle (must be mutable).
 * @param key    Key name.
 * @param child  JSON array handle to attach.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_json_set_arr(xylem_json_t* j, const char* key,
                              xylem_json_t* child);

/**
 * @brief Append a value to a JSON array.
 *
 * The item handle is consumed by this call. The caller must not use
 * or destroy it after a successful call.
 *
 * @param j     JSON array handle (must be mutable).
 * @param item  JSON value handle to append.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_json_arr_push(xylem_json_t* j, xylem_json_t* item);

/**
 * @brief Append a string value to a JSON array.
 *
 * @param j    JSON array handle (must be mutable).
 * @param val  String value.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_json_arr_push_str(xylem_json_t* j, const char* val);

/**
 * @brief Append a 64-bit signed integer to a JSON array.
 *
 * @param j    JSON array handle (must be mutable).
 * @param val  Integer value.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_json_arr_push_i64(xylem_json_t* j, int64_t val);

/**
 * @brief Append a double-precision floating-point number to a JSON array.
 *
 * @param j    JSON array handle (must be mutable).
 * @param val  Double value.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_json_arr_push_f64(xylem_json_t* j, double val);

/**
 * @brief Append a boolean value to a JSON array.
 *
 * @param j    JSON array handle (must be mutable).
 * @param val  Boolean value.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_json_arr_push_bool(xylem_json_t* j, bool val);
