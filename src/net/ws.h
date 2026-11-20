/* ============================================================
 *  Mystic OS - net/ws.h
 *
 *  An RFC 6455 WebSocket client, written from the RFC.
 *
 *  It owns no socket.  Like src/client/tls.h, every byte moves through
 *  two callbacks the caller supplies, so the same code runs over a TLS
 *  session, a plain TCP socket, or a pair of memory buffers in a test
 *  harness:
 *
 *      static long my_read(void *ctx, void *buf, size_t n);
 *      static long my_write(void *ctx, const void *buf, size_t n);
 *
 *      WsConfig cfg = {0};
 *      cfg.tr.ctx   = &my_socket;
 *      cfg.tr.read  = my_read;
 *      cfg.tr.write = my_write;
 *      WsConn *ws = ws_conn_new(&cfg);
 *      if (!ws) ...                // bad callbacks or out of memory
 *
 *      uint8_t key[16];
 *      fill_from_a_real_random_source(key);   // §4.1 needs 16 bytes
 *      WsRequest rq = {0};
 *      rq.host = "example.com";
 *      rq.path = "/chat";
 *      rq.key  = key;              // supplied, not generated: testable
 *      WsResponse rs;
 *      char err[200];
 *      int rc = ws_handshake(ws, &rq, &rs, err, sizeof err);
 *
 *      WsMessage m;
 *      for (;;) {
 *          rc = ws_recv(ws, &m);
 *          if (rc == WS_ERR_AGAIN) { wait_until_readable(); continue; }
 *          if (rc != WS_OK) break;          // err says which rule broke
 *          if (m.opcode == WS_OP_TEXT)   got_text(m.data, m.len);
 *          if (m.opcode == WS_OP_CLOSE)  break;
 *      }
 *      ws_conn_free(ws);
 *
 *  WHAT IS IMPLEMENTED
 *
 *    §4.1  The upgrade request (Host, Upgrade, Connection,
 *          Sec-WebSocket-Key, Sec-WebSocket-Version: 13, optional
 *          Origin and Sec-WebSocket-Protocol) and the response check:
 *          status 101, Upgrade: websocket, Connection: Upgrade, and
 *          Sec-WebSocket-Accept equal to base64(SHA-1(text || GUID))
 *          where text is the base64 key header value (not its 16
 *          decoded bytes).  A response with any other status, a
 *          missing or wrong header, an extension the client did not
 *          offer, or a subprotocol it did not ask for fails the
 *          handshake with a sentence naming the rule.
 *
 *    §5.2  All six opcodes, the FIN bit, and RSV1..3 refused: this
 *          client negotiates no extension, so §5.2's "MUST fail the
 *          connection" applies to any reserved bit.
 *          The 7-bit, 16-bit and 64-bit payload length forms, in both
 *          directions.  A 64-bit length with its top bit set is a
 *          protocol error (§5.2: "the most significant bit MUST be 0").
 *
 *    §5.3  Every client frame is masked, including a zero-length one,
 *          with a fresh 32-bit key per frame from the caller's random
 *          source (§5.3 requires a new unpredictable key per frame,
 *          RFC 4086 for the source).
 *
 *    §5.4  Fragmented messages: a text/binary frame with FIN=0 starts
 *          one, continuation frames extend it, control frames may be
 *          interleaved, and ws_recv delivers the assembled message once,
 *          whole.  A continuation with nothing to continue, or a new
 *          data frame in the middle of a fragmented message, fails the
 *          connection with 1002.
 *
 *    §5.5  Ping answered automatically with a pong carrying the same
 *          application data (§5.5.3), and both control frames delivered
 *          to the caller as messages.  The close handshake in both
 *          directions, the received status code checked against §7.4
 *          and the reason checked as UTF-8, and the reply sent
 *          automatically when the peer closes first (§5.5.1).
 *
 *    §5.6  Text payloads are checked as UTF-8 before delivery and
 *          before sending, because §8.1 says an endpoint that sees
 *          invalid UTF-8 in a text message MUST fail the connection.
 *          A fragmented text message is validated incrementally, so a
 *          codepoint split across two fragments is accepted while an
 *          overlong, surrogate or truncated one is refused.
 *
 *    §7.4  Close codes 1000-1003, 1007-1011 and 3000-4999 are the only
 *          ones accepted on the wire or sent.  1004 is reserved, 1005,
 *          1006 and 1015 MUST NOT appear in a Close frame, and 0-999
 *          are unused.
 *
 *  WHAT IS NOT
 *
 *    No extensions: no permessage-deflate (§7.1.1 is a separate
 *    negotiation), so RSV1 set in a server frame is a protocol error,
 *    and a server that selects an extension fails the handshake.
 *    No redirects, no proxying, no cookies, no client certificates:
 *    those belong to the HTTP layer that owns the transport.  A 3xx is
 *    reported in WsResponse.status and the caller decides.
 *    No HTTP/2: RFC 8441 bootstraps WebSocket over HTTP/2, which needs
 *    the HPACK and framing work in net/hpack.c and the h2 layer.
 *
 *  LIMITS ARE THIS IMPLEMENTATION'S, NOT THE RFC'S
 *
 *    WS_MAX_HANDSHAKE   response headers larger than this are refused
 *                       rather than buffered without bound.
 *    WS_DEFAULT_MAX_MESSAGE
 *                       a peer cannot make the client allocate more
 *                       than 16 MiB for one message; over that the
 *                       connection fails with 1009 (§7.4.1 defines
 *                       1009 as "message too big").
 *    WS_MAX_*           the request parts a caller may pass, so the
 *                       request is built in a fixed stack buffer and a
 *                       header-injection attempt has a length to trip.
 *
 *  THREADING: one WsConn is used by one thread at a time.  Nothing here
 *  is locked; a browser drives it from its network thread.
 * ============================================================ */
