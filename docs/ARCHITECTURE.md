# Mystic OS — architecture

This document describes what the code in this tree actually does, not what it is
meant to do one day. Every mechanism below was read out of the named file and
function; where the source and this text disagree, the source is right.

Two rules shape the whole design and explain most of the odd-looking decisions:

* **The desktop is one process.** `mystic-desktop` owns the framebuffer, the
  input devices, the window list and every app. There is no X11, no Wayland, no
  GPU driver, no toolkit, no udev, no display server.
* **Everything visible is computed from something real.** Resolution comes from
  `FBIOGET_VSCREENINFO`, memory from `/proc/meminfo`, processes from `/proc/<pid>`,
  input from `/dev/input/event*`, files from `readdir`/`stat`, persistence from a
  real ext4 filesystem on a virtio disk.

## Layers

```
QEMU  ──►  firmware loads bzImage + initramfs          (image.sh: -kernel, -initrd)
      ──►  Linux 6.18.63-0-virt unpacks the cpio into rootfs (RAM)
      ──►  /init = stage/init, PID 1 (rdinit=/init)
             mounts proc/sys/dev/devpts
             depmod + modprobe  (drm, bochs, evdev, virtio_blk, ext4, …)
             mkfs/mount /dev/vda on /root        (ext4, first boot only)
             unbinds the framebuffer console
      ──►  exec /usr/bin/mystic-desktop = src/main.c
             fb.c     /dev/fb0            → Display
             input.c  /dev/input/event*   → Input
             theme.c  /etc/mystic/themes/*.theme → Theme
             gfx.c    pixels              → Surface
             font.c   Lato .ttf via stb_truetype → glyph bitmaps
             term.c   forkpty + /bin/sh   → cell grid
             sha256.c password hashing    → lock screen check
             apps/*.c AppVT implementations
```

Everything under the desktop is Alpine's kernel and minirootfs with our
`stage/` overlay on top; the table at the end separates the two.

---

## 1. Boot chain

### 1.1 QEMU

`image.sh` builds one command line and reuses it for every mode:

```
-kernel build/cache/vmlinuz-virt
-initrd build/initramfs.cpio.gz
-m 256 -vga std -no-reboot
-drive file=build/disk.img,format=raw,if=virtio
-append "console=ttyS0 rdinit=/init loglevel=3 quiet"
```

(plus `-accel whpx` when `qemu-system-x86_64 -accel help` reports it). The
`probe` mode appends `mystic.probe=1 loglevel=0` and uses
`-display none -serial stdio`. `-vga std` is QEMU's bochs-compatible VGA, which
the kernel's `bochs` DRM driver binds; that is what creates `/dev/dri/card0` and
`/dev/fb0`. `-drive …,if=virtio` is what the OS sees as `/dev/vda`.

There is no bootloader in this path. QEMU's firmware loads the bzImage and the
initrd with the Linux boot protocol and starts the kernel. `boot/boot.asm` and
`build.sh` are the earlier 512-byte boot sector experiment; `image.sh` does not
use them.

### 1.2 Kernel and initramfs

The command line has no `root=`, so the kernel runs from the initramfs. It
unpacks the gzipped newc archive (built by `tools/mkimage.py`) into the `rootfs`
ramfs and executes `/init` (`rdinit=/init`) as PID 1. There is no
`switch_root`/`pivot_root`: the whole userland lives in RAM for the lifetime of
the boot, and `/root` is the only path backed by a disk (§5).

### 1.3 `/init` — our PID 1 (`stage/init`)

1. `mount -t proc none /proc`, `-t sysfs none /sys`, `-t devtmpfs none /dev`,
   and `-t devpts none /dev/pts` (devpts is required by `forkpty` in `term.c`).
2. `hostname mystic`; `PATH=/sbin:/usr/sbin:/bin:/usr/bin`;
   `mkdir -p /root /run /tmp /var/log`.
3. `load_modules()` — `KV=$(uname -r)`, then `depmod -a "$KV"` to generate
   `modules.dep` from the module tree that `tools/fetch-kernel.sh` staged, then
   `modprobe` for an explicit list:

   ```
   drm drm_kms_helper ttm drm_ttm_helper drm_vram_helper drm_shmem_helper
   drm_client_lib fb_sys_fops sysimgblt sysfillrect bochs virtio-gpu evdev
   psmouse virtio_blk crc16 mbcache jbd2 ext4 fat vfat nls_cp437 nls_iso8859-1
   ```

   Alpine's `linux-virt` builds these as modules, so without this step there is no
   framebuffer and no event device.
4. `setup_disk()` — see §5.
5. Probe mode: if `/proc/cmdline` contains `mystic.probe=1`, print kernel,
   `/proc/uptime`, `/proc/meminfo`, `/proc/modules`, DRM/fb facts from
   `/sys/class/drm/*` and `/sys/class/graphics/*/name`, input device names, the
   boot counter in `/root/boots.txt` and a permissions test file, then
   `sync; poweroff -f`. This is what `tools/smoke.sh` and `image.sh probe` read.
6. Unbind the framebuffer console: for each `/sys/class/vtconsole/vtcon*`, read
   `name`; if it contains `frame buffer`, write `0` to `bind`. Otherwise fbcon
   keeps painting its block cursor over the desktop's pixels. `src/fb.c`
   `claim_console()` applies a second guard: `open("/dev/tty0")` and
   `ioctl(KDSETMODE, KD_GRAPHICS)`.
