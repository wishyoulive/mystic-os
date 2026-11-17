/* ============================================================
 *  Mystic OS - notify.c
 *
 *  The notification service. Read notify.h first: it has the wire
 *  protocol and the API. This file is the ring, the expiry clock,
 *  the history, and the socket that lets any process post.
 *
 *  Two things are worth knowing before reading the code:
 *
 *  1. There is no malloc in this file, and there never should be.
 *     Everything - the 128-entry ring, the 8 client slots, every
 *     string - is part of the NotifyServer the caller owns. A full
 *     ring evicts; it never grows. You cannot leak what you never
 *     allocated.
 *
 *  2. Time comes from CLOCK_MONOTONIC, not from the wall clock, and
 *     is stored as the millisecond the entry was posted. Expiry is
 *     therefore just (now - posted >= ttl): it survives a clock
 *     jump, it cannot overflow, and it needs no timer wheel.
 * ============================================================ */
#include "notify.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>

static void log_item(NotifyServer *s, const char *ev, const NotifyItem *it,
                     const char *note);

/* ============================================================
 *  Small helpers
 * ============================================================ */

/* Monotonic milliseconds. CLOCK_REALTIME is the fallback because a
 * system where the monotonic clock fails still has to expire. */
static long long mono_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    }
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long wall_secs(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (long long)ts.tv_sec;
}

/* Copy with a hard cap, cutting on a UTF-8 boundary so a truncated
 * summary never ends in half a character (which would draw as a box). */
static void copy_trunc(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
        if (((unsigned char)src[n] & 0xC0) == 0x80) {
            while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Anything that arrives may be multi-line: a terminal pasting five
 * lines of compiler error, a curl output. A bubble is one line, and
 * the history is one line per event, so control bytes collapse to a
 * single space, leading and trailing spaces go. */
static void fold_text(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        unsigned char c = (unsigned char)*r;
        if (c < 0x20 || c == 0x7F) {
            if (w > s && w[-1] != ' ') *w++ = ' ';
            continue;
        }
        *w++ = (char)c;
    }
    while (w > s && w[-1] == ' ') w--;
    *w = 0;
}

static void norm_text(char *dst, size_t cap, const char *src) {
    copy_trunc(dst, cap, src ? src : "");
    fold_text(dst);
}

/* 1970-01-01T00:00:00Z from a unix time, without gmtime()'s static
 * buffer or the locale. days-from-civil, Hinnant's algorithm. */
static void iso_utc(long long secs, char *out, size_t cap) {
    long long days = secs / 86400;
    long long rem = secs % 86400;
    if (rem < 0) { rem += 86400; days--; }
    int hh = (int)(rem / 3600), mm = (int)(rem % 3600 / 60), ss = (int)(rem % 60);
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - era * 146097);
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long mp = (5 * doy + 2) / 153;    /* month, 0 = March */
    unsigned long d = doy - (153 * mp + 2) / 5 + 1;
    long m = (long)mp + (mp < 10 ? 3 : -9);     /* back to 1..12 */
    y += (m <= 2);
    snprintf(out, cap, "%04lld-%02ld-%02luT%02d:%02d:%02dZ", y, m, d, hh, mm, ss);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ============================================================
 *  Percent encoding - the one thing the wire needs
 *
 *  TAB separates fields and \n ends a line, so those two (plus CR
 *  and the escape itself) are the only bytes that must be hidden.
 *  Spaces stay as spaces, which is why a shell one-liner needs no
 *  escaping at all for ordinary text.
 *
 *  Both functions behave like snprintf: they return the length the
 *  result needs, and write as much as fits. Callers that care about
 *  truncation compare the result with the capacity.
 * ============================================================ */
int notify_escape(const char *in, char *out, size_t cap) {
    size_t j = 0, need = 0;
    if (!in) in = "";
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        const char *esc = NULL;
        if (*p == '%')       esc = "%25";
        else if (*p == '\t') esc = "%09";
        else if (*p == '\n') esc = "%0A";
        else if (*p == '\r') esc = "%0D";
        if (esc) {
            if (j + 3 < cap) { memcpy(out + j, esc, 3); j += 3; }
            need += 3;
        } else {
            if (j + 1 < cap) out[j++] = (char)*p;
            need++;
        }
    }
    if (cap) out[j] = 0;
    return (int)need;
}