#ifndef MYSTIC_NET_WS_H
#define MYSTIC_NET_WS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------
 *  Opcodes (RFC 6455 §5.2, the four-bit opcode field)
 * ------------------------------------------------------------ */
#define WS_OP_CONT      0x0   /* continuation of a fragmented message */
#define WS_OP_TEXT      0x1
#define WS_OP_BINARY    0x2
#define WS_OP_CLOSE     0x8
#define WS_OP_PING      0x9
#define WS_OP_PONG      0xA

/* ------------------------------------------------------------
 *  Status codes (RFC 6455 §7.4.1).  These are the numbers that go in
 *  the first two bytes of a Close frame's payload, not HTTP statuses.
 * ------------------------------------------------------------ */
#define WS_CLOSE_NORMAL          1000  /* normal closure                  */
#define WS_CLOSE_GOING_AWAY      1001  /* endpoint going away             */
#define WS_CLOSE_PROTOCOL_ERROR  1002  /* protocol error                  */
#define WS_CLOSE_UNSUPPORTED     1003  /* cannot accept the data type     */
#define WS_CLOSE_NO_STATUS       1005  /* never sent: no code was present */
#define WS_CLOSE_ABNORMAL        1006  /* never sent: no Close frame      */
#define WS_CLOSE_BAD_PAYLOAD     1007  /* inconsistent data, e.g. bad UTF-8 */
#define WS_CLOSE_POLICY          1008  /* policy violation                */
#define WS_CLOSE_TOO_BIG         1009  /* message too big                 */
#define WS_CLOSE_EXTENSION       1010  /* required extension not offered  */
#define WS_CLOSE_INTERNAL        1011  /* server hit an unexpected error  */
#define WS_CLOSE_TLS             1015  /* never sent: TLS handshake failed */

/* ------------------------------------------------------------
 *  Constants from the RFC
 * ------------------------------------------------------------ */
/* §1.3: the GUID concatenated with |Sec-WebSocket-Key| before hashing. */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* §4.1: the version this client speaks, and the only one it offers. */
#define WS_VERSION "13"

/* §4.1: a client key is 16 bytes, which base64 encodes to 24 characters. */
#define WS_KEY_LEN       16
#define WS_KEY_B64_LEN   24
/* §4.1: the accept value is base64 of a 20-byte SHA-1 digest. */
#define WS_ACCEPT_LEN    28
#define WS_ACCEPT_BUF    32

/* ------------------------------------------------------------
 *  Implementation limits (documented above, not RFC constants)
 * ------------------------------------------------------------ */
