/* ============================================================
 *  Mystic OS - client/cdpws.h
 *
 *  A DevTools (CDP) client that speaks the protocol the way the browser
 *  actually exposes it outside the pipe: TCP to 127.0.0.1 plus an RFC 6455
 *  WebSocket.  It replaces the --remote-debugging-pipe transport, which
 *  this kernel answers with exit 1 (two pipes) or exit 127 (two
 *  socketpairs).
 *
 *  WHY THIS FILE EXISTS
 *  -------------------
 *  --remote-debugging-pipe hands the browser two file descriptors and each
 *  message is a NUL-terminated JSON object: no HTTP, no handshake, no
 *  masking.  --remote-debugging-port instead makes the browser listen on a
 *  loopback port and publish that port in <user-data-dir>/DevToolsActivePort,
 *  with a WebSocket URL for the browser target.  That is the standard way a
 *  headless shell is driven, and it uses only connect/read/write - none of
 *  the pipe semantics this kernel gets wrong.
 *
 *  THE SHAPE OF THE API
 *  -------------------
 *  Everything below the protocol is two callbacks and a connect, so the
 *  client runs unchanged over a real socket, a fake socket in a test
 *  harness, or a proxy:
 *
 *      CdpwsNet net = { -1, 10000 };          // POSIX transport (below)
 *      CdpwsTransport tr = cdpws_net_transport(&net);
 *      unsigned char key[CDPWS_KEY_LEN];
 *      CdpwsOpenOpts o = {0};
 *      o.port = port_from_DevToolsActivePort;
 *      o.path = path_from_DevToolsActivePort; // "/devtools/browser/<id>"
 *      o.key  = key;                          // caller-supplied: testable
 *      char err[200];
 *      Cdpws *c = cdpws_open(&tr, &o, err, sizeof err);
 *      if (!c) ...                            // err says which rule broke
 *
 *      cdpws_send_json(c, "{\"id\":1,\"method\":\"Page.enable\"}");
 *      CdpwsMessage m;
 *      if (cdpws_recv(c, &m) == 1 && m.opcode == CDPWS_OP_TEXT)
 *          handle(m.data, (size_t)m.len);     // only whole messages
 *
 *  WHAT THE CLIENT GUARANTEES
 *  --------------------------
 *  * DevToolsActivePort is parsed strictly: line 1 is a decimal port in
 *    1..65535, line 2 starts with '/'.  A port of 0 or 70000, or a path
 *    without '/', is refused with a sentence, not clamped.
 *  * The upgrade request is RFC 6455 §4.1 with the caller's 16-byte key;
 *    the response must be 101 and Sec-WebSocket-Accept must equal
 *    base64(SHA-1(base64(key) || GUID)).  A wrong or missing accept fails
 *    the handshake and the connection is closed: the client never speaks
 *    CDP to a peer that failed the check.
 *  * Client frames are text frames, always masked (§5.3), with a fresh
 *    key per frame from the caller's random source (default /dev/urandom;
 *    there is no predictable fallback).
 *  * Server frames are decoded including the 64-bit length form, and
 *    fragmented messages are reassembled before delivery; a ping is
 *    answered with a pong carrying the same payload and never surfaces as
 *    a message; a close is answered with a close and reported once.
 *  * A protocol violation, a masked server frame, a reserved bit, a
 *    message over the size limit or invalid UTF-8 in a text frame closes
 *    the connection with the matching status code (1002/1009/1007).
 *  * JSON for the HTTP endpoints is scanned, not parsed into a tree: the
 *    field's string value is decoded (escapes, \uXXXX) and a missing
 *    field is an error, never an empty string.
 *
 *  WHAT IS NOT HERE
 *  ----------------
 *  No TLS (the browser is local; wss:// is not offered by a local DevTools
 *  endpoint), no permessage-deflate (not offered, and an extension the
 *  client did not ask for is refused), no CDP command routing or id
 *  matching (the caller owns the JSON, exactly as with the pipe), no
 *  IPv6 and no DNS: the transport accepts only the literal 127.0.0.1,
 *  because a DevTools port is always local and a name lookup would be a
 *  new way to fail.
 * ============================================================ */
#ifndef MYSTIC_CDPWS_H
#define MYSTIC_CDPWS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------
 *  Sizes and limits
 * ------------------------------------------------------------ */