int notify_unescape(const char *in, char *out, size_t cap) {
    size_t j = 0, need = 0;
    if (!in) in = "";
    for (size_t i = 0; in[i]; ) {
        int v = -1;
        if (in[i] == '%' && hexval(in[i + 1]) >= 0 && hexval(in[i + 2]) >= 0) {
            v = hexval(in[i + 1]) * 16 + hexval(in[i + 2]);
            i += 3;
            if (v == 0) continue;                /* a NUL would end the field */
        } else {
            v = (unsigned char)in[i];
            i++;
        }
        if (j + 1 < cap) out[j++] = (char)v;
        need++;
    }
    if (cap) out[j] = 0;
    return (int)need;
}

/* ============================================================
 *  The ring
 *
 *  Logical index 0 is the oldest entry, so the desktop draws the
 *  list in the order things happened. Removing an entry shifts the
 *  rest down by one, which at 128 entries costs nothing and keeps
 *  the order without a second index.
 * ============================================================ */
static NotifyItem *at(NotifyServer *s, int i) {
    return &s->ring[(s->head + i) % NOTIFY_MAX];
}

static void ring_remove(NotifyServer *s, int i) {
    for (int k = i; k + 1 < s->count; k++) *at(s, k) = *at(s, k + 1);
    s->count--;
    if (s->count == 0) s->head = 0;
}

static void ring_push(NotifyServer *s, const NotifyItem *it) {
    s->ring[(s->head + s->count) % NOTIFY_MAX] = *it;
    s->count++;
}

/* Drop everything past its timeout. Runs on every poll and on every
 * snapshot, so a caller that only ever draws still gets expiry. */
static int sweep(NotifyServer *s) {
    int n = 0;
    for (int i = 0; i < s->count; ) {
        NotifyItem *it = at(s, i);
        if (it->ttl_ms > 0 && s->now_ms - it->posted_ms >= it->ttl_ms) {
            log_item(s, "expire", it, "");
            ring_remove(s, i);
            s->expired++;
            n++;
        } else {
            i++;
        }
    }
    return n;
}

/* ============================================================
 *  History: /root/notifications/log
 *
 *  Append-only, one line per change, so a notification that came and
 *  went while the user was in a terminal is still there to read. One
 *  write() per line: a reader tailing the file never sees half of a
 *  line, and a power cut costs at most the last line.
 * ============================================================ */
static void mkdir_parent(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return;
    char dir[160];
    size_t n = (size_t)(slash - path);
    if (n >= sizeof dir) return;
    memcpy(dir, path, n);
    dir[n] = 0;
    mkdir(dir, 0755);          /* EEXIST is the normal case */
}

