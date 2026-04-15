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

struct xylem_serial_s {
    platform_serial_t fd;
    bool              closed;
};

xylem_serial_t* xylem_serial_open(xylem_serial_opts_t* opts) {
    if (!opts || !opts->device) {
        xylem_loge("serial: opts or device is NULL");
        return NULL;
    }
    platform_serial_baudrate_t baudrate;
    switch (opts->baudrate) {
    case XYLEM_SERIAL_BAUDRATE_9600:
        baudrate = PLATFORM_SERIAL_BAUDRATE_9600;
        break;
    case XYLEM_SERIAL_BAUDRATE_19200:
        baudrate = PLATFORM_SERIAL_BAUDRATE_19200;
        break;
    case XYLEM_SERIAL_BAUDRATE_38400:
        baudrate = PLATFORM_SERIAL_BAUDRATE_38400;
        break;
    case XYLEM_SERIAL_BAUDRATE_57600:
        baudrate = PLATFORM_SERIAL_BAUDRATE_57600;
        break;
    case XYLEM_SERIAL_BAUDRATE_115200:
        baudrate = PLATFORM_SERIAL_BAUDRATE_115200;
        break;
    default:
        xylem_loge("serial: invalid baudrate %d", (int)opts->baudrate);
        return NULL;
    }

    platform_serial_parity_t parity;
    switch (opts->parity) {
    case XYLEM_SERIAL_PARITY_NONE:
        parity = PLATFORM_SERIAL_PARITY_NONE;
        break;
    case XYLEM_SERIAL_PARITY_ODD:
        parity = PLATFORM_SERIAL_PARITY_ODD;
        break;
    case XYLEM_SERIAL_PARITY_EVEN:
        parity = PLATFORM_SERIAL_PARITY_EVEN;
        break;
    default:
        xylem_loge("serial: invalid parity %d", (int)opts->parity);
        return NULL;
    }

    platform_serial_databits_t databits;
    switch (opts->databits) {
    case XYLEM_SERIAL_DATABITS_7:
        databits = PLATFORM_SERIAL_DATABITS_7;
        break;
    case XYLEM_SERIAL_DATABITS_8:
        databits = PLATFORM_SERIAL_DATABITS_8;
        break;
    default:
        xylem_loge("serial: invalid databits %d", (int)opts->databits);
        return NULL;
    }

    platform_serial_stopbits_t stopbits;
    switch (opts->stopbits) {
    case XYLEM_SERIAL_STOPBITS_1:
        stopbits = PLATFORM_SERIAL_STOPBITS_1;
        break;
    case XYLEM_SERIAL_STOPBITS_2:
        stopbits = PLATFORM_SERIAL_STOPBITS_2;
        break;
    default:
        xylem_loge("serial: invalid stopbits %d", (int)opts->stopbits);
        return NULL;
    }

    platform_serial_config_t config = {
        .device     = opts->device,
        .baudrate   = baudrate,
        .parity     = parity,
        .databits   = databits,
        .stopbits   = stopbits,
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