#define WS_MAX_HANDSHAKE     16384
#define WS_DEFAULT_MAX_MESSAGE (16u * 1024u * 1024u)
#define WS_MAX_HOST          255
#define WS_MAX_PATH          2048
#define WS_MAX_ORIGIN        512
#define WS_MAX_PROTOCOLS     512
/* Worst case: request line + every header + CRLFCRLF.  The slack covers
 * the fixed header names and the longest decimal Content-Length-shaped
 * numbers this builder can emit (there are none, but the buffer must not
 * be sized by wishful thinking). */
#define WS_MAX_REQUEST (WS_MAX_PATH + WS_MAX_HOST + WS_MAX_ORIGIN + \
                        WS_MAX_PROTOCOLS + 256)

/* ------------------------------------------------------------
 *  Error codes.  Every public function returns WS_OK or one of these;
 *  nothing here sets errno, and nothing returns a positive value.
 *
 *  WS_ERR_AGAIN is not a failure: the transport had no bytes this time
 *  and the call may be repeated.  The connection keeps whatever it had
 *  read, so a frame split across two reads of a non-blocking socket is
 *  resumed correctly.
 * ------------------------------------------------------------ */
enum {
    WS_OK            =   0,
    WS_ERR_INVAL     =  -1,  /* bad argument, or a value the RFC forbids */
    WS_ERR_NOMEM     =  -2,  /* allocation failed                       */
    WS_ERR_AGAIN     =  -3,  /* no bytes available now; call again      */
    WS_ERR_EOF       =  -4,  /* transport ended without a Close frame   */
    WS_ERR_IO        =  -5,  /* transport read or write failed          */
    WS_ERR_HANDSHAKE =  -6,  /* response did not satisfy §4.1           */
    WS_ERR_STATUS    =  -7,  /* response was not 101; see WsResponse    */
    WS_ERR_PROTO     =  -8,  /* peer broke RFC 6455; a Close was sent   */
    WS_ERR_TOOBIG    =  -9,  /* message over max_message; Close 1009    */
    WS_ERR_UTF8      = -10,  /* text or close reason is not UTF-8       */
    WS_ERR_CLOSED    = -11,  /* connection already closed               */
    WS_ERR_STATE     = -12,  /* wrong opcode for the current state      */
    WS_ERR_RANDOM    = -13,  /* no mask source: nothing was sent        */
    WS_ERR_NOSPACE   = -14   /* caller's buffer is too small            */
};

/* A read callback returns WS_IO_AGAIN (-2) to mean "nothing now".
 * That value is part of the transport contract, not an error code. */
#define WS_IO_AGAIN (-2L)

/* ------------------------------------------------------------
 *  Transport.  The same shape as TlsReadFn/TlsWriteFn in
 *  src/client/tls.h so a caller can hand the result of a TLS session
 *  straight to this client:
 *
 *    read   > 0   bytes read (a short read is fine; the client loops)
 *           0     orderly end of stream
 *           WS_IO_AGAIN  nothing available right now
 *           other < 0    failure
 *
 *    write  = len  every byte was written (the normal case)
 *           > 0    a short write; the client calls again for the rest
 *           <= 0   failure.  A frame is never retried from its start, so
 *                  a failure here closes the connection: a half-written
 *                  frame cannot be repaired.
 *
 *  Neither callback may block forever: a socket with a receive timeout
 *  should map EAGAIN/EWOULDBLOCK to WS_IO_AGAIN, and a non-blocking
 *  writer should buffer inside the callback.
 * ------------------------------------------------------------ */
typedef long (*WsReadFn)(void *ctx, void *buf, size_t len);
typedef long (*WsWriteFn)(void *ctx, const void *buf, size_t len);

/* Fill buf with len bytes that an attacker cannot predict: RFC 6455
 * §5.3 asks for a fresh masking key from a strong source, and RFC 4086
 * says what "strong" means.  Return 0 on success, -1 on failure.
 *
 * If this is NULL the client reads /dev/urandom.  If that cannot be
 * opened (a host without it) every send fails with WS_ERR_RANDOM.  There
 * is deliberately no weaker fallback: a predictable masking key breaks
 * the reason the mask exists, and a quiet downgrade would be worse than
 * a loud error. */