7. `exec /usr/bin/mystic-desktop`. If the binary or `/dev/fb0` is missing, print
   `/etc/mystic-banner` plus kernel/memory/display facts and `exec /bin/sh`.

### 1.4 Input devices without udev

devtmpfs creates the device nodes when `evdev` probes, so there is nothing for a
device manager to do. `input_open()` in `src/input.c` scans
`/dev/input/event0` … `event7`, reads `EVIOCGNAME`, and decides what each device
is by capability bits (`has_bit()` wraps `EVIOCGBIT`):

| test | classification |
|---|---|
| `EV_REL/REL_X` or `EV_ABS/ABS_X` | mouse (first one wins) |
| `EV_KEY/KEY_A` or `EV_KEY/KEY_SPACE` | keyboard (first one wins) |

Everything else is closed and logged as ignored. Both fds are opened
`O_RDONLY|O_NONBLOCK`; `input_poll()` drains each with `read()` until `EAGAIN`.
`handle_event()` fills the `Input` struct declared in `src/mystic.h`:

* `REL_X`/`REL_Y` accumulate into `mouse_dx`/`mouse_dy` and move `mouse_x`/`mouse_y`;
  `ABS_X`/`ABS_Y` set the position directly; `REL_WHEEL` accumulates `wheel`.
* `BTN_LEFT`/`BTN_RIGHT`/`BTN_MIDDLE` set bits 1/2/4 in `buttons` plus the
  one-frame `clicked`/`released` masks.
* Other `EV_KEY` press events set `key` (raw evdev code), `key_pressed`, `shift`
  and `alt` (the modifier state on that event), and append a printable character
  to `text[]` via the explicit `KEYMAP` table and `key_to_char()`. Ctrl+letter is
  folded to a control byte (`Ctrl+S` → `0x13`).

`input_frame()` clears the per-frame fields at the top of every loop, so a click
is consumed exactly once. The pointer starts at (640, 400) because a relative
device has no absolute position to report until it moves.

---

## 2. The rendering model

### 2.1 Framebuffer (`src/fb.c`)

`disp_open()` `open`s `/dev/fb0`, reads `FBIOGET_VSCREENINFO` (`xres`, `yres`,
`bits_per_pixel`) and `FBIOGET_FSCREENINFO` (`line_length`), then

```c
d->stride = fi.line_length / (vi.bits_per_pixel / 8);   /* pixels per row */
d->mem = mmap(NULL, fi.line_length * vi.yres, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
```

`disp_present()` copies row by row, `min(surface width, display width)` pixels per
row, from `s->px + y*s->stride` to `d->mem + y*d->stride`. Going through the
stride on both sides means a padded framebuffer does not skew the image.

Colour is `0xAARRGGBB` (`RGB()`/`WITH_A()` in `src/mystic.h`). On little-endian
the low three bytes are exactly the `0x00RRGGBB` the framebuffer wants, so a
`u32` can be copied straight out; the alpha byte is ignored by the hardware.

### 2.2 Surfaces (`src/gfx.c`)

`Surface { u32 *px; int w, h, stride; }` is the only drawing target in the
system. `surf_set` bounds-checks per pixel; `surf_fill`, `surf_blend`,
`surf_copy` and `surf_push` clamp their rectangles to the surface. There is no
per-window clip rectangle — see §4.

### 2.3 Two full-screen buffers

`main()` allocates two surfaces at display size:

```c
screen.px = calloc(w*h, 4);   scene = screen;   scene.px = calloc(w*h, 4);
```

* `scene` is the compose target: `draw_desktop(&scene, &themes[theme_index])`.
* `screen` is the present target: `surf_copy(&screen, &scene)`, then
  `draw_cursor_at(&screen, …)`, then `disp_present(&disp, &screen)`.

The cursor is stamped only on `screen`, so it never enters the composed frame and
never has to be erased. At 1280×800 the pair costs 2 × 4.096 MB.

### 2.4 Full recompose on change

There is no damage tracking. Every loop sets `dirty` for any of: a click a window
or the dock handled, mouse motion (`mouse_dx || mouse_dy`), a key that opens or
closes a window or changes the theme, a changed wall-clock second (`time(NULL)`),
terminal output (`term_pump` is called for every terminal window and
unconditionally sets `dirty`), and terminal keys/wheel scrolling. When `dirty` is
set, the entire frame is rebuilt and presented, then `dirty = 0`. The loop ends
with `poll(…, 16)` ms, which is the only pacing — there is no vsync and no partial
redraw.

`draw_desktop()` composes in this order:

1. Background: `gfx_gradient` from `bg_top` to `bg_bottom`, then three
   `gfx_glow` calls (top-left, right, bottom) using the theme's `glow`/`accent`
   and `glow_strength`.
2. Top bar, 34 px: `gfx_blur_region` over the bar (radius `18 * blur`), then a
   55 %-alpha `panel` tint, then a 1 px `panel_border` line. Text: `"Mystic OS"`,
   the focused window's title centred, and on the right `clock_text()` +
   `mem_field("MemTotal:")`/`"MemAvailable:"` from `/proc/meminfo`, formatted as
   `HH:MM      N MB used of M MB`. `clock_text()` uses `gmtime_r`, so the clock is
   UTC.
