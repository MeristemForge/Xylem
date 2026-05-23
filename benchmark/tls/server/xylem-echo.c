#include "xylem.h"
#include "xylem/net/xylem-tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#define DEFAULT_PORT 9443
#define CERT_FILE    "bench-cert.pem"
#define KEY_FILE     "bench-key.pem"

static int _write_pem_to_file(const char* path,
                              int (*write_fn)(BIO*, void*),
                              void* obj) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) {
        return -1;
    }
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

    EVP_PKEY* pkey = EVP_PKEY_new();
    if (!pkey) {
        return -1;
    }

    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!pctx) {
        EVP_PKEY_free(pkey);
        return -1;
    }

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

    int rc = 0;
    if (_write_pem_to_file(CERT_FILE, _write_cert, x509) != 0) {
        rc = -1;
    }
    if (rc == 0 && _write_pem_to_file(KEY_FILE, _write_key, pkey) != 0) {
        rc = -1;
    }

    X509_free(x509);
    EVP_PKEY_free(pkey);
    return rc;
}

static void _on_read(xylem_tls_conn_t* conn, void* data, size_t len) {
    xylem_tls_write(conn, data, len);
}

int main(int argc, char** argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);


    loop_t* loop = loop_create();

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    if (!ctx) {
        fprintf(stderr, "failed to create tls context\n");
        return 1;
    }

    if (_ensure_cert() != 0) {
        fprintf(stderr, "failed to generate self-signed certificate\n");
        xylem_tls_ctx_destroy(ctx);
        return 1;
    }

    if (xylem_tls_ctx_load_cert(ctx, NULL, CERT_FILE, KEY_FILE) != 0) {
        fprintf(stderr, "failed to load %s / %s\n", CERT_FILE, KEY_FILE);
        xylem_tls_ctx_destroy(ctx);
        return 1;
    }
    xylem_tls_ctx_set_verify(ctx, false);

    xylem_addr_t addr;
    xylem_addr_pton("0.0.0.0", (uint16_t)port, &addr);

    xylem_tls_handler_t handler = {
        .on_read = _on_read,
    };

    xylem_tls_server_t* server = xylem_tls_listen(loop, &addr, ctx,
                                                   &handler, NULL);
    if (!server) {
        fprintf(stderr, "failed to listen on port %d\n", port);
        xylem_tls_ctx_destroy(ctx);
        return 1;
    }

    fprintf(stderr, "xylem tls echo server listening on 0.0.0.0:%d\n", port);
    loop_run(loop);

    xylem_tls_ctx_destroy(ctx);
    loop_destroy(loop);
    return 0;
}
