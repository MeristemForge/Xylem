# HTTP Client Features Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Basic authentication, Expect/100-Continue, and multipart form-data builder to the Xylem HTTP client.

**Architecture:** All three features are additive — they extend `xylem_http_opts_t` with new fields and augment existing code paths in `http_req_serialize` and `http_do_request`. No new source files needed; changes go into `http-utils.c`, `http.c`, and `xylem-http.h`.

**Tech Stack:** C11, llhttp, xylem_base64_encode_std (already available), miniz (existing).

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `include/xylem/net/xylem-http.h` | Modify | Add `xylem_http_auth_t`, `xylem_http_multipart_builder_t` types and API |
| `src/net/http/http-utils.h` | Modify | Add `auth` param to `http_req_serialize` signature |
| `src/net/http/http-utils.c` | Modify | Inject Authorization header; multipart builder implementation |
| `src/net/http/http.c` | Modify | Pass auth to serializer; 100-Continue send/receive logic |
| `tests/test-http.c` | Modify | Add unit and integration tests for all three features |

---

## Task 1: Basic Authentication

**Files:**
- Modify: `include/xylem/net/xylem-http.h:296-312` (opts struct)
- Modify: `src/net/http/http-utils.h:109-114` (serialize signature)
- Modify: `src/net/http/http-utils.c:209-345` (serialize impl)
- Modify: `src/net/http/http.c:1369-1371` (call site)
- Modify: `tests/test-http.c`

### Step 1: Add auth type to public header

- [ ] In `include/xylem/net/xylem-http.h`, add before the `xylem_http_proxy_t` definition:

```c
typedef struct {
    const char* username; /**< Basic auth username. */
    const char* password; /**< Basic auth password. */
} xylem_http_auth_t;
```

- [ ] Add field to `xylem_http_opts_t`:

```c
typedef struct {
    uint64_t                    timeout_ms;    /**< Request timeout, 0 = default 30s. */
    int                         max_redirects; /**< 0 = no redirect following. */
    size_t                      max_body_size; /**< 0 = default 10 MiB. */
    const xylem_http_hdr_t*     headers;       /**< Custom request headers. */
    size_t                      header_count;  /**< Number of custom headers. */
    const char*                 range;         /**< Range header value, NULL = omit. */
    xylem_http_cookie_jar_t*    cookie_jar;    /**< NULL = no cookie management. */
    const xylem_http_proxy_t*   proxy;         /**< NULL = direct connection. */
    const xylem_http_auth_t*    auth;          /**< NULL = no authentication. */
} xylem_http_opts_t;
```

### Step 2: Update http_req_serialize to accept auth

- [ ] In `src/net/http/http-utils.h`, update the declaration:

```c
extern char* http_req_serialize(const char* method, const http_url_t* url,
                                const void* body, size_t body_len,
                                const char* content_type,
                                bool expect_continue, size_t* out_len,
                                const xylem_http_hdr_t* custom_headers,
                                size_t custom_header_count,
                                const xylem_http_auth_t* auth);
```

### Step 3: Implement auth header injection in http_req_serialize

- [ ] In `src/net/http/http-utils.c`, update the function signature to match.

- [ ] Add `"Authorization"` to `check_names` so user-supplied Authorization headers take precedence:

```c
const char* check_names[] = {
    "Host", "Content-Length", "Content-Type", "Connection", "Expect",
    "Authorization"
};
bool overridden[6];
size_t custom_est = http_header_scan(custom_headers, custom_header_count,
                                     check_names, overridden, 6);
bool host_overridden           = overridden[0];
bool content_length_overridden = overridden[1];
bool content_type_overridden   = overridden[2];
bool connection_overridden     = overridden[3];
bool expect_overridden         = overridden[4];
bool auth_overridden           = overridden[5];
```

- [ ] After the Connection header block and before the Expect block, add auth header generation:

```c
size_t auth_est = 0;
char* auth_b64 = NULL;
int auth_b64_len = 0;

if (!auth_overridden && auth && auth->username && auth->password) {
    size_t ulen = strlen(auth->username);
    size_t plen = strlen(auth->password);
    size_t cred_len = ulen + 1 + plen;
    char* cred = (char*)malloc(cred_len + 1);
    if (cred) {
        memcpy(cred, auth->username, ulen);
        cred[ulen] = ':';
        memcpy(cred + ulen + 1, auth->password, plen);
        cred[cred_len] = '\0';

        int enc_size = xylem_base64_encode_size((int)cred_len);
        auth_b64 = (char*)malloc((size_t)enc_size + 1);
        if (auth_b64) {
            auth_b64_len = xylem_base64_encode_std(
                (const uint8_t*)cred, (int)cred_len,
                (uint8_t*)auth_b64, enc_size);
            if (auth_b64_len > 0) {
                auth_b64[auth_b64_len] = '\0';
            } else {
                free(auth_b64);
                auth_b64 = NULL;
            }
        }
        free(cred);
    }
}
```

- [ ] Include `auth_est` in the buffer size estimate (before malloc):

```c
if (auth_b64) {
    auth_est = 21 + (size_t)auth_b64_len + 2;  /* "Authorization: Basic " + b64 + "\r\n" */
}
```

Add `auth_est` to `est`.

- [ ] Write the Authorization header into the buffer (after Connection, before Expect):

```c
if (auth_b64) {
    memcpy(buf + off, "Authorization: Basic ", 21);
    off += 21;
    memcpy(buf + off, auth_b64, (size_t)auth_b64_len);
    off += (size_t)auth_b64_len;
    buf[off++] = '\r';
    buf[off++] = '\n';
    free(auth_b64);
}
```

### Step 4: Update call site in http.c

- [ ] In `src/net/http/http.c`, update the `http_req_serialize` call in `http_do_request`:

```c
char* req_buf = http_req_serialize(
    cur_method, &parsed, cur_body, cur_body_len, cur_content_type,
    false, &req_len, custom_hdrs, custom_hdr_count,
    opts ? opts->auth : NULL);
```

### Step 5: Write integration test

- [ ] In `tests/test-http.c`, add a handler that checks Authorization:

```c
static void _auth_handler(xylem_http_res_t* res,
                          xylem_http_req_t* req,
                          void* userdata) {
    (void)userdata;
    const char* auth = xylem_http_req_header(req, "Authorization");
    if (!auth || strncmp(auth, "Basic ", 6) != 0) {
        xylem_http_res_set_status(res, 401);
        xylem_http_res_set_header(res, "WWW-Authenticate", "Basic realm=\"test\"");
        xylem_http_res_write(res, "Unauthorized", 12);
        return;
    }
    xylem_http_res_set_status(res, 200);
    xylem_http_res_set_header(res, "Content-Type", "text/plain");
    xylem_http_res_write(res, auth + 6, strlen(auth) - 6);
}
```

- [ ] Add the test function:

```c
static void _test_basic_auth_main(void* arg) {
    (void)arg;

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _auth_handler, NULL, NULL);
    ASSERT(srv != NULL);
    uint16_t port = xylem_http_srv_port(srv);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/secret", (unsigned)port);

    /* Without auth: expect 401 */
    xylem_http_res_t* r1 = xylem_http_get(url, NULL);
    ASSERT(r1 != NULL);
    ASSERT(xylem_http_res_status(r1) == 401);
    xylem_http_res_destroy(r1);

    /* With auth: expect 200 + echoed base64 */
    xylem_http_auth_t auth = {.username = "user", .password = "pass"};
    xylem_http_opts_t opts = {.auth = &auth};
    xylem_http_res_t* r2 = xylem_http_get(url, &opts);
    ASSERT(r2 != NULL);
    ASSERT(xylem_http_res_status(r2) == 200);
    /* "user:pass" -> base64 = "dXNlcjpwYXNz" */
    ASSERT(xylem_http_res_body_len(r2) == 12);
    ASSERT(memcmp(xylem_http_res_body(r2), "dXNlcjpwYXNz", 12) == 0);
    xylem_http_res_destroy(r2);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_basic_auth(void) {
    xylem_run(_test_basic_auth_main, NULL, NULL);
}
```

- [ ] Add `test_basic_auth()` to `main()`.

### Step 6: Build and run tests

- [ ] Run:

```bash
cmake --build build && ctest --test-dir build -R test-http --output-on-failure
```

Expected: All tests pass including new auth test.

### Step 7: Commit

```bash
git add include/xylem/net/xylem-http.h src/net/http/http-utils.h src/net/http/http-utils.c src/net/http/http.c tests/test-http.c
git commit -m "feat(http): add Basic authentication support to client"
```