3. Windows, back to front (§3).
4. Dock: `dock_rect()` gives a centred bar `DOCK_N*40 + (DOCK_N-1)*20 + 28` wide,
   68 high, 22 px above the bottom edge; `gfx_shadow`, backdrop blur (radius
   `20 * blur`), rounded `panel`, then one 40×40 icon per `DOCK[]` entry with a
   hover tint and the icon drawn by a small vector routine
   (`icon_terminal`, `icon_folder`, `icon_gear`, `icon_bars`, `icon_pencil`).
   Under an icon whose app (or, for the terminal, whose pty) is open, a 4 px
   accent dot is drawn. If the launcher is open, `draw_launcher()` runs last.

### 2.5 Rounded rectangles: signed distance, not a mask

`gfx_rrect_aa()` is the primitive every panel, window and pill is built from. For
each pixel centre `p` it computes the signed distance to a rounded rectangle in
`rrect_dist()`:

```c
halfw = w/2; halfh = h/2;  r = min(r, min(halfw, halfh));
q  = |p - centre| - (half - r);          /* qx, qy */
d  = length(max(q,0)) + min(max(qx,qy),0) - r;
```

* `length(max(q,0))` is the distance outside the shape.
* `min(max(qx,qy),0)` is negative inside the core, zero inside the corner boxes.
* subtracting `r` turns the corner circles into the actual boundary.

Coverage is then a 1-pixel linear ramp, which is what produces antialiased edges
without any sampling tables:

```c
cov = 0.5f - d;   clamp to [0,1]
```

A border is a second distance field of the same rectangle inset by `border_w`,
with radius `radius - border_w`: `bd = clamp(0.5 - inner, 0, 1)`. The border band
is `cov - bd`; the fill is `bd`. Pixels are composited border-then-fill, each
weighted by its coverage times the colour's own alpha, then written with a forced
`0xFF` alpha.

### 2.6 Blur: three box passes on a 4×-downsampled copy

`gfx_blur_region(s, rx, ry, rw, rh, radius)` blurs whatever has already been
painted underneath the region, which is why the glass reads as glass: the
backdrop is genuinely convolved, not tinted.

1. Clip the region to the surface. `F = 4`; `sw = ceil(w/F)`, `sh = ceil(h/F)`.
2. Downsample by box-averaging each 4×4 block into a float RGB triple
   (`calloc(sw*sh*3)` twice: `small` and `tmp`).
3. `r = max(1, radius / F)`.
4. Three passes. Each pass is a horizontal box filter of radius `r` (edges
   clamped by repeating the border pixel) written to `tmp`, `memcpy` back, then
   the same vertically.
5. Upsample with nearest neighbour (`x/F`, `y/F`) and write back opaque.

Three boxes approximate a Gaussian closely enough that the difference is not
visible, and doing it at quarter resolution cuts the work by 16×. If
`radius <= 0`, or the allocation fails, the function returns without touching the
region.

`gfx_shadow()` reuses it: allocate a scratch `Surface` of the shadow bounds
(`pad = spread*3`), draw an opaque black rounded rect inset by `pad`, blur it with
`radius = spread`, then darken the destination by the blurred red channel
(`dst * (255 - a*alpha) / 255`). A shadow can therefore only darken, never paint.

### 2.7 Text: TrueType with a per-font glyph cache

`src/font.c` rasterises real Lato files through `stb_truetype` (vendored as
`src/stb_truetype.h`). `FONT_FILES` maps the three faces to
`Lato-Regular.ttf`, `Lato-Bold.ttf`, `Lato-Black.ttf` for `FONT_REGULAR`,
`FONT_MEDIUM`, `FONT_SEMIBOLD`; they are read from `/usr/share/fonts`
(`FONT_DIR` in `main.c`).

`font_get(face, size)` walks a linked list of loaded `Font`s and returns a match
within 0.01 px, otherwise `load_one()` reads the whole `.ttf` into memory,
`stbtt_InitFont`, `stbtt_ScaleForPixelHeight`, and records ascent/descent.
`font_height()` is `size * 1.4`.

Each `Font` owns its cache: `Glyph cache[2048]` keyed by **codepoint**, not by
glyph index. `glyph_get(f, cp)` starts at `slot = cp % MAX_GLYPHS` and probes
forward; the first unused slot is rasterised in place by `glyph_rasterise()`
(`stbtt_GetGlyphHMetrics` for the advance, `stbtt_GetGlyphBitmapBox` for the
offsets, `stbtt_MakeGlyphBitmap` into an 8-bit coverage buffer of `w*h` bytes,
refusing anything over 512 px). A full table makes `glyph_get` return `NULL` and
that character is skipped rather than evicting something.

`font_draw()` walks the UTF-8 string (`utf8_next`, 1–4 bytes), advances a pen by
`g->advance`, places each bitmap at `(pen + xoff, y + ascent + yoff)` and blends
`coverage/255 * colour_alpha` over the destination RGB, forcing alpha opaque.
`font_width()` is the same walk summing advances. Baseline placement is why callers
pass a box top and get text that sits on `y + ascent`.

### 2.8 The rest of the primitives

* `gfx_gradient` — one `color_mix()` per row, then a fill of that row.
* `gfx_glow` — radial: for each pixel inside `radius`,
  `t = 1 - d²/radius²`, `a = t² * strength`, blended in place.
* `gfx_line` — builds a quad from the segment and its normal, then calls
  `gfx_polygon`; there is no separate line rasteriser.
* `gfx_polygon` — scanline fill with **4 sub-scanlines per row** (vertical
  antialiasing), even-odd fill from a sorted intersection list (max 64), and
  per-pixel horizontal coverage `clamp(r - l)` weighted by `alpha/4`. The mouse
  cursor is a 7-point polygon drawn twice (black outline, white body).

