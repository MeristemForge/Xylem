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

#include "xylem/net/http/xylem-http.h"

#include "xylem/crypto/xylem-sha256.h"
#include "xylem/encoding/xylem-base64.h"
#include "xylem/encoding/xylem-gzip.h"
#include "xylem/sync/xylem-mutex.h"

#include "http-transport-tcp.h"
#include "http-transport-tls.h"
#include "http-utils.h"
#include "http.h"
#include "llhttp/llhttp.h"
#include "runtime/precond.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HTTP/1.1 client engine. The only server-side HTTP parser lives in WS. */

#define HTTP_IO_BUF_SIZE        4096
#define POOL_MAX_IDLE_PER_HOST  2
#define POOL_IDLE_TIMEOUT_MS    90000
#define POOL_MAX_HOSTS          100
#define HTTP_DEFAULT_BODY_LIMIT (16 * 1024 * 1024)

struct xylem_http_res_s {
    int32_t        status_code;
    http_header_t* headers;
    size_t         header_count;
    size_t         header_cap;
    uint8_t*       body;
    size_t         body_len;
    size_t         body_cap;
};

typedef struct _http_pool_idle_conn_s {
    http_transport_t transport;
    uint64_t         idle_since; /* xylem_utils_getnow(MSEC) timestamp */
} _http_pool_idle_conn_t;

typedef struct _http_pool_entry_s {
    char key[65]; /* SHA-256 of origin, proxy and TLS configuration. */
    _http_pool_idle_conn_t* conns; /* dynamic array of idle conns */
    size_t                  count;
    size_t                  cap;
} _http_pool_entry_t;

typedef struct {
    llhttp_t          parser;
    llhttp_settings_t settings;
    http_res_t*       res;
    char*             name;
    size_t            name_len, name_cap;
    char*             value;
    size_t            value_len, value_cap;
    size_t            limit, header_bytes;
    bool              complete, head;
} _http_cli_parser_t;

static _http_pool_entry_t _http_pool[POOL_MAX_HOSTS];

static size_t _http_pool_size;

static _Atomic(xylem_mutex_t*) _http_pool_mu;

static void _http_transport_close(http_transport_t* t) {
    t->close(t->conn);
}