typedef int (*WsRandFn)(void *ctx, void *buf, size_t len);

typedef struct {
    void       *ctx;
    WsReadFn    read;
    WsWriteFn   write;
} WsTransport;

typedef struct {
    WsTransport tr;           /* required: both callbacks               */
    size_t      max_message;  /* 0 = WS_DEFAULT_MAX_MESSAGE            */
    WsRandFn    rand;         /* NULL = /dev/urandom                    */
    void       *rand_ctx;
} WsConfig;

/* ------------------------------------------------------------
 *  The upgrade request (§4.1).  `key` points at exactly WS_KEY_LEN
 *  bytes and is the caller's, so a test can replay the RFC's own
 *  example instead of hoping a random key happens to match.
 * ------------------------------------------------------------ */
typedef struct {
    const char    *host;      /* required: Host: value, "name" or "name:port" */
    const char    *path;      /* required: starts with '/', e.g. "/chat?x=1"  */
    const char    *origin;    /* optional: Origin: value                      */
    const char    *protocols; /* optional: "chat, superchat", comma separated  */
    const uint8_t *key;       /* required: WS_KEY_LEN bytes                   */
} WsRequest;

/* What the server answered.  Strings are NUL terminated and empty when
 * the header was absent.  `extensions` counts Sec-WebSocket-Extensions
 * headers; a handshake that returns WS_OK always has it at 0. */
typedef struct {
    int  status;                    /* the three-digit status code      */
    char accept[WS_ACCEPT_BUF];     /* Sec-WebSocket-Accept as received */
    char protocol[128];             /* selected subprotocol, or ""      */
    char upgrade[32];
    char connection[64];
    int  extensions;
} WsResponse;

/* ------------------------------------------------------------
 *  A delivered message.  `data` points into the connection and stays
 *  valid until the next call on it, so a caller that keeps the bytes
 *  copies them.  Only complete messages are delivered: a fragmented
 *  text message arrives as one WS_OP_TEXT with the whole payload, and
 *  a control frame that interrupted it arrives separately, before it.
 *
 *  For WS_OP_CLOSE, `close_code` is the status code and data still
 *  points at the frame payload, so the reason text (if any) is
 *  data + 2 and len - 2 bytes.  close_code is 0 when the peer sent a
 *  Close with no status code at all (§5.5.1 allows that; it means 1005).
 * ------------------------------------------------------------ */
typedef struct {
    int             opcode;
    const uint8_t  *data;
    size_t          len;
    uint16_t        close_code;
} WsMessage;

typedef struct WsConn WsConn;

/* ============================================================
 *  Functions
 * ============================================================ */

/* base64(SHA-1(base64(key) || WS_GUID)), NUL terminated (29 bytes).
 * `key` is the 16 random bytes; §4.1 hashes the Sec-WebSocket-Key
 * header field *value* (the 24-character base64 of those bytes), not
 * the bytes themselves.  The RFC's example: the bytes spelling "the
 * sample nonce" encode to "dGhlIHNhbXBsZSBub25jZQ==" and must give
 * "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=".  This is the value of
 * Sec-WebSocket-Accept the server must return; exposed so a caller can
 * log or test it.  Returns WS_OK, WS_ERR_INVAL or (only if the fixed
 * output buffer were ever too small) WS_ERR_NOSPACE. */
int ws_accept_key(const uint8_t key[WS_KEY_LEN], char out[WS_ACCEPT_BUF]);

/* The RFC 6455 §5.3 masking transform.  XOR is its own inverse, so the
 * same call masks and unmasks; `offset` is the index of data[0] within
 * the frame payload, which matters when a payload is transformed in
 * pieces.  Exposed for tests and for callers that keep their own
 * buffers; the frame writer uses it internally. */
void ws_mask(void *data, size_t len, uint32_t mask_key, size_t offset);

/* Build the upgrade request into buf.  Returns the length written, not
 * counting the NUL, or WS_ERR_NOSPACE if cap is too small (nothing is
 * written then), or WS_ERR_INVAL for a missing/oversized/unsafe part.
 * CR, LF and other control bytes are refused in every part: a caller
 * that passes a request path built from a URL must not be able to
 * inject a header. */