Everything here is scalar C on one core, per pixel. The expensive primitive is
the blur, and it runs for the top bar, every visible window and the dock on every
composed frame — which is exactly why it is done at quarter resolution.

---

## 3. The window manager (`src/main.c`)

### 3.1 State

```c
typedef struct {
    int   used;
    int   kind;          /* WIN_TERM or WIN_APP */
    char  title[64];
    int   x, y, w, h;    /* screen rectangle, including the title bar */
    App  *app;           /* WIN_APP */
    Term *term;          /* WIN_TERM */
    int   z;             /* larger = in front */
} Window;

static Window wins[MAX_WINDOWS];   /* MAX_WINDOWS == 10 */
static int    z_top;
static int    drag_win = -1, drag_dx, drag_dy;
static int    resize_win = -1;
```

`z` is a monotonic counter, not an index: `win_focus()` does `w->z = ++z_top`.
No list is renumbered, and closing a window needs no fix-up because every
comparison is relative.

### 3.2 Z-order

* `win_top()` — linear scan for the maximum `z`; used for the top-bar title, for
  keyboard routing and for Esc.
* Painting back-to-front is a selection loop, not a sort: `last_z` starts at −1
  and for each of up to `MAX_WINDOWS` slots the compositor picks the window with
  the smallest `z` greater than `last_z`, draws it, and sets `last_z`. With
  `n ≤ 10` this is cheaper than sorting and needs no scratch array.

### 3.3 Open and close

`win_open(kind, app_id)` takes the first `used == 0` slot:

| | terminal | app |
|---|---|---|
| size | 640 × 380 | 520 × 360 |
| position | `60 + (i%3)*40`, `70 + (i%3)*40` | `140 + (i%3)*40`, `110 + (i%3)*40` |
| content | `term_create(cols, rows, "/bin/sh")` with `cols = (w-24)/7`, `rows = (h-56)/16` | `app_new(app_id, w-2, h-40-2)` |
| title | `"Terminal"` | `app_title(a)` |

The rectangle is then clamped to stay on screen (`x = disp.w - w - 20`,
`y = disp.h - h - 90` when it would overflow). A failed `term_create`/`app_new`
clears the slot and returns `NULL`, so a missing app cannot leave a half-open
window behind.

`win_close()` closes the pty fd, frees the `Term`, calls `app_free()` (which runs
`vt->free`), and `memset`s the slot. Terminals are also closed automatically when
`term_pump()` reports the shell has gone (`term_alive()` false), which is how
typing `exit` in a terminal closes its window. Closing through the red dot does
not `waitpid` the shell: the pty master is closed (the shell gets SIGHUP) and the
`Term` is freed, so only an exit that `term_pump()` observed is reaped.

### 3.4 Hit testing

`handle_click()` runs on the press edge (`input.clicked & 1`) and tests in this
order:

1. **Launcher, if it is open.** A press inside a tile opens that app and closes
   the launcher; a press anywhere else just closes the launcher. Nothing else is
   considered while it is open.
2. **Window body.** The topmost window whose rectangle contains the pointer wins
   (max `z`). It is focused first, so clicking anywhere raises.
3. **Close dot.** Local `ly < 40` and `14 <= lx < 24` — the red circle — calls
   `win_close()`.
4. **Resize grip.** `lx >= w-18 && ly >= h-18` sets `resize_win` to the slot index.
5. **Title bar.** `ly < 40` sets `drag_win` and records the grab offset
   (`drag_dx`, `drag_dy`) in window-local coordinates.
6. **Body click.** `app_click(a, lx-1, ly-tb-1, buttons)` — the same content
   origin (`x+1, y+tb+1`) the compositor draws at, in window-local coordinates.
7. **Dock.** If no window was hit, a pointer in the dock band over an icon calls
   `win_open()` with `WIN_APP` and that entry's app id, or `WIN_TERM` when the id
   is `NULL`.

There is no separate "click-through" or "accepts input" flag; a window is a
rectangle and the topmost one at the pointer always gets the event.

### 3.5 Drag and resize

Both are per-frame state machines while button 1 is held, not event streams:

* **Drag** — `nx = mouse_x - drag_dx`, `ny = mouse_y - drag_dy`, clamped so
  `x >= 0`, `y >= 34` (the top bar stays visible), `x+w <= disp.w`,
  `y+h <= disp.h`. The window is repositioned only when the result differs, so an
  immobile drag does not dirty the frame.
* **Resize** — `nw = mouse_x - w->x + 1`, `nh = mouse_y - w->y + 1`, floor
  280 × 160, then `vt->resize(a, w-2, h-42)` if the app implements it. Terminal
  windows are not resized: the pty's `winsize` is set once in `term_create`, and
  `term_draw` simply draws at most `t->cols` × `t->rows` cells inside whatever
  rectangle it is given.

Releasing button 1 clears both (`drag_win = resize_win = -1`). A release also
snaps the dragged window when it ended near a screen edge: top edge within 6 px of
the 34 px bar → full width (`x=0, y=bar, h=disp.h-bar-90`); `x <= 8` → left half;
`x+w >= disp.w-8` → right half. The snap goes through `vt->resize()` like a manual
resize, so an app that can reflow does.

### 3.6 Keyboard

Only the top window receives keys. Global keys are handled before the focused
window, every loop (and Ctrl+Alt+L before them all):

