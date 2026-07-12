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

#include "http-tunnel.h"

#include "xylem/encoding/xylem-base64.h"
#include "xylem/xylem-utils.h"

#include "runtime/iowait.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool _http_tunnel_is_again(int err) {
    return err == PLATFORM_SO_ERROR_EAGAIN
           || err == PLATFORM_SO_ERROR_EWOULDBLOCK;
}

static int _http_tunnel_write_all(
    iowait_t*       w,
    platform_sock_t fd,
    const void*     data,
    int             len) {
    const char* ptr = (const char*)data;
    int         rem = len;
    while (rem > 0) {
        int n = platform_socket_send(fd, ptr, rem);
        if (n > 0) {
            ptr += n;
            rem -= n;
            continue;
        }

        int err = platform_socket_get_lasterror();
        if (!_http_tunnel_is_again(err)) {
            return -1;
        }
        if (iowait_write(w) != IOWAIT_READY) {
            return -1;
        }
    }
    return 0;
}

static int _http_tunnel_read_some(
    iowait_t*       w,
    platform_sock_t fd,
    void*           buf,
    int             len) {
    for (;;) {
        int n = platform_socket_recv(fd, buf, len);
        if (n >= 0) {
            return n;
        }

        int err = platform_socket_get_lasterror();
        if (!_http_tunnel_is_again(err)) {
            return -1;
        }
        if (iowait_read(w) != IOWAIT_READY) {
            return -1;
        }
    }
}

static bool _http_tunnel_size_add(size_t* total, size_t add) {
    if (SIZE_MAX - *total < add) {
        return false;
    }
    *total += add;
    return true;
}

static char* _http_tunnel_append(char* p, const void* data, size_t len) {
    memcpy(p, data, len);
    return p + len;
}

static char* _http_tunnel_build_request(const char* target_host,
                                        uint16_t target_port,
                                        const char* username,
                                        const char* password,
                                        int* out_len) {
    char target_port_str[8];
    int  target_port_len = snprintf(
        target_port_str, sizeof(target_port_str), "%u", target_port);
    if (target_port_len < 0 || target_port_len >= (int)sizeof(target_port_str)) {
        return NULL;
    }

    char*  auth       = NULL;
    size_t auth_len   = 0;
    size_t target_len = strlen(target_host);

    if (username && password) {
        size_t ulen = strlen(username);
        size_t plen = strlen(password);
        if (SIZE_MAX - ulen < 1 || SIZE_MAX - ulen - 1 < plen) {
            return NULL;
        }

        size_t cred_len = ulen + 1 + plen;
        if (cred_len > (size_t)INT_MAX) {
            return NULL;
        }

        char* cred = (char*)malloc(cred_len);
        if (!cred) {
            return NULL;
        }
        memcpy(cred, username, ulen);
        cred[ulen] = ':';
        memcpy(cred + ulen + 1, password, plen);

        int encoded_len = xylem_base64_encode_size((int)cred_len);
        if (encoded_len < 0) {
            free(cred);
            return NULL;
        }

        auth = (char*)malloc((size_t)encoded_len + 1);
        if (!auth) {
            free(cred);
            return NULL;
        }

        int n = xylem_base64_encode_std(
            (const uint8_t*)cred, (int)cred_len,
            (uint8_t*)auth, encoded_len);
        free(cred);
        if (n != encoded_len) {
            free(auth);
            return NULL;
        }
        auth[encoded_len] = '\0';
        auth_len = (size_t)encoded_len;
    }

    size_t total = 0;
    bool ok = _http_tunnel_size_add(&total, strlen("CONNECT "))
              && _http_tunnel_size_add(&total, target_len)
              && _http_tunnel_size_add(&total, strlen(":"))
              && _http_tunnel_size_add(&total, (size_t)target_port_len)
              && _http_tunnel_size_add(&total, strlen(" HTTP/1.1\r\n"))
              && _http_tunnel_size_add(&total, strlen("Host: "))
              && _http_tunnel_size_add(&total, target_len)
              && _http_tunnel_size_add(&total, strlen(":"))
              && _http_tunnel_size_add(&total, (size_t)target_port_len)
              && _http_tunnel_size_add(&total, strlen("\r\n"));
    if (ok && auth) {
        ok = _http_tunnel_size_add(
                 &total, strlen("Proxy-Authorization: Basic "))
             && _http_tunnel_size_add(&total, auth_len)
             && _http_tunnel_size_add(&total, strlen("\r\n"));
    }
    ok = ok && _http_tunnel_size_add(&total, strlen("\r\n"));
    if (!ok || total > (size_t)INT_MAX) {
        free(auth);
        return NULL;
    }

    char* req = (char*)malloc(total + 1);
    if (!req) {
        free(auth);
        return NULL;
    }

    char* p = req;
    p = _http_tunnel_append(p, "CONNECT ", strlen("CONNECT "));
    p = _http_tunnel_append(p, target_host, target_len);
    p = _http_tunnel_append(p, ":", strlen(":"));
    p = _http_tunnel_append(p, target_port_str, (size_t)target_port_len);
    p = _http_tunnel_append(p, " HTTP/1.1\r\n", strlen(" HTTP/1.1\r\n"));
    p = _http_tunnel_append(p, "Host: ", strlen("Host: "));
    p = _http_tunnel_append(p, target_host, target_len);
    p = _http_tunnel_append(p, ":", strlen(":"));
    p = _http_tunnel_append(p, target_port_str, (size_t)target_port_len);
    p = _http_tunnel_append(p, "\r\n", strlen("\r\n"));
    if (auth) {
        p = _http_tunnel_append(
            p, "Proxy-Authorization: Basic ",
            strlen("Proxy-Authorization: Basic "));
        p = _http_tunnel_append(p, auth, auth_len);
        p = _http_tunnel_append(p, "\r\n", strlen("\r\n"));
    }
    p = _http_tunnel_append(p, "\r\n", strlen("\r\n"));
    *p = '\0';

    free(auth);
    *out_len = (int)total;
    return req;
}

