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

/**
 * Serial Echo
 *
 * Opens a serial port, sends "hello", reads back the response, then exits.
 * Connect TX to RX (loopback) or pair with a device that echoes data.
 *
 * Usage: serial-echo <device>
 *   Windows:  serial-echo COM3
 *   Linux:    serial-echo /dev/ttyUSB0
 *   macOS:    serial-echo /dev/cu.usbserial-0001
 */

#include "xylem.h"
#include "xylem/xylem-serial.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <device>\n", argv[0]);
        return 1;
    }

    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    xylem_serial_opts_t opts = {
        .device     = argv[1],
        .baudrate   = XYLEM_SERIAL_BAUDRATE_115200,
        .databits   = XYLEM_SERIAL_DATABITS_8,
        .stopbits   = XYLEM_SERIAL_STOPBITS_1,
        .parity     = XYLEM_SERIAL_PARITY_NONE,
        .timeout_ms = 2000,
    };

    xylem_serial_t* serial = xylem_serial_open(&opts);
    if (!serial) {
        xylem_loge("failed to open %s", argv[1]);
        return 1;
    }

    const char* msg = "hello";
    int nw = xylem_serial_write(serial, msg, strlen(msg));
    if (nw < 0) {
        xylem_loge("write failed");
        xylem_serial_close(serial);
        return 1;
    }
    xylem_logi("sent: %s", msg);

    char buf[256];
    int nr = xylem_serial_read(serial, buf, sizeof(buf) - 1);
    if (nr > 0) {
        buf[nr] = '\0';
        xylem_logi("recv: %s", buf);
    } else if (nr == 0) {
        xylem_logw("recv: timeout, no data");
    } else {
        xylem_loge("read failed");
    }

    xylem_serial_close(serial);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