| input | action |
|---|---|
| Ctrl+Alt+L (evdev 38) | lock the screen (§3.9) |
| `input.text` contains `1`…`9` | `theme_index = digit-1` (theme order is filename order, `theme_load_all` sorts) |
| evdev 125 (Super) | `launcher_open = !launcher_open` |
| evdev 1 (Esc) with the launcher open | close the launcher (and nothing else) |
| Alt+Tab | focus the window with the next-highest `z`, i.e. toggle the top two |
| evdev 59…63 (F1…F5) | open terminal, files, settings, monitor, notes |
| evdev 1 (Esc) | `win_close(win_top())` |

Terminal windows get `key_bytes()` (arrows/Home/End/PgUp/PgDn/Delete → xterm escape
sequences), then the printable `text`; Shift+PgUp/PgDn scroll the history by 8
lines and any typing resets it to live. App windows get the printable `text`,
evdev 14 → `"\b"`, evdev 28 → `"\n"`. Every non-top window is deaf, and there is
no focus-follows-mouse.

The theme can also be changed from disk: once per wall-clock second `main()`
re-reads `/etc/mystic/current-theme` and matches the first line against
`Theme::name`, which is how the Settings app is meant to ask for a theme. Note
that `settings.c` writes the theme *file name* (`1-obsidian.theme`) while this
comparison uses the `name =` field inside the file (`Obsidian`), so today the
file path does not take effect and the number keys are the working switcher.

### 3.7 Terminals (`src/term.c`)

A terminal window is a `Term`: a grid of `Cell` values
(`struct { u32 ch; u8 fg, bg, bold; }`), a 16-entry `PALETTE`, and a scrollback
ring of `HIST_ROWS` (600) rows.

`term_create(cols, rows, "/bin/sh")` clamps the geometry (minimum 10×4, maximum
400×200), sets `winsize`, and calls `forkpty()`; the child sets
`TERM=xterm-256color`, `PATH`, `HOME=/root`, `PS1` and `execl`s the shell with
`-i`. The master fd is put in non-blocking mode, so the desktop never blocks on a
shell.

`term_pump()` reads up to 2048 bytes per call and feeds them one at a time to
`feed_byte()`, a small state machine: state 0 is text (CR, LF, BS, TAB and BEL are
handled there), state 1 is `ESC`, state 2 accumulates CSI parameters until a final
byte reaches `csi_dispatch()` — cursor addressing (`H`/`f`), relative moves
(`A`–`D`), erase (`J`, `K`), and SGR (`m`: bold, the 30–37/40–47/90–97/100–107
palettes, and 256-colour `38;5`/`48;5` folded down to the 16-colour palette) — and
state 3 swallows OSC strings until BEL or `ESC`. `h`/`l` mode set/reset are
ignored.

Printing past the last column wraps and calls `newline()`; scrolling pushes the
top row into the history ring before `memmove`ing the grid up. `term_draw()` fills
the rectangle with the default background, draws history rows above the live grid
according to the scroll offset, renders each cell with `font_draw()` at a 7×16 px
advance, and paints the cursor as a 2 px underline. When the shell exits,
`term_pump()` sets `alive = 0` and reaps it with `waitpid(…, WNOHANG)`, and the
next loop closes the window.

### 3.8 Launcher (`src/main.c`)

Super (`KEY_LEFTMETA`, evdev 125) toggles a full-screen application grid.
`LAUNCHER[]` is a list of `{ app_id, label, icon }` — a superset of `DOCK[]` that
also exposes `sysinfo` — and `launcher_geom(i, …)` lays the tiles out in three
columns of 128 × 118, 18 px apart, centred on the display.

`draw_launcher()` darkens the whole frame with `surf_blend(0,0,w,h,0xAA000000)`,
then paints a rounded panel with a shadow and a backdrop blur of radius `22*blur`,
one tile per entry with hover tint, icon and label, and a `"Super or Esc closes"`
hint underneath. A click on a tile opens the app and closes the launcher; a click
elsewhere closes it; Esc closes it before it can reach the focused window.

### 3.9 Lock screen (`src/main.c`, `src/sha256.c`)

Ctrl+Alt+L sets `locked` and clears the typed password. While `locked`, the main
loop runs a separate branch before any global key handling: printable bytes are
appended to `pin`, Backspace removes one, Enter calls `try_unlock()`, and every
iteration redraws and sleeps 16 ms — no window, dock, launcher or app receives
input, and the loop `continue`s instead of falling through.

`draw_lock_screen()` blurs the whole frame (`radius 40 * blur`), dims it with
`0xB0000000`, and paints a 380×210 panel containing the UTC clock, the caption
"Mystic OS is locked", a password field showing one 8 px dot per character (up to
24), an optional message line and "Enter to unlock".

`try_unlock()` reads the first line of `/etc/mystic/passwd` and verifies it with
`mystic_check_password()` from `src/sha256.c`. The file format is
`salt_hex:rounds:hash_hex` (e.g. `6ae89c69…:20000:0ba99560…`): the salt is
hex-decoded, `mystic_hash_password()` hashes `salt||password` with the in-house
SHA-256, then re-hashes the previous 32-byte digest together with the salt
`rounds - 1` more times, and the result is compared against the stored digest with
a full-length constant-time XOR loop. A mismatch shows `"wrong password"`; a
match clears `locked`.

The password file ships in the initramfs (`stage/etc/mystic/passwd`), so this
locks the *session*, not the machine: anyone who can read the image can also read
the salt and the digest. `mystic_make_password_line()` exists to produce such a
line but has no in-tree caller yet.

---

## 4. The app contract (`src/mystic.h`, `src/apps.c`)

