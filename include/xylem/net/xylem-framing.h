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

typedef enum xylem_framing_type_e {
    XYLEM_FRAMING_NONE,             /*< Raw mode, recv returns available bytes. */
    XYLEM_FRAMING_FIXED,            /*< Fixed-length frames. */
    XYLEM_FRAMING_LENFIELD_FIXINT,  /*< Length-prefixed, fixed-width integer. */
    XYLEM_FRAMING_LENFIELD_VARINT,  /*< Length-prefixed, variable-length integer (LEB128). */
    XYLEM_FRAMING_DELIMITER,        /*< Delimiter-terminated frames. */
} xylem_framing_type_t;

typedef struct xylem_framing_opts_s {
    xylem_framing_type_t type;
    union {
        struct {
            size_t len; /*< Fixed frame length in bytes. */
        } fixed;
        struct {
            uint32_t header_size;   /*< Total header size in bytes. */
            uint32_t field_offset;  /*< Byte offset of the length field. */
            uint32_t field_size;    /*< Length field width (1-8 bytes). */
            int32_t  adjustment;    /*< Added to decoded length for payload size. */
            bool     big_endian;    /*< true: big-endian byte order. */
        } lenfield_fixint;
        struct {
            uint32_t prefix_size;   /*< Fixed bytes before the varint. */
            int32_t  adjustment;    /*< Added to decoded length for payload size. */
        } lenfield_varint;
        struct {
            const char* delim;      /*< Delimiter bytes. */
            size_t      delim_len;  /*< Delimiter length, 0 = auto strlen. */
        } delimiter;
    };
} xylem_framing_opts_t;
