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

#include "xylem/net/xylem-uds.h"

#include "xylem/encoding/xylem-varint.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "platform/platform-socket.h"
#include "runtime/iowait.h"
#include "runtime/runtime.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_READ_BUF_SIZE 65536
#define UDS_MAX_PATH          104

struct xylem_uds_conn_s {
    iowait_t*              waiter;
    platform_sock_t        fd;
    xylem_uds_frame_opts_t frame_opts;
    char*                  read_buf;
    size_t                 read_buf_cap;
    size_t                 read_buf_pos;
    size_t                 read_buf_len;
    _Atomic int32_t        refcnt;
    _Atomic bool           closed;
};

struct xylem_uds_listener_s {
    iowait_t*       waiter;
    platform_sock_t fd;
    char            path[UDS_MAX_PATH];
    _Atomic int32_t refcnt;
    _Atomic bool    closed;
};

static void _uds_conn_ref(xylem_uds_conn_t* uds) {
    atomic_fetch_add_explicit(&uds->refcnt, 1, memory_order_relaxed);
}

static void _uds_conn_unref(xylem_uds_conn_t* uds) {
    if (atomic_fetch_sub_explicit(&uds->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }
    if (uds->waiter) {
        iowait_destroy(uds->waiter);
    }
    if (uds->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        shutdown(uds->fd, PLATFORM_SHUT_WR);
        platform_socket_close(uds->fd);
    }
    free(uds->read_buf);
    free(uds);
}

static void _uds_listener_ref(xylem_uds_listener_t* ln) {
    atomic_fetch_add_explicit(&ln->refcnt, 1, memory_order_relaxed);
}

static void _uds_listener_unref(xylem_uds_listener_t* ln) {
    if (atomic_fetch_sub_explicit(&ln->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }
    if (ln->waiter) {
        iowait_destroy(ln->waiter);
    }
    if (ln->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(ln->fd);
    }
    free(ln);
}

static xylem_uds_conn_t* _uds_conn_alloc(platform_sock_t fd) {
    xylem_uds_conn_t* uds
        = (xylem_uds_conn_t*)calloc(1, sizeof(xylem_uds_conn_t));
    if (!uds) {
        return NULL;
    }

    uds->fd     = fd;
    uds->waiter = iowait_create(fd);
    if (!uds->waiter) {
        free(uds);
        return NULL;
    }

    uds->read_buf_cap = DEFAULT_READ_BUF_SIZE;

    _uds_conn_ref(uds);
    return uds;
}

static int64_t _uds_raw_recv(xylem_uds_conn_t* uds, void* buf, size_t len) {
    if (atomic_load_explicit(&uds->closed, memory_order_acquire)) {
        return -1;
    }

    for (;;) {
        ssize_t n = platform_socket_recv(uds->fd, buf, (int)len);
        if (n > 0) {
            return n;
        }
        if (n == 0) {
            return 0;
        }

        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN
            && err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            xylem_loge("uds fd=%d recv error: %s",
                       (int)uds->fd, platform_socket_tostring(err));
            return -1;
        }

        iowait_result_t r = iowait_read(uds->waiter);
        if (r != IOWAIT_READY
            || atomic_load_explicit(&uds->closed, memory_order_acquire)) {
            return -1;
        }
    }
}

static int _uds_read_exact(xylem_uds_conn_t* uds, void* buf, size_t len) {
    char*  ptr = (char*)buf;
    size_t rem = len;

    while (rem > 0) {
        size_t avail = uds->read_buf_len - uds->read_buf_pos;
        if (avail > 0) {
            size_t copy = avail < rem ? avail : rem;
            memcpy(ptr, uds->read_buf + uds->read_buf_pos, copy);
            uds->read_buf_pos += copy;
            ptr += copy;
            rem -= copy;
            continue;
        }

        uds->read_buf_pos = 0;
        uds->read_buf_len = 0;

        int64_t n = _uds_raw_recv(uds, uds->read_buf, uds->read_buf_cap);
        if (n <= 0) {
            return -1;
        }
        uds->read_buf_len = (size_t)n;
    }
    return 0;
}

static int64_t
_uds_recv_fixed(xylem_uds_conn_t* uds, void* buf, size_t len) {
    size_t frame_len = uds->frame_opts.fixed.len;
    if (frame_len > len) {
        xylem_loge("uds fd=%d recv: fixed frame %zu exceeds buffer %zu",
                   (int)uds->fd, frame_len, len);
        return -1;
    }
    if (_uds_read_exact(uds, buf, frame_len) != 0) {
        return -1;
    }
    return (int64_t)frame_len;
}

static int64_t
_uds_recv_length(xylem_uds_conn_t* uds, void* buf, size_t len) {
    uint8_t  hdr[16];
    uint32_t hdr_sz = uds->frame_opts.length.header_size;

    if (hdr_sz > sizeof(hdr)) {
        xylem_loge("uds fd=%d recv: header_size %u exceeds limit",
                   (int)uds->fd, hdr_sz);
        return -1;
    }

    if (_uds_read_exact(uds, hdr, hdr_sz) != 0) {
        return -1;
    }

    uint64_t body_len = 0;

    if (uds->frame_opts.length.coding == XYLEM_UDS_LENGTH_VARINT) {
        size_t pos = (size_t)uds->frame_opts.length.field_offset;
        if (!xylem_varint_decode(hdr, hdr_sz, &pos, &body_len)) {
            xylem_loge("uds fd=%d recv: varint decode failed",
                       (int)uds->fd);
            return -1;
        }
    } else {
        uint8_t* field = hdr + uds->frame_opts.length.field_offset;

        if (uds->frame_opts.length.big_endian) {
            for (uint32_t i = 0; i < uds->frame_opts.length.field_size;
                 i++) {
                body_len = (body_len << 8) | field[i];
            }
        } else {
            for (uint32_t i = 0; i < uds->frame_opts.length.field_size;
                 i++) {
                body_len |= (uint64_t)field[i] << (i * 8);
            }
        }
    }

    int64_t adjusted
        = (int64_t)body_len + uds->frame_opts.length.adjustment;
    if (adjusted < 0) {
        xylem_loge("uds fd=%d recv: negative payload length", (int)uds->fd);
        return -1;
    }

    size_t payload_len = (size_t)adjusted;
    if (payload_len > len) {
        xylem_loge("uds fd=%d recv: payload %zu exceeds buffer %zu",
                   (int)uds->fd, payload_len, len);
        return -1;
    }

    if (payload_len > 0 && _uds_read_exact(uds, buf, payload_len) != 0) {
        return -1;
    }
    return (int64_t)payload_len;
}

static int64_t
_uds_recv_delimiter(xylem_uds_conn_t* uds, void* buf, size_t len) {
    const char* delim     = uds->frame_opts.delimiter.delim;
    size_t      delim_len = uds->frame_opts.delimiter.delim_len;
    if (delim_len == 0) {
        delim_len = strlen(delim);
    }

    char*  dst = (char*)buf;
    size_t pos = 0;

    while (pos < len) {
        size_t avail = uds->read_buf_len - uds->read_buf_pos;
        if (avail == 0) {
            uds->read_buf_pos = 0;
            uds->read_buf_len = 0;
            int64_t n
                = _uds_raw_recv(uds, uds->read_buf, uds->read_buf_cap);
            if (n <= 0) {
                return -1;
            }
            uds->read_buf_len = (size_t)n;
            avail             = (size_t)n;
        }

        char* src = uds->read_buf + uds->read_buf_pos;
        for (size_t i = 0; i < avail && pos < len; i++) {
            dst[pos++] = src[i];
            uds->read_buf_pos++;

            if (pos >= delim_len
                && memcmp(dst + pos - delim_len, delim, delim_len) == 0) {
                pos -= delim_len;
                dst[pos] = '\0';
                return (int64_t)pos;
            }
        }
    }

    xylem_loge("uds fd=%d recv: delimiter not found within buffer",
               (int)uds->fd);
    return -1;
}

static int
_uds_raw_send(xylem_uds_conn_t* uds, const void* data, size_t len) {
    if (atomic_load_explicit(&uds->closed, memory_order_acquire)) {
        return -1;
    }

    const char* ptr = (const char*)data;
    size_t      rem = len;

    while (rem > 0) {
        ssize_t n = platform_socket_send(uds->fd, ptr, (int)rem);
        if (n > 0) {
            ptr += n;
            rem -= (size_t)n;
            continue;
        }

        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN
            && err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            xylem_loge("uds fd=%d send error: %s",
                       (int)uds->fd, platform_socket_tostring(err));
            return -1;
        }

        iowait_result_t r = iowait_write(uds->waiter);
        if (r != IOWAIT_READY
            || atomic_load_explicit(&uds->closed, memory_order_acquire)) {
            return -1;
        }
    }
    return 0;
}

