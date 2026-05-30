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

#include "xylem/net/http/xylem-http-cookie.h"

#include <stdlib.h>
#include <string.h>

typedef struct _cookie_s {
    char*            domain;
    char*            path;
    char*            name;
    char*            value;
    uint64_t         expires_ms;
    bool             secure;
    bool             http_only;
    struct _cookie_s* next;
} _cookie_t;

struct xylem_http_cookie_jar_s {
    _cookie_t* cookies;
};

xylem_http_cookie_jar_t* xylem_http_cookie_jar_create(void) {
    xylem_http_cookie_jar_t* jar =
        (xylem_http_cookie_jar_t*)calloc(1, sizeof(xylem_http_cookie_jar_t));
    return jar;
}

static void _cookie_free(_cookie_t* c) {
    free(c->domain);
    free(c->path);
    free(c->name);
    free(c->value);
    free(c);
}

static void _cookie_jar_clear(xylem_http_cookie_jar_t* jar) {
    if (!jar) {
        return;
    }
    _cookie_t* c = jar->cookies;
    while (c) {
        _cookie_t* next = c->next;
        _cookie_free(c);
        c = next;
    }
    jar->cookies = NULL;
}

void xylem_http_cookie_jar_destroy(xylem_http_cookie_jar_t* jar) {
    if (!jar) {
        return;
    }
    _cookie_jar_clear(jar);
    free(jar);
}

static char* _strdup_n(const char* s, size_t len) {
    char* d = (char*)malloc(len + 1);
    if (!d) {
        return NULL;
    }
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

static void _parse_url_domain_path(const char* url,
                                   char** domain, char** path) {
    *domain = NULL;
    *path   = NULL;
    const char* p = strstr(url, "://");
    if (!p) {
        return;
    }
    p += 3;
    const char* slash = strchr(p, '/');
    if (slash) {
        *domain = _strdup_n(p, (size_t)(slash - p));
        *path   = _strdup_n(slash, strlen(slash));
    } else {
        *domain = _strdup_n(p, strlen(p));
        *path   = _strdup_n("/", 1);
    }
}

int xylem_http_cookie_jar_set(
    xylem_http_cookie_jar_t* jar,
    const char*              url,
    const char*              name,
    const char*              value) {
    if (!jar || !url || !name || !value) {
        return -1;
    }

    char* domain = NULL;
    char* path   = NULL;
    _parse_url_domain_path(url, &domain, &path);
    if (!domain) {
        return -1;
    }

    /* update existing cookie if name+domain match */
    _cookie_t* c = jar->cookies;
    while (c) {
        if (strcmp(c->domain, domain) == 0 && strcmp(c->name, name) == 0) {
            free(c->value);
            c->value = _strdup_n(value, strlen(value));
            free(c->path);
            c->path = path;
            free(domain);
            return 0;
        }
        c = c->next;
    }

    /* insert new cookie */
    _cookie_t* nc = (_cookie_t*)calloc(1, sizeof(_cookie_t));
    if (!nc) {
        free(domain);
        free(path);
        return -1;
    }
    nc->domain = domain;
    nc->path   = path;
    nc->name   = _strdup_n(name, strlen(name));
    nc->value  = _strdup_n(value, strlen(value));
    nc->next   = jar->cookies;
    jar->cookies = nc;
    return 0;
}

const char* xylem_http_cookie_jar_get(
    const xylem_http_cookie_jar_t* jar,
    const char*                    url,
    const char*                    name) {
    if (!jar || !url || !name) {
        return NULL;
    }

    char* domain = NULL;
    char* path   = NULL;
    _parse_url_domain_path(url, &domain, &path);
    if (!domain) {
        return NULL;
    }

    const char* result = NULL;
    _cookie_t* c = jar->cookies;
    while (c) {
        if (strcmp(c->domain, domain) == 0 && strcmp(c->name, name) == 0) {
            result = c->value;
            break;
        }
        c = c->next;
    }

    free(domain);
    free(path);
    return result;
}
