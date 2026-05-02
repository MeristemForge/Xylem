#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#define CERT_FILE "bench-cert.pem"
#define KEY_FILE  "bench-key.pem"

static uv_loop_t* loop;
static SSL_CTX*   g_ssl_ctx = NULL;

typedef struct {
    uv_tcp_t   handle;
    SSL*       ssl;
    BIO*       rbio;
    BIO*       wbio;
    char       read_buf[65536];
    int        handshake_done;
} tls_conn_t;

static int _write_pem_to_file(const char* path,
                              int (*write_fn)(BIO*, void*),
                              void* obj) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) return -1;
    if (write_fn(bio, obj) != 1) {
        BIO_free(bio);
        return -1;
    }
    char* data = NULL;
    long  len  = BIO_get_mem_data(bio, &data);
    FILE* f    = fopen(path, "wb");
    if (!f) {
        BIO_free(bio);
        return -1;
    }
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
    BIO_free(bio);
    return 0;
}

static int _write_cert(BIO* bio, void* obj) {
    return PEM_write_bio_X509(bio, (X509*)obj);
}

static int _write_key(BIO* bio, void* obj) {
    return PEM_write_bio_PrivateKey(bio, (EVP_PKEY*)obj,
                                    NULL, NULL, 0, NULL, NULL);
}

static void _ensure_cert(void) {
    FILE* f = fopen(CERT_FILE, "r");
    if (f) {
        fclose(f);
        return;
    }

    fprintf(stderr, "generating self-signed certificate...\n");

    EVP_PKEY* pkey = EVP_PKEY_new();
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048);
    EVP_PKEY_keygen(pctx, &pkey);
    EVP_PKEY_CTX_free(pctx);

    X509* x509 = X509_new();
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 3600);
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char*)"localhost", -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());

    _write_pem_to_file(CERT_FILE, _write_cert, x509);
    _write_pem_to_file(KEY_FILE, _write_key, pkey);

    X509_free(x509);
    EVP_PKEY_free(pkey);
}

static void _flush_wbio(tls_conn_t* conn) {
    char buf[16384];
    int pending;
    while ((pending = BIO_read(conn->wbio, buf, sizeof(buf))) > 0) {
        uv_buf_t wbuf = uv_buf_init(malloc((size_t)pending), (unsigned int)pending);
        memcpy(wbuf.base, buf, (size_t)pending);
        uv_write_t* req = malloc(sizeof(uv_write_t));
        req->data = wbuf.base;
        uv_write(req, (uv_stream_t*)&conn->handle, &wbuf, 1,
                 (uv_write_cb)free);
    }
}

static void _do_handshake(tls_conn_t* conn) {
    int rc = SSL_do_handshake(conn->ssl);
    _flush_wbio(conn);
    if (rc == 1) {
        conn->handshake_done = 1;
    }
}

static void _tls_echo(tls_conn_t* conn) {
    char buf[65536];
    int n;
    while ((n = SSL_read(conn->ssl, buf, sizeof(buf))) > 0) {
        SSL_write(conn->ssl, buf, n);
        _flush_wbio(conn);
    }
}

static void alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    (void)handle;
    (void)suggested_size;
    tls_conn_t* conn = (tls_conn_t*)handle;
    buf->base = conn->read_buf;
    buf->len  = sizeof(conn->read_buf);
}

static void read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    (void)buf;
    tls_conn_t* conn = (tls_conn_t*)stream;

    if (nread <= 0) {
        if (nread < 0) {
            SSL_free(conn->ssl);
            uv_close((uv_handle_t*)stream, (uv_close_cb)free);
        }
        return;
    }

    BIO_write(conn->rbio, buf->base, (int)nread);

    if (!conn->handshake_done) {
        _do_handshake(conn);
    } else {
        _tls_echo(conn);
    }
}

static void on_connection(uv_stream_t* server, int status) {
    if (status < 0) return;

    tls_conn_t* conn = calloc(1, sizeof(tls_conn_t));
    uv_tcp_init(loop, &conn->handle);

    if (uv_accept(server, (uv_stream_t*)&conn->handle) != 0) {
        uv_close((uv_handle_t*)&conn->handle, (uv_close_cb)free);
        return;
    }

    uv_tcp_nodelay(&conn->handle, 1);

    conn->ssl  = SSL_new(g_ssl_ctx);
    conn->rbio = BIO_new(BIO_s_mem());
    conn->wbio = BIO_new(BIO_s_mem());
    SSL_set_bio(conn->ssl, conn->rbio, conn->wbio);
    SSL_set_accept_state(conn->ssl);
    conn->handshake_done = 0;

    uv_read_start((uv_stream_t*)&conn->handle, alloc_cb, read_cb);
}

int main(int argc, char** argv) {
    int port = 9443;
    if (argc > 1) port = atoi(argv[1]);

    SSL_library_init();
    SSL_load_error_strings();

    g_ssl_ctx = SSL_CTX_new(TLS_server_method());
    _ensure_cert();
    SSL_CTX_use_certificate_chain_file(g_ssl_ctx, CERT_FILE);
    SSL_CTX_use_PrivateKey_file(g_ssl_ctx, KEY_FILE, SSL_FILETYPE_PEM);

    loop = uv_default_loop();

    uv_tcp_t server;
    uv_tcp_init(loop, &server);

    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", port, &addr);
    uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);

    int r = uv_listen((uv_stream_t*)&server, 4096, on_connection);
    if (r) {
        fprintf(stderr, "listen error: %s\n", uv_strerror(r));
        return 1;
    }

    fprintf(stderr, "libuv+openssl tls echo server listening on 0.0.0.0:%d\n", port);
    uv_run(loop, UV_RUN_DEFAULT);

    SSL_CTX_free(g_ssl_ctx);
    return 0;
}
