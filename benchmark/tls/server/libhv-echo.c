#include <hv/hloop.h>
#include <hv/hsocket.h>
#include <hv/hssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#define CERT_FILE "bench-cert.pem"
#define KEY_FILE  "bench-key.pem"

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

static void on_read(hio_t* io, void* buf, int readbytes) {
    hio_write(io, buf, readbytes);
}

static void on_accept(hio_t* io) {
    hio_setcb_read(io, on_read);
    hio_read(io);
}

int main(int argc, char** argv) {
    int port = 9443;
    if (argc > 1) port = atoi(argv[1]);

    _ensure_cert();

    hloop_t* loop = hloop_new(0);

    hio_t* listenio = hloop_create_ssl_server(loop, "0.0.0.0", port,
                                               on_accept);
    if (!listenio) {
        fprintf(stderr, "failed to listen on port %d\n", port);
        return 1;
    }

    hssl_ctx_opt_t ssl_opt;
    memset(&ssl_opt, 0, sizeof(ssl_opt));
    ssl_opt.crt_file = CERT_FILE;
    ssl_opt.key_file = KEY_FILE;
    hio_new_ssl_ctx(listenio, &ssl_opt);

    fprintf(stderr, "libhv tls echo server listening on 0.0.0.0:%d\n", port);
    hloop_run(loop);
    hloop_free(&loop);
    return 0;
}
