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

#include "xylem/xylem-serial.h"
#include "xylem/xylem-logger.h"

#include "platform/platform-serial.h"

#include <stdbool.h>
#include <stdlib.h>

static const uint32_t _serial_baudrate_map[] = {
    [XYLEM_SERIAL_BAUDRATE_9600]   = 9600,
    [XYLEM_SERIAL_BAUDRATE_19200]  = 19200,
    [XYLEM_SERIAL_BAUDRATE_38400]  = 38400,
    [XYLEM_SERIAL_BAUDRATE_57600]  = 57600,
    [XYLEM_SERIAL_BAUDRATE_115200] = 115200,
};

struct xylem_serial_s {
    platform_serial_t fd;
    bool              closed;
};

xylem_serial_t* xylem_serial_open(xylem_serial_opts_t* opts) {
    if (!opts || !opts->device) {
        xylem_loge("serial: opts or device is NULL");
        return NULL;
    }
    if (opts->baudrate > XYLEM_SERIAL_BAUDRATE_115200) {
        xylem_loge("serial: invalid baudrate %d", (int)opts->baudrate);
        return NULL;
    }
    if (opts->parity > XYLEM_SERIAL_PARITY_EVEN) {
        xylem_loge("serial: invalid parity %d", (int)opts->parity);
        return NULL;
    }
    if (opts->databits > XYLEM_SERIAL_DATABITS_8) {
        xylem_loge("serial: invalid databits %d", (int)opts->databits);
        return NULL;
    }
    if (opts->stopbits > XYLEM_SERIAL_STOPBITS_2) {
        xylem_loge("serial: invalid stopbits %d", (int)opts->stopbits);
        return NULL;
    }

    platform_serial_config_t config = {
        .device     = opts->device,
        .baudrate   = _serial_baudrate_map[opts->baudrate],
        .databits   = (uint8_t)(opts->databits == XYLEM_SERIAL_DATABITS_7
                               ? PLATFORM_SERIAL_DATABITS_7
                               : PLATFORM_SERIAL_DATABITS_8),
        .stopbits   = (uint8_t)(opts->stopbits == XYLEM_SERIAL_STOPBITS_1
                               ? PLATFORM_SERIAL_STOPBITS_1
                               : PLATFORM_SERIAL_STOPBITS_2),
        .parity     = (uint8_t)opts->parity,
        .timeout_ms = opts->timeout_ms,
    };

    platform_serial_t fd = platform_serial_open(&config);
    if (fd == PLATFORM_SERIAL_INVALID) {
        xylem_loge("serial: failed to open %s", opts->device);
        return NULL;
    }

    xylem_serial_t* serial =
        (xylem_serial_t*)calloc(1, sizeof(xylem_serial_t));
    if (!serial) {
        platform_serial_close(fd);
        return NULL;
    }
    serial->fd     = fd;
    serial->closed = false;
    return serial;
}

void xylem_serial_close(xylem_serial_t* serial) {
    if (!serial) {
        return;
    }
    if (serial->closed) {
        return;
    }
    serial->closed = true;
    platform_serial_close(serial->fd);
    free(serial);
}

int xylem_serial_read(xylem_serial_t* serial, void* buf, size_t len) {
    if (!serial || serial->closed) {
        return -1;
    }
    return platform_serial_read(serial->fd, buf, len);
}

int xylem_serial_write(xylem_serial_t* serial,
                       const void* buf, size_t len) {
    if (!serial || serial->closed) {
        return -1;
    }
    return platform_serial_write(serial->fd, buf, len);
}