---

## Task 2: Expect/100-Continue

**Files:**
- Modify: `include/xylem/net/xylem-http.h` (add field to opts)
- Modify: `src/net/http/http.c:1300-1472` (do_request logic)
- Modify: `tests/test-http.c`

### Step 1: Add expect_continue field to opts

- [ ] In `include/xylem/net/xylem-http.h`, add to `xylem_http_opts_t`:

```c
typedef struct {
    uint64_t                    timeout_ms;       /**< Request timeout, 0 = default 30s. */
    int                         max_redirects;    /**< 0 = no redirect following. */
    size_t                      max_body_size;    /**< 0 = default 10 MiB. */
    const xylem_http_hdr_t*     headers;          /**< Custom request headers. */
    size_t                      header_count;     /**< Number of custom headers. */
    const char*                 range;            /**< Range header value, NULL = omit. */
    xylem_http_cookie_jar_t*    cookie_jar;       /**< NULL = no cookie management. */
    const xylem_http_proxy_t*   proxy;            /**< NULL = direct connection. */
    const xylem_http_auth_t*    auth;             /**< NULL = no authentication. */
    bool                        expect_continue;  /**< Send Expect: 100-continue for bodies. */
} xylem_http_opts_t;
```

### Step 2: Implement 100-Continue in http_do_request

- [ ] In `src/net/http/http.c`, within `http_do_request`, replace the serialization + send block. The key change: when `opts->expect_continue && cur_body_len > 0`:

1. Serialize with `expect_continue=true` (headers only, no body)
2. Send headers
3. Read response: if 100, send body and continue reading final response; if 4xx/5xx, return that response
4. Timeout: if no response within 1 second, send body anyway

- [ ] Replace the block from `char* req_buf = http_req_serialize(...)` through `if (wrc != 0)` with:

```c
    bool use_expect = opts && opts->expect_continue && cur_body_len > 0;

    size_t req_len = 0;
    char* req_buf = http_req_serialize(
        cur_method, &parsed, cur_body, cur_body_len, cur_content_type,
        use_expect, &req_len, custom_hdrs, custom_hdr_count,
        opts ? opts->auth : NULL);

    free(merged_hdrs);
    free(cookie_val);

    if (!req_buf) {
        _transport_close(&transport);
        free(readbuf);
        return NULL;
    }

    int wrc = _transport_write(&transport, req_buf, (int)req_len);
    free(req_buf);
    if (wrc != 0) {
        _transport_close(&transport);
        free(readbuf);
        return NULL;
    }

    if (use_expect) {
        /* Wait up to 1 second for 100 Continue. */
        uint64_t expect_deadline =
            xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 1000;
        if (transport.set_rd_deadline) {
            transport.set_rd_deadline(transport.conn, expect_deadline);
        }

        xylem_http_res_t* interim = (xylem_http_res_t*)calloc(1, sizeof(*interim));
        if (!interim) {
            _transport_close(&transport);
            free(readbuf);
            return NULL;
        }
        _cli_parser_t icp;
        _cli_parser_init(&icp, interim);

        bool got_100 = false;
        bool got_final = false;
        bool timed_out = false;

        while (!icp.complete) {
            int n = _transport_read(&transport, readbuf, HTTP_IO_BUF_SIZE);
            if (n <= 0) {
                if (n == 0) {
                    _cli_parser_destroy(&icp);
                    _transport_close(&transport);
                    xylem_http_res_destroy(interim);
                    free(readbuf);
                    return NULL;
                }
                /* Timeout: send body anyway per RFC 7231. */
                timed_out = true;
                break;
            }
            llhttp_errno_t err = llhttp_execute(&icp.parser, readbuf, (size_t)n);
            if (err == HPE_PAUSED) {
                llhttp_resume(&icp.parser);
            } else if (err != HPE_OK) {
                _cli_parser_destroy(&icp);
                _transport_close(&transport);
                xylem_http_res_destroy(interim);
                free(readbuf);
                return NULL;
            }
        }

        if (icp.complete) {
            if (interim->status_code == 100) {
                got_100 = true;
            } else {
                got_final = true;
            }
        }

        _cli_parser_destroy(&icp);

        if (got_final) {
            /* Server rejected without wanting body. */
            bool ka = llhttp_should_keep_alive(&icp.parser) != 0;
            if (ka) {
                _pool_release(&parsed, &transport);
            } else {
                _transport_close(&transport);
            }
            free(readbuf);
            return interim;
        }

        xylem_http_res_destroy(interim);

        /* Restore original deadline for body send + response read. */
        if (transport.set_rd_deadline) {
            transport.set_rd_deadline(transport.conn, deadline_ms);
        }
        if (transport.set_wr_deadline) {
            transport.set_wr_deadline(transport.conn, deadline_ms);
        }

        /* Send body. */
        if (cur_body && cur_body_len > 0) {
            wrc = _transport_write(&transport, cur_body, (int)cur_body_len);
            if (wrc != 0) {
                _transport_close(&transport);
                free(readbuf);
                return NULL;
            }
        }
    }
```