static int _http_tunnel_handshake(iowait_t* w, platform_sock_t fd,
                                  const char* target_host,
                                  uint16_t target_port,
                                  const char* username,
                                  const char* password) {
    int   req_len = 0;
    char* req = _http_tunnel_build_request(
        target_host, target_port, username, password, &req_len);
    if (!req) {
        return -1;
    }

    if (_http_tunnel_write_all(w, fd, req, req_len) != 0) {
        free(req);
        return -1;
    }
    free(req);

    char resp[1024];
    size_t resp_len = 0;

    while (resp_len < sizeof(resp) - 1) {
        int n = _http_tunnel_read_some(
            w, fd, resp + resp_len, (int)(sizeof(resp) - 1 - resp_len));
        if (n <= 0) {
            return -1;
        }
        resp_len += (size_t)n;
        resp[resp_len] = '\0';
        if (strstr(resp, "\r\n\r\n")) {
            break;
        }
    }

    if (strncmp(resp, "HTTP/1.1 200", 12) != 0 &&
        strncmp(resp, "HTTP/1.0 200", 12) != 0) {
        return -1;
    }

    return 0;
}

platform_sock_t http_tunnel_connect(const char* proxy_host,
                                uint16_t proxy_port,
                                const char* target_host,
                                uint16_t target_port,
                                uint64_t timeout_ms,
                                const char* username,
                                const char* password) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", proxy_port);

    bool connected = false;
    platform_sock_t fd = platform_socket_dial(
        proxy_host, port_str, SOCK_STREAM, &connected, true, false);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }

    iowait_t* w = iowait_create(fd);
    if (!w) {
        platform_socket_close(fd);
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }

    if (timeout_ms > 0) {
        uint64_t deadline =
            xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms;
        iowait_set_wr_deadline(w, deadline);
        iowait_set_rd_deadline(w, deadline);
    }

    if (!connected) {
        iowait_result_t r = iowait_write(w);
        if (r != IOWAIT_READY) {
            iowait_destroy(w);
            platform_socket_close(fd);
            return PLATFORM_SO_ERROR_INVALID_SOCKET;
        }
    }

    if (_http_tunnel_handshake(w, fd, target_host, target_port,
                                      username, password) != 0) {
        iowait_destroy(w);
        platform_socket_close(fd);
        return PLATFORM_SO_ERROR_INVALID_SOCKET;
    }

    iowait_destroy(w);
    return fd;
}
