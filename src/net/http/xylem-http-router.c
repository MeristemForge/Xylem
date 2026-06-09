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

#include "xylem/net/http/xylem-http-router.h"

#include "http.h"
#include "runtime/precond.h"

#include <stdlib.h>
#include <string.h>

#define ROUTER_MAX_PARAMS 16

typedef struct _route_s {
    char*                   method;
    char*                   pattern;
    xylem_http_handler_fn_t handler;
    void*                   userdata;
} _route_t;

typedef struct _middleware_s {
    xylem_http_handler_fn_t handler;
    void*                   userdata;
} _middleware_t;

struct xylem_http_router_s {
    _route_t*               routes;
    size_t                  route_count;
    size_t                  route_cap;
    _middleware_t*          middlewares;
    size_t                  mw_count;
    size_t                  mw_cap;
    char*                   prefix;
    xylem_http_router_t*    parent;
    xylem_http_router_t**   groups;
    size_t                  group_count;
    size_t                  group_cap;
    xylem_http_handler_fn_t not_found_handler;
    void*                   not_found_userdata;
};

xylem_http_router_t* xylem_http_router_create(void) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_router_create");

    return (xylem_http_router_t*)calloc(1, sizeof(xylem_http_router_t));
}

void xylem_http_router_destroy(xylem_http_router_t* router) {
    if (!router) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_router_destroy");
    for (size_t i = 0; i < router->route_count; i++) {
        free(router->routes[i].method);
        free(router->routes[i].pattern);
    }
    free(router->routes);
    free(router->middlewares);
    free(router->prefix);
    for (size_t i = 0; i < router->group_count; i++) {
        xylem_http_router_destroy(router->groups[i]);
    }
    free(router->groups);
    free(router);
}

int xylem_http_router_handle(
    xylem_http_router_t*    router,
    const char*             method,
    const char*             pattern,
    xylem_http_handler_fn_t handler,
    void*                   userdata) {
    if (!router || !method || !pattern || !handler) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_router_handle");

    if (router->route_count >= router->route_cap) {
        size_t new_cap = router->route_cap ? router->route_cap * 2 : 8;
        _route_t* tmp = (_route_t*)realloc(
            router->routes, new_cap * sizeof(_route_t));
        if (!tmp) {
            return -1;
        }
        router->routes = tmp;
        router->route_cap = new_cap;
    }

    char* full_pattern = NULL;
    if (router->prefix) {
        size_t plen = strlen(router->prefix);
        size_t patlen = strlen(pattern);
        full_pattern = (char*)malloc(plen + patlen + 1);
        if (!full_pattern) {
            return -1;
        }
        memcpy(full_pattern, router->prefix, plen);
        memcpy(full_pattern + plen, pattern, patlen + 1);
    } else {
        full_pattern = (char*)malloc(strlen(pattern) + 1);
        if (!full_pattern) {
            return -1;
        }
        memcpy(full_pattern, pattern, strlen(pattern) + 1);
    }

    _route_t* r = &router->routes[router->route_count++];
    r->method   = (char*)malloc(strlen(method) + 1);
    r->pattern  = full_pattern;
    r->handler  = handler;
    r->userdata = userdata;
    if (!r->method) {
        free(full_pattern);
        router->route_count--;
        return -1;
    }
    memcpy(r->method, method, strlen(method) + 1);

    /* also add to parent if this is a group */
    if (router->parent) {
        return xylem_http_router_handle(
            router->parent, method, full_pattern, handler, userdata);
    }
    return 0;
}

int xylem_http_router_use(
    xylem_http_router_t*    router,
    xylem_http_handler_fn_t middleware,
    void*                   userdata) {
    if (!router || !middleware) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_router_use");

    if (router->mw_count >= router->mw_cap) {
        size_t new_cap = router->mw_cap ? router->mw_cap * 2 : 4;
        _middleware_t* tmp = (_middleware_t*)realloc(
            router->middlewares, new_cap * sizeof(_middleware_t));
        if (!tmp) {
            return -1;
        }
        router->middlewares = tmp;
        router->mw_cap = new_cap;
    }

    _middleware_t* mw = &router->middlewares[router->mw_count++];
    mw->handler  = middleware;
    mw->userdata = userdata;
    return 0;
}

xylem_http_router_t* xylem_http_router_group(
    xylem_http_router_t* parent,
    const char*          prefix) {
    if (!parent || !prefix) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_router_group");

    xylem_http_router_t* group =
        (xylem_http_router_t*)calloc(1, sizeof(xylem_http_router_t));
    if (!group) {
        return NULL;
    }

    size_t plen = parent->prefix ? strlen(parent->prefix) : 0;
    size_t nlen = strlen(prefix);
    group->prefix = (char*)malloc(plen + nlen + 1);
    if (!group->prefix) {
        free(group);
        return NULL;
    }
    if (parent->prefix) {
        memcpy(group->prefix, parent->prefix, plen);
    }
    memcpy(group->prefix + plen, prefix, nlen + 1);
    group->parent = parent;

    if (parent->group_count >= parent->group_cap) {
        size_t new_cap = parent->group_cap ? parent->group_cap * 2 : 4;
        xylem_http_router_t** tmp = (xylem_http_router_t**)realloc(
            parent->groups, new_cap * sizeof(xylem_http_router_t*));
        if (!tmp) {
            free(group->prefix);
            free(group);
            return NULL;
        }
        parent->groups = tmp;
        parent->group_cap = new_cap;
    }
    parent->groups[parent->group_count++] = group;
    return group;
}