static void log_open(NotifyServer *s) {
    if (s->log_fd >= 0) return;
    mkdir_parent(s->log_path);
    int fd = open(s->log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;                       /* history is best effort */
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    s->log_fd = fd;
    struct stat st;
    s->log_bytes = (fstat(fd, &st) == 0 && st.st_size > 0) ? (int)st.st_size : 0;
}

/* One generation of rotation: log -> log.1. A desktop that runs for
 * months must not fill the disk with one file. */
static void log_rotate(NotifyServer *s) {
    if (s->log_fd >= 0) close(s->log_fd);
    s->log_fd = -1;
    char old[192];
    snprintf(old, sizeof old, "%s.1", s->log_path);
    if (rename(s->log_path, old) != 0) {
        remove(old);
        rename(s->log_path, old);
    }
    s->log_bytes = 0;
    log_open(s);
}

static int fd_write_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        long w = (long)write(fd, p, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void log_printf(NotifyServer *s, const char *fmt, ...) {
    if (s->log_fd < 0) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;

    if (s->log_bytes + n > NOTIFY_LOG_MAX_BYTES) log_rotate(s);
    if (s->log_fd < 0) return;
    if (fd_write_all(s->log_fd, buf, (size_t)n) != 0) {
        close(s->log_fd);
        s->log_fd = -1;
        return;
    }
    s->log_bytes += n;
    s->logged++;
}

static void log_item(NotifyServer *s, const char *ev, const NotifyItem *it,
                     const char *note) {
    if (s->log_fd < 0) return;
    char iso[24], app[NOTIFY_APP_MAX + 8], tag[NOTIFY_TAG_MAX + 8];
    char sum[NOTIFY_SUMMARY_MAX + 8], body[NOTIFY_BODY_MAX + 8];
    iso_utc(wall_secs(), iso, sizeof iso);
    /* strip anything that would break the one-line rule */
    copy_trunc(app, sizeof app, it->app);
    copy_trunc(tag, sizeof tag, it->tag);
    copy_trunc(sum, sizeof sum, it->summary);
    copy_trunc(body, sizeof body, it->body);
    fold_text(app); fold_text(tag); fold_text(sum); fold_text(body);
    log_printf(s, "%s %s id=%d urgency=%d ttl=%d app=%s tag=%s summary=%s body=%s%s\n",
               iso, ev, it->id, it->urgency, it->ttl_ms, app, tag, sum, body,
               note ? note : "");
}

/* ============================================================
 *  Posting
 * ============================================================ */
int notify_insert(NotifyServer *s, const char *app, const char *tag,
                  const char *summary, const char *body,
                  int urgency, int timeout_ms) {
    if (!s) { errno = EINVAL; return -1; }
    if (!summary || !*summary) { errno = EINVAL; s->rejected++; return -1; }    if (urgency < NOTIFY_NORMAL || urgency > NOTIFY_CRITICAL) {
        errno = EINVAL;
        s->rejected++;
        return -1;
    }
    if (timeout_ms < 0) { errno = EINVAL; s->rejected++; return -1; }
    if (timeout_ms > NOTIFY_TTL_MAX) timeout_ms = NOTIFY_TTL_MAX;
    if (s->next_id <= 0) s->next_id = 1;      /* a zeroed server still works in-process */

    s->now_ms = mono_ms();
    sweep(s);       /* a tagged update must not revive an expired bubble */

    NotifyItem it;
    memset(&it, 0, sizeof it);
    norm_text(it.app, sizeof it.app, (app && *app) ? app : "unknown");
    norm_text(it.tag, sizeof it.tag, tag);
    norm_text(it.summary, sizeof it.summary, summary);
    norm_text(it.body, sizeof it.body, body);
    it.urgency = urgency;
    it.ttl_ms = timeout_ms;
    it.posted_ms = s->now_ms;

    /* Same tag: this is the third "47% done" of one download, not a
     * stack of three bubbles. It keeps its id, so a click that was
     * arriving still refers to the same thing. */
    if (it.tag[0]) {
        for (int i = 0; i < s->count; i++) {
            NotifyItem *cur = at(s, i);
            if (strcmp(cur->tag, it.tag) != 0) continue;
            it.id = cur->id;
            *cur = it;
            s->replaced++;
            log_item(s, "replace", cur, "");
            return cur->id;
        }
    }

    it.id = s->next_id++;
    if (s->next_id <= 0) s->next_id = 1;      /* ids never go backwards */

    if (s->dnd && urgency != NOTIFY_CRITICAL) {
        s->suppressed++;
        log_item(s, "suppress", &it, " dnd=1");
        return it.id;                          /* accepted, not shown */
    }

    if (s->count >= NOTIFY_MAX) {
        /* Full ring: the oldest notification that is not critical goes;
         * if every single one is critical, the oldest goes anyway. The
         * ring cannot grow, so this is the only choice there is. */
        int victim = 0;
        for (int i = 0; i < s->count; i++) {
            if (at(s, i)->urgency != NOTIFY_CRITICAL) { victim = i; break; }
        }
        log_item(s, "evict", at(s, victim), " reason=ring-full");
        ring_remove(s, victim);
        s->evicted++;
    }

    ring_push(s, &it);
    s->posted++;
    log_item(s, "post", &it, "");
    return it.id;
}

int notify_dismiss(NotifyServer *s, int id) {
    if (!s) return 0;
    s->now_ms = mono_ms();
    for (int i = 0; i < s->count; i++) {
        NotifyItem *it = at(s, i);
        if (it->id != id) continue;
        log_item(s, "dismiss", it, "");
        ring_remove(s, i);
        s->dismissed++;
        return 1;
    }
    return 0;
}

int notify_clear(NotifyServer *s) {
    if (!s) return 0;
    s->now_ms = mono_ms();
    int n = s->count;
    while (s->count > 0) {
        log_item(s, "dismiss", at(s, 0), " all=1");
        ring_remove(s, 0);
        s->dismissed++;
    }
    return n;
}

int notify_dnd(const NotifyServer *s) {
    return s ? s->dnd : 0;
}

int notify_dnd_set(NotifyServer *s, int on) {
    if (!s) return 0;
    int prev = s->dnd;
    s->dnd = on ? 1 : 0;
    if (prev != s->dnd) {
        char iso[24];
        iso_utc(wall_secs(), iso, sizeof iso);
        log_printf(s, "%s dnd id=0 urgency=0 ttl=0 app=service tag= "
                      "summary=do-not-disturb body=%s\n",
                   iso, s->dnd ? "on" : "off");
    }
    return prev;
}

/* ============================================================
 *  Reading
 * ============================================================ */
static void fill_view(NotifyServer *s, int i, long long now, NotifyItem *out) {
    const NotifyItem *it = at(s, i);
    *out = *it;
    long long age = now - it->posted_ms;
    if (age < 0) age = 0;
    out->age_ms = age > 0x7FFFFFFF ? 0x7FFFFFFF : (int)age;
    if (it->ttl_ms > 0) {
        long long left = (long long)it->ttl_ms - age;
        out->remaining_ms = left > 0 ? (int)left : 0;
    } else {
        out->remaining_ms = -1;
    }
}

int notify_snapshot(NotifyServer *s, NotifyItem *out, int max) {
    if (!s) return 0;
    long long now = mono_ms();
    s->now_ms = now;
    sweep(s);                     /* even a read-only caller keeps it clean */
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < s->count && n < max; i++) fill_view(s, i, now, &out[n++]);
    return n;
}

int notify_count(const NotifyServer *s) {
    return s ? s->count : 0;
}

void notify_stats(const NotifyServer *s, NotifyStats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!s) return;
    out->active = s->count;
    out->dnd = s->dnd;
    out->listening = s->listen_fd >= 0;
    out->logging = s->log_fd >= 0;
    for (int i = 0; i < NOTIFY_MAX_CLIENTS; i++) if (s->conns[i].fd >= 0) out->clients++;
    out->posted = s->posted;
    out->replaced = s->replaced;
    out->expired = s->expired;
    out->dismissed = s->dismissed;
    out->evicted = s->evicted;
    out->suppressed = s->suppressed;
    out->rejected = s->rejected;
    out->logged = s->logged;
}

