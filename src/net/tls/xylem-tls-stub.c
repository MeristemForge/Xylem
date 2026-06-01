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

/*
 * Stub TLS read/write primitives for builds without TLS support. The
 * reader, writer, and mux transport tables name xylem_tls_read and
 * xylem_tls_write in their XYLEM_*_TLS cases; those files compile
 * unconditionally, so the symbols must exist even when the real TLS
 * engine (xylem-tls.c) is excluded. A caller can never reach these
 * stubs through a live connection without TLS enabled, but they keep
 * the link clean and degrade to an error if the dead case is ever hit.
 */

#include "xylem/net/xylem-tls.h"

int xylem_tls_read(xylem_tls_conn_t* tls, void* buf, int len) {
    (void)tls;
    (void)buf;
    (void)len;
    return -1;
}

int xylem_tls_write(xylem_tls_conn_t* tls, const void* data, int len) {
    (void)tls;
    (void)data;
    (void)len;
    return -1;
}