static int
_http_transport_write(http_transport_t* t, const void* data, size_t len) {
    const char* p = data;
    while (len) {
        int n = len > INT_MAX ? INT_MAX : (int)len;
        if (t->write(t->conn, p, n) != 0) {
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

static _http_pool_entry_t* _http_pool_find_entry(const char* key) {
    for (size_t i = 0; i < _http_pool_size; i++) {
        if (strcmp(_http_pool[i].key, key) == 0) {
            return &_http_pool[i];
        }
    }
    return NULL;
}

static bool _http_pool_ensure_mu(void) {
    if (!atomic_load(&_http_pool_mu)) {
        xylem_mutex_t* created = xylem_mutex_create();
        if (!created) {
            return false;
        }
        xylem_mutex_t* expected = NULL;
        if (!atomic_compare_exchange_strong(
                &_http_pool_mu,
                &expected,
                created)) {
            xylem_mutex_destroy(created);
        }
    }
    return true;
}

/* Evicts stale connections while searching. */
static bool _http_pool_acquire(const char* key, http_transport_t* out) {
    if (!_http_pool_ensure_mu()) {
        return false;
    }
    xylem_mutex_lock(_http_pool_mu);

    _http_pool_entry_t* entry = _http_pool_find_entry(key);
    if (!entry || entry->count == 0) {
        xylem_mutex_unlock(_http_pool_mu);
        return false;
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    while (entry->count > 0) {
        size_t                  idx = entry->count - 1;
        _http_pool_idle_conn_t* ic  = &entry->conns[idx];

        if (now - ic->idle_since > POOL_IDLE_TIMEOUT_MS) {
            _http_transport_close(&ic->transport);
            entry->count--;
            continue;
        }

        *out = ic->transport;
        entry->count--;
        xylem_mutex_unlock(_http_pool_mu);
        return true;
    }

    xylem_mutex_unlock(_http_pool_mu);
    return false;
}

/* Evicts oldest if pool for this host is full. */
static void _http_pool_release(const char* key, http_transport_t* t) {
    if (!_http_pool_ensure_mu()) {
        _http_transport_close(t);
        return;
    }
    xylem_mutex_lock(_http_pool_mu);

    _http_pool_entry_t* entry = _http_pool_find_entry(key);
    if (!entry) {
        if (_http_pool_size >= POOL_MAX_HOSTS) {
            xylem_mutex_unlock(_http_pool_mu);
            _http_transport_close(t);
            return;
        }
        entry = &_http_pool[_http_pool_size++];
        memcpy(entry->key, key, strlen(key) + 1);
        entry->conns = NULL;
        entry->count = 0;
        entry->cap   = 0;
    }

    if (entry->count >= POOL_MAX_IDLE_PER_HOST) {
        _http_transport_close(&entry->conns[0].transport);
        memmove(
            &entry->conns[0],
            &entry->conns[1],
            (entry->count - 1) * sizeof(_http_pool_idle_conn_t));
        entry->count--;
    }

    if (entry->count >= entry->cap) {
        size_t new_cap = entry->cap == 0 ? 4 : entry->cap * 2;
        if (new_cap > POOL_MAX_IDLE_PER_HOST) {
            new_cap = POOL_MAX_IDLE_PER_HOST;
        }
        _http_pool_idle_conn_t* tmp = (_http_pool_idle_conn_t*)realloc(
            entry->conns,
            new_cap * sizeof(_http_pool_idle_conn_t));
        if (!tmp) {
            xylem_mutex_unlock(_http_pool_mu);
            _http_transport_close(t);
            return;
        }
        entry->conns = tmp;
        entry->cap   = new_cap;
    }

    entry->conns[entry->count].transport = *t;
    entry->conns[entry->count].idle_since =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    entry->count++;
    xylem_mutex_unlock(_http_pool_mu);
}

static int _http_header_append_field(
    char**      name,
    size_t*     name_len,
    size_t*     name_cap,
    const char* at,
    size_t      len) {
    if (len > SIZE_MAX - *name_len - 1) {
        return -1;
    }
    size_t needed = *name_len + len + 1;
    if (needed > *name_cap) {
        size_t new_cap = *name_cap ? *name_cap * 2 : 64;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        char* tmp = (char*)realloc(*name, new_cap);
        if (!tmp) {
            return -1;
        }
        *name     = tmp;
        *name_cap = new_cap;
    }
    memcpy(*name + *name_len, at, len);
    *name_len += len;
    (*name)[*name_len] = '\0';
    return 0;
}

static int _http_body_append(
    uint8_t**   body,
    size_t*     body_len,
    size_t*     body_cap,
    const char* at,
    size_t      len) {
    if (len > SIZE_MAX - *body_len) {
        return -1;
    }
    size_t needed = *body_len + len;
    if (needed > *body_cap) {
        size_t new_cap = *body_cap ? *body_cap * 2 : 4096;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        uint8_t* tmp = (uint8_t*)realloc(*body, new_cap);
        if (!tmp) {
            return -1;
        }
        *body     = tmp;
        *body_cap = new_cap;
    }
    if (len) {
        memcpy(*body + *body_len, at, len);
    }
    *body_len += len;
    return 0;
}

static int _http_cli_header_field_cb(llhttp_t* p, const char* at, size_t n) {
    _http_cli_parser_t* c = p->data;
    if (n > 65536 - c->header_bytes) {
        return -1;
    }
    c->header_bytes += n;
    return _http_header_append_field(
        &c->name,
        &c->name_len,
        &c->name_cap,
        at,
        n);
}

static int _http_cli_header_value_cb(llhttp_t* p, const char* at, size_t n) {
    _http_cli_parser_t* c = p->data;
    if (n > 65536 - c->header_bytes) {
        return -1;
    }
    c->header_bytes += n;
    return _http_header_append_field(
        &c->value,
        &c->value_len,
        &c->value_cap,
        at,
        n);
}

static int _http_cli_header_complete_cb(llhttp_t* p) {
    _http_cli_parser_t* c = p->data;
    if (c->res->header_count >= 256) {
        return -1;
    }
    int rc = http_header_add(
        &c->res->headers,
        &c->res->header_count,
        &c->res->header_cap,
        c->name ? c->name : "",
        c->name_len,
        c->value ? c->value : "",
        c->value_len);
    c->name_len = c->value_len = 0;
    return rc;
}

static int _http_cli_headers_complete_cb(llhttp_t* p) {
    _http_cli_parser_t* c = p->data;
    c->res->status_code   = p->status_code;
    return c->head ? 1 : 0;
}

static int _http_cli_body_data_cb(llhttp_t* p, const char* at, size_t n) {
    _http_cli_parser_t* c = p->data;
    if (n > c->limit - c->res->body_len) {
        return -1;
    }
    return _http_body_append(
        &c->res->body,
        &c->res->body_len,
        &c->res->body_cap,
        at,
        n);
}

static int _http_cli_message_complete_cb(llhttp_t* p) {
    ((_http_cli_parser_t*)p->data)->complete = true;
    return HPE_PAUSED;
}

static void _http_cli_parser_init(
    _http_cli_parser_t* c,
    http_res_t*         res,
    size_t              limit,
    bool                head) {
    memset(c, 0, sizeof(*c));
    c->res   = res;
    c->limit = limit;
    c->head  = head;
    llhttp_settings_init(&c->settings);
    c->settings.on_header_field          = _http_cli_header_field_cb;
    c->settings.on_header_value          = _http_cli_header_value_cb;
    c->settings.on_header_value_complete = _http_cli_header_complete_cb;
    c->settings.on_headers_complete      = _http_cli_headers_complete_cb;
    c->settings.on_body                  = _http_cli_body_data_cb;
    c->settings.on_message_complete      = _http_cli_message_complete_cb;
    llhttp_init(&c->parser, HTTP_RESPONSE, &c->settings);
    c->parser.data = c;
}

static void _http_cli_parser_deinit(_http_cli_parser_t* c) {
    free(c->name);
    free(c->value);
}

static void _http_cli_res_reset(http_res_t* res) {
    http_headers_free(res->headers, res->header_count);
    free(res->body);
    memset(res, 0, sizeof(*res));
}

static uint64_t _http_get_time_ms(void) {
    return xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
}

/* Keep bytes following informational responses in the same read. */
static int _http_cli_receive(
    http_transport_t*   t,
    _http_cli_parser_t* c,
    char*               buf,
    size_t*             pending,
    bool                expect,
    uint64_t            expect_deadline) {
    int informational = 0;
    for (;;) {
        int n;
        if (*pending) {
            n        = (int)*pending;
            *pending = 0;
        } else {
            n = t->read(t->conn, buf, HTTP_IO_BUF_SIZE);
            if (n <= 0) {
                if (expect && !c->header_bytes &&
                    _http_get_time_ms() >= expect_deadline) {
                    return 100;
                }
                if (n == 0) {
                    llhttp_errno_t end = llhttp_finish(&c->parser);
                    if ((end == HPE_OK || end == HPE_PAUSED) && c->complete) {
                        return 0;
                    }
                }
                return -1;
            }
        }
        llhttp_errno_t err = llhttp_execute(&c->parser, buf, (size_t)n);
        if (err != HPE_OK && err != HPE_PAUSED) {
            return -1;
        }
        if (!c->complete) {
            continue;
        }
        size_t used = (size_t)(llhttp_get_error_pos(&c->parser) - buf);
        if (err == HPE_PAUSED && used < (size_t)n) {
            *pending = (size_t)n - used;
            memmove(buf, buf + used, *pending);
        }
        int status = c->res->status_code;
        if (status >= 200) {
            return 0;
        }
        if (status == 101 || ++informational > 16) {
            return -1;
        }
        bool        got_continue = status == 100;
        size_t      limit        = c->limit;
        bool        head         = c->head;
        http_res_t* res          = c->res;
        _http_cli_parser_deinit(c);
        _http_cli_res_reset(res);
        _http_cli_parser_init(c, res, limit, head);
        if (expect && got_continue) {
            return 100;
        }
    }
}

static bool _http_pool_make_key(
    const http_url_t*            url,
    const xylem_http_cli_opts_t* opts,
    char                         key[65]) {
    const xylem_http_proxy_t* p = opts->proxy;
    const xylem_http_tls_t*   t =
        !strcmp(url->scheme, "https") ? opts->tls : NULL;
    const char* fields[] = {
        url->host,
        url->scheme,
        p ? p->host : NULL,
        p ? p->username : NULL,
        p ? p->password : NULL,
        t ? t->ca : NULL,
        t ? t->cert : NULL,
        t ? t->key : NULL};
    xylem_sha256_t* hash = xylem_sha256_create();
    if (!hash) {
        return false;
    }
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        size_t len = fields[i] ? strlen(fields[i]) + 1 : 0;
        xylem_sha256_update(hash, (const uint8_t*)&len, sizeof(len));
        if (len) {
            xylem_sha256_update(hash, (const uint8_t*)fields[i], len);
        }
    }
    uint32_t flags[] = {url->port, p ? p->port : 0, t && t->skip_verify};
    xylem_sha256_update(hash, (const uint8_t*)flags, sizeof(flags));
    uint8_t digest[32];
    xylem_sha256_final(hash, digest);
    xylem_sha256_destroy(hash);
    for (int i = 0; i < 32; ++i) {
        snprintf(key + i * 2, 3, "%02x", digest[i]);
    }
    return true;
}

static bool _http_url_same_origin(const http_url_t* a, const http_url_t* b) {
    return a->port == b->port && http_header_eq(a->host, b->host) &&
           !strcmp(a->scheme, b->scheme);
}

static int _http_url_resolve_redirect(
    const char*       location,
    const http_url_t* base,
    http_url_t*       out) {
    if (!location || !*location) {
        return -1;
    }
    if (strstr(location, "://")) {
        return http_url_parse(location, out);
    }
    char url[4096];
    if (location[0] == '/' && location[1] == '/') {
        int n = snprintf(url, sizeof(url), "%s:%s", base->scheme, location);
        return n < 0 || n >= sizeof(url) ? -1 : http_url_parse(url, out);
    }
    *out = *base;
    if (location[0] == '#') {
        return 0;
    }
    char combined[4096];
    if (location[0] == '/') {
        snprintf(combined, sizeof(combined), "%s", location);
    } else if (location[0] == '?') {
        size_t n = strcspn(base->path, "?");
        snprintf(
            combined,
            sizeof(combined),
            "%.*s%s",
            (int)n,
            base->path,
            location);
    } else {
        char base_path[2048];
        snprintf(base_path, sizeof(base_path), "%s", base->path);
        base_path[strcspn(base_path, "?")] = 0;
        char*  slash                       = strrchr(base_path, '/');
        size_t n   = slash ? (size_t)(slash - base_path + 1) : 1;
        int    len = snprintf(
            combined,
            sizeof(combined),
            "%.*s%s",
            (int)n,
            base_path,
            location);
        if (len < 0 || len >= sizeof(combined)) {
            return -1;
        }
    }
    combined[strcspn(combined, "#")] = 0;
    /* Remove dot segments from the path, preserving the query. */
    char* query        = strchr(combined, '?');
    char  suffix[2048] = "";
    if (query) {
        if (strlen(query) >= sizeof(suffix)) {
            return -1;
        }
        snprintf(suffix, sizeof(suffix), "%s", query);
        *query = 0;
    }
    size_t      pos = 0;
    const char* cur = combined;
    while (*cur) {
        if (*cur == '/') {
            ++cur;
        }
        const char* end = strchr(cur, '/');
        if (!end) {
            end = cur + strlen(cur);
        }
        size_t n = (size_t)(end - cur);
        if (n == 2 && cur[0] == '.' && cur[1] == '.') {
            while (pos && out->path[pos - 1] != '/') {
                --pos;
            }
            if (pos) {
                --pos;
            }
        } else if (n && !(n == 1 && cur[0] == '.')) {
            if (pos + n + 1 >= sizeof(out->path)) {
                return -1;
            }
            out->path[pos++] = '/';
            memcpy(out->path + pos, cur, n);
            pos += n;
        }
        cur = end;
    }
    if (!pos || (combined[0] && combined[strlen(combined) - 1] == '/')) {
        out->path[pos++] = '/';
    }
    if (pos + strlen(suffix) >= sizeof(out->path)) {
        return -1;
    }
    snprintf(out->path + pos, sizeof(out->path) - pos, "%s", suffix);
    return 0;
}

static int _http_cli_decompress(http_res_t* res, size_t limit) {
    const char* encoding =
        http_header_find(res->headers, res->header_count, "Content-Encoding");
    if (!encoding || !http_header_eq(encoding, "gzip") || !res->body_len) {
        return 0;
    }
    size_t cap = res->body_len < 4096 ? 4096 : res->body_len;
    if (cap > limit) {
        cap = limit;
    }
    for (;;) {
        uint8_t* buf = (uint8_t*)malloc(cap ? cap : 1);
        if (!buf) {
            return -1;
        }
        int n = xylem_gzip_decompress(res->body, res->body_len, buf, cap);
        if (n >= 0) {
            free(res->body);
            res->body     = buf;
            res->body_len = (size_t)n;
            res->body_cap = cap;
            return 0;
        }
        free(buf);
        if (cap == limit) {
            return -1;
        }
        cap = cap > limit / 2 ? limit : cap * 2;
    }
}

static bool _http_header_is_token(const char* s) {
    if (!s || !*s) {
        return false;
    }
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c))) {
            return false;
        }
    }
    return true;
}

