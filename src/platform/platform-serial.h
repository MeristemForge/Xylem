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

#if defined(_WIN32)
#undef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
typedef HANDLE platform_serial_t;
#define PLATFORM_SERIAL_INVALID  INVALID_HANDLE_VALUE
#else
typedef int    platform_serial_t;
#define PLATFORM_SERIAL_INVALID  -1
#endif

typedef enum platform_serial_baudrate_e {
    PLATFORM_SERIAL_BAUDRATE_9600,
    PLATFORM_SERIAL_BAUDRATE_19200,
    PLATFORM_SERIAL_BAUDRATE_38400,
    PLATFORM_SERIAL_BAUDRATE_57600,
    PLATFORM_SERIAL_BAUDRATE_115200,
} platform_serial_baudrate_t;

typedef enum platform_serial_parity_e {
    PLATFORM_SERIAL_PARITY_NONE,
    PLATFORM_SERIAL_PARITY_ODD,
    PLATFORM_SERIAL_PARITY_EVEN,
} platform_serial_parity_t;

typedef enum platform_serial_databits_e {
    PLATFORM_SERIAL_DATABITS_7,
    PLATFORM_SERIAL_DATABITS_8,
} platform_serial_databits_t;

typedef enum platform_serial_stopbits_e {
    PLATFORM_SERIAL_STOPBITS_1,
    PLATFORM_SERIAL_STOPBITS_2,
} platform_serial_stopbits_t;

typedef struct platform_serial_config_s {
    const char*                device;
    platform_serial_baudrate_t baudrate;
    platform_serial_parity_t   parity;
    platform_serial_databits_t databits;
    platform_serial_stopbits_t stopbits;
    uint32_t                   timeout_ms;
} platform_serial_config_t;

/**
 * @brief Open and configure a serial port.
 *
 * @param config  Serial port configuration.
 *
 * @return Platform serial handle, or PLATFORM_SERIAL_INVALID on failure.
 */
extern platform_serial_t platform_serial_open(platform_serial_config_t* config);

/**
 * @brief Close a serial port.
 *
 * @param fd  Platform serial handle.
 */
extern void platform_serial_close(platform_serial_t fd);

/**
 * @brief Read data from a serial port.
 *
 * Blocks until data is available, timeout expires, or error.
 *
 * @param fd   Platform serial handle.
 * @param buf  Buffer to read into.
 * @param len  Maximum bytes to read.
 *
 * @return Bytes read, 0 on timeout, or -1 on error.
 */
extern int platform_serial_read(platform_serial_t fd, void* buf, size_t len);

/**
 * @brief Write all data to a serial port.
 *
 * Loops internally until all bytes are written or error.
 *
 * @param fd   Platform serial handle.
 * @param buf  Buffer containing data to write.
 * @param len  Number of bytes to write.
 *
 * @return Bytes written (always len on success), or -1 on error.
 */
extern int platform_serial_write(platform_serial_t fd,
                                 const void* buf, size_t len);