### 4.1 The table

```c
typedef struct {
    const char *id;      /* matched against the dock / F-key id */
    const char *title;   /* window title */
    void (*init)(App *a, int w, int h);
    void (*draw)(App *a, Surface *s, int x, int y, int w, int h, const Theme *th);
    void (*key)(App *a, const char *bytes, int n);
    void (*click)(App *a, int x, int y, int buttons);
    void (*resize)(App *a, int w, int h);
    void (*free)(App *a);
} AppVT;

struct App { const AppVT *vt; void *state; };
```

An app file exports exactly one symbol, e.g. `const AppVT app_vt_files`
(`.id = "files"`), and keeps all of its data in `a->state` (allocated in `init`,
released in `free`).

### 4.2 Who calls what

`src/apps.c` holds `REGISTRY[]`, built at compile time:

```c
#ifdef WITH_FILES
    &app_vt_files,
#endif
    …
    &app_vt_sysinfo,     /* always built */
```

`tools/build-desktop.sh` adds `-DWITH_FILES`, `-DWITH_SETTINGS`, `-DWITH_MONITOR`,
`-DWITH_NOTES` for whichever `src/apps/<name>.c` files exist, and links them into
the one binary.

| call | caller | when |
|---|---|---|
| `vt->init(a, w, h)` | `app_new()` | window opened; `w,h` are the content size |
| `vt->draw(a, s, x, y, w, h, th)` | `app_draw()` ← `draw_desktop()` | every composed frame, content rectangle `(x+1, y+41, w-2, h-43)` |
| `vt->key(a, bytes, n)` | `app_key()` ← `main()` | top window only, printable bytes or `"\b"`/`"\n"` |
| `vt->click(a, x, y, buttons)` | `app_click()` ← `handle_click()` | left press inside the body, window-local coordinates |
| `vt->resize(a, w, h)` | `main()` drag handler | window resized by the corner grip; called with `w-2, h-42` (one pixel taller than the draw rectangle, as is the `init` size from `win_open`) |
| `vt->free(a)` | `app_free()` ← `win_close()` | window closed |

`app_draw`, `app_key`, `app_click` and `app_free` are all null-safe: a missing
handler is a no-op, `app_title` falls back to `"App"`, and an unknown id makes
`app_new` return `NULL` (so the dock can list an app that was not compiled in and
nothing breaks).

Registered today: `files`, `settings`, `monitor`, `notes`, `sysinfo`. The dock
shows terminal, files, settings, monitor, notes; `sysinfo` is the reference app
that shows the smallest useful implementation of the table.

### 4.3 Why an app cannot reach into the desktop

The contract is an API boundary, not a memory boundary:

* An app is handed only a `Surface *`, a rectangle, a `const Theme *`, a small
  event vocabulary (bytes, window-local coordinates, button mask) instead of the
  raw `Input` struct, and its own `App *`. It never sees the `Display`, the
  `Window` array, `wins[]`, `input`, `z_top`, or `main()`'s buffers. None of the
  desktop's state is exported as a global an app could assign to.
* All app state is behind `a->state`, so two windows of the same app are two
  independent `App` objects with independent `init`/`free` lifecycles.
* The frame is recomposed from the theme and the window list every time `dirty`
  is set, so pixels an app draws in the wrong place are overwritten at the next
  compose rather than accumulating.

What it is **not**: process isolation, and not per-window clipping. Apps are
functions linked into `mystic-desktop` and run in its address space; a stray
pointer or an out-of-bounds write in an app can corrupt the compositor or kill the
whole desktop. `Surface` clipping is only to the surface bounds — the compositor
does not hand out a sub-surface or a clip rect — so an app that ignores its
rectangle will paint over neighbouring windows until the next recompose. Both
facts matter when reading the security claims in `README.md`.

### 4.4 Apps read real data

No app invents numbers. `sysinfo` reads `/proc/meminfo`, `/proc/uptime`,
`/proc/loadavg` and `/proc/<pid>/comm`; `monitor` walks `opendir("/proc")` and
reads `/proc/<pid>/stat` (comm, utime/stime), `/proc/<pid>/statm` (RSS),
`/proc/meminfo`, `/proc/loadavg` and `/proc/uptime`; `files` uses
`opendir`/`readdir`/`stat` on a path it walks with Enter/Backspace; `notes` reads
and writes `/root/notes.txt`; `settings` scans `/etc/mystic/themes` and writes
`/etc/mystic/current-theme`.

---

## 5. The disk

### 5.1 The device

`image.sh` creates the disk once, and only if it is absent:

```bash
dd if=/dev/zero of=build/disk.img bs=1048576 count=64
```

64 MB of zeros, attached with `if=virtio`, which the kernel names `/dev/vda`.
Deleting `build/disk.img` therefore means "give me a new, unformatted disk"; the
OS formats it on the next boot.

### 5.2 Format on first boot, mount afterwards

`setup_disk()` in `stage/init`:

```sh
[ -b /dev/vda ] || { echo "disk: no /dev/vda"; return 0; }
mkdir -p /root
if mount -t ext4 /dev/vda /root 2>/dev/null; then
    echo "disk: mounted /dev/vda (ext4) on /root"
    return 0
fi
echo "disk: /dev/vda is unformatted, creating ext4 filesystem"
if [ -x /sbin/mkfs.ext4 ]; then
    /sbin/mkfs.ext4 -q -F /dev/vda >/dev/null 2>&1
else
    echo "disk: no mkfs.ext4 in the image"
fi
if mount -t ext4 /dev/vda /root 2>/dev/null; then
    chmod 755 /root
    echo "disk: formatted ext4 and mounted on /root"
fi
```