xylem_http_res_t* http_do_request(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* options) {
    if (!_http_header_is_token(method) || strlen(method) > 64 || !url ||
        (body_len && !body) || (header_count && !headers) ||
        header_count > 256 || body_len > SIZE_MAX - 131072) {
        return NULL;
    }
    if (content_type &&
        (strchr(content_type, '\r') || strchr(content_type, '\n'))) {
        return NULL;
    }
    size_t header_size = 0;
    for (size_t i = 0; i < header_count; ++i) {
        if (!_http_header_is_token(headers[i].name) || !headers[i].value ||
            strchr(headers[i].value, '\r') || strchr(headers[i].value, '\n')) {
            return NULL;
        }
        size_t n = strlen(headers[i].name) + strlen(headers[i].value) + 4;
        if (n > 65536 - header_size) {
            return NULL;
        }
        header_size += n;
        /* The API sends a fixed-size body, never caller-specified framing. */
        if (http_header_eq(headers[i].name, "Transfer-Encoding")) {
            return NULL;
        }
        if (http_header_eq(headers[i].name, "Content-Length")) {
            char expected[32];
            snprintf(expected, sizeof(expected), "%zu", body_len);
            if (strcmp(headers[i].value, expected)) {
                return NULL;
            }
        }
    }
    http_url_t parsed;
    if (http_url_parse(url, &parsed)) {
        return NULL;
    }
    xylem_http_cli_opts_t base = {0};
    base.max_redirects         = 10;
    if (options) {
        base = *options;
    }
    if (base.max_redirects < 0) {
        return NULL;
    }
    size_t   limit = base.max_response_body ? base.max_response_body
                                            : HTTP_DEFAULT_BODY_LIMIT;
    uint64_t start = _http_get_time_ms();
    if (base.timeout_ms > UINT64_MAX - start) {
        return NULL;
    }
    uint64_t    deadline   = base.timeout_ms ? start + base.timeout_ms : 0;
    bool        strip_auth = false, rebuild_host = false, drop_entity = false;
    int         redirects  = 0;
    const char* cur_method = method;
    const void* cur_body   = body;
    size_t      cur_len    = body_len;
    const char* cur_type   = content_type;
    for (;;) {
        if (deadline && _http_get_time_ms() >= deadline) {
            return NULL;
        }
        char target[4096];
        if (http_url_serialize(&parsed, target, sizeof(target))) {
            return NULL;
        }
        xylem_http_proxy_t*   env  = NULL;
        xylem_http_cli_opts_t opts = base;
        if (!opts.proxy) {
            env        = http_proxy_from_env(target);
            opts.proxy = env;
        }
        bool secure = !strcmp(parsed.scheme, "https");
        bool proxy  = opts.proxy && opts.proxy->host && *opts.proxy->host;
        xylem_http_hdr_t* filtered = (xylem_http_hdr_t*)calloc(
            header_count + 1,
            sizeof(xylem_http_hdr_t));
        if (!filtered) {
            http_proxy_from_env_free(env);
            return NULL;
        }
        size_t count  = 0;
        bool   expect = base.expect_continue && cur_len > 0;
        for (size_t i = 0; i < header_count; ++i) {
            const char* name = headers[i].name;
            if (rebuild_host && http_header_eq(name, "Host")) {
                continue;
            }
            if (strip_auth && (http_header_eq(name, "Authorization") ||
                               http_header_eq(name, "Cookie") ||
                               http_header_eq(name, "X-API-Key") ||
                               http_header_eq(name, "API-Key"))) {
                continue;
            }
            if (http_header_eq(name, "Proxy-Authorization") &&
                (!proxy || secure || strip_auth)) {
                continue;
            }
            if (drop_entity && (http_header_eq(name, "Content-Length") ||
                                http_header_eq(name, "Content-Type") ||
                                http_header_eq(name, "Content-Encoding") ||
                                http_header_eq(name, "Expect"))) {
                continue;
            }
            if (http_header_eq(name, "Expect")) {
                expect = cur_len > 0 &&
                         http_header_eq(headers[i].value, "100-continue");
            }
            filtered[count++] = headers[i];
        }
        char* auth = NULL;
        if (proxy && !secure && opts.proxy->username) {
            const char* pass = opts.proxy->password ? opts.proxy->password : "";
            size_t      n    = strlen(opts.proxy->username) + strlen(pass) + 1;
            if (n > 16384) {
                free(filtered);
                http_proxy_from_env_free(env);
                return NULL;
            }
            char* raw = (char*)malloc(n + 1);
            int   cap = xylem_base64_encode_size((int)n);
            auth      = (char*)malloc((size_t)cap + 7);
            if (!raw || !auth) {
                free(raw);
                free(auth);
                free(filtered);
                http_proxy_from_env_free(env);
                return NULL;
            }
            snprintf(raw, n + 1, "%s:%s", opts.proxy->username, pass);
            memcpy(auth, "Basic ", 6);
            int written = xylem_base64_encode_std(
                (uint8_t*)raw,
                (int)n,
                (uint8_t*)auth + 6,
                cap);
            free(raw);
            if (written < 0) {
                free(auth);
                free(filtered);
                http_proxy_from_env_free(env);
                return NULL;
            }
            auth[6 + written] = 0;
            /* The selected proxy's credentials replace a caller header. */
            for (size_t i = 0; i < count;) {
                if (http_header_eq(filtered[i].name, "Proxy-Authorization")) {
                    filtered[i] = filtered[--count];
                } else {
                    ++i;
                }
            }
            filtered[count++] = (xylem_http_hdr_t){"Proxy-Authorization", auth};
        }
        char             key[65];
        http_transport_t t     = {0};
        bool             keyed = _http_pool_make_key(&parsed, &opts, key);
        if (keyed) {
            _http_pool_acquire(key, &t);
        }
        if (!t.conn) {
            uint64_t now = _http_get_time_ms();
            if (deadline && now >= deadline) {
                free(auth);
                free(filtered);
                http_proxy_from_env_free(env);
                return NULL;
            }
            uint64_t remaining = deadline ? deadline - now : 0;
            t = secure
                    ? http_tls_dial(parsed.host, parsed.port, remaining, &opts)
                    : http_tcp_dial(parsed.host, parsed.port, remaining, &opts);
        }
        http_proxy_from_env_free(env);
        if (!t.conn) {
            free(auth);
            free(filtered);
            return NULL;
        }
        if (t.set_rd_deadline) {
            t.set_rd_deadline(t.conn, deadline);
        }
        if (t.set_wr_deadline) {
            t.set_wr_deadline(t.conn, deadline);
        }
        size_t request_len = 0;
        char*  request     = http_req_serialize(
            cur_method,
            &parsed,
            cur_body,
            cur_len,
            cur_type,
            expect,
            proxy && !secure,
            &request_len,
            filtered,
            count);
        free(auth);
        free(filtered);
        if (!request) {
            t.close(t.conn);
            return NULL;
        }
        int rc = _http_transport_write(&t, request, request_len);
        free(request);
        if (rc) {
            t.close(t.conn);
            return NULL;
        }
        http_res_t* res = (http_res_t*)calloc(1, sizeof(http_res_t));
        if (!res) {
            t.close(t.conn);
            return NULL;
        }
        _http_cli_parser_t cp;
        _http_cli_parser_init(
            &cp,
            res,
            limit,
            http_header_eq(cur_method, "HEAD"));
        char   buf[HTTP_IO_BUF_SIZE];
        size_t pending = 0;
        if (expect) {
            uint64_t wait = _http_get_time_ms() + 1000;
            if (deadline && deadline < wait) {
                wait = deadline;
            }
            if (t.set_rd_deadline) {
                t.set_rd_deadline(t.conn, wait);
            }
            rc = _http_cli_receive(&t, &cp, buf, &pending, true, wait);
            if (rc == 100) {
                if (deadline && _http_get_time_ms() >= deadline) {
                    rc = -1;
                } else {
                    if (t.set_rd_deadline) {
                        t.set_rd_deadline(t.conn, deadline);
                    }
                    rc = _http_transport_write(&t, cur_body, cur_len);
                    if (!rc) {
                        rc =
                            _http_cli_receive(&t, &cp, buf, &pending, false, 0);
                    }
                }
            }
        } else {
            rc = _http_cli_receive(&t, &cp, buf, &pending, false, 0);
        }
        bool keep = !rc && !pending && llhttp_should_keep_alive(&cp.parser) &&
                    !(deadline && _http_get_time_ms() >= deadline);
        _http_cli_parser_deinit(&cp);
        if (keep && keyed) {
            if (t.set_rd_deadline) {
                t.set_rd_deadline(t.conn, 0);
            }
            if (t.set_wr_deadline) {
                t.set_wr_deadline(t.conn, 0);
            }
            _http_pool_release(key, &t);
        } else {
            t.close(t.conn);
        }
        if (rc || (deadline && _http_get_time_ms() >= deadline)) {
            _http_cli_res_reset(res);
            free(res);
            return NULL;
        }
        int         status = res->status_code;
        const char* location =
            http_header_find(res->headers, res->header_count, "Location");
        if (location && redirects < base.max_redirects &&
            (status == 301 || status == 302 || status == 303 || status == 307 ||
             status == 308)) {
            http_url_t next;
            if (_http_url_resolve_redirect(location, &parsed, &next)) {
                _http_cli_res_reset(res);
                free(res);
                return NULL;
            }
            if (secure && !strcmp(next.scheme, "http") &&
                !base.allow_https_downgrade) {
                _http_cli_res_reset(res);
                free(res);
                return NULL;
            }
            if (!_http_url_same_origin(&parsed, &next)) {
                strip_auth   = true;
                rebuild_host = true;
            }
            if ((status == 303 && !http_header_eq(cur_method, "HEAD")) ||
                ((status == 301 || status == 302) &&
                 http_header_eq(cur_method, "POST"))) {
                cur_method  = "GET";
                cur_body    = NULL;
                cur_len     = 0;
                cur_type    = NULL;
                drop_entity = true;
            }
            parsed = next;
            ++redirects;
            _http_cli_res_reset(res);
            free(res);
            continue;
        }
        if (_http_cli_decompress(res, limit)) {
            _http_cli_res_reset(res);
            free(res);
            return NULL;
        }
        return res;
    }
}

