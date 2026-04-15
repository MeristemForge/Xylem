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

#include "platform/platform-serial.h"

void platform_serial_close(platform_serial_t fd) {
    CloseHandle(fd);
}

int platform_serial_read(platform_serial_t fd, void* buf, size_t len) {
    DWORD bytes_read = 0;
    if (!ReadFile(fd, buf, (DWORD)len, &bytes_read, NULL)) {
        return -1;
    }
    return (int)bytes_read;
}

int platform_serial_write(platform_serial_t fd,
                          const void* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        DWORD written = 0;
        DWORD chunk   = (DWORD)(len - off);
        if (!WriteFile(fd, (const char*)buf + off, chunk, &written, NULL)) {
            return -1;
        }
        off += (size_t)written;
    }
    return (int)off;
}

platform_serial_t platform_serial_open(platform_serial_config_t* config) {
    HANDLE h = CreateFileA(
        config->device,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return PLATFORM_SERIAL_INVALID;
    }

    DCB dcb;
    SecureZeroMemory(&dcb, sizeof(DCB));
    dcb.DCBlength = sizeof(DCB);
    dcb.fBinary   = TRUE;

    switch (config->baudrate) {
    case PLATFORM_SERIAL_BAUDRATE_9600:
        dcb.BaudRate = CBR_9600;
        break;
    case PLATFORM_SERIAL_BAUDRATE_19200:
        dcb.BaudRate = CBR_19200;
        break;
    case PLATFORM_SERIAL_BAUDRATE_38400:
        dcb.BaudRate = CBR_38400;
        break;
    case PLATFORM_SERIAL_BAUDRATE_57600:
        dcb.BaudRate = CBR_57600;
        break;
    case PLATFORM_SERIAL_BAUDRATE_115200:
        dcb.BaudRate = CBR_115200;
        break;
    default:
        CloseHandle(h);
        return PLATFORM_SERIAL_INVALID;
    }

    switch (config->databits) {
    case PLATFORM_SERIAL_DATABITS_7:
        dcb.ByteSize = 7;
        break;
    case PLATFORM_SERIAL_DATABITS_8:
        dcb.ByteSize = 8;
        break;
    default:
        CloseHandle(h);
        return PLATFORM_SERIAL_INVALID;
    }

    switch (config->stopbits) {
    case PLATFORM_SERIAL_STOPBITS_1:
        dcb.StopBits = ONESTOPBIT;
        break;
    case PLATFORM_SERIAL_STOPBITS_2:
        dcb.StopBits = TWOSTOPBITS;
        break;
    default:
        CloseHandle(h);
        return PLATFORM_SERIAL_INVALID;
    }

    switch (config->parity) {
    case PLATFORM_SERIAL_PARITY_NONE:
        dcb.Parity = NOPARITY;
        break;
    case PLATFORM_SERIAL_PARITY_ODD:
        dcb.fParity = TRUE;
        dcb.Parity  = ODDPARITY;
        break;
    case PLATFORM_SERIAL_PARITY_EVEN:
        dcb.fParity = TRUE;
        dcb.Parity  = EVENPARITY;
        break;
    default:
        CloseHandle(h);
        return PLATFORM_SERIAL_INVALID;
    }

    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        return PLATFORM_SERIAL_INVALID;
    }

    COMMTIMEOUTS timeouts;
    SecureZeroMemory(&timeouts, sizeof(COMMTIMEOUTS));

    if (config->timeout_ms > 0) {
        /* Return after timeout_ms even if no data arrived. */
        timeouts.ReadIntervalTimeout        = 0;
        timeouts.ReadTotalTimeoutMultiplier  = 0;
        timeouts.ReadTotalTimeoutConstant    = (DWORD)config->timeout_ms;
    } else {
        /*
         * Block until at least 1 byte arrives (matches Unix VMIN=1).
         * MAXDWORD interval + MAXDWORD multiplier + constant in [1, MAXDWORD-1]
         * tells Windows to wait indefinitely for the first byte, then return
         * immediately with whatever is available.
         */
        timeouts.ReadIntervalTimeout        = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier  = MAXDWORD;
        timeouts.ReadTotalTimeoutConstant    = MAXDWORD - 1;
    }

    if (!SetCommTimeouts(h, &timeouts)) {
        CloseHandle(h);
        return PLATFORM_SERIAL_INVALID;
    }
    return h;
}