/* ============================================================
 *  The socket
 * ============================================================ */

/* send() never raises SIGPIPE here, so a client that disconnects at
 * the wrong moment cannot kill the desktop. */
static int sock_send_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        long w = (long)send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;                          /* includes EAGAIN */
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return 0;
}

static void conn_close(NotifyConn *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->rxlen = 0;
    c->discard = 0;
    c->app[0] = 0;
}

/* A reply that cannot be written is not worth keeping the connection
 * for: close it and let the client reconnect. */
static int conn_send(NotifyConn *c, const char *text) {
    if (c->fd < 0) return -1;
    char buf[NOTIFY_LINE];
    size_t n = strlen(text);
    if (n + 2 > sizeof buf) return -1;
    memcpy(buf, text, n);
    buf[n++] = '\n';
    if (sock_send_all(c->fd, buf, n) != 0) {
        conn_close(c);
        return -1;
    }
    return 0;
}

static int split_tabs(char *line, char **f, int max) {
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = 0;
        p = t + 1;
    }
    return n;
}

static int parse_int(const char *s, int fallback) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || (end && *end) || errno != 0) return fallback;
    if (v < -2147483647L - 1 || v > 2147483647L) return fallback;
    return (int)v;
}

/* ITEM <id> <urgency> <remaining_ms> <age_ms> <app> <tag> <summary> <body> */
static int format_item(char *out, size_t cap, const NotifyItem *it) {
    char app[NOTIFY_APP_MAX * 3 + 1], tag[NOTIFY_TAG_MAX * 3 + 1];
    char sum[NOTIFY_SUMMARY_MAX * 3 + 1], body[NOTIFY_BODY_MAX * 3 + 1];
    if (notify_escape(it->app, app, sizeof app) >= (int)sizeof app) return -1;
    if (notify_escape(it->tag, tag, sizeof tag) >= (int)sizeof tag) return -1;
    if (notify_escape(it->summary, sum, sizeof sum) >= (int)sizeof sum) return -1;
    if (notify_escape(it->body, body, sizeof body) >= (int)sizeof body) return -1;
    int n = snprintf(out, cap, "ITEM\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%s",
                     it->id, it->urgency, it->remaining_ms, it->age_ms,
                     app, tag, sum, body);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

static int handle_line(NotifyServer *s, NotifyConn *c, char *line) {
    char *f[8];
    int nf = split_tabs(line, f, 8);
    const char *verb = f[0];

    if (strcmp(verb, "POST") == 0) {
        if (nf < 6) {
            s->rejected++;
            conn_send(c, "ERR\tPOST needs urgency, timeout, app, tag, summary[, body]");
            return c->fd >= 0;
        }
        char app[NOTIFY_APP_MAX], tag[NOTIFY_TAG_MAX];
        char sum[NOTIFY_SUMMARY_MAX], body[NOTIFY_BODY_MAX];
        notify_unescape(f[3], app, sizeof app);
        notify_unescape(f[4], tag, sizeof tag);
        notify_unescape(f[5], sum, sizeof sum);
        notify_unescape(nf >= 7 ? f[6] : "", body, sizeof body);
        if (!app[0]) copy_trunc(app, sizeof app, c->app[0] ? c->app : "unknown");

        int urgency = parse_int(f[1], -1);
        int ttl = parse_int(f[2], -1);
        int id = notify_insert(s, app, tag, sum, body, urgency, ttl);
        if (id < 0) {
            char b[160];
            snprintf(b, sizeof b, "ERR\t%s", strerror(errno));
            conn_send(c, b);
        } else {
            char b[64];
            snprintf(b, sizeof b, "OK\t%d", id);
            conn_send(c, b);
        }
        return c->fd >= 0;
    }

    if (strcmp(verb, "LIST") == 0) {
        char b[NOTIFY_LINE];
        long long now = mono_ms();
        s->now_ms = now;
        sweep(s);
        for (int i = 0; i < s->count; i++) {
            NotifyItem it;
            fill_view(s, i, now, &it);
            if (format_item(b, sizeof b, &it) >= 0) {
                if (conn_send(c, b) != 0) return 0;
            }
        }
        snprintf(b, sizeof b, "END\t%d", s->count);
        conn_send(c, b);
        return c->fd >= 0;
    }

    if (strcmp(verb, "DISMISS") == 0) {
        int id = nf >= 2 ? parse_int(f[1], -1) : -1;
        if (notify_dismiss(s, id)) conn_send(c, "OK");
        else                       conn_send(c, "ERR\tno such id");
        return c->fd >= 0;
    }

    if (strcmp(verb, "DND") == 0) {
        int on = nf >= 2 ? parse_int(f[1], 0) : 0;
        notify_dnd_set(s, on);
        char b[32];
        snprintf(b, sizeof b, "OK\t%d", s->dnd);
        conn_send(c, b);
        return c->fd >= 0;
    }

    if (strcmp(verb, "STATS") == 0) {
        NotifyStats st;
        notify_stats(s, &st);
        char b[320];
        snprintf(b, sizeof b,
                 "STATS\tactive=%d\tclients=%d\tdnd=%d\tlistening=%d\tlogging=%d"
                 "\tposted=%lu\treplaced=%lu\texpired=%lu\tdismissed=%lu"
                 "\tevicted=%lu\tsuppressed=%lu\trejected=%lu\tlogged=%lu",
                 st.active, st.clients, st.dnd, st.listening, st.logging,
                 st.posted, st.replaced, st.expired, st.dismissed,
                 st.evicted, st.suppressed, st.rejected, st.logged);
        conn_send(c, b);
        return c->fd >= 0;
    }

    if (strcmp(verb, "HELLO") == 0) {
        if (nf >= 2) notify_unescape(f[1], c->app, sizeof c->app);
        else         c->app[0] = 0;
        fold_text(c->app);
        conn_send(c, "OK\tnotify/1");
        return c->fd >= 0;
    }

    if (strcmp(verb, "PING") == 0) {
        conn_send(c, "OK\tnotify/1");
        return c->fd >= 0;
    }

    if (strcmp(verb, "CLOSE") == 0) {
        conn_send(c, "OK");
        return 0;                               /* caller closes the slot */
    }

    conn_send(c, "ERR\tunknown command");
    return c->fd >= 0;
}

/* Split the receive buffer into lines and dispatch them. Returns 1 if
 * a command was handled. Close-on-return means the slot is free. */
static int conn_lines(NotifyServer *s, NotifyConn *c) {
    int handled = 0, start = 0;
    for (int i = 0; i < c->rxlen; i++) {
        if (c->rx[i] != '\n') continue;
        c->rx[i] = 0;
        if (i > start && c->rx[i - 1] == '\r') c->rx[i - 1] = 0;
        if (c->rx[start] != 0) {
            handled = 1;
            if (!handle_line(s, c, c->rx + start)) { conn_close(c); return handled; }
            if (c->fd < 0) return handled;
        }
        start = i + 1;
    }
    if (start > 0) {
        c->rxlen -= start;
        if (c->rxlen > 0) memmove(c->rx, c->rx + start, (size_t)c->rxlen);
    }
    return handled;
}

/* Read what is there. Every exit path either leaves the slot usable or
 * closes it: EOF (client died mid-line), an error, or a line that does
 * not fit. Bounded at 64 reads so one chatty client cannot hold the
 * frame. */
static int conn_service(NotifyServer *s, NotifyConn *c) {
    int handled = 0;
    for (int guard = 0; guard < 64 && c->fd >= 0; guard++) {
        if (c->discard) {
            char tmp[256];
            int n = (int)recv(c->fd, tmp, sizeof tmp, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) return handled;
                conn_close(c);
                return handled;
            }
            if (n == 0) { conn_close(c); return handled; }
            char *nl = memchr(tmp, '\n', (size_t)n);
            if (nl) {
                int tail = (int)(tmp + n - (nl + 1));
                if (tail <= (int)sizeof c->rx - 1) {
                    if (tail > 0) memcpy(c->rx, nl + 1, (size_t)tail);
                    c->rxlen = tail;
                    c->discard = 0;
                    /* the bytes after that newline are the next command
                     * and they are already here: handle them now, or they
                     * would sit in the buffer until the client spoke again */
                    if (tail > 0) handled |= conn_lines(s, c);
                }
            }
            continue;
        }

        int space = (int)sizeof c->rx - 1 - c->rxlen;
        if (space <= 0) {
            /* The line is longer than any command we accept. Refuse it
             * whole - parsing a mangled tail would invent a command the
             * client never sent - then swallow the rest of it. */
            conn_send(c, "ERR\tline too long");
            c->rxlen = 0;
            c->discard = 1;
            handled = 1;
            continue;
        }

        int n = (int)recv(c->fd, c->rx + c->rxlen, (size_t)space, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) return handled;
            conn_close(c);
            return handled;
        }
        if (n == 0) { conn_close(c); return handled; }
        c->rxlen += n;
        handled |= conn_lines(s, c);
        if (n < space) return handled;           /* drained */
    }
    return handled;
}

