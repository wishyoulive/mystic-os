# The display protocol

How a separate process puts pixels on a Mystic OS screen.

This document describes `src/ipc.h`, `src/wmsrv.c` (server), `src/client/mystic_client.c`
(SDK) and the `WIN_CLIENT` path in `src/main.c`.

## Why it exists

The desktop used to be the only program that could draw: every app was compiled
into it. That makes an operating system that can never run anyone else's code.

This protocol is the smallest thing that fixes that. An app is a normal Linux
process. It connects to a socket, receives a region of shared memory, draws into
it, and receives input. The window manager composites that memory into a window
exactly as if it had drawn it itself.

## The two channels

| Channel | What it carries | Why |
|---|---|---|
| Unix socket `/run/mystic/wm.sock` | control: hello, title, frame-ready, input | line-oriented, debuggable by eye |
| `/dev/shm/mystic-<pid>-<seq>` | pixels, `w * h` 32-bit words | a `memcpy`, no serialization |

Pixels are `0xAARRGGBB`, which is byte-identical to the framebuffer's
`0x00RRGGBB` on little-endian, so the compositor copies rows without conversion.

## Messages

Every message is ASCII, terminated by `\n`.

### App → window manager

```
HELLO <id> <width> <height> <title...>
FRAME
TITLE <new title>
CLOSE
```

`HELLO` must be first. `id` is a short name (`clock`, `paint`); `width`/`height`
are the window's content size in pixels and are clamped to 80..1600 × 60..1200.
The title is the rest of the line and may contain spaces.

`FRAME` means "the shared buffer is ready to be composited". The server
currently treats it as a whole-window update; there is no damage rectangle yet.

### Window manager → app

```
OK <shm-path> <stride>
ERR <reason>
KEY <keycode> <mods> <press>
TEXT <utf8>
MOUSE <x> <y> <buttons>
RESIZE <width> <height>
CLOSE
```

`mods` is a bitmask: `1` shift, `2` ctrl, `4` alt.
`buttons` is a bitmask: `1` left, `2` right, `4` middle.
`MOUSE` coordinates are relative to the window's content area, not the screen.
`KEY` codes are raw Linux `input-event-codes.h` values, not Unicode.

`stride` is pixels per row. It is currently always equal to `width`, but a
client must use the value it is given rather than assuming.

## A complete session

```
app:  HELLO clock 380 240 Clock
wm :  OK /dev/shm/mystic-512-1 380
app:  (mmap the file, size = stride * height * 4, draw into it)
app:  FRAME
wm :  (composites; window becomes visible)
wm :  MOUSE 120 80 0
wm :  KEY 38 0 1
app:  TITLE 09:23
app:  CLOSE
wm :  (window destroyed, shm unmapped and unlinked)
```

## Lifecycle, from the server's side

1. `wms_start()` removes any stale socket, binds `/run/mystic/wm.sock`
   (creating `/run/mystic` if needed) and listens non-blocking.
2. `wms_poll()` is called every frame. It accepts new connections into a free
   slot of `clients[WM_MAX_CLIENTS]` (8), then reads each client.
3. On `HELLO` the server creates the shm file with `ftruncate` + `mmap`, zeroes
   it, and replies `OK`.
4. `main.c` notices a client with no window, allocates a `Window` with
   `kind = WIN_CLIENT` and stores the slot index in `window->client`.
5. Compositing copies `client->px` rows into the window's content rect.
6. Closing the window sends `CLOSE`, unmaps and unlinks the shm, and frees the
   slot. A client that dies is handled the same way when `read()` returns 0.

## Writing an app

The whole of a minimal app:

```c
#include "../mystic_client.h"

int main(void) {
    MClient c;
    if (mc_open(&c, "demo", "Demo", 320, 200) < 0) return 1;   /* connect + shm */

    mc_fill(&c, 0xFF12161Fu);                 /* your pixels */
    mc_rect(&c, 40, 40, 120, 60, 0xFF7AA2F7u);
    mc_present(&c);                           /* ask to be composited */

    for (;;) {
        MEvent ev;
        if (mc_poll(&c, &ev) && ev.type == MC_KEY && ev.press && ev.key == 16) {
            mc_close(&c);                     /* 'q' */
            return 0;
        }
        /* redraw when something changes, then mc_present again */
    }
}
```

Build it with the SDK and the drawing library:

```bash
zig cc -target x86_64-linux-musl -Os -static -Isrc -Isrc/client \
    -o build/app-demo src/client/apps/demo.c \
       src/client/mystic_client.c src/gfx.c src/font.c -lm
```

Package it as a bundle the launcher can see:

```bash
./tools/mkapp.sh demo
```

`mkapp.sh` writes `/usr/share/mystic/apps/demo/{manifest,bin}`. The manifest is
one `key = value` per line:

```
name = Demo
desc = What this app is, in one line.
exec = bin
```

The launcher reads `name`, `src/wmsrv.c:wms_scan_apps` reads all three, and
`wms_launch` executes `<dir>/<exec>`.

## Things an app must not do

- **Do not write outside your buffer.** The Surface you build is exactly
  `w × h`; the compositor copies at most that. Out-of-bounds writes are your
  own memory corruption, not the server's.
- **Do not assume stride == width.** Use the value from `OK`.
- **Do not block in `mc_present`.** It is a `write()` of 6 bytes on a
  non-blocking socket.
- **Do not free the mapping before sending `CLOSE`** if you want the window to
  disappear cleanly.

## Known limitations (current, not aspirational)

- **Whole-window updates.** No damage rectangles; `FRAME` re-blits everything.
- **No clipboard.** There is no `CLIPBOARD` message yet.
- **No app-initiated resize.** The server can send `RESIZE` but never does.
- **No security boundary.** Apps run as root, the same as the desktop. A
  sandbox is a later milestone, and this document will change when it lands.
- **No z-order hints.** A new window is raised; nothing else is negotiated.
- **No partial reads protection on `TEXT` beyond 31 bytes per event**, because
  `MEvent.text` is 32 bytes.

## Debugging

The server logs to `stderr` (visible on the serial console):

```
wms: listening on /run/mystic/wm.sock
wms: connection accepted
wms: clock connected 380x240
wms: cannot exec /usr/share/mystic/apps/clock/bin
```

`cannot exec` almost always means the bundle's payload is not executable after
being packed into the image — see the `is_prog` rule in `tools/mkimage.py`.
