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

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

platform_serial_t platform_serial_open(platform_serial_config_t* config) {
    int fd = open(config->device, O_RDWR | O_NOCTTY);
    if (fd == -1) {
        return PLATFORM_SERIAL_INVALID;
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));

    speed_t speed;
    switch (config->baudrate) {
    case 9600:   speed = B9600;   break;
    case 19200:  speed = B19200;  break;
    case 38400:  speed = B38400;  break;
    case 57600:  speed = B57600;  break;
    case 115200: speed = B115200; break;
    default:
        close(fd);
        return PLATFORM_SERIAL_INVALID;
    }
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);

    tio.c_cflag &= ~CSIZE;
    switch (config->databits) {
    case PLATFORM_SERIAL_DATABITS_7: tio.c_cflag |= CS7; break;
    case PLATFORM_SERIAL_DATABITS_8: tio.c_cflag |= CS8; break;
    default:
        close(fd);
        return PLATFORM_SERIAL_INVALID;
    }

    switch (config->stopbits) {
    case PLATFORM_SERIAL_STOPBITS_1: tio.c_cflag &= ~CSTOPB; break;
    case PLATFORM_SERIAL_STOPBITS_2: tio.c_cflag |=  CSTOPB; break;
    default:
        close(fd);
        return PLATFORM_SERIAL_INVALID;
    }

    switch (config->parity) {
    case PLATFORM_SERIAL_PARITY_NONE:
        tio.c_cflag &= ~PARENB;
        break;
    case PLATFORM_SERIAL_PARITY_ODD:
        tio.c_cflag |= PARENB;
        tio.c_cflag |= PARODD;
        break;
    case PLATFORM_SERIAL_PARITY_EVEN:
        tio.c_cflag |= PARENB;
        tio.c_cflag &= ~PARODD;
        break;
    default:
        close(fd);
        return PLATFORM_SERIAL_INVALID;
    }

    tio.c_cflag |= CLOCAL | CREAD;

    switch (config->flowcontrol) {
    case PLATFORM_SERIAL_FLOW_HARDWARE:
        tio.c_cflag |= CRTSCTS;
        break;
    case PLATFORM_SERIAL_FLOW_NONE:
    default:
        break;
    }

    if (config->timeout_ms > 0) {
        /* VTIME unit is 1/10 second. Minimum 1 (100ms) to avoid busy-wait. */
        uint32_t vtime = config->timeout_ms / 100;
        if (vtime == 0) {
            vtime = 1;
        }
        if (vtime > 255) {
            vtime = 255;
        }
        tio.c_cc[VMIN]  = 0;
        tio.c_cc[VTIME] = (cc_t)vtime;
    } else {
        /* Block until at least 1 byte arrives. */
        tio.c_cc[VMIN]  = 1;
        tio.c_cc[VTIME] = 0;
    }

    tcflush(fd, TCIOFLUSH);
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        close(fd);
        return PLATFORM_SERIAL_INVALID;
    }
    return fd;
}

void platform_serial_close(platform_serial_t fd) {
    close(fd);
}

int platform_serial_read(platform_serial_t fd, void* buf, size_t len) {
    ssize_t n;
    do {
        n = read(fd, buf, len);
    } while (n == -1 && errno == EINTR);

    if (n < 0) {
        return -1;
    }
    return (int)n;
}

int platform_serial_write(platform_serial_t fd,
                          const void* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n;
        do {
            n = write(fd, (const char*)buf + off, len - off);
        } while (n == -1 && errno == EINTR);

        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return (int)off;
}