static int listen_open(NotifyServer *s) {
    if (strlen(s->sock_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    unlink(s->sock_path);                        /* a stale socket from before */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, s->sock_path, strlen(s->sock_path));
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    if (listen(fd, 16) != 0) {
        int e = errno;
        close(fd);
        unlink(s->sock_path);
        errno = e;
        return -1;
    }
    set_nonblock(fd);
    s->listen_fd = fd;
    s->sock_owned = 1;
    return 0;
}

int notify_open(NotifyServer *s, const char *sock_path, const char *log_path) {
    if (!s) { errno = EINVAL; return -1; }
    memset(s, 0, sizeof *s);
    for (int i = 0; i < NOTIFY_MAX_CLIENTS; i++) s->conns[i].fd = -1;
    s->listen_fd = -1;
    s->log_fd = -1;
    s->next_id = 1;
    s->now_ms = mono_ms();
    copy_trunc(s->sock_path, sizeof s->sock_path, sock_path ? sock_path : NOTIFY_SOCKET);
    copy_trunc(s->log_path, sizeof s->log_path, log_path ? log_path : NOTIFY_LOG);

    log_open(s);                                /* best effort, never fatal */
    listen_open(s);                             /* -1: in-process still works */
    s->opened = 1;
    return 0;
}

void notify_close(NotifyServer *s) {
    if (!s || !s->opened) return;
    for (int i = 0; i < NOTIFY_MAX_CLIENTS; i++)
        if (s->conns[i].fd >= 0) conn_close(&s->conns[i]);
    if (s->listen_fd >= 0) close(s->listen_fd);
    s->listen_fd = -1;
    if (s->sock_owned) unlink(s->sock_path);
    s->sock_owned = 0;
    if (s->log_fd >= 0) close(s->log_fd);
    s->log_fd = -1;
    s->opened = 0;
}

int notify_poll(NotifyServer *s) {
    if (!s || !s->opened) return 0;             /* never touch fd 0 by mistake */
    int changed = 0;
    s->now_ms = mono_ms();
    changed |= sweep(s) > 0;

    if (s->listen_fd >= 0) {
        for (int guard = 0; guard < 8; guard++) {
            int fd = (int)accept(s->listen_fd, NULL, NULL);
            if (fd < 0) break;
            int slot = -1;
            for (int k = 0; k < NOTIFY_MAX_CLIENTS; k++) {
                if (s->conns[k].fd < 0) { slot = k; break; }
            }
            if (slot < 0) {                      /* bounded, so this is the answer */
                static const char full[] = "ERR\ttoo many clients\n";
                send(fd, full, sizeof full - 1, MSG_NOSIGNAL);
                close(fd);
                continue;
            }
            set_nonblock(fd);
            NotifyConn *c = &s->conns[slot];
            memset(c, 0, sizeof *c);
            c->fd = fd;
            changed = 1;
        }
    }

    for (int i = 0; i < NOTIFY_MAX_CLIENTS; i++) {
        if (s->conns[i].fd >= 0) changed |= conn_service(s, &s->conns[i]) != 0;
    }
    return changed ? 1 : 0;
}

/* ============================================================
 *  The library side: one command, one reply, over the socket
 * ============================================================ */
static const char *client_path(void) {
    const char *p = getenv("MYSTIC_NOTIFY_SOCKET");
    return (p && *p) ? p : NOTIFY_SOCKET;
}

static int rpc(const char *line, char *reply, size_t cap) {
    if (!line || !reply || cap < 2) { errno = EINVAL; return -1; }
    const char *path = client_path();
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, path, strlen(path));
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    /* A service that dies between our send and its reply must not hang
     * the caller's frame. */
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    size_t len = strlen(line);
    char buf[NOTIFY_LINE];
    if (len + 2 > sizeof buf) { close(fd); errno = EMSGSIZE; return -1; }
    memcpy(buf, line, len);
    buf[len++] = '\n';
    if (sock_send_all(fd, buf, len) != 0) {
        int e = errno;
        close(fd);
        errno = e ? e : EIO;
        return -1;
    }

    int n = 0;
    while ((size_t)n + 1 < cap) {
        int r = (int)recv(fd, reply + n, cap - 1 - (size_t)n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd);
            errno = e;
            return -1;
        }
        if (r == 0) break;
        n += r;
        if (memchr(reply, '\n', (size_t)n)) break;
    }
    close(fd);
    reply[n] = 0;
    char *nl = strchr(reply, '\n');
    if (nl) *nl = 0;
    nl = strchr(reply, '\r');
    if (nl) *nl = 0;
    return n;
}