`mkfs.ext4` is not part of the Alpine minirootfs, so it is staged explicitly:
`stage/sbin/mkfs.ext4` plus the shared libraries it needs
(`stage/usr/lib/libext2fs.so.2`, `libe2p.so.2`, `libcom_err.so.2`, `libss.so.2`,
`libuuid.so.1`, `libblkid.so.1`, `libeconf.so.0`) and `stage/etc/mke2fs.conf`.
`mkimage.py` marks anything under `sbin/` executable, so the staged binary lands
with the right mode. Every failure path is non-fatal and prints to the serial
console: a boot without a mounted `/root` still reaches the desktop, it just has
nowhere to persist.

Because `/init` runs from the initramfs, everything outside `/root` is RAM:
`/etc/mystic/themes/*.theme` and `/etc/mystic/passwd` are unpacked from the cpio
at boot, and writes to `/etc` (the theme request file) disappear on reboot.

### 5.3 fsync is the difference between saving and losing

`save()` in `src/apps/notes.c`:

```c
FILE *f = fopen("/root/notes.txt", "w");
fwrite(n->buf, 1, n->len, f);
fflush(f);
fsync(fileno(f));     /* without this the bytes sit in the page cache */
fclose(f);
```

`fwrite` only reaches the kernel, and `fflush` only reaches the kernel's page
cache; with `-no-reboot` and a VM that is killed rather than shut down, unsynced
bytes are simply gone. `fsync` is what forces ext4 to hand them to the virtio
device. The same reasoning is why probe mode runs `sync` before `poweroff -f`.

Persistence is observable: probe mode increments `/root/boots.txt`, creates
`/root/permtest` with mode 600 and ownership 1000:1000 and prints what
`ls -l` says, and Notes keeps `/root/notes.txt` across boots.

---

## 6. The build

`image.sh` is the pipeline; the three tools under `tools/` do the work. Everything
runs on the host (Windows or Linux) — nothing is built inside the guest.

### 6.1 `tools/fetch-kernel.sh` — kernel + exactly the modules

* Downloads `linux-virt-6.18.63-r0.apk` from Alpine v3.24 `main/x86_64` into
  `build/cache`, extracts it there, and copies the package's `boot/vmlinuz-virt`
  out as `build/cache/vmlinuz-virt`.
* Takes **whole module subtrees**, not a hand-picked file list:
  `kernel/drivers/{gpu,video,i2c,input,block}` and
  `kernel/fs/{fat,ext4,jbd2,nls}`, decompressing every `*.ko.gz` into
  `stage/lib/modules/$(KV)/…`, plus `kernel/fs/mbcache.ko`.
* Kernel and modules come from the same package on purpose: their versions match,
  so `modprobe` cannot fail with "invalid module format". The script prints the
  module count and total size.

### 6.2 `tools/build-desktop.sh` — one static binary

Finds `zig` (on `PATH`, or the WinGet install glob) and compiles the desktop with
zig as a cross-compiler:

```
zig cc -target x86_64-linux-musl -Os -static -fno-sanitize=all -Isrc \
       -Wall -Wextra -Wno-unused-parameter \
       -o build/mystic-desktop <sources> -lm -lutil
```

Sources: the nine core files — `main.c`, `apps.c`, `term.c`, `sha256.c`, `gfx.c`,
`font.c`, `fb.c`, `input.c`, `theme.c` — plus `apps/sysinfo.c`, and every optional
app that exists, each with its `-DWITH_*` define. `-static` plus musl means the
binary needs nothing but the kernel; `-lutil` is for `forkpty`. The result is
copied to `stage/usr/bin/mystic-desktop` (and `chmod +x`), and the Lato `.ttf`
files are copied to `stage/usr/share/fonts`.

### 6.3 `tools/mkimage.py` — the cpio writer, and why it exists

A Linux rootfs is full of symlinks (`/bin/sh` → `busybox`,
`/usr/lib/libc.so` → `libc.musl-x86_64.so.1`, …). Windows cannot create them
without a privilege the build does not have, and `cpio` is not installed there
either. So the rootfs is **never extracted to the host filesystem**. The script
reads the Alpine tarball with Python's `tarfile` and writes a `newc` archive
itself (`CpioWriter`), which stores a symlink as a target string in a file entry
— data, not a filesystem object.

Pipeline inside `mkimage.py`:

1. Every member of the base tarballs becomes an entry
   `(mode, mtime, data | target)`; directories `S_IFDIR|0755`, symlinks and hard
   links `S_IFLNK|0777` with the target text, regular files `S_IFREG` with their
   mode bits and contents. Later tarballs and later entries overwrite earlier
   ones, and `stage/` is applied last.
2. `stage/` is walked and overlaid on top. Modes are decided by path because
   NTFS has no executable bit:
   `init`, `bin/…`, `sbin/…`, `usr/bin/…`, `usr/sbin/…`, `usr/local/bin/…` are
   `0755`, everything else `0644`.
3. Missing parent directories are synthesised, so the overlay can add
   `stage/sbin/mkfs.ext4` without listing `stage/sbin`.
4. Entries are emitted sorted by path depth, so a directory always precedes its
   children. Directory `nlink` is computed as `2 + <direct subdirectories>`.