static int
_uds_send_length(xylem_uds_conn_t* uds, const void* data, size_t len) {
    uint8_t  hdr[16];
    uint32_t hdr_sz = uds->frame_opts.length.header_size;

    if (hdr_sz > sizeof(hdr)) {
        xylem_loge("uds fd=%d send: header_size %u exceeds limit",
                   (int)uds->fd, hdr_sz);
        return -1;
    }

    int64_t wire_len = (int64_t)len - uds->frame_opts.length.adjustment;
    if (wire_len < 0) {
        xylem_loge("uds fd=%d send: negative wire length", (int)uds->fd);
        return -1;
    }

    memset(hdr, 0, hdr_sz);

    if (uds->frame_opts.length.coding == XYLEM_UDS_LENGTH_VARINT) {
        size_t pos = (size_t)uds->frame_opts.length.field_offset;
        if (!xylem_varint_encode(
                (uint64_t)wire_len, hdr, hdr_sz, &pos)) {
            xylem_loge("uds fd=%d send: varint encode failed",
                       (int)uds->fd);
            return -1;
        }
        if (_uds_raw_send(uds, hdr, pos) != 0) {
            return -1;
        }
    } else {
        uint8_t* field = hdr + uds->frame_opts.length.field_offset;
        uint64_t val   = (uint64_t)wire_len;

        if (uds->frame_opts.length.big_endian) {
            for (int32_t i
                 = (int32_t)uds->frame_opts.length.field_size - 1;
                 i >= 0;
                 i--) {
                field[i] = (uint8_t)(val & 0xFF);
                val >>= 8;
            }
        } else {
            for (uint32_t i = 0;
                 i < uds->frame_opts.length.field_size;
                 i++) {
                field[i] = (uint8_t)(val & 0xFF);
                val >>= 8;
            }
        }

        if (_uds_raw_send(uds, hdr, hdr_sz) != 0) {
            return -1;
        }
    }
    return _uds_raw_send(uds, data, len);
}