int notify_post_full(const char *app, const char *tag,
                     const char *summary, const char *body,
                     int urgency, int timeout_ms) {
    if (!summary || !*summary) { errno = EINVAL; return -1; }
    if (urgency < NOTIFY_NORMAL || urgency > NOTIFY_CRITICAL) { errno = EINVAL; return -1; }
    if (timeout_ms < 0) { errno = EINVAL; return -1; }

    char a[NOTIFY_APP_MAX], t[NOTIFY_TAG_MAX];
    char m[NOTIFY_SUMMARY_MAX], b[NOTIFY_BODY_MAX];
    char ea[NOTIFY_APP_MAX * 3 + 1], et[NOTIFY_TAG_MAX * 3 + 1];
    char em[NOTIFY_SUMMARY_MAX * 3 + 1], eb[NOTIFY_BODY_MAX * 3 + 1];
    norm_text(a, sizeof a, (app && *app) ? app : "unknown");
    norm_text(t, sizeof t, tag);
    norm_text(m, sizeof m, summary);
    norm_text(b, sizeof b, body);
    notify_escape(a, ea, sizeof ea);
    notify_escape(t, et, sizeof et);
    notify_escape(m, em, sizeof em);
    notify_escape(b, eb, sizeof eb);

    char line[NOTIFY_LINE];
    int n = snprintf(line, sizeof line, "POST\t%d\t%d\t%s\t%s\t%s\t%s",
                     urgency, timeout_ms, ea, et, em, eb);
    if (n < 0 || (size_t)n >= sizeof line) { errno = EMSGSIZE; return -1; }

    char reply[256];
    if (rpc(line, reply, sizeof reply) < 0) return -1;
    if (strncmp(reply, "ERR\t", 4) == 0) { errno = EINVAL; return -1; }
    if (strncmp(reply, "OK\t", 3) != 0) { errno = EIO; return -1; }
    int id = parse_int(reply + 3, -1);
    if (id <= 0) { errno = EIO; return -1; }
    return id;
}