int ws_build_request(const WsRequest *req, char *buf, size_t cap);

WsConn *ws_conn_new(const WsConfig *cfg);
void    ws_conn_free(WsConn *c);

/* Perform the upgrade.  On WS_OK the connection is open, `resp` holds
 * what the server said and err is empty.  On WS_ERR_AGAIN nothing has
 * failed; call again with the same arguments (the request is sent once,
 * and the half-read response is kept).  On any other error, err holds a
 * sentence naming the rule that was not met and describes the response,
 * e.g. "Sec-WebSocket-Accept is ... but SHA-1 of the key gives ...".
 * The connection is not usable after a failed handshake. */
int ws_handshake(WsConn *c, const WsRequest *req, WsResponse *resp,
                 char *err, size_t errsz);

/* Read the next message.
 *
 *   WS_OK          *msg is a complete message (text, binary, ping, pong
 *                  or close).  Pings have already been answered.
 *   WS_ERR_AGAIN   nothing available; call again later.
 *   WS_ERR_CLOSED  the close handshake is over; nothing more will come.
 *   WS_ERR_EOF     the transport ended with no Close frame (1006).
 *   WS_ERR_PROTO, WS_ERR_UTF8, WS_ERR_TOOBIG
 *                  the peer broke the protocol.  A Close frame with the
 *                  matching status code (1002, 1007, 1009) has been sent
 *                  first, best effort, and the connection is finished.
 *
 * A received Close is returned once as WS_OP_CLOSE; the reply Close has
 * already been sent, and the next call returns WS_ERR_CLOSED. */
int ws_recv(WsConn *c, WsMessage *msg);

/* Write one frame.  `fin` is 0 for a fragment to be continued.  This is
 * the only way to send a fragmented message: start with TEXT or BINARY
 * and fin=0, continue with WS_OP_CONT, end with fin=1.  Control frames
 * may be sent between fragments.  CLOSE is refused here; use
 * ws_send_close so the close bookkeeping cannot be bypassed.
 *
 * Returns WS_OK, WS_ERR_CLOSED after a Close has been sent,
 * WS_ERR_STATE for a continuation with nothing to continue (or a new
 * data frame while one is open), WS_ERR_INVAL for a control frame with
 * fin=0, more than 125 payload bytes, or an unknown opcode,
 * WS_ERR_UTF8 when a TEXT or continuation fragment leaves the text
 * message invalid (nothing from that fragment is sent), and
 * WS_ERR_RANDOM if no masking key could be produced (nothing is sent).
 * A UTF-8 failure on a continuation means earlier fragments are
 * already on the wire; the caller should call ws_send_close with 1007
 * rather than continue the message. */
int ws_send_frame(WsConn *c, int opcode, int fin, const void *data, size_t len);

/* One complete message: opcode TEXT or BINARY, fin=1.  Text is checked
 * as UTF-8 first.  Returns WS_ERR_STATE if a fragmented message is open. */
int ws_send(WsConn *c, int opcode, const void *data, size_t len);
int ws_send_text(WsConn *c, const char *text);   /* strlen, UTF-8 checked */

int ws_ping(WsConn *c, const void *data, size_t len);  /* len <= 125 */
int ws_pong(WsConn *c, const void *data, size_t len);  /* len <= 125 */

/* Start the closing handshake (§5.5.1).  code 0 sends a Close with no
 * status code (the same thing as 1005, which may not be put on the
 * wire); otherwise code must be one this client may send and reason, if
 * present, must be valid UTF-8 of at most 123 bytes so the payload fits
 * the 125-byte control frame limit.  After this, no further frames are
 * sent; call ws_recv until it reports the peer's Close or EOF. */
int ws_send_close(WsConn *c, uint16_t code, const char *reason);

int ws_is_closed(const WsConn *c);      /* 1: a Close was sent or received */
int ws_close_sent(const WsConn *c);     /* 1: our Close went out           */

const char *ws_strerror(int rc);        /* static string, never NULL       */
const char *ws_opcode_name(int opcode); /* "text", "binary", ...           */

#ifdef __cplusplus
}
#endif

#endif /* MYSTIC_NET_WS_H */
