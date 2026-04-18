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

#include "xylem.h"
#include "assert.h"

static void test_open_null_opts(void) {
    xylem_serial_t* s = xylem_serial_open(NULL);
    ASSERT(s == NULL);
}

static void test_open_null_device(void) {
    xylem_serial_opts_t opts = {.device = NULL};
    xylem_serial_t* s = xylem_serial_open(&opts);
    ASSERT(s == NULL);
}

static void test_open_invalid_baudrate(void) {
    xylem_serial_opts_t opts = {
        .device   = "/dev/null",
        .baudrate = (xylem_serial_baudrate_t)99,
    };
    xylem_serial_t* s = xylem_serial_open(&opts);
    ASSERT(s == NULL);
}

static void test_open_invalid_parity(void) {
    xylem_serial_opts_t opts = {
        .device = "/dev/null",
        .parity = (xylem_serial_parity_t)99,
    };
    xylem_serial_t* s = xylem_serial_open(&opts);
    ASSERT(s == NULL);
}

static void test_open_invalid_databits(void) {
    xylem_serial_opts_t opts = {
        .device   = "/dev/null",
        .databits = (xylem_serial_databits_t)99,
    };
    xylem_serial_t* s = xylem_serial_open(&opts);
    ASSERT(s == NULL);
}

static void test_open_invalid_stopbits(void) {
    xylem_serial_opts_t opts = {
        .device   = "/dev/null",
        .stopbits = (xylem_serial_stopbits_t)99,
    };
    xylem_serial_t* s = xylem_serial_open(&opts);
    ASSERT(s == NULL);
}

static void test_open_invalid_flowcontrol(void) {
    xylem_serial_opts_t opts = {
        .device      = "/dev/null",
        .flowcontrol = (xylem_serial_flowcontrol_t)99,
    };
    xylem_serial_t* s = xylem_serial_open(&opts);
    ASSERT(s == NULL);
}

static void test_close_null(void) {
    xylem_serial_close(NULL);
}

static void test_read_null(void) {
    char buf[16];
    int rc = xylem_serial_read(NULL, buf, sizeof(buf));
    ASSERT(rc == -1);
}

static void test_write_null(void) {
    int rc = xylem_serial_write(NULL, "x", 1);
    ASSERT(rc == -1);
}

int main(void) {
    xylem_startup();

    test_open_null_opts();
    test_open_null_device();
    test_open_invalid_baudrate();
    test_open_invalid_parity();
    test_open_invalid_databits();
    test_open_invalid_stopbits();
    test_open_invalid_flowcontrol();
    test_close_null();
    test_read_null();
    test_write_null();

    xylem_cleanup();
    return 0;
}
