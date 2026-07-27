# TLS Hardening Design

## Scope

Fix seven confirmed issues in `xylem-tls` without changing public function
signatures. DTLS, HTTPS, and WSS are outside this change.

## Backend Ownership And State

The stream transport BIO is passed as both the read and write BIO. In this
case `SSL_set_bio()` consumes the caller's single reference, so connection
creation must not call `BIO_up_ref()` first.

Each backend connection records whether a fatal OpenSSL error occurred.
`SSL_ERROR_SSL` and `SSL_ERROR_SYSCALL` mark the connection fatal. Shutdown
sends a best-effort `close_notify` only when the connection is not fatal;
fatal connections proceed directly to transport teardown.

## Certificate Loading

Certificate and private-key file paths must be non-NULL and non-empty. Invalid
paths return `-1` before entering OpenSSL.

Encrypted private keys are unsupported by the current public API. PEM parsing
uses a password callback that returns failure without reading from a terminal,
so encrypted keys fail deterministically instead of blocking a scheduler
worker.

SNI identities remain case-insensitive by hostname. Loading an identity for an
existing hostname replaces the old certificate, key, and chain in place after
the new identity has parsed and validated successfully. This preserves the old
identity if parsing fails and makes successful configuration overrides
effective.

## ALPN Lifetime

`xylem_tls_get_alpn()` returns the connection-owned negotiated ALPN string.
It returns NULL before handshake completion, after close, or when no protocol
was negotiated. The returned pointer is read-only and remains valid until
`xylem_tls_destroy()`; querying another connection cannot overwrite it.

## Deadline Arithmetic

Conversion from relative connect timeout to absolute deadline uses saturating
addition. A timeout larger than the remaining `uint64_t` range produces
`UINT64_MAX` instead of wrapping into an already-expired deadline. Zero keeps
its existing meaning of no deadline.

## Verification

Targeted tests cover NULL and empty certificate paths, encrypted private-key
rejection, duplicate SNI replacement, retained ALPN values across connections,
and timeout saturation where the internal boundary is testable. Backend state
tests cover BIO destruction and suppression of shutdown after a fatal error if
those behaviors cannot be observed reliably through the public TLS API.

After targeted tests pass, build the existing project and run the existing TLS
test suite. No DTLS, HTTPS, or WSS behavior is changed.