static bool _match_route(const char* pattern, const char* path,
                         http_router_param_t* params, size_t* param_count) {
    *param_count = 0;
    const char* pp = pattern;
    const char* up = path;

    while (*pp && *up) {
        if (*pp == ':') {
            pp++;
            const char* key_start = pp;
            while (*pp && *pp != '/') {
                pp++;
            }
            size_t key_len = (size_t)(pp - key_start);

            const char* val_start = up;
            while (*up && *up != '/') {
                up++;
            }
            size_t val_len = (size_t)(up - val_start);

            if (*param_count >= ROUTER_MAX_PARAMS) {
                return false;
            }
            http_router_param_t* p = &params[*param_count];
            p->key = (char*)malloc(key_len + 1);
            p->value = (char*)malloc(val_len + 1);
            if (!p->key || !p->value) {
                free(p->key);
                free(p->value);
                return false;
            }
            memcpy(p->key, key_start, key_len);
            p->key[key_len] = '\0';
            memcpy(p->value, val_start, val_len);
            p->value[val_len] = '\0';
            (*param_count)++;
            continue;
        }

        if (*pp == '*') {
            pp++;
            const char* key_start = pp;
            while (*pp) {
                pp++;
            }
            size_t key_len = (size_t)(pp - key_start);
            size_t val_len = strlen(up);

            if (*param_count >= ROUTER_MAX_PARAMS) {
                return false;
            }
            http_router_param_t* p = &params[*param_count];
            p->key = (char*)malloc(key_len + 1);
            p->value = (char*)malloc(val_len + 1);
            if (!p->key || !p->value) {
                free(p->key);
                free(p->value);
                return false;
            }
            memcpy(p->key, key_start, key_len);
            p->key[key_len] = '\0';
            memcpy(p->value, up, val_len);
            p->value[val_len] = '\0';
            (*param_count)++;
            return true;
        }

        if (*pp != *up) {
            return false;
        }
        pp++;
        up++;
    }

    return *pp == '\0' && *up == '\0';
}

static void _free_params(http_router_param_t* params, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(params[i].key);
        free(params[i].value);
    }
}

typedef struct _mw_chain_s {
    _middleware_t*          middlewares;
    size_t                  mw_count;
    size_t                  index;
    xylem_http_handler_fn_t handler;
    void*                   handler_userdata;
} _mw_chain_t;

void xylem_http_router_next(xylem_http_res_t* res, xylem_http_req_t* req_pub) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_router_next");

    http_req_t* req = (http_req_t*)req_pub;
    _mw_chain_t* chain = (_mw_chain_t*)req->_mw_chain;
    if (!chain) {
        return;
    }
    if (chain->index < chain->mw_count) {
        _middleware_t* mw = &chain->middlewares[chain->index++];
        mw->handler(res, req_pub, mw->userdata);
    } else if (chain->handler) {
        chain->handler(res, req_pub, chain->handler_userdata);
    }
}

static void _router_dispatch(xylem_http_res_t* res,
                             xylem_http_req_t* req_pub,
                             void* userdata) {
    http_req_t* req = (http_req_t*)req_pub;
    xylem_http_router_t* router = (xylem_http_router_t*)userdata;
    const char* method = xylem_http_req_method(req_pub);
    const char* url    = xylem_http_req_url(req_pub);

    /* match route */
    http_router_param_t params[ROUTER_MAX_PARAMS];
    size_t param_count = 0;
    _route_t* matched = NULL;

    for (size_t i = 0; i < router->route_count; i++) {
        _route_t* route = &router->routes[i];
        if (strcmp(route->method, method) != 0) {
            continue;
        }
        param_count = 0;
        if (_match_route(route->pattern, url, params, &param_count)) {
            matched = route;
            break;
        }
        _free_params(params, param_count);
    }

    /* build middleware chain: middlewares -> final handler */
    _mw_chain_t chain;
    chain.middlewares     = router->middlewares;
    chain.mw_count       = router->mw_count;
    chain.index          = 0;

    if (matched) {
        chain.handler          = matched->handler;
        chain.handler_userdata = matched->userdata;
        req->router_params      = params;
        req->router_param_count = param_count;
    } else {
        chain.handler          = router->not_found_handler;
        chain.handler_userdata = router->not_found_userdata;
    }

    void* prev_chain = req->_mw_chain;
    req->_mw_chain = &chain;

    /* kick off the chain */
    xylem_http_router_next(res, req_pub);

    /* if no middleware/handler responded and no route matched, send default 404 */
    if (!matched && !chain.handler && !((http_res_t*)res)->_headers_sent) {
        xylem_http_res_set_status(res, 404);
        xylem_http_res_write(res, "Not Found", 9);
    }

    req->_mw_chain = prev_chain;
    if (matched) {
        req->router_params      = NULL;
        req->router_param_count = 0;
        _free_params(params, param_count);
    }
}

const char* xylem_http_router_param(
    const xylem_http_req_t* req_pub,
    const char*             name) {
    const http_req_t* req = (const http_req_t*)req_pub;
    if (!req || !name || !req->router_params) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_router_param");

    for (size_t i = 0; i < req->router_param_count; i++) {
        if (strcmp(req->router_params[i].key, name) == 0) {
            return req->router_params[i].value;
        }
    }
    return NULL;
}

xylem_http_handler_fn_t xylem_http_router_handler(
    xylem_http_router_t* router) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_router_handler");

    (void)router;
    return _router_dispatch;
}

void* xylem_http_router_handler_userdata(
    xylem_http_router_t* router) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_router_handler_userdata");

    return (void*)router;
}

void xylem_http_router_set_not_found(
    xylem_http_router_t*    router,
    xylem_http_handler_fn_t handler,
    void*                   userdata) {
    if (!router) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_router_set_not_found");

    router->not_found_handler  = handler;
    router->not_found_userdata = userdata;
}