### Step 3: Write integration test for 100-Continue success

- [ ] Add handler and test:

```c
static void _expect_handler(xylem_http_res_t* res,
                            xylem_http_req_t* req,
                            void* userdata) {
    (void)userdata;
    size_t blen = xylem_http_req_body_len(req);
    xylem_http_res_set_status(res, 200);
    xylem_http_res_set_header(res, "Content-Type", "text/plain");
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%zu", blen);
    xylem_http_res_write(res, buf, (size_t)n);
}

static void _test_expect_continue_main(void* arg) {
    (void)arg;

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _expect_handler, NULL, NULL);
    ASSERT(srv != NULL);
    uint16_t port = xylem_http_srv_port(srv);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/upload", (unsigned)port);

    char body[4096];
    memset(body, 'A', sizeof(body));

    xylem_http_opts_t opts = {.expect_continue = true};
    xylem_http_res_t* res = xylem_http_post(
        url, body, sizeof(body), "application/octet-stream", &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(memcmp(xylem_http_res_body(res), "4096", 4) == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_expect_continue(void) {
    xylem_run(_test_expect_continue_main, NULL, NULL);
}
```

- [ ] Add `test_expect_continue()` to `main()`.

### Step 4: Build and run tests

- [ ] Run:

```bash
cmake --build build && ctest --test-dir build -R test-http --output-on-failure
```

Expected: All tests pass.

### Step 5: Commit

```bash
git add include/xylem/net/xylem-http.h src/net/http/http.c tests/test-http.c
git commit -m "feat(http): implement Expect/100-Continue for client uploads"
```

---

## Task 3: Multipart Form-Data Builder

**Files:**
- Modify: `include/xylem/net/xylem-http.h` (add builder API)
- Modify: `src/net/http/http-utils.c` (builder implementation)
- Modify: `tests/test-http.c` (unit + integration tests)

### Step 1: Add builder types and API to public header

- [ ] In `include/xylem/net/xylem-http.h`, add after the existing `xylem_http_multipart_destroy` declaration:

```c
typedef struct xylem_http_multipart_builder_s xylem_http_multipart_builder_t;

/**
 * @brief Create a multipart form-data builder.
 *
 * @return Builder handle, or NULL on failure.
 */
extern xylem_http_multipart_builder_t* xylem_http_multipart_build_create(void);

/**
 * @brief Add a text field to the multipart body.
 *
 * @param b          Builder handle.
 * @param name       Field name (null-terminated).
 * @param value      Field value.
 * @param value_len  Value length in bytes.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_multipart_build_field(
    xylem_http_multipart_builder_t* b,
    const char* name,
    const void* value,
    size_t value_len);

/**
 * @brief Add a file part to the multipart body.
 *
 * @param b             Builder handle.
 * @param name          Field name (null-terminated).
 * @param filename      Filename for Content-Disposition.
 * @param content_type  MIME type, or NULL for application/octet-stream.
 * @param data          File data.
 * @param data_len      File data length.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_multipart_build_file(
    xylem_http_multipart_builder_t* b,
    const char* name,
    const char* filename,
    const char* content_type,
    const void* data,
    size_t data_len);

/**
 * @brief Finalize and serialize the multipart body.
 *
 * Produces a buffer suitable for xylem_http_post(). The caller must free
 * both *body and *content_type when done.
 *
 * @param b             Builder handle.
 * @param body          Output: malloc'd body buffer.
 * @param body_len      Output: body length.
 * @param content_type  Output: malloc'd Content-Type string with boundary.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_multipart_build_finish(
    xylem_http_multipart_builder_t* b,
    void** body,
    size_t* body_len,
    char** content_type);

/**
 * @brief Destroy a multipart builder.
 *
 * @param b  Builder handle, or NULL (no-op).
 */
extern void xylem_http_multipart_build_destroy(
    xylem_http_multipart_builder_t* b);
```