xylem_uds_listener_t* xylem_uds_listen(const char* path) {
    if (!path || strlen(path) >= UDS_MAX_PATH) {
        xylem_loge("uds listen: path is NULL or too long (max %d)",
                   UDS_MAX_PATH - 1);
        return NULL;
    }

    platform_sock_t fd = platform_socket_listen_unix(path, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("uds listen: socket creation failed for %s", path);
        return NULL;
    }

    xylem_uds_listener_t* listener = (xylem_uds_listener_t*)calloc(
        1, sizeof(xylem_uds_listener_t));
    if (!listener) {
        platform_socket_close(fd);
        return NULL;
    }

    listener->fd = fd;
    snprintf(listener->path, UDS_MAX_PATH, "%s", path);

    listener->waiter = iowait_create(fd);
    if (!listener->waiter) {
        platform_socket_close(fd);
        free(listener);
        return NULL;
    }

    _uds_listener_ref(listener);
    return listener;
}

xylem_uds_conn_t* xylem_uds_accept(xylem_uds_listener_t* listener) {
    _uds_listener_ref(listener);

    xylem_uds_conn_t* result = NULL;
    uint64_t          backoff_ms = 5;

    for (;;) {
        if (atomic_load_explicit(
                &listener->closed, memory_order_acquire)) {
            break;
        }

        platform_sock_t fd
            = platform_socket_accept_unix(listener->fd, true);
        if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN
                || err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                if (iowait_read(listener->waiter) != IOWAIT_READY) {
                    break;
                }
                continue;
            }

            xylem_logw("uds listener fd=%d accept error=%d (%s)",
                       (int)listener->fd,
                       err,
                       platform_socket_tostring(err));
            runtime_sleep(backoff_ms);
            if (backoff_ms < 1000) {
                backoff_ms *= 2;
            }
            continue;
        }

        backoff_ms = 5;

        xylem_uds_conn_t* uds = _uds_conn_alloc(fd);
        if (!uds) {
            platform_socket_close(fd);
            continue;
        }

        result = uds;
        break;
    }

    _uds_listener_unref(listener);
    return result;
}

void xylem_uds_close_listener(xylem_uds_listener_t* listener) {
    if (atomic_exchange(&listener->closed, true)) {
        return;
    }

    iowait_close(listener->waiter);

    if (listener->path[0] != '\0') {
        remove(listener->path);
    }

    _uds_listener_unref(listener);
}