#define CDPWS_KEY_LEN        16   /* §4.1: 16 random bytes              */
#define CDPWS_KEY_B64_LEN    24   /* base64 of those 16 bytes, no NUL   */
#define CDPWS_ACCEPT_LEN     28   /* base64 of a 20-byte SHA-1, no NUL  */
#define CDPWS_ACCEPT_BUF     29
#define CDPWS_PATH_MAX      512   /* /devtools/browser/<uuid> is 58     */
#define CDPWS_ERR_MAX       200
#define CDPWS_DEFAULT_MAX_MESSAGE (4u << 20)  /* one CDP message        */
#define CDPWS_HEAD_MAX    16384   /* upgrade / HTTP response header     */

/* Transport callbacks use this for "nothing available right now".  It is
 * negative and distinct from every CDPWS_ERR_* code, so a callback cannot
 * be confused with a protocol failure by accident. */
#define CDPWS_IO_AGAIN (-100L)

/* ------------------------------------------------------------
 *  Result codes.  Zero is success; a positive return from cdpws_recv
 *  means "a message was delivered".  Everything else is negative.
 * ------------------------------------------------------------ */
enum {
    CDPWS_OK             =   0,
    CDPWS_ERR_IO         =  -1,  /* connect/read/write failed           */
    CDPWS_ERR_TIMEOUT    =  -2,  /* connect or I/O deadline expired     */
    CDPWS_ERR_CLOSED     =  -3,  /* the peer closed, or we already did  */
    CDPWS_ERR_PROTOCOL   =  -4,  /* RFC 6455 / HTTP violation           */
    CDPWS_ERR_HANDSHAKE  =  -5,  /* status or Sec-WebSocket-Accept bad  */
    CDPWS_ERR_TOO_BIG    =  -6,  /* a message or header over its limit  */
    CDPWS_ERR_ARG        =  -7,  /* bad argument, nothing was sent      */
    CDPWS_ERR_NOMEM      =  -8,  /* allocation failed                   */
    CDPWS_ERR_HTTP       =  -9,  /* HTTP status/headers unusable        */
    CDPWS_ERR_NOTFOUND   = -10,  /* the JSON field is absent            */
    CDPWS_ERR_UTF8       = -11,  /* text payload is not valid UTF-8     */
    CDPWS_ERR_RANDOM     = -12,  /* no masking key could be produced    */
    CDPWS_ERR_NOSPACE    = -13   /* the caller's buffer is too small    */
};

/* Frame opcodes, RFC 6455 §5.2, as delivered/asked for. */
enum {
    CDPWS_OP_CONT   = 0x0,
    CDPWS_OP_TEXT   = 0x1,
    CDPWS_OP_BINARY = 0x2,
    CDPWS_OP_CLOSE  = 0x8,
    CDPWS_OP_PING   = 0x9,
    CDPWS_OP_PONG   = 0xA
};

/* ------------------------------------------------------------
 *  Transport
 *
 *    connect(ctx, host, port, timeout_ms)
 *        Open a connection.  Returns CDPWS_OK, CDPWS_ERR_TIMEOUT or a
 *        negative CDPWS_ERR_*.  `timeout_ms` is the whole connect
 *        deadline; a transport that cannot enforce one must fail rather
 *        than block forever.
 *
 *    read(ctx, buf, len)
 *        > 0            bytes read (a short read is normal)
 *        0              orderly end of stream (peer closed)
 *        CDPWS_IO_AGAIN nothing available now; call again later
 *        other < 0      failure, CDPWS_ERR_TIMEOUT if a deadline expired
 *
 *        During cdpws_open and cdpws_http_get the read callback must not
 *        return CDPWS_IO_AGAIN: an HTTP head has no event loop to come
 *        back to.  A non-blocking transport should wait on its own poll
 *        with opts.timeout_ms; the POSIX transport does exactly that.
 *
 *    write(ctx, buf, len)
 *        > 0            bytes written; the client calls again for the rest
 *        <= 0           failure.  A partly written frame is never
 *                       retried from its start: the connection is failed,
 *                       because a half frame cannot be repaired.
 *
 *    close(ctx)         optional.  Called exactly once by cdpws_free (and
 *                       by a failed open).  Must be idempotent.
 * ------------------------------------------------------------ */
typedef int  (*CdpwsConnectFn)(void *ctx, const char *host, int port,
                               int timeout_ms);
typedef long (*CdpwsReadFn)(void *ctx, void *buf, size_t len);
typedef long (*CdpwsWriteFn)(void *ctx, const void *buf, size_t len);
typedef void (*CdpwsCloseFn)(void *ctx);