### Step 2: Implement the builder in http-utils.c

- [ ] At the end of `src/net/http/http-utils.c`, add the builder implementation:

```c
#define MULTIPART_BOUNDARY_LEN 24

typedef struct {
    char*   name;
    char*   filename;     /* NULL for text fields */
    char*   content_type; /* NULL for text fields */
    uint8_t* data;
    size_t  data_len;
} _build_part_t;

struct xylem_http_multipart_builder_s {
    _build_part_t* parts;
    size_t         count;
    size_t         cap;
    char           boundary[MULTIPART_BOUNDARY_LEN + 1];
};

static void _build_gen_boundary(char* buf) {
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyz0123456789";
    uint64_t seed = xylem_utils_getnow(XYLEM_TIME_PRECISION_NSEC);
    for (int i = 0; i < MULTIPART_BOUNDARY_LEN; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = charset[(seed >> 33) % 36];
    }
    buf[MULTIPART_BOUNDARY_LEN] = '\0';
}

xylem_http_multipart_builder_t* xylem_http_multipart_build_create(void) {
    xylem_http_multipart_builder_t* b =
        (xylem_http_multipart_builder_t*)calloc(1, sizeof(*b));
    if (!b) {
        return NULL;
    }
    _build_gen_boundary(b->boundary);
    return b;
}

static int _build_add_part(xylem_http_multipart_builder_t* b,
                           const char* name, const char* filename,
                           const char* content_type,
                           const void* data, size_t data_len) {
    if (!b || !name) {
        return -1;
    }
    if (b->count >= b->cap) {
        size_t new_cap = b->cap ? b->cap * 2 : 4;
        _build_part_t* tmp = (_build_part_t*)realloc(
            b->parts, new_cap * sizeof(*tmp));
        if (!tmp) {
            return -1;
        }
        b->parts = tmp;
        b->cap = new_cap;
    }

    _build_part_t* p = &b->parts[b->count];
    memset(p, 0, sizeof(*p));

    size_t nlen = strlen(name);
    p->name = (char*)malloc(nlen + 1);
    if (!p->name) {
        return -1;
    }
    memcpy(p->name, name, nlen + 1);

    if (filename) {
        size_t flen = strlen(filename);
        p->filename = (char*)malloc(flen + 1);
        if (!p->filename) {
            free(p->name);
            return -1;
        }
        memcpy(p->filename, filename, flen + 1);
    }

    if (content_type) {
        size_t clen = strlen(content_type);
        p->content_type = (char*)malloc(clen + 1);
        if (!p->content_type) {
            free(p->name);
            free(p->filename);
            return -1;
        }
        memcpy(p->content_type, content_type, clen + 1);
    }

    if (data && data_len > 0) {
        p->data = (uint8_t*)malloc(data_len);
        if (!p->data) {
            free(p->name);
            free(p->filename);
            free(p->content_type);
            return -1;
        }
        memcpy(p->data, data, data_len);
        p->data_len = data_len;
    }

    b->count++;
    return 0;
}

int xylem_http_multipart_build_field(
    xylem_http_multipart_builder_t* b,
    const char* name,
    const void* value,
    size_t value_len) {
    return _build_add_part(b, name, NULL, NULL, value, value_len);
}

int xylem_http_multipart_build_file(
    xylem_http_multipart_builder_t* b,
    const char* name,
    const char* filename,
    const char* content_type,
    const void* data,
    size_t data_len) {
    if (!filename) {
        return -1;
    }
    const char* ct = content_type ? content_type : "application/octet-stream";
    return _build_add_part(b, name, filename, ct, data, data_len);
}

int xylem_http_multipart_build_finish(
    xylem_http_multipart_builder_t* b,
    void** body,
    size_t* body_len,
    char** content_type) {
    if (!b || !body || !body_len || !content_type || b->count == 0) {
        return -1;
    }

    /* Estimate total size. */
    size_t bnd_len = strlen(b->boundary);
    size_t total = 0;
    for (size_t i = 0; i < b->count; i++) {
        _build_part_t* p = &b->parts[i];
        total += 2 + bnd_len + 2;  /* "--" + boundary + "\r\n" */
        total += 38 + strlen(p->name);  /* Content-Disposition line base */
        if (p->filename) {
            total += 12 + strlen(p->filename);  /* ; filename="..." */
        }
        total += 2;  /* "\r\n" after disposition */
        if (p->content_type) {
            total += 14 + strlen(p->content_type) + 2;  /* Content-Type: ...\r\n */
        }
        total += 2;  /* "\r\n" separating headers from body */
        total += p->data_len;
        total += 2;  /* "\r\n" after data */
    }
    total += 2 + bnd_len + 4;  /* "--" + boundary + "--\r\n" */

    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf) {
        return -1;
    }

    size_t off = 0;
    for (size_t i = 0; i < b->count; i++) {
        _build_part_t* p = &b->parts[i];

        buf[off++] = '-';
        buf[off++] = '-';
        memcpy(buf + off, b->boundary, bnd_len);
        off += bnd_len;
        buf[off++] = '\r';
        buf[off++] = '\n';

        if (p->filename) {
            off += (size_t)snprintf(
                (char*)buf + off, total - off,
                "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n",
                p->name, p->filename);
        } else {
            off += (size_t)snprintf(
                (char*)buf + off, total - off,
                "Content-Disposition: form-data; name=\"%s\"\r\n",
                p->name);
        }

        if (p->content_type) {
            off += (size_t)snprintf(
                (char*)buf + off, total - off,
                "Content-Type: %s\r\n", p->content_type);
        }

        buf[off++] = '\r';
        buf[off++] = '\n';

        if (p->data && p->data_len > 0) {
            memcpy(buf + off, p->data, p->data_len);
            off += p->data_len;
        }

        buf[off++] = '\r';
        buf[off++] = '\n';
    }

    /* Final boundary. */
    buf[off++] = '-';
    buf[off++] = '-';
    memcpy(buf + off, b->boundary, bnd_len);
    off += bnd_len;
    buf[off++] = '-';
    buf[off++] = '-';
    buf[off++] = '\r';
    buf[off++] = '\n';

    *body = buf;
    *body_len = off;

    /* Build Content-Type string. */
    size_t ct_len = 30 + bnd_len + 1;  /* "multipart/form-data; boundary=" + bnd */
    char* ct = (char*)malloc(ct_len);
    if (!ct) {
        free(buf);
        *body = NULL;
        *body_len = 0;
        return -1;
    }
    snprintf(ct, ct_len, "multipart/form-data; boundary=%s", b->boundary);
    *content_type = ct;

    return 0;
}

void xylem_http_multipart_build_destroy(xylem_http_multipart_builder_t* b) {
    if (!b) {
        return;
    }
    for (size_t i = 0; i < b->count; i++) {
        free(b->parts[i].name);
        free(b->parts[i].filename);
        free(b->parts[i].content_type);
        free(b->parts[i].data);
    }
    free(b->parts);
    free(b);
}
```

