# Notifications: the drawing hook

`src/notify.c` is the service: it owns the list, the timeouts, the history and
the socket. It draws **nothing**. This file is the other half of the contract -
what the desktop must paint, where, and with which numbers - so the service can
be finished without the desktop and the desktop can be finished without the
service.

Everything below uses only `src/mystic.h` (`Surface`, `Theme`, `font_*`,
`gfx_*`) and `src/notify.h`. No new colours are invented: every colour comes
from the theme that is already being drawn with.

---

## 1. Wiring, in four lines

In `src/main.c`, near the other globals:

```c
#include "notify.h"
static NotifyServer notify;          /* ~73 KB of static, no heap */
```

After the display, fonts and theme are up:

```c
notify_open(&notify, NOTIFY_SOCKET, NOTIFY_LOG);   /* ignore the result:
                                                     in-process posting works
                                                     even if the socket is not */
```

Once per frame, before drawing:

```c
notify_poll(&notify);                /* accept, read, EXPIRE. Never blocks. */
```

After the windows are drawn:

```c
draw_toasts(&notify, screen, th);    /* §4 */
```

And on a left click, before giving the click to the desktop:

```c
if (click_toasts(&notify, input.mouse_x, input.mouse_y, screen)) continue;
```

`notify_poll()` is what makes entries disappear on their own. If it is not
called, `notify_snapshot()` still expires them (it sweeps too), so a desktop
that only draws is still correct - it just does the sweep once per frame
instead of twice.

---

## 2. Geometry

The stack is anchored to the **top right**, under the top bar
(`topbar_h() == ui_px(34)`).