void http_pool_clear(void) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_pool_clear");
    for (size_t i = 0; i < _http_pool_size; ++i) {
        for (size_t j = 0; j < _http_pool[i].count; ++j) {
            _http_transport_close(&_http_pool[i].conns[j].transport);
        }
        free(_http_pool[i].conns);
    }
    memset(_http_pool, 0, sizeof(_http_pool));
    _http_pool_size   = 0;
    xylem_mutex_t* mu = atomic_exchange(&_http_pool_mu, NULL);
    if (mu) {
        xylem_mutex_destroy(mu);
    }
}

int http_res_status(const xylem_http_res_t* res) {
    if (!res) {
        return 0;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_status");

    return res->status_code;
}

const char* http_res_header(const xylem_http_res_t* res, const char* name) {
    if (!res || !name) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_header");

    return http_header_find(res->headers, res->header_count, name);
}

const void* http_res_body(const xylem_http_res_t* res) {
    if (!res) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_body");

    return (const void*)res->body;
}

size_t http_res_body_len(const xylem_http_res_t* res) {
    if (!res) {
        return 0;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_body_len");

    return res->body_len;
}

void http_res_destroy(xylem_http_res_t* res) {
    if (!res) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_destroy");

    http_headers_free(res->headers, res->header_count);
    free(res->body);
    free(res);
}
