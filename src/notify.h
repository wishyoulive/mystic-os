/* ============================================================
 *  Mystic OS - notify.h
 *
 *  A notification service. One owner of the list of things that
 *  want the user's attention; three ways to put something in it:
 *
 *    1. in the desktop's own process ........ notify_insert(&srv, ...)
 *    2. from any process, with this library . notify_post(...)
 *    3. from any process, with no library ... one line of text on
 *       /run/mystic/notify.sock (protocol below)
 *
 *  All three land in the same ring, so a shell script's message and
 *  the desktop's own "screenshot saved" look and behave identically.
 *
 *  Design rules, because this runs on every frame of the desktop:
 *
 *    - It allocates nothing. The ring and the client slots live in
 *      the NotifyServer the caller owns, so an entry cannot leak by
 *      construction: the worst case is a full ring, bounded by
 *      NOTIFY_MAX. notify.c never calls malloc.
 *    - It never blocks. Socket clients are non-blocking; a client
 *      that goes away mid-line costs one read() that returns 0.
 *    - It expires entries itself. notify_poll() (or any call to
 *      notify_snapshot()) drops whatever is past its timeout, so
 *      there is no timer to forget and no zombie bubble.
 *    - It draws nothing. notify_snapshot() hands the desktop a copy
 *      of the list with the milliseconds left on each one; see
 *      docs/HOOKS-NOTIFY.md for the drawing contract.
 *
 *  ---- the wire protocol ------------------------------------
 *
 *  One message per line, terminated by \n, fields separated by a TAB.
 *  Inside a field, % = %25, TAB = %09, LF = %0A, CR = %0D; raw bytes
 *  >= 0x20 (including spaces) are sent as themselves, so a message
 *  with no tabs and no newlines needs no escaping at all.
 *
 *    client -> service   HELLO <app>        remembers who is talking
 *    service -> client    OK<TAB>notify/1
 *
 *    client -> service   POST <urgency> <timeout_ms> <app> <tag> <summary> [<body>]
 *    service -> client    OK<TAB><id>                 a new bubble
 *                         OK<TAB><id><TAB>replaced     updated the bubble with that tag
 *                         ERR<TAB><reason>
 *
 *    client -> service   LIST
 *    service -> client    ITEM<TAB><id><TAB><urgency><TAB><remaining_ms><TAB><age_ms>
 *                              <TAB><app><TAB><tag><TAB><summary><TAB><body>
 *                         ...one per active notification, oldest first
 *                         END<TAB><count>
 *
 *    client -> service   DISMISS <id>       ->  OK  /  ERR<TAB>no such id
 *    client -> service   DND <0|1>          ->  OK<TAB><dnd>
 *    client -> service   STATS              ->  STATS<TAB><k>=<v>... one line
 *    client -> service   PING               ->  OK<TAB>notify/1
 *    client -> service   CLOSE              ->  OK, then the service closes
 *
 *  urgency:      0 normal, 1 low, 2 revieweral.
 *  timeout_ms:   >0 expires after that many milliseconds, 0 = stays
 *                until it is dismissed. Capped at NOTIFY_TTL_MAX.
 *  tag:          a non-empty tag replaces the bubble that already has
 *                that tag instead of stacking a second one - this is
 *                how a download reports progress. An empty tag never
 *                replaces.
 *
 *  A message with no library at all:
 *
 *    printf 'POST\t0\t4000\tsh\t\tBuild done\t42 files in 3.1 s\n' |
 *        nc -U /run/mystic/notify.sock
 *
 *  That prints "OK<TAB><id>" back.
 * ============================================================ */
#ifndef MYSTIC_NOTIFY_H
#define MYSTIC_NOTIFY_H

#include <stddef.h>

/* ------------------------------------------------------------
 * Where things live. The socket is created by the service; the
 * history is appended to for every change so a notification the
 * user never saw can still be read afterwards.
 * ------------------------------------------------------------ */
#define NOTIFY_SOCKET        "/run/mystic/notify.sock"
#define NOTIFY_LOG           "/root/notifications/log"
#define NOTIFY_LOG_MAX_BYTES (512 * 1024)   /* rotate to log.1 past this */

