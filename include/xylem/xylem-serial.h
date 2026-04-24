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

#include <stddef.h>
#include <stdint.h>

typedef struct xylem_serial_s xylem_serial_t;

/* Serial port baud rate. */
typedef enum xylem_serial_baudrate_e {
    XYLEM_SERIAL_BAUDRATE_9600,   /*< 9600 baud. */
    XYLEM_SERIAL_BAUDRATE_19200,  /*< 19200 baud. */
    XYLEM_SERIAL_BAUDRATE_38400,  /*< 38400 baud. */
    XYLEM_SERIAL_BAUDRATE_57600,  /*< 57600 baud. */
    XYLEM_SERIAL_BAUDRATE_115200, /*< 115200 baud. */
} xylem_serial_baudrate_t;

/* Serial port parity mode. */
typedef enum xylem_serial_parity_e {
    XYLEM_SERIAL_PARITY_NONE,   /*< No parity. */
    XYLEM_SERIAL_PARITY_ODD,    /*< Odd parity. */
    XYLEM_SERIAL_PARITY_EVEN,   /*< Even parity. */
} xylem_serial_parity_t;

/* Serial port data bits. */
typedef enum xylem_serial_databits_e {
    XYLEM_SERIAL_DATABITS_7,    /*< 7 data bits. */
    XYLEM_SERIAL_DATABITS_8,    /*< 8 data bits. */
} xylem_serial_databits_t;

/* Serial port stop bits. */
typedef enum xylem_serial_stopbits_e {
    XYLEM_SERIAL_STOPBITS_1,    /*< 1 stop bit. */
    XYLEM_SERIAL_STOPBITS_2,    /*< 2 stop bits. */
} xylem_serial_stopbits_t;

/* Serial port flow control. */
typedef enum xylem_serial_flowcontrol_e {
    XYLEM_SERIAL_FLOW_NONE,     /*< No flow control. */
    XYLEM_SERIAL_FLOW_HARDWARE, /*< Hardware (RTS/CTS). */
} xylem_serial_flowcontrol_t;

/* Serial port configuration. */
typedef struct xylem_serial_opts_s {
    const char*                  device;       /*< Device path ("COM3", "/dev/ttyUSB0"). */
    xylem_serial_baudrate_t      baudrate;     /*< Baud rate. */
    xylem_serial_parity_t        parity;       /*< Parity mode. */
    xylem_serial_databits_t      databits;     /*< Data bits. */
    xylem_serial_stopbits_t      stopbits;     /*< Stop bits. */
    xylem_serial_flowcontrol_t   flowcontrol;  /*< Flow control, default NONE. */
    uint32_t                     timeout_ms;   /*< Read timeout in ms, 0 = blocking. */
} xylem_serial_opts_t;

/**
 * @brief Open a serial port.
 *
 * Configures the serial port with the specified parameters and
 * returns an opaque handle for subsequent read/write operations.
 * All I/O through this handle is synchronous (blocking).
 *
 * @param opts  Serial port configuration. Must not be NULL.
 *              opts->device must not be NULL.
 *
 * @return Serial handle, or NULL on failure.
 */
extern xylem_serial_t* xylem_serial_open(xylem_serial_opts_t* opts);

/**
 * @brief Close a serial port.
 *
 * Releases the underlying OS handle and frees the serial object.
 * Safe to call with NULL. Idempotent (second call is a no-op).
 *
 * @param serial  Serial handle, or NULL.
 */
extern void xylem_serial_close(xylem_serial_t* serial);

/**
 * @brief Read data from a serial port.
 *
 * Blocks until at least one byte is available, the configured
 * timeout expires, or an error occurs. On timeout with no data
 * the return value is 0.
 *
 * @param serial  Serial handle.
 * @param buf     Buffer to read into.
 * @param len     Maximum number of bytes to read.
 *
 * @return Number of bytes read (may be less than len), 0 on
 *         timeout with no data, or -1 on error.
 */
extern int xylem_serial_read(xylem_serial_t* serial,
                             void* buf, size_t len);

/**
 * @brief Write data to a serial port.
 *
 * Blocks until all bytes are written or an error occurs.
 *
 * @param serial  Serial handle.
 * @param buf     Buffer containing data to write.
 * @param len     Number of bytes to write.
 *
 * @return Number of bytes written (always len on success), or -1
 *         on error.
 */
extern int xylem_serial_write(xylem_serial_t* serial,
                              const void* buf, size_t len);