### Step 3: Write unit test (builder + roundtrip with parser)

- [ ] In `tests/test-http.c`, add:

```c
static void test_multipart_build_basic(void) {
    xylem_http_multipart_builder_t* b = xylem_http_multipart_build_create();
    ASSERT(b != NULL);

    int rc = xylem_http_multipart_build_field(b, "name", "Alice", 5);
    ASSERT(rc == 0);

    rc = xylem_http_multipart_build_file(
        b, "avatar", "photo.png", "image/png", "\x89PNG", 4);
    ASSERT(rc == 0);

    void* body = NULL;
    size_t body_len = 0;
    char* ct = NULL;
    rc = xylem_http_multipart_build_finish(b, &body, &body_len, &ct);
    ASSERT(rc == 0);
    ASSERT(body != NULL);
    ASSERT(body_len > 0);
    ASSERT(ct != NULL);
    ASSERT(strncmp(ct, "multipart/form-data; boundary=", 30) == 0);

    /* Roundtrip: parse what we built. */
    xylem_http_multipart_t* mp = xylem_http_multipart_parse(ct, body, body_len);
    ASSERT(mp != NULL);
    ASSERT(xylem_http_multipart_count(mp) == 2);

    ASSERT(strcmp(xylem_http_multipart_name(mp, 0), "name") == 0);
    ASSERT(xylem_http_multipart_data_len(mp, 0) == 5);
    ASSERT(memcmp(xylem_http_multipart_data(mp, 0), "Alice", 5) == 0);
    ASSERT(xylem_http_multipart_filename(mp, 0) == NULL);

    ASSERT(strcmp(xylem_http_multipart_name(mp, 1), "avatar") == 0);
    ASSERT(strcmp(xylem_http_multipart_filename(mp, 1), "photo.png") == 0);
    ASSERT(strcmp(xylem_http_multipart_content_type(mp, 1), "image/png") == 0);
    ASSERT(xylem_http_multipart_data_len(mp, 1) == 4);
    ASSERT(memcmp(xylem_http_multipart_data(mp, 1), "\x89PNG", 4) == 0);

    xylem_http_multipart_destroy(mp);
    free(body);
    free(ct);
    xylem_http_multipart_build_destroy(b);
}

static void test_multipart_build_empty(void) {
    xylem_http_multipart_builder_t* b = xylem_http_multipart_build_create();
    ASSERT(b != NULL);

    void* body = NULL;
    size_t body_len = 0;
    char* ct = NULL;
    int rc = xylem_http_multipart_build_finish(b, &body, &body_len, &ct);
    ASSERT(rc == -1);  /* Empty builder should fail. */

    xylem_http_multipart_build_destroy(b);
}

static void test_multipart_build_destroy_null(void) {
    xylem_http_multipart_build_destroy(NULL);  /* Should not crash. */
}
```

