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
 * Interactive Serial Terminal
 *
 * Opens a serial port and provides a bidirectional interactive terminal.
 * A reader thread continuously prints incoming data, while the main
 * thread reads lines from stdin and sends them over the serial port.
 * Type "quit" or press Ctrl+C to exit.
 *
 * Connect TX to RX (loopback) or pair with a real serial device.
 *
 * Usage: serial-echo <device> [baudrate]
 *   Windows:  serial-echo COM3 115200
 *   Linux:    serial-echo /dev/ttyUSB0 9600
 *   macOS:    serial-echo /dev/cu.usbserial-0001
 *
 * Supported baud rates: 9600, 19200, 38400, 57600, 115200 (default)
 */

#include "xylem.h"
#include "xylem/xylem-serial.h"

#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic bool _running = true;

static void _signal_handler(int sig) {
    (void)sig;
    _running = false;
}

typedef struct {
    xylem_serial_t* serial;
} _reader_ctx_t;

/* Reader thread: continuously read from serial and print to stdout. */
static int _reader_thread(void* arg) {
    _reader_ctx_t* ctx = (_reader_ctx_t*)arg;
    char buf[256];

    while (_running) {
        int nr = xylem_serial_read(ctx->serial, buf, sizeof(buf) - 1);
        if (nr > 0) {
            buf[nr] = '\0';
            printf("\r[recv] %s\n> ", buf);
            fflush(stdout);
        } else if (nr < 0) {
            if (_running) {
                xylem_loge("serial read error");
            }
            break;
        }
        /* nr == 0: timeout, loop again */
    }
    return 0;
}

static const uint32_t _baudrate_values[] = {
    [XYLEM_SERIAL_BAUDRATE_9600]   = 9600,
    [XYLEM_SERIAL_BAUDRATE_19200]  = 19200,
    [XYLEM_SERIAL_BAUDRATE_38400]  = 38400,
    [XYLEM_SERIAL_BAUDRATE_57600]  = 57600,
    [XYLEM_SERIAL_BAUDRATE_115200] = 115200,
};

static xylem_serial_baudrate_t _parse_baudrate(const char* str) {
    char* end  = NULL;
    long  val  = strtol(str, &end, 10);
    if (end == str || *end != '\0') {
        fprintf(stderr, "invalid baudrate \"%s\", using 115200\n", str);
        return XYLEM_SERIAL_BAUDRATE_115200;
    }
    switch (val) {
    case 9600:   return XYLEM_SERIAL_BAUDRATE_9600;
    case 19200:  return XYLEM_SERIAL_BAUDRATE_19200;
    case 38400:  return XYLEM_SERIAL_BAUDRATE_38400;
    case 57600:  return XYLEM_SERIAL_BAUDRATE_57600;
    case 115200: return XYLEM_SERIAL_BAUDRATE_115200;
    default:
        fprintf(stderr, "unsupported baudrate %ld, using 115200\n", val);
        return XYLEM_SERIAL_BAUDRATE_115200;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <device> [baudrate]\n"
                "  defaults: 115200 baud, 8N1\n",
                argv[0]);
        return 1;
    }

    signal(SIGINT, _signal_handler);
    signal(SIGTERM, _signal_handler);

    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    xylem_serial_baudrate_t baudrate = XYLEM_SERIAL_BAUDRATE_115200;
    if (argc >= 3) {
        baudrate = _parse_baudrate(argv[2]);
    }

    xylem_serial_opts_t opts = {
        .device     = argv[1],
        .baudrate   = baudrate,
        .databits   = XYLEM_SERIAL_DATABITS_8,
        .stopbits   = XYLEM_SERIAL_STOPBITS_1,
        .parity     = XYLEM_SERIAL_PARITY_NONE,
        .timeout_ms = 500,
    };

    xylem_serial_t* serial = xylem_serial_open(&opts);
    if (!serial) {
        xylem_loge("failed to open %s", argv[1]);
        xylem_logger_deinit();
        xylem_cleanup();
        return 1;
    }

    xylem_logi("opened %s -- %" PRIu32 " 8N1, type lines to send, "
               "\"quit\" to exit",
               argv[1], _baudrate_values[baudrate]);

    _reader_ctx_t reader_ctx = {.serial = serial};
    thrd_t reader;
    if (thrd_create(&reader, _reader_thread, &reader_ctx) != thrd_success) {
        xylem_loge("failed to create reader thread");
        xylem_serial_close(serial);
        xylem_logger_deinit();
        xylem_cleanup();
        return 1;
    }

    /* Main thread: read lines from stdin and send over serial. */
    char line[256];
    while (_running) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }

        /* Strip trailing newline. */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (len == 0) {
            continue;
        }

        if (strcmp(line, "quit") == 0) {
            break;
        }

        int nw = xylem_serial_write(serial, line, len);
        if (nw < 0) {
            xylem_loge("write failed");
            break;
        }
        printf("[send] %s\n", line);
    }

    _running = false;
    thrd_join(reader, NULL);

    xylem_serial_close(serial);
    xylem_logi("bye");
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
