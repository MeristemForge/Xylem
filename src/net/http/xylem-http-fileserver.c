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

#include "xylem/net/http/xylem-http-fileserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _fileserver_ctx_s {
    char* root_dir;
    char* index_file;
} _fileserver_ctx_t;

static const char* _mime_type(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    dot++;
    if (strcmp(dot, "html") == 0 || strcmp(dot, "htm") == 0) {
        return "text/html";
    }
    if (strcmp(dot, "css") == 0) {
        return "text/css";
    }
    if (strcmp(dot, "js") == 0) {
        return "application/javascript";
    }
    if (strcmp(dot, "json") == 0) {
        return "application/json";
    }
    if (strcmp(dot, "png") == 0) {
        return "image/png";
    }
    if (strcmp(dot, "jpg") == 0 || strcmp(dot, "jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcmp(dot, "gif") == 0) {
        return "image/gif";
    }
    if (strcmp(dot, "svg") == 0) {
        return "image/svg+xml";
    }
    if (strcmp(dot, "ico") == 0) {
        return "image/x-icon";
    }
    if (strcmp(dot, "woff") == 0) {
        return "font/woff";
    }
    if (strcmp(dot, "woff2") == 0) {
        return "font/woff2";
    }
    if (strcmp(dot, "ttf") == 0) {
        return "font/ttf";
    }
    if (strcmp(dot, "txt") == 0) {
        return "text/plain";
    }
    if (strcmp(dot, "xml") == 0) {
        return "application/xml";
    }
    if (strcmp(dot, "pdf") == 0) {
        return "application/pdf";
    }
    if (strcmp(dot, "zip") == 0) {
        return "application/zip";
    }
    if (strcmp(dot, "wasm") == 0) {
        return "application/wasm";
    }
    return "application/octet-stream";
}

static bool _has_path_traversal(const char* path) {
    if (strstr(path, "..")) {
        return true;
    }
    return false;
}

static void _fileserver_handler(xylem_http_res_t* res,
                                xylem_http_req_t* req,
                                void* userdata) {
    _fileserver_ctx_t* ctx = (_fileserver_ctx_t*)userdata;
    const char* filepath = xylem_http_router_param(req, "filepath");
    if (!filepath) {
        filepath = "";
    }

    if (_has_path_traversal(filepath)) {
        xylem_http_res_set_status(res, 403);
        xylem_http_res_write(res, "Forbidden", 9);
        return;
    }

    size_t root_len = strlen(ctx->root_dir);
    size_t file_len = strlen(filepath);
    size_t idx_len  = ctx->index_file ? strlen(ctx->index_file) : 0;
    char* full_path = (char*)malloc(root_len + 1 + file_len + 1 + idx_len + 1);
    if (!full_path) {
        xylem_http_res_set_status(res, 500);
        xylem_http_res_write(res, "Internal Server Error", 21);
        return;
    }

    snprintf(full_path, root_len + 1 + file_len + 1,
             "%s/%s", ctx->root_dir, filepath);

    FILE* f = fopen(full_path, "rb");

    /* try index file if path is a directory or empty */
    if (!f && ctx->index_file &&
        (file_len == 0 || filepath[file_len - 1] == '/')) {
        snprintf(full_path, root_len + 1 + file_len + idx_len + 2,
                 "%s/%s%s", ctx->root_dir, filepath, ctx->index_file);
        f = fopen(full_path, "rb");
    }

    if (!f) {
        free(full_path);
        xylem_http_res_set_status(res, 404);
        xylem_http_res_write(res, "Not Found", 9);
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        free(full_path);
        xylem_http_res_set_status(res, 204);
        return;
    }

    void* data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        free(full_path);
        xylem_http_res_set_status(res, 500);
        xylem_http_res_write(res, "Internal Server Error", 21);
        return;
    }

    fread(data, 1, (size_t)size, f);
    fclose(f);

    const char* mime = _mime_type(full_path);
    xylem_http_serve_content(res, req, data, (size_t)size, mime);

    free(data);
    free(full_path);
}

int xylem_http_fileserver(
    xylem_http_router_t*                router,
    const char*                         url_prefix,
    const char*                         root_dir,
    const xylem_http_fileserver_opts_t* opts) {
    if (!router || !url_prefix || !root_dir) {
        return -1;
    }

    _fileserver_ctx_t* ctx =
        (_fileserver_ctx_t*)calloc(1, sizeof(_fileserver_ctx_t));
    if (!ctx) {
        return -1;
    }
    ctx->root_dir = (char*)malloc(strlen(root_dir) + 1);
    if (!ctx->root_dir) {
        free(ctx);
        return -1;
    }
    memcpy(ctx->root_dir, root_dir, strlen(root_dir) + 1);

    const char* index = "index.html";
    if (opts) {
        index = opts->index_file;
    }
    if (index) {
        ctx->index_file = (char*)malloc(strlen(index) + 1);
        if (ctx->index_file) {
            memcpy(ctx->index_file, index, strlen(index) + 1);
        }
    }

    /* build pattern: url_prefix followed by a catch-all filepath segment */
    size_t plen = strlen(url_prefix);
    char* pattern = (char*)malloc(plen + 10 + 1);
    if (!pattern) {
        free(ctx->root_dir);
        free(ctx->index_file);
        free(ctx);
        return -1;
    }
    snprintf(pattern, plen + 11, "%s/*filepath", url_prefix);

    int rc = xylem_http_router_handle(
        router, "GET", pattern, _fileserver_handler, ctx);
    if (rc == 0) {
        rc = xylem_http_router_handle(
            router, "HEAD", pattern, _fileserver_handler, ctx);
    }
    free(pattern);
    return rc;
}