### Step 4: Write integration test (client POST multipart to server)

- [ ] Add handler and test:

```c
static void _multipart_echo_handler(xylem_http_res_t* res,
                                    xylem_http_req_t* req,
                                    void* userdata) {
    (void)userdata;
    const char* ct = xylem_http_req_header(req, "Content-Type");
    const void* body = xylem_http_req_body(req);
    size_t body_len = xylem_http_req_body_len(req);

    xylem_http_multipart_t* mp = xylem_http_multipart_parse(ct, body, body_len);
    if (!mp) {
        xylem_http_res_set_status(res, 400);
        xylem_http_res_write(res, "bad multipart", 13);
        return;
    }

    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%zu", xylem_http_multipart_count(mp));
    xylem_http_res_set_status(res, 200);
    xylem_http_res_write(res, buf, (size_t)n);
    xylem_http_multipart_destroy(mp);
}

static void _test_multipart_post_main(void* arg) {
    (void)arg;

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _multipart_echo_handler, NULL, NULL);
    ASSERT(srv != NULL);
    uint16_t port = xylem_http_srv_port(srv);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/upload", (unsigned)port);

    xylem_http_multipart_builder_t* b = xylem_http_multipart_build_create();
    ASSERT(b != NULL);
    xylem_http_multipart_build_field(b, "key", "value", 5);
    xylem_http_multipart_build_file(
        b, "file", "data.bin", NULL, "ABCDEF", 6);

    void* body = NULL;
    size_t body_len = 0;
    char* ct = NULL;
    int rc = xylem_http_multipart_build_finish(b, &body, &body_len, &ct);
    ASSERT(rc == 0);

    xylem_http_res_t* res = xylem_http_post(url, body, body_len, ct, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(memcmp(xylem_http_res_body(res), "2", 1) == 0);
    xylem_http_res_destroy(res);

    free(body);
    free(ct);
    xylem_http_multipart_build_destroy(b);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_multipart_post(void) {
    xylem_run(_test_multipart_post_main, NULL, NULL);
}
```

### Step 5: Register new tests in main()

- [ ] Add to `main()` in `tests/test-http.c`:

```c
    /* Multipart builder */
    test_multipart_build_basic();
    test_multipart_build_empty();
    test_multipart_build_destroy_null();

    /* Integration: multipart POST */
    test_multipart_post();
```

### Step 6: Build and run tests

- [ ] Run:

```bash
cmake --build build && ctest --test-dir build -R test-http --output-on-failure
```

Expected: All tests pass including multipart builder roundtrip and integration tests.

### Step 7: Commit

```bash
git add include/xylem/net/xylem-http.h src/net/http/http-utils.c tests/test-http.c
git commit -m "feat(http): add multipart form-data builder for client uploads"
```

---

## Task 4: Final Verification

- [ ] Run full test suite:

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

- [ ] Verify no regressions in other modules.