| what | value |
|---|---|
| right margin | `ui_px(16)` |
| top | `ui_px(34) + ui_px(10)` |
| width | `ui_px(330)`, never more than `screen->w - ui_px(32)` |
| gap between cards | `ui_px(8)` |
| cards drawn | at most **4**; the rest are counted in one dim line |
| corner radius | `th->corner` (the theme's own, so it matches windows) |
| padding inside | `ui_px(9)` vertical, `ui_px(14)` on the left, `ui_px(12)` right |

**Heights are computed from the font metrics at draw time.** Never store a card
height: the global text scale changes the fonts, and a 64 px constant would
clip the second line at 1.25x. The contract is:

```
line_summary = (int)(font_height(fs) + 0.5f)          fs = FONT_SEMIBOLD 13
line_body    = (int)(font_height(fb) + 0.5f)          fb = FONT_REGULAR  12

height = low          -> pad*2 + line_summary
       = body shown   -> pad*2 + line_summary + line_body + ui_px(2)
       = critical     -> pad*2 + line_summary + line_body + ui_px(12)   (+ hint line)
```

Note `font_get()` already multiplies by `ui_scale_get()` internally: ask for
**13**, not `ui_px(13)`, or the text is scaled twice. Geometry (`ui_px`) is the
only thing that needs the scale by hand.

Order: `notify_snapshot()` returns **oldest first**. Draw index 0 at the top and
let new ones appear below it, so the bubble a person is reading never moves.

---

## 3. What each urgency looks like

| | stripe (3 px, left edge) | card fill | border | summary | body |
|---|---|---|---|---|---|
| `NOTIFY_NORMAL` (0) | `th->panel_border` | `WITH_A(th->panel, 0xE6)` | `th->panel_border` | `th->text` | `th->text_dim` |
| `NOTIFY_LOW` (1) | `WITH_A(th->accent, 0x40)` | `WITH_A(th->panel, 0xE6)` | `th->panel_border` | `th->text_dim` | *not shown* |
| `NOTIFY_CRITICAL` (2) | `WITH_A(th->accent, 0xFF)` | `WITH_A(th->panel, 0xE6)` | `WITH_A(th->accent, 0xC0)` | `th->text` | `th->text_dim` + a hint line |

So urgency is told by *structure* (stripe alpha, louder border, an extra line),
not by a red that would fight the theme. A critical bubble is also the only one
that never arrives pre-suppressed by do-not-disturb.

**Time left.** If `item.ttl_ms > 0`, draw a 2 px hairline along the bottom edge
of the card, from the left, `(w-2) * remaining_ms / ttl_ms` long, in
`WITH_A(th->accent, 0x99)`. This is the whole reason the snapshot carries
`remaining_ms`: a bubble that is about to vanish says so before it does. A
sticky bubble (`remaining_ms == -1`, `ttl_ms == 0`) has no hairline; it leaves
only when it is clicked.

**Entrance.** For `age_ms < 180`, slide in from the right:

```
t  = 180 - age_ms                     /* 180 -> 0 */
dx = t * t * ui_px(48) / (180 * 180)   /* ease-out, 0 at the end */
```

Only the painter applies `dx`; the hit test uses the settled rect. Nobody
clicks a bubble in its first 180 ms.

---

## 4. The code

Paste this into `src/main.c` (or a new file that includes `main.c`'s globals -
it only needs the two fonts, which it asks for itself).

```c
/* ---------- notifications ---------- */

/* One place that decides where bubble i is and how tall it is, so the
 * painter and the hit test can never disagree. Returns 0 when i is not
 * one of the four that fit on screen. */
static int toast_rect(Surface *screen, Font *fs, Font *fb,
                      const NotifyItem *list, int n, int i,
                      int *ox, int *oy, int *ow, int *oh) {
    if (i < 0 || i >= n || i >= 4) return 0;
    int pad = ui_px(9);
    int gap = ui_px(8);
    int w = ui_px(330);
    if (w > screen->w - ui_px(32)) w = screen->w - ui_px(32);
    if (w < ui_px(120)) return 0;

    int line_s = (int)(font_height(fs) + 0.5f);
    int line_b = (int)(font_height(fb) + 0.5f);

    int y = ui_px(34) + ui_px(10);
    for (int k = 0; k < i; k++) {
        int crit = list[k].urgency == NOTIFY_CRITICAL;
        int low  = list[k].urgency == NOTIFY_LOW;
        int has_body = list[k].body[0] != 0 && !low;
        int h = pad * 2 + line_s;
        if (has_body) h += line_b + ui_px(2);
        if (crit) h += ui_px(12);
        y += h + gap;
    }

    int crit = list[i].urgency == NOTIFY_CRITICAL;
    int low  = list[i].urgency == NOTIFY_LOW;
    int has_body = list[i].body[0] != 0 && !low;
    int h = pad * 2 + line_s;
    if (has_body) h += line_b + ui_px(2);
    if (crit) h += ui_px(12);

    *ox = screen->w - w - ui_px(16);
    *oy = y;
    *ow = w;
    *oh = h;
    return 1;
}

/* Draw text clipped to maxw, with three dots when it does not fit. The
 * cut never lands inside a UTF-8 sequence. */
static void draw_ellipsis(Surface *s, Font *f, float x, float y,
                          const char *text, float maxw, u32 color) {
    if (!f || !text) return;
    if (font_width(f, text) <= maxw) { font_draw(s, f, x, y, text, color); return; }
    char cut[192];
    size_t n = strlen(text);
    if (n > sizeof cut - 4) n = sizeof cut - 4;
    for (size_t k = n; k > 0; k--) {
        size_t m = k;
        while (m > 0 && ((unsigned char)text[m] & 0xC0) == 0x80) m--;
        memcpy(cut, text, m);
        memcpy(cut + m, "...", 3);
        cut[m + 3] = 0;
        if (font_width(f, cut) <= maxw) break;
    }
    font_draw(s, f, x, y, cut, color);
}

static void draw_toasts(NotifyServer *srv, Surface *screen, const Theme *th) {
    NotifyItem list[8];
    int n = notify_snapshot(srv, list, 8);
    if (n <= 0) return;

    Font *fs = font_get(FONT_SEMIBOLD, 13);
    Font *fb = font_get(FONT_REGULAR, 12);
    if (!fs || !fb) return;

    int drawn = 0;
    for (int i = 0; i < n && i < 4; i++) {
        int x, y, w, h;
        if (!toast_rect(screen, fs, fb, list, n, i, &x, &y, &w, &h)) break;
        NotifyItem *it = &list[i];
        int crit = it->urgency == NOTIFY_CRITICAL;
        int low  = it->urgency == NOTIFY_LOW;
        int has_body = it->body[0] != 0 && !low;

        /* the first 180 ms slide in from the right edge */
        int dx = 0;
        if (it->age_ms < 180) {
            int t = 180 - it->age_ms;
            dx = t * t * ui_px(48) / (180 * 180);
        }
        int cx = x + dx;
        if (cx >= screen->w) continue;

        gfx_shadow(screen, (float)cx, (float)y, (float)w, (float)h,
                   th->corner, 16.0f, 0.30f);

        /* A Surface over exactly the card: surf_set, surf_fill, gfx_* and
         * font_draw all clip to it, so no bubble can paint outside itself
         * even while it is half off the screen edge. */
        Surface card;
        card.px     = screen->px + (size_t)y * (size_t)screen->stride + (size_t)cx;
        card.w      = w;
        card.h      = h;
        card.stride = screen->stride;
        if (cx + card.w > screen->w) card.w = screen->w - cx;
        if (card.w < ui_px(24)) continue;

        gfx_rrect_aa(&card, 0.0f, 0.0f, (float)card.w, (float)h, th->corner,
                     WITH_A(th->panel, 0xE6),
                     crit ? WITH_A(th->accent, 0xC0) : th->panel_border, 1.0f);

        u32 stripe = crit ? WITH_A(th->accent, 0xFF)
                          : low  ? WITH_A(th->accent, 0x40) : th->panel_border;
        if (h > ui_px(16)) surf_fill(&card, 1, ui_px(8), ui_px(3), h - ui_px(16), stripe);

        float tx = (float)ui_px(14);
        float ty = (float)ui_px(9);
        float tw = (float)card.w - tx - (float)ui_px(12);
        draw_ellipsis(&card, fs, tx, ty, it->summary, tw,
                      low ? th->text_dim : th->text);
        if (has_body)
            draw_ellipsis(&card, fb, tx, ty + font_height(fs) + (float)ui_px(2),
                          it->body, tw, th->text_dim);
        if (crit)
            draw_ellipsis(&card, fb, tx, (float)(h - ui_px(9) - (int)font_height(fb)),
                          "click to dismiss", tw, th->text_dim);

        /* how long it has left, if it is going to leave on its own */
        if (it->ttl_ms > 0 && it->remaining_ms > 0 && h > ui_px(4)) {
            int ww = (int)((long long)(card.w - 2) * it->remaining_ms / it->ttl_ms);
            surf_fill(&card, 1, h - ui_px(3), ww, ui_px(2), WITH_A(th->accent, 0x99));
        }

        drawn++;
    }

    if (n > drawn) {
        char more[64];
        int x, y, w, h;
        if (toast_rect(screen, fs, fb, list, n, drawn - 1 < 0 ? 0 : drawn - 1,
                       &x, &y, &w, &h)) {
            snprintf(more, sizeof more, "+%d more - see /root/notifications/log",
                     n - drawn);
            Font *f = font_get(FONT_REGULAR, 11);
            if (f) font_draw(screen, f, (float)x, (float)(y + h + ui_px(8)),
                             more, th->text_dim);
        }
    }
}

/* Returns 1 when the click hit a bubble and was consumed. */
static int click_toasts(NotifyServer *srv, int mx, int my, Surface *screen) {
    NotifyItem list[8];
    int n = notify_snapshot(srv, list, 8);
    if (n <= 0) return 0;
    Font *fs = font_get(FONT_SEMIBOLD, 13);
    Font *fb = font_get(FONT_REGULAR, 12);
    if (!fs || !fb) return 0;

    for (int i = 0; i < n && i < 4; i++) {
        int x, y, w, h;
        if (!toast_rect(screen, fs, fb, list, n, i, &x, &y, &w, &h)) break;
        if (mx < x || mx >= x + w || my < y || my >= y + h) continue;
        notify_dismiss(srv, list[i].id);
        return 1;
    }
    return 0;
}
```

---

## 5. Behaviour the hook must respect

- **Never keep a copy of the list.** A `NotifyItem` is a snapshot; ids are the
  only stable handle. If an id is not in the current snapshot, it is gone.
- **Dismiss by id.** `notify_dismiss(&notify, id)` removes it and writes a
  `dismiss` line to the history. Nothing else may remove an entry.
- **Do not draw more than four.** The fifth is not invisible, it is in the
  history, and the `+N more` line says so. A wall of toasts is not a
  notification system.
- **Do not disturb draws nothing.** With DND on, `notify_insert()` already
  refuses to put normal and low notifications in the list, so the snapshot is
  short by exactly the suppressed ones. If the desktop wants a DND indicator,
  that is `notify_dnd(&notify)` returning 1 - put a dot in the top bar, not a
  bubble.
- **Redraw at least 4 Hz while any card has `ttl_ms > 0`,** or the hairline
  will freeze and the bubble will seem to vanish at random. The desktop redraws
  every frame already, so this comes for free.
- **No timer, no alarm, no thread.** Expiry is `now - posted >= ttl`, evaluated
  in `notify_poll()` and `notify_snapshot()`. There is nothing to cancel when a
  bubble is clicked.
- **A bubble is one line of summary and one line of body.** The service already
  folded every newline to a space on receipt; the hook clips, it does not wrap.
  Wrapping would change the card height after the hit test was computed.

---

## 6. The socket, for apps and scripts

`/run/mystic/notify.sock`, one line per message, TAB between fields, `%25` `%09`
`%0A` `%0D` escapes inside a field, `\n` at the end. `notify.h` has the full
protocol; these are the four things anyone actually does with `nc`:

```sh
# a message with a 4 s lifetime
printf 'POST\t0\t4000\tsh\t\tBuild done\t42 files in 3.1 s\n' |
    nc -U /run/mystic/notify.sock
# -> OK<TAB>1

# a download that updates instead of stacking: same tag every time
printf 'POST\t0\t0\tdl\tdownload-7\tDownloading 47%%\t14 of 30 MB\n' |
    nc -U /run/mystic/notify.sock
# -> OK<TAB>2     ... and again for 48%:  OK<TAB>2  (same bubble)

# what is on screen right now, with the milliseconds left
printf 'LIST\n' | nc -U /run/mystic/notify.sock
# -> ITEM  2  0  1430  200  dl  download-7  Downloading 47%  14 of 30 MB
# -> END   1

# silence, and back
printf 'DND\t1\n' | nc -U /run/mystic/notify.sock   # -> OK 1
```

Urgency is `0` normal, `1` low, `2` critical. `timeout_ms` is `0` for "until
dismissed". A tag is optional; an empty tag never replaces anything. A client
that connects, sends half a line and disappears costs the service one `read()`
that returns 0 - it has no effect on the list or on the next client.

(`nc -U` is the shortest way to say it; anything that can open an `AF_UNIX`
stream works, including a 40-line C client.)

### One thing the sandbox needs

External apps are launched inside a mount namespace (`wmsrv.c`), and that jail
currently binds **only** `wm.sock` into `<jail>/run/mystic/`. So an app inside
the sandbox cannot see `/run/mystic/notify.sock` yet: its post fails with
`ENOENT`, which `notify_post()` reports honestly rather than pretending. The
fix belongs to `wmsrv.c` and is the same three lines as the existing wm.sock
bind:

```c
snprintf(path, sizeof(path), "%s/run/mystic/notify.sock", jail);
int fd = open(path, O_CREAT | O_RDWR, 0666);
if (fd >= 0) close(fd);
mount(NOTIFY_SOCKET, path, NULL, MS_BIND, NULL);
```

Until that lands, in-desktop apps and the desktop itself can post (they are not
in the jail), and the socket is reachable from a shell on tty1. That is also
why `MYSTIC_NOTIFY_SOCKET` exists: if the launcher would rather mount it
somewhere else, one `setenv()` in `wms_launch()` is enough, no library change.

---

## 7. The history

Every accepted change is appended to `/root/notifications/log` as one line, so a
notification that expired while the screen was locked can still be read:

```
2026-11-17T16:14:46Z post id=7 urgency=0 ttl=5000 app=build tag= summary=Build finished body=42 files in 3.1 s
2026-11-17T16:14:51Z expire id=7 urgency=0 ttl=5000 app=build tag= summary=Build finished body=42 files in 3.1 s
```

Events: `post`, `replace`, `dismiss`, `expire`, `evict`, `suppress`, `dnd`.
The file rotates to `log.1` at 512 KiB, so it cannot fill the disk. A line is
written with a single `write()`, so `tail -f` never shows half a line. If the
directory is not writable the service still runs and `STATS` reports
`logging=0` - the bubbles work, only the history is missing.

---

## 8. Building the service on its own

The desktop runs it in-process, which is the normal case. If notifications must
outlive the desktop, the same file builds a daemon:

```sh
zig cc -target x86_64-linux-musl -Os -static -Isrc -DNOTIFY_DAEMON \
       src/notify.c -o build/notifyd
```

It is a 38 KB static binary and it writes nothing to stdout.

---

## 9. What was actually verified

`src/notify.c` was driven by a host harness (the same file, built for the host,
not a copy) through 70 checks: 50 posts with a 100 ms timeout all expired on
their own and the list came back empty; three posts with one tag produced one
bubble with one id and `replaced=2`; the ring capped at 128 and evicted; 20 000
post/dismiss cycles left it empty with the private heap byte count unchanged
and no id reused; a service with an unwritable history still posted and
expired; and the socket handled a command split across two writes, a client
that died mid-line, a 3000-byte line followed by a good command, `LIST`,
`STATS`, `DND`, `DISMISS`, `CLOSE`, eight concurrent clients and a ninth that
was refused.

The part this document describes - the pixels - is deliberately not in that
list: nothing here has been drawn, on purpose, so that the desktop keeps
ownership of its own look.