typedef struct {
    void            *ctx;
    CdpwsConnectFn   connect;
    CdpwsReadFn      read;
    CdpwsWriteFn     write;
    CdpwsCloseFn     close;   /* optional */
} CdpwsTransport;

/* A random source for masking keys (§5.3).  Fill all len bytes and return
 * 0, or return -1 and nothing is sent.  NULL asks the client to use
 * /dev/urandom (POSIX builds only). */
typedef int (*CdpwsRandFn)(void *ctx, void *buf, size_t len);

/* ------------------------------------------------------------
 *  Opening
 * ------------------------------------------------------------ */
typedef struct {
    const char          *host;        /* NULL or "127.0.0.1" (the only
                                         value the POSIX transport takes) */
    int                  port;        /* 1..65535, from DevToolsActivePort */
    const char          *path;        /* "/devtools/browser/<id>"          */
    const char          *origin;      /* optional Origin: value            */
    const unsigned char *key;         /* CDPWS_KEY_LEN bytes; NULL means
                                         "make one from rand/urandom"     */
    int                  timeout_ms;  /* connect deadline; 0 -> 10000      */
    size_t               max_message; /* 0 -> CDPWS_DEFAULT_MAX_MESSAGE   */
    CdpwsRandFn          rand;        /* NULL -> /dev/urandom              */
    void                *rand_ctx;
} CdpwsOpenOpts;

typedef struct Cdpws Cdpws;

/* Connect and upgrade.  Returns NULL on failure and writes a sentence into
 * err (never left unterminated).  On failure the transport is closed when
 * it has a close callback: the caller owns nothing.  On success
 * cdpws_free closes it. */
Cdpws *cdpws_open(const CdpwsTransport *tr, const CdpwsOpenOpts *opts,
                  char *err, size_t esz);

/* Close (with a best-effort Close frame if still open), free, and call
 * tr.close.  Safe on NULL. */
void cdpws_free(Cdpws *c);

/* ------------------------------------------------------------
 *  Messages
 *
 *  `data` is NUL-terminated for convenience and is valid until the next
 *  call on the connection.  Only complete messages are delivered; a
 *  fragmented text message arrives as one CDPWS_OP_TEXT with the whole
 *  payload, and a Ping never arrives at all (it is answered inside).
 *
 *  For CDPWS_OP_CLOSE, `code` is the status code (0 when the peer's Close
 *  carried none) and `len`/`data` are the frame payload.
 * ------------------------------------------------------------ */
typedef struct {
    int                  opcode;
    int                  len;
    int                  code;
    const unsigned char *data;
} CdpwsMessage;

/* 1  a message was delivered into *m
 * 0  no complete message yet: the transport had nothing (CDPWS_IO_AGAIN)
 *    or its read deadline expired.  An event loop can keep calling; the
 *    connection is not marked failed by a timeout.
 * <0 a CDPWS_ERR_*; after a protocol/UTF-8/size failure a Close has been
 *    sent where the RFC asks for one.
 */
int cdpws_recv(Cdpws *c, CdpwsMessage *m);

/* One masked text frame.  JSON is sent with cdpws_send_json, which takes
 * strlen.  Invalid UTF-8 is refused before anything is written. */
int cdpws_send_text(Cdpws *c, const void *data, size_t len);
int cdpws_send_json(Cdpws *c, const char *json);

/* Control frames.  Ping payload <= 125 bytes (§5.5).  cdpws_close sends a
 * Close frame and moves the connection to CLOSING; the reply is still read
 * by cdpws_recv.  code 0 sends an empty Close payload. */
int cdpws_send_ping(Cdpws *c, const void *data, size_t len);
int cdpws_close(Cdpws *c, int code, const char *reason);

int      cdpws_is_open(const Cdpws *c);   /* 0 once a Close is sent/received */
unsigned cdpws_pings(const Cdpws *c);     /* Ping frames answered            */
const char *cdpws_last_error(const Cdpws *c);
int         cdpws_last_error_code(const Cdpws *c);

/* A static sentence for any CDPWS_* code; never NULL. */
const char *cdpws_strerror(int rc);

/* ------------------------------------------------------------
 *  Small pieces that are worth testing on their own
 * ------------------------------------------------------------ */