int notify_post(const char *summary, const char *body, int urgency, int timeout_ms) {
    return notify_post_full(NULL, NULL, summary, body, urgency, timeout_ms);
}

/* A tiny line reader, used only by notify_fetch(). */
typedef struct {
    int  fd;
    int  len, pos;
    char buf[NOTIFY_LINE];
} LineReader;

static int lr_line(LineReader *lr, char *out, size_t cap) {
    size_t n = 0;
    for (;;) {
        if (lr->pos >= lr->len) {
            int r = (int)recv(lr->fd, lr->buf, sizeof lr->buf, 0);
            if (r < 0) { if (errno == EINTR) continue; return -1; }
            if (r == 0) return 0;
            lr->len = r;
            lr->pos = 0;
        }
        char ch = lr->buf[lr->pos++];
        if (ch == '\n') break;
        if (ch == '\r') continue;
        if (n + 1 < cap) out[n++] = ch;
    }
    out[n] = 0;
    return 1;
}

static int decode_item(char *line, NotifyItem *out) {
    char *f[9];
    if (split_tabs(line, f, 9) < 9) return 0;
    memset(out, 0, sizeof *out);
    out->id = parse_int(f[1], 0);
    out->urgency = parse_int(f[2], NOTIFY_NORMAL);
    out->remaining_ms = parse_int(f[3], -1);
    out->age_ms = parse_int(f[4], 0);
    notify_unescape(f[5], out->app, sizeof out->app);
    notify_unescape(f[6], out->tag, sizeof out->tag);
    notify_unescape(f[7], out->summary, sizeof out->summary);
    notify_unescape(f[8], out->body, sizeof out->body);
    /* remaining + age is exactly the ttl the service was given, because
     * both were computed from the same monotonic reading. */
    out->ttl_ms = out->remaining_ms > 0 ? out->remaining_ms + out->age_ms : 0;
    return out->id > 0;
}