xylem_uds_conn_t* xylem_uds_dial(
    const char* path,
    uint64_t    connect_timeout_ms) {
    if (!path || strlen(path) >= UDS_MAX_PATH) {
        xylem_loge("uds dial: path is NULL or too long (max %d)",
                   UDS_MAX_PATH - 1);
        return NULL;
    }

    bool connected = false;
    platform_sock_t fd
        = platform_socket_dial_unix(path, &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("uds dial: socket creation failed for %s", path);
        return NULL;
    }

    xylem_uds_conn_t* uds = _uds_conn_alloc(fd);
    if (!uds) {
        platform_socket_close(fd);
        return NULL;
    }

    if (!connected) {
        if (connect_timeout_ms > 0) {
            uint64_t deadline
                = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                  + connect_timeout_ms;
            iowait_set_wr_deadline(uds->waiter, deadline);
        }
        iowait_result_t r = iowait_write(uds->waiter);
        iowait_set_wr_deadline(uds->waiter, 0);

        if (r != IOWAIT_READY) {
            xylem_loge("uds dial: connect timeout for %s", path);
            xylem_uds_close(uds);
            return NULL;
        }

        int32_t   err    = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen);
        if (err != 0) {
            xylem_loge("uds dial fd=%d connect error=%d (%s)",
                       (int)fd,
                       err,
                       platform_socket_tostring(err));
            xylem_uds_close(uds);
            return NULL;
        }
    }

    return uds;
}

void xylem_uds_set_framing(
    xylem_uds_conn_t* uds, xylem_uds_frame_opts_t* opts) {
    if (opts) {
        uds->frame_opts = *opts;
    } else {
        memset(&uds->frame_opts, 0, sizeof(uds->frame_opts));
    }
}

void xylem_uds_set_read_deadline(
    xylem_uds_conn_t* uds, uint64_t deadline_ms) {
    iowait_set_rd_deadline(uds->waiter, deadline_ms);
}

void xylem_uds_set_write_deadline(
    xylem_uds_conn_t* uds, uint64_t deadline_ms) {
    iowait_set_wr_deadline(uds->waiter, deadline_ms);
}

int64_t
xylem_uds_recv(xylem_uds_conn_t* uds, void* buf, size_t len) {
    _uds_conn_ref(uds);

    if (uds->frame_opts.type == XYLEM_UDS_FRAME_NONE) {
        int64_t ret = _uds_raw_recv(uds, buf, len);
        _uds_conn_unref(uds);
        return ret;
    }

    if (!uds->read_buf) {
        uds->read_buf = (char*)malloc(uds->read_buf_cap);
        if (!uds->read_buf) {
            _uds_conn_unref(uds);
            return -1;
        }
    }

    int64_t ret;
    switch (uds->frame_opts.type) {
    case XYLEM_UDS_FRAME_FIXED:
        ret = _uds_recv_fixed(uds, buf, len);
        break;
    case XYLEM_UDS_FRAME_LENGTH:
        ret = _uds_recv_length(uds, buf, len);
        break;
    case XYLEM_UDS_FRAME_DELIMITER:
        ret = _uds_recv_delimiter(uds, buf, len);
        break;
    default:
        ret = -1;
        break;
    }
    _uds_conn_unref(uds);
    return ret;
}

int xylem_uds_send(xylem_uds_conn_t* uds, const void* data, size_t len) {
    _uds_conn_ref(uds);
    int ret;
    switch (uds->frame_opts.type) {
    case XYLEM_UDS_FRAME_LENGTH:
        ret = _uds_send_length(uds, data, len);
        break;
    default:
        ret = _uds_raw_send(uds, data, len);
        break;
    }
    _uds_conn_unref(uds);
    return ret;
}

void xylem_uds_close(xylem_uds_conn_t* uds) {
    if (atomic_exchange(&uds->closed, true)) {
        return;
    }
    iowait_close(uds->waiter);
    _uds_conn_unref(uds);
}

int xylem_uds_shutdown_wr(xylem_uds_conn_t* uds) {
    return shutdown(uds->fd, PLATFORM_SHUT_WR) == 0 ? 0 : -1;
}

int xylem_uds_shutdown_rd(xylem_uds_conn_t* uds) {
    return shutdown(uds->fd, PLATFORM_SHUT_RD) == 0 ? 0 : -1;
}
