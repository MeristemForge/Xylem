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

_Pragma("once")

/**
 * Shared helpers for the test suite. Provides test watchdogs (abort the
 * process if a test hangs) and, for tests that define TEST_WITH_TLS before
 * including this header, self-signed certificate generation. All helpers
 * are static inline so each test translation unit gets its own copy with
 * no link conflicts.
 */

#include "xylem.h"
#include "assert.h"

#define SAFETY_TIMEOUT_MS 10000
/* ASAN can 2-5x slow down; bump watchdog when instrumented. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#undef SAFETY_TIMEOUT_MS
#define SAFETY_TIMEOUT_MS 60000
#endif
#endif

/**
 * Watchdog for public timers. Pass this as the callback; it aborts the
 * test if the deadline is reached. The caller keeps the returned handle
 * and cancels it on the success path.
 */
static inline void _utils_watchdog_cb(xylem_timer_t* timer, void* userdata) {
    (void)timer;
    (void)userdata;
    ASSERT(0 && "test timed out");
}

static xylem_timer_t* _utils_watchdog_timer;

static inline void _utils_watchdog_start(uint64_t timeout_ms) {
    ASSERT(_utils_watchdog_timer == NULL);
    _utils_watchdog_timer =
        xylem_timer_after(timeout_ms, _utils_watchdog_cb, NULL);
    ASSERT(_utils_watchdog_timer != NULL);
}

static inline void _utils_watchdog_stop(void) {
    xylem_timer_t* timer = _utils_watchdog_timer;
    _utils_watchdog_timer = NULL;
    xylem_timer_destroy(timer);
}

#ifdef TEST_WITH_TLS

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <stdio.h>

/**
 * Write PEM data to a file via a memory BIO instead of passing FILE*
 * directly to OpenSSL (e.g. PEM_write_X509). On Windows the OpenSSL DLL
 * and the application may link against different C runtimes whose FILE
 * structs are incompatible; passing a FILE* across the DLL boundary
 * triggers the OPENSSL_Applink error. Routing all FILE* operations
 * through the application's own CRT via a memory BIO avoids it entirely.
 */
static inline int _utils_cert_write_pem(const char* path,
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

static inline int _utils_cert_write_x509(BIO* bio, void* obj) {
    return PEM_write_bio_X509(bio, (X509*)obj);
}

static inline int _utils_cert_write_key(BIO* bio, void* obj) {
    return PEM_write_bio_PrivateKey(bio, (EVP_PKEY*)obj,
                                    NULL, NULL, 0, NULL, NULL);
}

/**
 * Generate a self-signed cert/key pair with the given subject common name
 * and subjectAltName (e.g. "DNS:localhost,IP:127.0.0.1"). Returns 0 on
 * success, -1 on failure.
 */
static inline int _utils_cert_gen_ex(const char* cert_path,
                                     const char* key_path,
                                     const char* cn, const char* san) {
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
                               (const unsigned char*)cn, -1, -1, 0);
    X509_set_issuer_name(x509, name);

    /* SAN required by OpenSSL 3.x for hostname verification. */
    X509_EXTENSION* ext_san = X509V3_EXT_nconf_nid(
        NULL, NULL, NID_subject_alt_name, san);
    if (ext_san) {
        X509_add_ext(x509, ext_san, -1);
        X509_EXTENSION_free(ext_san);
    }

    X509_sign(x509, pkey, EVP_sha256());

    int rc = 0;
    if (_utils_cert_write_pem(cert_path, _utils_cert_write_x509, x509) != 0) {
        rc = -1;
    }
    if (rc == 0 &&
        _utils_cert_write_pem(key_path, _utils_cert_write_key, pkey) != 0) {
        rc = -1;
    }

    X509_free(x509);
    EVP_PKEY_free(pkey);
    return rc;
}

/**
 * Generate a self-signed cert/key valid for localhost / 127.0.0.1.
 * Returns 0 on success, -1 on failure.
 */
static inline int _utils_cert_gen(const char* cert_path,
                                  const char* key_path) {
    return _utils_cert_gen_ex(cert_path, key_path, "localhost",
                              "DNS:localhost,IP:127.0.0.1");
}

#endif /* TEST_WITH_TLS */
