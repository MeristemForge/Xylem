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

#include <openssl/ssl.h>

/**
 * @brief Add the platform's system root certificates to a SSL_CTX.
 *
 * Loads the OS trust store so a client can verify servers presenting
 * certificates from public CAs. The mechanism is platform specific:
 * Windows reads the system ROOT store via the OpenSSL winstore loader,
 * while Unix uses OpenSSL's default verify paths (the system CA bundle).
 *
 * This is excluded from the platform.h umbrella because it depends on
 * OpenSSL, which is only present in TLS-enabled builds.
 *
 * @param ssl_ctx  OpenSSL context to add the trust anchors to.
 *
 * @return 0 on success, -1 on failure.
 */
extern int platform_tls_load_system_ca(SSL_CTX* ssl_ctx);