int notify_fetch(NotifyItem *out, int max) {
    if (!out || max <= 0) { errno = EINVAL; return -1; }
    const char *path = client_path();
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, path, strlen(path));
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (sock_send_all(fd, "LIST\n", 5) != 0) {
        int e = errno;
        close(fd);
        errno = e ? e : EIO;
        return -1;
    }

    LineReader lr;
    memset(&lr, 0, sizeof lr);
    lr.fd = fd;
    char line[NOTIFY_LINE];
    int n = 0, done = 0;
    while (!done) {
        int r = lr_line(&lr, line, sizeof line);
        if (r <= 0) {
            int e = errno;
            close(fd);
            errno = r < 0 ? (e ? e : EIO) : ECONNRESET;
            return -1;
        }
        if (strncmp(line, "ITEM\t", 5) == 0) {
            if (n < max && decode_item(line, &out[n])) n++;
        } else if (strncmp(line, "END\t", 4) == 0) {
            done = 1;
        } else if (strncmp(line, "ERR\t", 4) == 0) {
            close(fd);
            errno = EIO;
            return -1;
        }
    }
    close(fd);
    return n;
}

/* ============================================================
 *  The daemon
 *
 *  The desktop normally runs this in-process (notify_open +
 *  notify_poll once a frame) because it has to draw the list anyway.
 *  notify_daemon() exists for the case where the notifications have
 *  to work without the desktop, and for the harness.
 * ============================================================ */
int notify_daemon(const char *sock_path, const char *log_path) {
    NotifyServer s;
    notify_open(&s, sock_path, log_path);
    if (s.listen_fd < 0) return -1;

    for (;;) {
        struct pollfd pfd[1 + NOTIFY_MAX_CLIENTS];
        int n = 0;
        pfd[n].fd = s.listen_fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        n++;
        for (int i = 0; i < NOTIFY_MAX_CLIENTS; i++) {
            if (s.conns[i].fd < 0) continue;
            pfd[n].fd = s.conns[i].fd;
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            n++;
        }
        int r = poll(pfd, (nfds_t)n, 200);
        if (r < 0 && errno == EINTR) continue;
        notify_poll(&s);            /* accepts, reads, and expires */
    }
}

#ifdef NOTIFY_DAEMON
int main(void) {
    return notify_daemon(NOTIFY_SOCKET, NOTIFY_LOG) == 0 ? 0 : 1;
}
#endif
