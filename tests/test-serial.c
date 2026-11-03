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

static void test_invalid_options(void) {
    ASSERT(xylem_serial_open(NULL) == NULL);
    /* Keep rejection checks independent of attached serial hardware. */
    const char*         missing_device = "xylem-test-missing-serial-device";
    xylem_serial_opts_t cases[]        = {
        {.device = NULL},
        {.device = missing_device, .baudrate = (xylem_serial_baudrate_t)99},
        {.device = missing_device, .parity = (xylem_serial_parity_t)99},
        {.device = missing_device, .databits = (xylem_serial_databits_t)99},
        {.device = missing_device, .stopbits = (xylem_serial_stopbits_t)99},
        {.device      = missing_device,
         .flowcontrol = (xylem_serial_flowcontrol_t)99},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ASSERT(xylem_serial_open(&cases[i]) == NULL);
    }
}

static void test_null_handles(void) {
    char buf[16];
    xylem_serial_close(NULL);
    ASSERT(xylem_serial_read(NULL, buf, sizeof(buf)) == -1);
    ASSERT(xylem_serial_write(NULL, "x", 1) == -1);
}

int main(void) {
    test_invalid_options();
    test_null_handles();
    return 0;
}
