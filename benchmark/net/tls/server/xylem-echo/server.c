/* Xylem TLS echo server (single-threaded).
 *
 * Coroutine model mirroring the TCP server: one acceptor coroutine loops
 * xylem_tls_accept() (TCP accept + TLS handshake handled internally) and
 * spawns a handler coroutine per connection that echoes plaintext. A
 * self-signed certificate is generated at startup via OpenSSL. Run under
 * one scheduler worker (ST). */
#include "xylem.h"
#include "xylem/net/xylem-tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#define DEFAULT_PORT 9443
#define CERT_FILE    "bench-cert.pem"
#define KEY_FILE     "bench-key.pem"

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

static int _ensure_cert(void) {
    FILE* f = fopen(CERT_FILE, "r");
    if (f) {
        fclose(f);
        return 0;
    }

    fprintf(stderr, "generating self-signed certificate...\n");

    /* ECDSA P-256 to match the go/rust bench servers (rcgen / ecdsa
     * default), so connrate compares like-for-like server signing cost
     * instead of RSA-2048 vs EC. */
    EVP_PKEY*     pkey = NULL;
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!pctx) return -1;

    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1);
    EVP_PKEY_keygen(pctx, &pkey);
    EVP_PKEY_CTX_free(pctx);
    if (!pkey) return -1;

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

    int rc = 0;
    if (_write_pem_to_file(CERT_FILE, _write_cert, x509) != 0) rc = -1;
    if (rc == 0 && _write_pem_to_file(KEY_FILE, _write_key, pkey) != 0) rc = -1;

    X509_free(x509);
    EVP_PKEY_free(pkey);
    return rc;
}

static void _handle_conn(void* arg) {
    xylem_tls_conn_t* conn = (xylem_tls_conn_t*)arg;
    char* buf = (char*)malloc(65536);
    if (!buf) { xylem_tls_close(conn); return; }

    for (;;) {
        int n = xylem_tls_read(conn, buf, 65536);
        if (n <= 0) break;
        if (xylem_tls_write(conn, buf, n) != 0) break;
    }

    free(buf);
    xylem_tls_close(conn);
}

static void _acceptor(void* arg) {
    int port = *(int*)arg;

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    if (!ctx) {
        fprintf(stderr, "failed to create tls context\n");
        xylem_shutdown();
        return;
    }

    if (_ensure_cert() != 0) {
        fprintf(stderr, "failed to generate self-signed certificate\n");
        xylem_tls_ctx_destroy(ctx);
        xylem_shutdown();
        return;
    }

    if (xylem_tls_ctx_load_cert(ctx, NULL, CERT_FILE, KEY_FILE) != 0) {
        fprintf(stderr, "failed to load %s / %s\n", CERT_FILE, KEY_FILE);
        xylem_tls_ctx_destroy(ctx);
        xylem_shutdown();
        return;
    }
    xylem_tls_ctx_verify_client(ctx, false);

    xylem_tls_listener_t* server =
        xylem_tls_listen("0.0.0.0", (uint16_t)port, ctx, NULL);
    if (!server) {
        fprintf(stderr, "failed to listen on port %d\n", port);
        xylem_tls_ctx_destroy(ctx);
        xylem_shutdown();
        return;
    }

    fprintf(stderr, "xylem tls echo server listening on 0.0.0.0:%d\n", port);

    for (;;) {
        xylem_tls_conn_t* conn = xylem_tls_accept(server);
        if (!conn) break;
        xylem_spawn(_handle_conn, conn);
    }

    xylem_tls_close_listener(server);
    xylem_tls_ctx_destroy(ctx);
}

int main(int argc, char** argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);

    xylem_opts_t rt_opts = {0};
    rt_opts.workers = 1;
    xylem_run(_acceptor, &port, &rt_opts);
    return 0;
}
