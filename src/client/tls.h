/* ============================================================
 *  Mystic OS - TLS client interface
 *
 *  tls_connect() wraps an already-connected TCP socket in a real
 *  TLS 1.2 session. The caller owns the socket and supplies the two
 *  byte-moving callbacks; the TLS code never opens a socket itself.
 *
 *  TLS 1.2 only: the engine's version range is pinned to 1.2 on both
 *  ends, so a server that speaks nothing but 1.3 (or 1.1 and below)
 *  fails the handshake instead of silently falling back. TLS 1.3 is
 *  not implemented here.
 *
 *  On success it returns a connection whose certificate chain has
 *  already been validated against the bundled Mozilla roots, whose
 *  host name has been matched and whose dates have been checked.
 *  On failure it returns NULL and fills err with the reason.
 * ============================================================ */
#ifndef MYSTIC_TLS_H
#define MYSTIC_TLS_H

#include <stddef.h>

typedef struct TlsConn TlsConn;

/* >0 bytes moved, 0 on orderly EOF, -1 on error (errno should be set). */
typedef long (*TlsReadFn)(void *ctx, void *buf, size_t len);
typedef long (*TlsWriteFn)(void *ctx, const void *buf, size_t len);

/* Facts about the session, all read from the handshake and the leaf
 * certificate. Strings are NUL terminated. */
typedef struct {
    char version[12];      /* "TLS 1.2"                               */
    char cipher[40];       /* "ECDHE-RSA-AES128-GCM-SHA256"           */
    char subject[200];     /* "CN=example.com, O=..."                 */
    char issuer[200];
    char cn[128];
    char not_before[32];   /* "2025-01-01 00:00:00 UTC"               */
    char not_after[32];
    int  chain_ok;         /* 1: chain, name and dates all verified   */
    int  sni;              /* 1: the name was sent as SNI             */
} TlsInfo;

/* Perform the handshake over the caller's socket. `hostname` is used
 * for SNI and for the certificate host-name check. */
TlsConn *tls_connect(void *ioctx, TlsReadFn rd, TlsWriteFn wr,
                     const char *hostname, char *err, size_t esz);

/* Application data. Return >0 bytes, 0 on orderly close, -1 on error.
 * A peer that closes the TCP connection without sending close_notify is
 * reported as 0: everything BearSSL delivered was authenticated first, so
 * the bytes are genuine, and the protocol above (HTTP) decides from its
 * own framing whether the message was complete. A transport failure on a
 * socket that is still open stays -1. */
long tls_read(TlsConn *t, void *buf, size_t len, char *err, size_t esz);
int  tls_write(TlsConn *t, const void *buf, size_t len, char *err, size_t esz);

/* Sends close_notify (best effort) and frees the connection. The
 * caller still owns and closes the socket. */
void tls_close(TlsConn *t);

const TlsInfo *tls_info(const TlsConn *t);

/* Trust store. Passing NULL resets to the default search. The default
 * paths are /etc/ssl/certs/ca-bundle.crt, /etc/ssl/cert.pem,
 * /etc/pki/tls/certs/ca-bundle.crt and /etc/ssl/certs/ca-certificates.crt;
 * $MYSTIC_CA_BUNDLE overrides them. Returns 0 on success. */
int  tls_load_ca_bundle(const char *path);
int  tls_trust_count(void);
const char *tls_trust_path(void);

#endif /* MYSTIC_TLS_H */