/* base64(SHA-1(key_b64 || "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")).
 * key_b64 is exactly 24 characters of base64 (the Sec-WebSocket-Key
 * header value).  The RFC's example key "dGhlIHNhbXBsZSBub25jZQ==" must
 * produce "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=".  out is 29 bytes. */
int cdpws_accept_key(const char *key_b64, char out[CDPWS_ACCEPT_BUF]);

/* base64 of 16 bytes with padding: 24 characters + NUL in a 25-byte
 * buffer.  Exposed because a caller that makes its own key needs to log
 * the header value it used. */
int cdpws_key_to_b64(const unsigned char key[CDPWS_KEY_LEN],
                     char out[CDPWS_KEY_B64_LEN + 1]);

/* Fill buf from /dev/urandom.  POSIX builds only (the Windows host that
 * runs the harness supplies its own source through CdpwsOpenOpts.rand). */
#ifndef _WIN32
int cdpws_random_bytes(void *buf, size_t len);
#endif

/* DevToolsActivePort, strictly:
 *
 *     41235
 *     /devtools/browser/5b1f3d2e-9c47-4a1e-8b6f-2f0c9a1d4e7b
 *
 * Line 1 must be 1..5 decimal digits with no sign or space and must be in
 * 1..65535; line 2 must start with '/' and contain no space or control
 * byte.  Trailing CR is accepted (the browser writes LF on Linux but a
 * file that passed through a DOS tool has CRLF).  Returns CDPWS_OK,
 * CDPWS_ERR_ARG for a shape violation, or CDPWS_ERR_NOSPACE if path_cap
 * cannot hold the path.
 *
 * cdpws_read_active_port reads the file first; its err says whether the
 * file or its content was the problem. */
int cdpws_parse_active_port(const char *text, size_t len,
                            int *port, char *path, size_t path_cap,
                            char *err, size_t esz);
int cdpws_read_active_port(const char *file, int *port,
                           char *path, size_t path_cap,
                           char *err, size_t esz);

/* Find "field": "value" in a JSON document and decode the string value
 * into out (escapes, \uXXXX and surrogate pairs; lone surrogates become
 * U+FFFD rather than invalid UTF-8).  A field that is absent, or present
 * with a non-string value, is CDPWS_ERR_NOTFOUND.  out is NUL terminated.
 * This is a scanner for a fixed field, not a JSON parser: a string that
 * merely contains the field name inside another string's value does not
 * match, because the scanner consumes whole strings and requires the ':'
 * after a key. */
int cdpws_json_string(const char *json, size_t len, const char *field,
                      char *out, size_t cap);

/* ws://127.0.0.1:41235/devtools/browser/<id>  ->  /devtools/browser/<id>.
 * Accepts only ws:// (a local DevTools endpoint never offers wss://). */
int cdpws_ws_url_path(const char *url, char *path, size_t cap);

/* GET path over one fresh connection on the same transport, then close it.
 * Used for /json/version before the upgrade.  The response must be 200
 * with a Content-Length (the DevTools HTTP handler always sends one) or
 * end at EOF; chunked responses are refused rather than guessed.  The body
 * is NUL terminated and *body_len excludes the NUL.  This never speaks
 * WebSocket and is safe to call before cdpws_open. */
int cdpws_http_get(const CdpwsTransport *tr, const char *host, int port,
                   const char *path, char *body, size_t cap,
                   size_t *body_len, char *err, size_t esz);

/* ------------------------------------------------------------
 *  The POSIX transport: connect/read/write on a TCP socket
 *
 *  Not compiled on Windows (the host harness supplies its own transport,
 *  which is the point of the ops struct).  `fd` starts at -1;
 *  io_timeout_ms is used by read/write polls (0 -> 10000).  A viewer with
 *  its own poll loop sets it short (say 20 ms): cdpws_recv then returns 0
 *  instead of blocking, and the connection survives the timeout.  The fd
 *  is non-blocking and the callbacks translate EAGAIN into polling, so the
 *  client's loops see short reads and timeouts, never a hang.
 *
 *  cdpws_net_transport() copies the four callbacks and ctx = net.
 *  cdpws_net_close() is idempotent.
 * ------------------------------------------------------------ */
#ifndef _WIN32
typedef struct {
    int fd;
    int io_timeout_ms;
} CdpwsNet;

CdpwsTransport cdpws_net_transport(CdpwsNet *net);
void           cdpws_net_close(CdpwsNet *net);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MYSTIC_CDPWS_H */