/* Capacities. All of them are compile-time: no growth, no realloc. */
#define NOTIFY_MAX           128            /* active notifications    */
#define NOTIFY_MAX_CLIENTS   8              /* concurrent socket users */
#define NOTIFY_LINE          2048           /* one protocol line, max  */
#define NOTIFY_APP_MAX       32
#define NOTIFY_TAG_MAX       48
#define NOTIFY_SUMMARY_MAX   96
#define NOTIFY_BODY_MAX      240

/* urgency */
#define NOTIFY_NORMAL        0
#define NOTIFY_LOW           1
#define NOTIFY_CRITICAL      2

/* the longest a notification may ask to live: one day */
#define NOTIFY_TTL_MAX       (24 * 60 * 60 * 1000)

/* ------------------------------------------------------------
 * One notification, as the desktop sees it.
 *
 * `posted_ms` and `age_ms` are CLOCK_MONOTONIC milliseconds; they
 * are only meaningful relative to each other, which is exactly what
 * an animation needs. `remaining_ms` is -1 for a notification that
 * stays until dismissed.
 * ------------------------------------------------------------ */
typedef struct {
    int  id;                             /* stable, never reused while alive */
    char app[NOTIFY_APP_MAX];            /* who posted it */
    char tag[NOTIFY_TAG_MAX];            /* "" when untagged */
    char summary[NOTIFY_SUMMARY_MAX];    /* one line, folded on receipt */
    char body[NOTIFY_BODY_MAX];          /* may be "" */
    int  urgency;                        /* NOTIFY_NORMAL/LOW/CRITICAL */
    int  ttl_ms;                         /* lifetime it was given, 0 = sticky */
    int  remaining_ms;                   /* -1 = sticky, else ms left */
    int  age_ms;                         /* since it was last posted */
    long long posted_ms;                 /* internal: monotonic ms; do not use */
} NotifyItem;

/* One socket client. fd == -1 means the slot is free. */
typedef struct {
    int  fd;
    int  rxlen;
    int  discard;                        /* dropping an over-long line */
    char app[NOTIFY_APP_MAX];            /* from HELLO, or "" */
    char rx[NOTIFY_LINE];
} NotifyConn;

/* The whole service. Own it somewhere permanent (a static in the
 * desktop, or the stack of the daemon's main). 73 KB; no heap. */
typedef struct {
    NotifyItem ring[NOTIFY_MAX];
    int        head;                     /* index of the oldest entry */
    int        count;                    /* live entries, 0..NOTIFY_MAX */
    int        next_id;                  /* monotonic; 1, 2, 3, ... */
    int        dnd;                      /* do not disturb */
    long long  now_ms;                   /* monotonic, refreshed on entry */

    int        log_fd;                   /* -1 if the history is unwritable */
    int        log_bytes;
    unsigned long logged;

    int        listen_fd;                /* -1 if the socket is not up */
    int        sock_owned;               /* we bound it, so we unlink it */
    int        opened;                   /* notify_open() ran on this struct */
    char       sock_path[108];
    char       log_path[160];
    NotifyConn conns[NOTIFY_MAX_CLIENTS];

    /* counters: every one of them is real, none is a guess */
    unsigned long posted, replaced, expired, dismissed, evicted,
                  suppressed, rejected;
} NotifyServer;

typedef struct {
    int active;                          /* entries in the ring */
    int clients;                         /* connected sockets */
    int dnd;
    int listening;                       /* 1 = the socket is up */
    int logging;                         /* 1 = the history is open */
    unsigned long posted, replaced, expired, dismissed, evicted,
                  suppressed, rejected, logged;
} NotifyStats;

/* ------------------------------------------------------------
 * Lifecycle
 *
 * notify_open() may be called once per server, before anything else.
 * It creates the socket directory and the log directory if they are
 * missing. It returns 0 if the service is usable; if the socket
 * cannot be created it still returns 0 with listen_fd == -1, because
 * in-process insert and snapshot keep working. errno is left from
 * the socket attempt so a caller that cares can look.
 *
 * notify_poll() and notify_close() do nothing on a server that was
 * never opened, so a zeroed struct cannot touch fd 0 by accident.
 * ------------------------------------------------------------ */
