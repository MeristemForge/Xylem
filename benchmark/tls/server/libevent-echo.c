#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/event.h>
#include <event2/listener.h>

#include <netinet/tcp.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CERT_FILE "bench-cert.pem"
#define KEY_FILE  "bench-key.pem"

static SSL_CTX* g_ssl_ctx = NULL;

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

static void read_cb(struct bufferevent* bev, void* ctx) {
    (void)ctx;
    struct evbuffer* input  = bufferevent_get_input(bev);
    struct evbuffer* output = bufferevent_get_output(bev);
    evbuffer_add_buffer(output, input);
}

static void event_cb(struct bufferevent* bev, short events, void* ctx) {
    (void)ctx;
    if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
        bufferevent_free(bev);
    }
}

static void accept_cb(struct evconnlistener* listener,
                      evutil_socket_t fd,
                      struct sockaddr* addr, int addrlen, void* ctx) {
    (void)addr;
    (void)addrlen;
    (void)ctx;

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    struct event_base* base = evconnlistener_get_base(listener);

    SSL* ssl = SSL_new(g_ssl_ctx);
    struct bufferevent* bev = bufferevent_openssl_socket_new(
        base, fd, ssl, BUFFEREVENT_SSL_ACCEPTING,
        BEV_OPT_CLOSE_ON_FREE);

    bufferevent_setcb(bev, read_cb, NULL, event_cb, NULL);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
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

    struct event_base* base = event_base_new();

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family      = AF_INET;
    sin.sin_port        = htons((uint16_t)port);
    sin.sin_addr.s_addr = INADDR_ANY;

    struct evconnlistener* listener = evconnlistener_new_bind(
        base, accept_cb, NULL,
        LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE,
        4096, (struct sockaddr*)&sin, sizeof(sin));

    if (!listener) {
        fprintf(stderr, "failed to listen on port %d\n", port);
        return 1;
    }

    fprintf(stderr, "libevent+openssl tls echo server listening on 0.0.0.0:%d\n", port);
    event_base_dispatch(base);

    evconnlistener_free(listener);
    event_base_free(base);
    SSL_CTX_free(g_ssl_ctx);
    return 0;
}
