# TLS Server-Side SNI Support

## Summary

Add server-side SNI (Server Name Indication) support to the xylem-tls module,
allowing a single TLS listener to serve multiple domains with different
certificates.

## API Change

```c
extern int xylem_tls_ctx_load_cert(xylem_tls_ctx_t* ctx,
                                   const char* hostname,
                                   const char* cert,
                                   const char* key);
```

- `hostname = NULL`: load a default certificate (fallback when no SNI matches).
- `hostname = "api.example.com"`: register a certificate for that domain.

Multiple calls are allowed: one NULL for the default, plus N calls with
hostnames for domain-specific certificates.

## Approach

Each hostname gets its own `SSL_CTX` (Approach A -- industry standard, used
by Nginx). The main `xylem_tls_ctx_t` holds a dynamic array of SNI entries.
An OpenSSL SNI callback (`SSL_CTX_set_tlsext_servername_callback`) selects the
matching child `SSL_CTX` during handshake.

## Internal Data Structures

```c
typedef struct _tls_sni_entry_s {
    char     hostname[256];
    SSL_CTX* ssl_ctx;
} _tls_sni_entry_t;

struct xylem_tls_ctx_s {
    SSL_CTX*          ssl_ctx;       /* main ctx (default cert) */
    uint8_t*          alpn_wire;
    size_t            alpn_wire_len;
    FILE*             keylog_file;
    _tls_sni_entry_t* sni_entries;   /* dynamic array */
    size_t            sni_count;
    size_t            sni_cap;
};
```

## SNI Callback

```c
static int _tls_sni_cb(SSL* ssl, int* al, void* arg) {
    xylem_tls_ctx_t* ctx = (xylem_tls_ctx_t*)arg;
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (!name) {
        return SSL_TLSEXT_ERR_OK;  /* no SNI, use default ctx */
    }
    for (size_t i = 0; i < ctx->sni_count; i++) {
        if (strcasecmp(name, ctx->sni_entries[i].hostname) == 0) {
            SSL_set_SSL_CTX(ssl, ctx->sni_entries[i].ssl_ctx);
            return SSL_TLSEXT_ERR_OK;
        }
    }
    return SSL_TLSEXT_ERR_OK;  /* no match, fallback to default */
}
```

Registered on the main `ssl_ctx` the first time `load_cert` is called with a
non-NULL hostname.

## Child SSL_CTX Creation

Each child `SSL_CTX` inherits from the main ctx:

- Min protocol version (TLS 1.2)
- Verify mode and CA store (`SSL_CTX_set1_verify_cert_store`)
- ALPN settings (protos + select callback)
- Keylog callback
- `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER`

Then loads the domain-specific cert + key.

## Configuration Granularity

All domains share the main ctx settings (ALPN, CA, verify mode). Only
the certificate and private key differ per hostname.

## Destroy

`xylem_tls_ctx_destroy` iterates `sni_entries`, calls `SSL_CTX_free` on each
child, then frees the array.

## Caller Migration

All existing callers add `NULL` as the hostname parameter:

```c
/* before */
xylem_tls_ctx_load_cert(ctx, cert, key);
/* after */
xylem_tls_ctx_load_cert(ctx, NULL, cert, key);
```

Affected files:
- `src/net/xylem-tls.c` (implementation)
- `include/xylem/net/xylem-tls.h` (declaration)
- `tests/test-tls.c` (~15 call sites)
- `src/net/http/http-transport-tls.c`
- `src/net/ws/ws-transport-tls.c`
- `benchmark/tls/server/xylem-echo.c`

## Platform Note

`strcasecmp` is POSIX. On Windows MSVC use `_stricmp`. Route through the
platform layer or use a conditional macro.

## Testing

Add a new test case `test_sni_select` that:
1. Creates a ctx with two hostname-specific certs
2. Starts a TLS listener
3. Dials with `opts.hostname = "domain-a"`, verifies connection succeeds
4. Dials with `opts.hostname = "domain-b"`, verifies connection succeeds
5. Dials with `opts.hostname = "unknown"`, verifies fallback to default cert