int  notify_open(NotifyServer *s, const char *sock_path, const char *log_path);
void notify_close(NotifyServer *s);

/* Accept clients, read them, expire what is due.
 * Call once per frame (or every 100-200 ms in a daemon loop).
 * Returns 1 if the visible list changed, 0 if nothing happened. */
int  notify_poll(NotifyServer *s);

/* ------------------------------------------------------------
 * Posting
 * ------------------------------------------------------------ */

/* In-process. Safe to call from the thread that owns the server, and
 * the only form that is safe when the caller *is* the service (a
 * socket post from inside the desktop would wait for a reply that
 * only its own poll loop can send). Returns the id, or -1 with errno.
 *
 * Text is truncated to the field size on a UTF-8 boundary and folded
 * to one line: newlines, tabs and other control bytes become a single
 * space. A non-empty tag that is already on screen updates that entry
 * and keeps its id, and the update restarts its timeout (a download
 * that keeps moving must not expire under the progress bar); otherwise
 * the id is new. Under do-not-disturb a new NORMAL or LOW notification
 * is logged and then dropped (the id is still returned); CRITICAL ones
 * are never suppressed. */
int  notify_insert(NotifyServer *s, const char *app, const char *tag,
                   const char *summary, const char *body,
                   int urgency, int timeout_ms);

/* Over the socket, for any other process. notify_post() is the plain
 * four-argument form; notify_post_full() lets the caller set the app
 * name and the tag. Returns the id, or -1 with errno:
 *   ENOENT/ECONNREFUSED  the service is not running
 *   EINVAL               empty summary, urgency not 0..2, timeout < 0
 *   EIO                  the service answered something unexpected
 * MYSTIC_NOTIFY_SOCKET, if set, overrides NOTIFY_SOCKET. */
int  notify_post(const char *summary, const char *body,
                 int urgency, int timeout_ms);
int  notify_post_full(const char *app, const char *tag,
                      const char *summary, const char *body,
                      int urgency, int timeout_ms);

/* Fetch the live list over the socket (for a notification-centre app
 * that is not the desktop). Returns the number stored, or -1. */
int  notify_fetch(NotifyItem *out, int max);

/* ------------------------------------------------------------
 * Reading the list - what the desktop draws from
 *
 * Copies at most `max` entries, oldest first, with age_ms and
 * remaining_ms computed against the clock at the moment of the call.
 * Also runs the expiry sweep, so a snapshot is never stale.
 * Returns the number written; notify_count() has the true total.
 * ------------------------------------------------------------ */
int  notify_snapshot(NotifyServer *s, NotifyItem *out, int max);
int  notify_count(const NotifyServer *s);

/* Dismiss / clear. Return 1 if something was removed. Both log. */
int  notify_dismiss(NotifyServer *s, int id);
int  notify_clear(NotifyServer *s);

/* Do not disturb: get, and set (returns the previous value). While it
 * is on, new NORMAL and LOW notifications are written to the history
 * and not shown. CRITICAL ones still are. */
int  notify_dnd(const NotifyServer *s);
int  notify_dnd_set(NotifyServer *s, int on);

void notify_stats(const NotifyServer *s, NotifyStats *out);

/* ------------------------------------------------------------
 * The wire encoding, public because a client that speaks the
 * protocol without linking this library needs exactly this.
 * Both behave like snprintf: they return the length the result needs
 * (not counting the NUL) and write as much as fits in `cap`.
 * ------------------------------------------------------------ */
int  notify_escape(const char *in, char *out, size_t cap);
int  notify_unescape(const char *in, char *out, size_t cap);

/* ------------------------------------------------------------
 * The daemon. Blocks forever; returns only if the socket cannot be
 * created (returns -1, errno set). Build it with -DNOTIFY_DAEMON to
 * get a main(), or call it from your own main.
 * ------------------------------------------------------------ */
int  notify_daemon(const char *sock_path, const char *log_path);

#endif /* MYSTIC_NOTIFY_H */