5. Headers are the newc format written by hand: magic `070701`, thirteen 8-digit
   hex fields (inode, mode, uid, gid, nlink, mtime, size, devmajor, devminor,
   rdevmajor, rdevminor, namesize, check), the NUL-terminated name, padding to a
   4-byte boundary, the data and its padding. A `TRAILER!!!` entry and 512 zero
   bytes close the archive.
6. The whole thing is gzipped at level 9 to `build/initramfs.cpio.gz`, and the
   script prints file/symlink/dir counts and both sizes.

This is why the build works identically on Windows and Linux, and why the image
can be assembled without root.

### 6.4 `image.sh` — the pipeline

1. `tools/fetch-kernel.sh` if `build/cache/vmlinuz-virt` (or the extracted
   package) is missing.
2. Download `alpine-minirootfs-3.24.4-x86_64.tar.gz` into `build/cache` if absent.
3. `tools/build-desktop.sh` (output suppressed).
4. `python tools/mkimage.py --base <minirootfs> --stage stage -o build/initramfs.cpio.gz`.
5. `dd` a 64 MB `build/disk.img` if absent.
6. Boot with the fixed QEMU command line from §1.1, in one of four modes:
   `run` (a window), `probe` (serial facts, then poweroff),
   `shot N` (headless, QEMU monitor `screendump` after N seconds →
   `tools/ppm2png.py` → PNG), `themes` (one boot, `sendkey 2`, `sendkey 3`,
   three screendumps).

`build.sh` and `run.sh` are the stage-1 leftovers: they assemble `boot/boot.asm`
with nasm, verify it is exactly 512 bytes and ends in `0xAA55`, write it to a
1.44 MB floppy image and boot that with no kernel. `image.sh` does not use them.

---

## Ours vs borrowed

| piece | origin | where |
|---|---|---|
| Compositor, layout, dock, launcher, cursor, event loop | ours | `src/main.c` |
| CPU renderer: SDF rounded rects, gradients, glow, blur, shadow, polygons | ours | `src/gfx.c` |
| Framebuffer access (`/dev/fb0`, stride, mmap, present) | ours | `src/fb.c` |
| evdev discovery, keymap, per-frame input state | ours | `src/input.c` |
| Glyph cache, text blending, UTF-8, face/size cache | ours | `src/font.c` |
| TrueType rasteriser | **stb_truetype.h** (public domain, Sean Barrett) | `src/stb_truetype.h` |
| Theme file format, parser, three shipped themes | ours | `src/theme.c`, `stage/etc/mystic/themes/*.theme` |
| Terminal emulator: pty, VT parser, cell grid, palette, scrollback | ours | `src/term.c` |
| `AppVT` contract and app registry | ours | `src/mystic.h`, `src/apps.c` |
| SHA-256, iterated salted password hash, password-file check | ours | `src/sha256.c` |
| Five apps (files, settings, monitor, notes, sysinfo) | ours | `src/apps/*.c` |
| PID 1: mounts, module loading, disk setup, fbcon unbind, desktop exec | ours | `stage/init` |
| initramfs builder (newc writer) | ours | `tools/mkimage.py` |
| kernel/module fetcher, desktop build, boot harness | ours | `tools/fetch-kernel.sh`, `tools/build-desktop.sh`, `image.sh` |
| Screenshot converter (QEMU PPM → PNG) | ours | `tools/ppm2png.py` |
| 512-byte boot sector (stage-1 history, unused by `image.sh`) | ours | `boot/boot.asm`, `build.sh`, `run.sh` |
| Kernel 6.18.63-0-virt | Alpine `linux-virt` (GPL-2.0) | `build/cache/vmlinuz-virt` |
| DRM (`bochs`, `virtio-gpu`), evdev, psmouse, virtio_blk, ext4/jbd2, vfat modules | Alpine `linux-virt` | `stage/lib/modules/6.18.63-0-virt/` |
| Userland: `sh`/busybox (`mount`, `depmod`, `modprobe`, `awk`, `ls`, `poweroff`, …), musl libc | Alpine minirootfs 3.24.4 | inside `build/cache/minirootfs.tar.gz`, packed into the cpio |
| `mkfs.ext4` and its shared libraries | Alpine e2fsprogs | `stage/sbin/mkfs.ext4`, `stage/usr/lib/*`, `stage/etc/mke2fs.conf` |
| Lato Regular/Bold/Black | SIL Open Font License | `assets/fonts/*.ttf` → `stage/usr/share/fonts` |
| QEMU (build/test tool, not part of the OS) | QEMU project (GPL-2.0) | host |

---

## Known rough edges

These are behaviours of the current tree, not intentions:

* The theme request file is written as a filename (`settings.c`) but matched as a
  display name (`main.c`), so only the number keys change the theme.
* The README's disk row still says FAT; `setup_disk()` formats and mounts ext4,
  and the `fat`/`vfat` modules are loaded but never used.
* Opening a terminal forces `dirty` on every loop iteration, so the desktop
  recomposes continuously (with the blur) while any terminal window exists.
* There is no per-window clip rectangle, so an app that draws outside its
  rectangle overpaints its neighbours until the next compose (§4.3).
* Resizing a terminal window does not update the pty `winsize`; the grid simply
  draws fewer cells than the window could hold.
* A digit typed into an app also switches the theme, because the global digit
  handler runs before the focused window sees the keystroke.
* Closing a terminal with the red dot closes the pty but does not `waitpid` the
  shell; the desktop is PID 1, so only exits seen by `term_pump()` are reaped.
* Clicking a dock icon always opens a new window even when that app already has
  one; the dot under the icon only reports that it is open.
