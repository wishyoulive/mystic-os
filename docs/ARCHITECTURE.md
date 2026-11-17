# Mystic OS — architecture

This document describes what the code in this tree actually does, not what it is
meant to do one day. Every mechanism below was read out of the named file and
function; where the source and this text disagree, the source is right.

Two rules shape the whole design and explain most of the odd-looking decisions:

* **The desktop is one process.** `mystic-desktop` owns the framebuffer, the
  input devices, the window list and every in-desktop app. There is no X11, no
  Wayland, no GPU driver, no toolkit, no udev, no display server. Apps that need
  their own address space are separate processes that speak a small protocol to
  the desktop (§4.5, `docs/IPC.md`).
* **Everything visible is computed from something real.** Resolution comes from
  `FBIOGET_VSCREENINFO`, memory from `/proc/meminfo`, processes from `/proc/<pid>`,
  input from `/dev/input/event*`, files from `readdir`/`stat`, persistence from a
  real ext4 filesystem on a virtio disk.

## Layers

```
QEMU  ──►  firmware loads bzImage + initramfs          (image.sh: -kernel, -initrd)
      ──►  Linux 6.18.63-0-virt unpacks the cpio into rootfs (RAM)
      ──►  /init = stage/init, PID 1 (rdinit=/init)
             mounts proc/sys/dev/devpts/tmpfs
             depmod + modprobe  (drm, bochs, evdev, virtio_blk, virtio_net, ext4, …)
             udhcpc on eth0 when the machine has one
             mkfs/mount /dev/vda on /root        (ext4, first boot only)
             unbinds the framebuffer console
      ──►  exec /usr/bin/mystic-desktop = src/main.c
             fb.c     /dev/fb0            → Display
             input.c  /dev/input/event*   → Input
             theme.c  /etc/mystic/themes/*.theme → Theme
             ui.c     /etc/mystic/ui.conf → the global text scale
             gfx.c    pixels              → Surface
             font.c   Lato + JetBrains Mono via stb_truetype → glyph bitmaps
             term.c   forkpty + /bin/sh   → cell grid
             sha256.c password hashing    → lock screen check
             wmsrv.c  /run/mystic/wm.sock → external app processes
             apps/*.c AppVT implementations linked into the binary
```

Everything under the desktop is Alpine's kernel and minirootfs with our
`stage/` overlay on top; the table at the end separates the two.

---

## 1. Boot chain

### 1.1 QEMU

`image.sh` builds one argument array and reuses it for every mode:

```
-kernel build/cache/vmlinuz-virt
-initrd build/initramfs.cpio.gz
-m 256 -vga std -no-reboot
-drive file=build/disk.img,format=raw,if=virtio
-netdev user,id=n0 -device virtio-net-pci,netdev=n0
-append "console=ttyS0 rdinit=/init loglevel=3 quiet"
```

(plus `-accel whpx` when `qemu-system-x86_64 -accel help` reports it). The
`probe` mode appends `mystic.probe=1 loglevel=0` and uses
`-display none -serial stdio`. `-vga std` is QEMU's bochs-compatible VGA, which
the kernel's `bochs` DRM driver binds; that is what creates `/dev/dri/card0` and
`/dev/fb0`. `-drive …,if=virtio` is what the OS sees as `/dev/vda`. The
`-netdev user` / `-device virtio-net-pci` pair gives the guest an `eth0` behind
QEMU's user-mode stack, which is what `setup_net()` in `stage/init` brings up.
The memory figure is `-m "${MYSTIC_RAM:-256}"`: 256 MB by default, overridable
from the environment for experiments that need more.

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
   `mkdir -p /root /run/mystic /tmp /var/log /dev/shm`.
3. `mount -t tmpfs none /dev/shm` — the shared pixel buffers of the display
   protocol live here (§4.5), and `wmsrv.c` creates them with `O_EXCL` in that
   directory.
4. `load_modules()` — `KV=$(uname -r)`, then `depmod -a "$KV"` to generate
   `modules.dep` from the module tree that `tools/fetch-kernel.sh` staged, then
   `modprobe` for an explicit list:

   ```
   drm drm_kms_helper ttm drm_ttm_helper drm_vram_helper drm_shmem_helper
   drm_client_lib fb_sys_fops sysimgblt sysfillrect bochs virtio-gpu evdev
   psmouse virtio_blk virtio_net net_failover failover crc16 mbcache jbd2
   ext4 fat vfat nls_cp437 nls_iso8859-1
   ```

   Alpine's `linux-virt` builds these as modules, so without this step there is no
   framebuffer and no event device.
5. `setup_net()` — returns silently unless `/sys/class/net/eth0` exists, so a
   machine without a NIC still boots. With one: `ip link set eth0 up`, then
   `udhcpc -i eth0 -q -n -t 4 -T 2 -s /etc/udhcpc.script`, then it prints either
   the leased IPv4 address or `net: eth0 present but no lease`.
6. `setup_disk()` — see §5.
7. Probe mode: if `/proc/cmdline` contains `mystic.probe=1`, print kernel,
   `/proc/uptime`, `/proc/meminfo`, `/proc/modules`, DRM/fb facts from
   `/sys/class/drm/*` and `/sys/class/graphics/*/name`, input device names, the
   boot counter in `/root/boots.txt` and a permissions test file, then
   `sync; poweroff -f`. This is what `tools/smoke.sh` and `image.sh probe` read.
8. Unbind the framebuffer console: for each `/sys/class/vtconsole/vtcon*`, read
   `name`; if it contains `frame buffer`, write `0` to `bind`. Otherwise fbcon
   keeps painting its block cursor over the desktop's pixels. `src/fb.c`
   `claim_console()` applies a second guard: `open("/dev/tty0")` and
   `ioctl(KDSETMODE, KD_GRAPHICS)`.
9. `exec /usr/bin/mystic-desktop`. If the binary or `/dev/fb0` is missing, print
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
  one-frame `clicked`/`released` masks. A left press within 350 ms and 6 px of
  the previous one also sets `clicked2`, which is the only thing double-click
  means in this tree.
* Other `EV_KEY` events set `key` (raw evdev code), `key_pressed`, and the
  modifier state carried on that event (`shift`, `alt`, `ctrl`), append the code
  to `keys[]` (`nkeys`), and append a printable character to `text[]` via the
  explicit `KEYMAP` table and `key_to_char()`. Ctrl+letter is folded to a
  control byte (`Ctrl+S` → `0x13`). Releases append to `keys_up[]` (`nkeys_up`)
  so a window can see the press *and* the release, which the external app
  protocol carries as the `press` field.
* evdev value 2 is kernel autorepeat and is accepted only for keys that
  `key_repeats()` allows: character keys plus the editing and navigation keys.
  Esc, Super, F1…F5 and the other one-shot shortcuts do not repeat, because
  `main()` acts on every entry in `keys[]` — a held Esc would close every window
  and a held F1 would open thirty terminals.
* The per-frame queues are fixed size (`Input::keys[16]`, `text[32]`); when one
  fills, `push_key()`/`push_char()` drop the **oldest** entry and keep the newest,
  because the frame consumes the queue once and the newest event is the one the
  user is reacting to.

`input_frame()` clears the per-frame fields (`clicked`, `released`, `clicked2`,
`wheel`, `key`, `nkeys`, `nkeys_up`, `mouse_dx`, `text_len`) at the top of every
loop, so a click is consumed exactly once. The pointer starts at (640, 400)
because a relative device has no absolute position to report until it moves.

---

## 2. The rendering model

### 2.1 Framebuffer (`src/fb.c`)

`disp_open()` `open`s `/dev/fb0`, reads `FBIOGET_VSCREENINFO` (`xres`, `yres`,
`bits_per_pixel`) and `FBIOGET_FSCREENINFO` (`line_length`), then

```c
d->stride = fi.line_length / (vi.bits_per_pixel / 8);
size_t bytes = (size_t)fi.line_length * (size_t)vi.yres;
d->mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
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
per-window clip rectangle for in-desktop apps — see §4.3. The one blit that *is*
clipped is the external client's buffer into its window (§4.5).

### 2.3 Two full-screen buffers

`main()` allocates two surfaces at display size:

```c
screen.px = calloc((size_t)screen.w * screen.h, 4);
scene = screen;
scene.px = calloc((size_t)screen.w * screen.h, 4);
```

* `scene` is the compose target: `draw_desktop(&scene, &themes[theme_index])`.
* `screen` is the present target: `surf_copy(&screen, &scene)`, then
  `draw_cursor_at(&screen, …)`, then `disp_present(&disp, &screen)`.

The cursor is stamped only on `screen`, so it never enters the composed frame and
never has to be erased. At 1280×800 the pair costs 2 × 4.096 MB.

### 2.4 Full recompose on change

There is no damage tracking. Every loop sets `dirty` for any of: a click that a
window, the taskbar, the eye or the launcher handled, mouse motion
(`mouse_dx || mouse_dy`), a key that opens or closes a window, moves the launcher
selection or changes the theme, a changed wall-clock second, terminal output
(`term_pump()` returning a positive byte count), a resize or a snap, a click on a
window title that maximises it, a client that sent `FRAME` (`want_frame`), an
animated wallpaper (every ≥ 0.08 s — §2.10), and — while it lasts — the eye blink.
When `dirty` is set, the entire frame is rebuilt and presented, then `dirty = 0`.
The loop ends with `poll(…, 16)` ms, which is the only pacing — there is no vsync
and no partial redraw.

`draw_desktop()` composes in this order:

1. Background: `wallpaper_draw(s, th, wall_index, now_sec() - wall_t0)` (§2.10),
   which repaints the whole surface from the theme.
2. Top bar, `topbar_h()` = `ui_px(34)`: `gfx_blur_region` over the bar (radius
   `18 * blur`), then `surf_blend(…, color_alpha(th->panel, 0.55f))` — the panel
   colour at 55 % of *its own* alpha, which works out at ≈30 % opacity with
   Obsidian and Aurora (`panel = #8C…`: 0x8C × 0.55 → 0x4D) and ≈42 % with
   Porcelain (`#C0…`: 0xC0 × 0.55 → 0x6A) — then a 1 px `panel_border` line.
   Left to right: the start button (`draw_start_button()` → `draw_eye()`, §3.10),
   the word `"Mystic"` in `FONT_MEDIUM` 13 at `x = 46`, the focused window's
   title centred in `FONT_REGULAR` 11, and on the right `clock_text()` +
   `mem_field("MemTotal:")`/`"MemAvailable:"` from `/proc/meminfo`, formatted as
   `HH:MM      N MB used of M MB`. `clock_text()` uses `gmtime_r`, so the clock is
   UTC. Every baseline is `(bar - font_height(face)) / 2`, not a constant, so the
   text stays centred when the scale changes the bar height; the focused title is
   truncated with `text_fit()` to the band between the brand label and the clock
   block (ellipsis at a UTF-8 boundary), because `font_draw()` has no clip.
3. Windows, back to front (§3.2). Minimised windows (`hidden`) are skipped
   entirely. Each one is composed in this order:
   `gfx_shadow(x, y, w, h, corner, 24, 0.5f)` → a backdrop blur over the window
   rect (radius `14 * blur`) → the rounded body in `window_bg` with a 1 px
   `window_border` → a title strip `title_h()` = `ui_px(40)` tall, tinted with
   the panel colour at 30 % of its own alpha (`color_alpha(th->panel, 0.30f)`,
   ≈16 % with Obsidian/Aurora) and a 1 px divider → three `DOT_SZ` (10 px)
   circles at `+14/+32/+50` on the vertical centre `traffic_y(title_h())`
   (red closes, yellow minimises, green is decoration) → the title in
   `FONT_MEDIUM` 13 at `x+72`, vertically centred on the bar and truncated with
   `text_fit()` to `w-72-12` → three grip strokes in the bottom-right corner →
   the content rectangle `(x+1, y+title_h()+1, w-2, h-title_h()-2)` handed to
   `term_draw()`, `app_draw()` or the client blit.
4. Taskbar: `dock_rect()` returns 0 (and nothing is drawn) when no window is
   open; otherwise a centred bar `n*dock_isz() + (n-1)*dock_gap() + 2*dock_pad()`
   wide and `dock_isz() + 2*dock_pad()` high, 22 px above the bottom edge, with
   `gfx_shadow` (spread 18), a backdrop blur (radius `20 * blur`), a rounded
   `panel`, and one icon per **open window** (not per app — §3.11). The focused
   window's tile is tinted with the accent and gets a 14×3 accent pill under it;
   a hovered tile shows the window title in a tooltip above the bar.
5. `draw_launcher()` if `launcher_open`, then `draw_lock_screen()` if `locked`.

Every **direct** backdrop blur in that list is guarded by `th->blur > 0`: with
`blur = 0` the top bar, windows, taskbar and launcher lose their blur (the lock
screen's fixed radius 40 is skipped too). All six shipped themes use
`blur = 1.0`, so the radii above are the ones you actually get. The shadows are a
separate path: `gfx_shadow()` blurs its own scratch surface inside
`gfx_blur_region(&tmp, 0, 0, rw, rh, (int)spread)` (`src/gfx.c`) with a **fixed**
radius equal to its `spread` argument — 24 for windows, 18 for the taskbar, 30 for
the launcher, 26 for the lock panel — and it runs whether or not the theme has
blur enabled.

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
(`pad = spread*3`), paint a **white** rounded rect at `(pad, pad)` inset by `pad`,
blur it with `radius = spread`, then use the blurred **red channel** as the shadow
mask and multiply the destination by `(255 - a*alpha)/255` per channel. White, not
black: the blurred alpha of a black rect is zero, so a black mask drew nothing at
all. A shadow can therefore only darken, never paint.

### 2.7 Text: TrueType with a per-font glyph cache

`src/font.c` rasterises real typefaces through `stb_truetype` (vendored as
`src/stb_truetype.h`). `FONT_FILES` maps the four faces to
`Lato-Regular.ttf`, `Lato-Bold.ttf`, `Lato-Black.ttf` for `FONT_REGULAR`,
`FONT_MEDIUM`, `FONT_SEMIBOLD` and to `JetBrainsMono-Regular.ttf` for
`FONT_MONO`; they are read from `/usr/share/fonts` (`FONT_DIR` in `main.c`).

`font_get(face, size)` first multiplies the requested size by
`ui_scale_get()` (§2.9), quantises it to half a pixel, clamps it to 5…96, then
walks a linked list of loaded `Font`s and returns a match within 0.01 px;
otherwise `load_one()` reads the whole `.ttf` into memory, `stbtt_InitFont`,
`stbtt_ScaleForPixelHeight`, and records ascent/descent. `font_height()` is
`size * 1.4`.

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
  cursor is a 7-point polygon drawn twice: a black copy shifted by (+1.2, +1.2)
  px, which reads as a drop shadow, then the white body on top.

Everything here is scalar C on one core, per pixel. The expensive primitive is
the blur: with the shipped themes (`blur = 1.0`) every composed frame blurs the
top bar, each visible window and the taskbar, plus the launcher or lock screen
when shown; a theme that sets `blur = 0` skips all of those. The shadowed
elements — windows (24), the taskbar (18), the launcher (30) and the lock panel
(26), but not the top bar — each blur a scratch surface as well, at their fixed
radii. That cost is why the blur runs at quarter resolution.

### 2.9 The text scale: one number, every metric derives from it (`src/ui.c`)

`static float ui_scale` starts at 1.0 and is set by
`ui_scale_load("/etc/mystic/ui.conf")`, which `main()` calls **before**
`font_init()` — the scale has to be known before the first glyph is
rasterised, or the first window would be laid out at the wrong size. The parser
looks for the first line beginning with `scale`, takes the `atof()` of the text
after `=`, rejects a non-finite value (`!isfinite(v)`: `NaN` passes both
comparisons below and would make every metric 1 px and every font 5 px), clamps
it to 0.8 … 1.6, and stops. A missing file, or a file with no `scale` line,
leaves 1.0. The shipped file is `scale = 1.0`.

The same function is called again once per wall-clock second (§3.6), which is how
a change to the file — from Settings or from a shell — takes effect without a
restart: the next `font_get()` loads new sizes and the next compose lays every
band out with the new `ui_px()` value.

Two accessors use it:

* `font_get(face, size)` multiplies the nominal size by it (§2.7), so *every*
  string in the system — window titles, list rows, terminal cells, the lock
  clock — grows from one place, and no caller can forget.
* `ui_px(base)` returns `(int)(base * scale + 0.5f)`, floored at 1 so a 1 px
  border stays a 1 px border at 0.8.

`src/main.c` names the four bands it needs instead of using numbers:
`title_h()` = `ui_px(40)`, `topbar_h()` = `ui_px(34)`, `dock_isz()` = `ui_px(40)`,
plus `dock_gap()` = `ui_px(16)` and `dock_pad()` = `ui_px(12)` for the taskbar.
Window open sizes (`ui_px(640) × ui_px(380)` for a terminal,
`ui_px(520) × ui_px(360)` for an app), the launcher grid (`ui_px(128)` tiles,
`ui_px(18)` gaps, rows fitted between `ui_px(56)` and `ui_px(118)`, icons capped
at `ui_px(52)`) and the lock panel (`ui_px(380) × ui_px(210)`) all go through
`ui_px()` too. The in-desktop apps do
the same in their own `layout_build()`-style code: `src/apps/files.c` derives its
toolbar, breadcrumb, row and footer heights from the fonts it draws with, so the
whole app grows with the scale rather than clipping.

Not everything scales, and the code is explicit about it: the traffic-light dots
(10 px at `+14/+32/+50`), the eye (§3.10, a 30×28 hit box), the 18 px resize
corner, the 22 px gap between the taskbar and the screen edge, and the fixed
offsets inside the lock panel stay put; only the bands that hold text follow
`ui_px()`. See *Known rough edges* for the places where that is visible.

### 2.10 Wallpapers (`src/wallpaper.c`, `src/wallpaper.h`)

`draw_desktop()` starts every frame with
`wallpaper_draw(s, th, wall_index, now_sec() - wall_t0)`. A wallpaper is a pure
function of the `Theme`, an index and (for the animated ones) a time in seconds;
nothing in `wallpaper.c` touches the filesystem or `/proc`. That is what lets a
static wallpaper cost exactly one paint and an animated one cost a paint per
frame, with no state to keep.

| index | name | animated | what it paints |
|---|---|---|---|
| 0 | Dusk | no | `bg_top` → `bg_bottom` gradient plus the three soft glows the desktop used to draw itself |
| 1 | Mesh | no | four corner colours (each a mix of the theme background with one of its light sources) bilinearly blended, plus a soft oval vignette |
| 2 | Waves | yes | layered sine bands drifting sideways |
| 3 | Orbits | yes | rings and discs orbiting the centre |

Every colour is derived from `th` — `bg_top`, `bg_bottom`, `glow`, `accent`,
`text`, `panel` and `glow_strength` — so any wallpaper works with any theme
instead of assuming one palette. Dusk is deliberately the original background:
same three lights, same positions, same strengths.

The index comes from `/etc/mystic/wallpaper`: `main()` reads the first line at
startup and again on the once-per-second tick (§3.6), `wallpaper_from_name()`
matches it case-insensitively (ignoring spaces and a stray `\r`), and an unknown
name leaves the current wallpaper alone. `wall_index`, `wall_t0` and `wall_last`
are statics in `main.c`; `wallpaper_animated()` decides whether the loop forces a
repaint, and it does so at most every 0.08 s (about 12 fps) so a still wallpaper
never keeps the CPU awake. `wallpaper_draw()` clamps an out-of-range index to 0
and fills black if the theme is missing, so the screen can never be left
unpainted.

The shipped `stage/etc/mystic/wallpaper` says `Orbits`, so the desktop animates
out of the box.

---

## 3. The window manager (`src/main.c`)

### 3.1 State

Abridged from `src/main.c` (the lock-screen statics are omitted):

```c
/* ------------------------------------------------------------ windows */
enum { WIN_TERM = 0, WIN_APP, WIN_CLIENT };

typedef struct {
    int   used;
    int   kind;
    char  title[64];
    int   x, y, w, h;
    App  *app;
    Term *term;
    int   client;        /* index into clients[] when kind == WIN_CLIENT */
    int   hidden;        /* minimised: still alive, not drawn, not hit-tested */
    int   maxed;         /* maximised by double-click */
    int   restore_x, restore_y, restore_w, restore_h;
    int   z;
} Window;

static Window wins[MAX_WINDOWS];
static int    z_top;
static int    drag_win = -1, drag_dx, drag_dy;
static int    resize_win = -1;

static WmClient clients[WM_MAX_CLIENTS];
static WmApp   bundles[WM_MAX_APPS];
```

`MAX_WINDOWS` is 10, `WM_MAX_CLIENTS` 8 and `WM_MAX_APPS` 32; the last two come
from `src/ipc.h`. The launcher (`launcher_open`, `launcher_sel`), the starting
cursor position for the pointer (`mouse_prev`) and the wallpaper state
(`wall_index`, `wall_t0`, `wall_last`) are further file statics.

### 3.2 Z-order

`z` is a monotonic counter, not an index: `win_focus()` does `w->z = ++z_top`.
No list is renumbered, and closing a window needs no fix-up because every
comparison is relative.

* `win_top()` — linear scan for the maximum `z` **among non-minimised windows**;
  used for the top-bar title, for keyboard routing and for Esc.
* Painting back-to-front is a selection loop, not a sort: `last_z` starts at −1
  and for each of up to `MAX_WINDOWS` slots the compositor picks the window with
  the smallest `z` greater than `last_z`, draws it, and sets `last_z`. Hidden
  windows are skipped in the same loop. With `n ≤ 10` this is cheaper than
  sorting and needs no scratch array.

### 3.3 Open and close

`win_open(kind, app_id)` takes the first `used == 0` slot:

| | terminal | in-desktop app |
|---|---|---|
| size | `ui_px(640) × ui_px(380)` | `ui_px(520) × ui_px(360)` |
| position | `60 + (i%3)*40`, `70 + (i%3)*40` | `140 + (i%3)*40`, `110 + (i%3)*40` |
| content | `term_create(cols, rows, "/bin/sh")` with `cols = (w-24)/font_width(fm,"M")`, `rows = (h-56)/font_height(fm)`, `fm = font_get(FONT_MONO, 12)` | `app_new(app_id, w-2, h-40-2)` |
| title | `"Terminal"` | `app_title(a)` |

The rectangle is then clamped to stay on screen (`x = disp.w - w - 20`,
`y = disp.h - h - 90` when it would overflow). A failed `term_create`/`app_new`
clears the slot and returns `NULL`, so a missing app cannot leave a half-open
window behind.

External clients do not go through `win_open()`: the frame loop binds a free
`Window` to a `WmClient` as soon as the client has a buffer, sized
`cl->w + 2 × (cl->h + title_h() + 2)` (§4.5), so the client's pixels get exactly
the content rectangle the compositor will hand them at any text scale.

`win_close()` calls `term_destroy()` (which closes the pty, hangs the shell up,
reaps it and frees the grid plus the 600-line scrollback), `app_free()` (which
runs `vt->free`), and for a client `wms_notify_close()` then
`wms_shutdown_client()` — then `memset`s the slot. Terminals are also closed
automatically when `term_pump()` reports the shell has gone (`term_alive()` false),
which is how typing `exit` in a terminal closes its window. The frame loop also
runs `waitpid(-1, NULL, WNOHANG)` until it returns 0, which reaps launched apps
and any shell that exited while nobody was looking.

### 3.4 Hit testing

`handle_click()` runs on the press edge (`input.clicked & 1`) and tests in this
order:

1. **Taskbar.** `taskbar_click()` runs first, because the taskbar is drawn above
the windows: a press in the bar over a tile restores and focuses that window if
it is minimised, minimises it if it is already the focused (top) window, and
otherwise just focuses it. Without this order a window dragged over the bar
swallowed its clicks.
2. **Start button.** The eye's hit box (`EYE_HIT_X/Y/W/H`, fixed at 4,3,30,28) is
   checked next: a press there starts the blink and toggles the launcher,
   whatever is underneath.
3. **Launcher, if it is open.** A press inside a tile opens that app (or launches
   that bundle) and closes the launcher; a press anywhere else just closes the
   launcher. Nothing else is considered while it is open.
4. **Window body.** The topmost non-minimised window whose rectangle contains the
   pointer wins (max `z`). It is focused first, so clicking anywhere raises.
5. **Close dot.** Local `13 <= ly < 27` and `14 <= lx < 24` — the red circle —
   calls `win_close()`. The hit box is the dot itself, not the whole title strip.
6. **Minimise dot.** The same rows, `32 <= lx < 42` — the yellow circle — sets
   `hidden = 1`.
7. **Resize grip.** `lx >= w-18 && ly >= h-18` sets `resize_win` to the slot index.
8. **Double-click on the title bar.** `ly < title_h()` and `input.clicked2`
   toggles maximise. The press that carries `clicked2` records `restore_*` and
   sets the window to `(0, topbar_h(), disp.w, disp.h - topbar_h() - reserved)`,
   where `reserved` is the taskbar band plus `ui_px(12)` when the bar is showing
   and `ui_px(16)` when it is not; the next double click restores the saved
   rectangle. An in-desktop app gets `vt->resize()` here, so an app that can
   reflow does; a client window is only moved and resized (no `RESIZE` is sent
   from this path, so its buffer keeps its size and the blit clips).
9. **Title bar.** `ly < title_h()` sets `drag_win` and records the grab offset
   (`drag_dx`, `drag_dy`) in window-local coordinates.
10. **Body click.** `app_click(a, lx-1, ly-tb-1, buttons)` — the same content
    origin (`x+1, y+tb+1`) the compositor draws at, in window-local coordinates —
    or, for a client window, `wms_notify_mouse()`.

There is no separate "click-through" or "accepts input" flag; a window is a
rectangle and the topmost one at the pointer always gets the event, except that a
minimised window is not part of the world until the taskbar brings it back.

### 3.5 Drag, resize, snap, maximise

Drag and resize are per-frame state machines while button 1 is held, not event
streams:

* **Drag** — `nx = mouse_x - drag_dx`, `ny = mouse_y - drag_dy`, clamped so
  `x >= 0`, `y >= 34` (the top bar stays visible), `x+w <= disp.w`,
  `y+h <= disp.h`. The window is repositioned only when the result differs, so an
  immobile drag does not dirty the frame.
* **Resize** — `nw = mouse_x - w->x + 1`, `nh = mouse_y - w->y + 1`, floor
  280 × 160, then `vt->resize(a, w-2, h-42)` for an in-desktop app or
  `wms_notify_resize()` for a client. `term.c` does have `term_resize()` /
  `term_resize_px()`, which rebuild the grid and scrollback at a new size and
  `ioctl(TIOCSWINSZ)` the pty, but the window manager does not call them yet, so
  a terminal keeps the column count it was opened with.

Releasing button 1 clears both (`drag_win = resize_win = -1`). A release also
snaps the dragged window when it ended near a screen edge: top edge within 6 px of
the `topbar_h()` bar → full width (`x=0, y=bar, w=disp.w, h=disp.h-bar-90`);
`x <= 8` → left half; `x+w >= disp.w-8` → right half. The snap calls
`vt->resize()` for an in-desktop app, so an app that can reflow does; a client
window is only moved and given a new rectangle here (only the corner grip sends
`RESIZE`), so its buffer keeps the size it asked for and the blit clips.

### 3.6 Keyboard and the clock tick

Only the top window receives keys. Every key in `input.keys[]` is handled in
order (handling only the last one collapsed fast bursts), and the global keys are
handled before the focused window:

| input | action |
|---|---|
| Ctrl+Alt+L (evdev 38) | lock the screen (§3.9) |
| Ctrl + `1`…`9` | `theme_index = digit-1` (theme order is filename order, `theme_load_all` sorts); the byte is zeroed so the app never sees it |
| evdev 125 (Super) | `launcher_open = !launcher_open` |
| arrows with the launcher open | move `launcher_sel` by ±1 / ±3 with wrap-around at the ends |
| Enter (evdev 28) with the launcher open | open/launch the selected tile |
| evdev 1 (Esc) with the launcher open | close the launcher (and nothing else) |
| Alt+Tab (evdev 15) | focus the window with the highest `z` other than the current top, skipping minimised ones |
| evdev 59…63 (F1…F5) | open terminal, files, settings, monitor, notes |
| evdev 1 (Esc) | `win_close(win_top())` |

While the launcher is open it is modal: after Super/Esc/arrows/Enter are handled,
any other key is swallowed (`dirty = 1; continue;`) instead of falling through to
the window behind, and printable text is delivered to nobody, because the
delivery step starts from `launcher_open ? NULL : win_top()`.

Terminal windows get `key_bytes()` (arrows/Home/End/PgUp/PgDn/Delete → xterm escape
sequences), then the printable `text`; Shift+PgUp/PgDn scroll the history by 8
lines and any typing resets it to live. In-desktop app windows get the same
`key_bytes()` escapes only (their printable text arrives through the once-per-frame
`text[]` delivery); client windows get `wms_notify_key()` on press **and** on
release, with `mods` = shift|ctrl|alt. Every non-top window is deaf, and there is
no focus-follows-mouse.

The wheel is not routed to windows: `main()` finds the topmost **terminal**
window under the pointer and calls `term_scroll(t, wheel * 3)`, so three lines per
notch. Apps and clients never see `input.wheel`.

Once per wall-clock second the loop re-reads four things, which is what lets the
in-desktop apps change the desktop without a private API:

* `/etc/mystic/ui.conf` — the text scale (§2.9); a changed value marks the frame
  dirty.
* `/etc/mystic/wallpaper` — the wallpaper name (§2.10); a different valid name
  switches the background and marks the frame dirty.
* `/etc/mystic/current-theme` — the first line is matched against `Theme::name`
  (the `name =` field inside the file, not its file name) and switches the theme.
  `src/apps/settings.c` writes exactly that name, so choosing a theme there works;
  the write itself is lost on reboot because `/etc` is the initramfs (§5.2).
* `/run/mystic/launch.request` — the Apps window cannot fork, so it writes a
  bundle id there and the desktop launches it, then `remove()`s the file (§4.6).

### 3.7 Terminals (`src/term.c`)

A terminal window is a `Term`: a grid of `Cell` values
(`struct { u32 ch; u8 fg, bg, bold; }`), a 16-entry `PALETTE`, and a scrollback
ring of `HIST_ROWS` (600) rows.

`term_create(cols, rows, "/bin/sh")` clamps the geometry (minimum 10×4, maximum
400×200), sets `winsize`, and calls `forkpty()`; the child sets
`TERM=xterm-256color`, `PATH`, `HOME=/root`, `PS1` and `execl`s the shell with
`-i`. The master fd is put in non-blocking mode, so the desktop never blocks on a
shell, and a `pidfd_open()` handle is kept when the kernel has one.

The cell grid is sized from the font, not from a constant: the caller passes
`cw = font_width(fm, "M")` and `ch = font_height(fm)` for
`fm = font_get(FONT_MONO, 12)`, and both are floored at 4 and 8 px. That is why
the terminal uses JetBrains Mono: with a proportional face the columns drift and
`ls` output stops lining up, and at a larger text scale the drift only grows.

`term_key()` does not assume the pty accepts a whole chunk: `write()` on a
non-blocking master can return a short count or `EAGAIN`, so it loops from the
bytes the kernel accepted, retries `EINTR`, and on `EAGAIN` waits for `POLLOUT`
for a bounded number of polls before dropping the rest. A stopped reader costs the
frame about 100 ms instead of losing the user's keystrokes or hanging the desktop.

`term_resize(t, cols, rows)` reallocates the grid and the scrollback ring at the
new width (keeping the surviving top-left cells, blanking the rest, rebuilding the
history in age order) and sends `TIOCSWINSZ`, which is what makes the shell reflow
with `SIGWINCH`; `term_resize_px()` is the same thing from the pixel rectangle the
terminal is drawn into, using the cell metrics above.
`term_pump()` reads 2048-byte chunks up to a 64 KiB budget per call and feeds
them one at a time to `feed_byte()`, a small state machine: state 0 is text (CR,
LF, BS, TAB and BEL are handled there), state 1 is `ESC`, state 2 accumulates CSI
parameters until a final byte reaches `csi_dispatch()` — cursor addressing
(`H`/`f`), relative moves (`A`–`D`), erase (`J`, `K`), and SGR (`m`: bold, the
30–37/40–47/90–97/100–107 palettes, and 256-colour `38;5`/`48;5` folded down to
the 16-colour palette) — states 3/4 swallow OSC strings until BEL or `ESC`, and
state 5 eats the one byte after `ESC (`/`ESC )`. `h`/`l` mode set/reset are
ignored. The parameter buffer is `TERM_PARAM_BUF` (256) bytes and
`csi_param_byte()` keeps the **newest** parameters when it fills, cutting at a
`;` so a number is never sliced in half; a single parameter longer than the whole
buffer abandons the sequence.

Printing past the last column wraps and calls `newline()`; scrolling pushes the
top row into the history ring before `memmove`ing the grid up. `term_draw()` fills
the rectangle with the default background, draws history rows above the live grid
according to the scroll offset, renders each cell with `font_draw()` at the cell
metrics it was handed, and paints the cursor as a 2 px underline. When the shell
exits, `term_pump()` sets `alive = 0` and reaps it with `waitpid(…, WNOHANG)`, and
the next loop closes the window.

### 3.8 Launcher (`src/main.c`)

Super (`KEY_LEFTMETA`, evdev 125) or a press on the eye toggles the launcher: a
three-column tile panel centred over a full-screen dim. `LAUNCHER[]` is a list of
`{ app_id, label, icon }` — Terminal, Files, Settings, Monitor, Notes, Apps
(`store`), Calculator (`calc`), System (`sysinfo`) — and `launcher_items()` adds
one more tile per installed bundle, in the id order `wms_scan_apps()` sorted them
into.

`launcher_geom(i, …)` is the one place the grid is computed, and both the painter
and the hit test call it. Tiles are `ui_px(128)` wide and `ui_px(18)` apart in
three columns, but the row height is fitted rather than fixed: rows are
`(disp.h - ui_px(96) - (rows-1)*gap) / rows`, clamped to `ui_px(56)` …
`ui_px(118)`, and the grid origin is pushed down to at least `ui_px(44)`. That is
what keeps 14 tiles at text scale 1.6 on the screen; with a fixed 118 px tile the
first row's icons went above the top edge. Inside a tile the icon shrinks to
`grid_h - label_h - 12` (capped at `ui_px(52)`) so the label always fits under it.

`draw_launcher()` first darkens the frame with
`surf_blend(s, 0, 0, s->w, s->h, 0xAA000000u)`, then paints a rounded panel
(`color_alpha(th->window_bg, 0.97f)`) with a shadow and, when `blur > 0`, a
backdrop blur of radius `22*blur`. In-desktop tiles draw their icon; a bundle tile
draws its manifest name's first letter in `FONT_SEMIBOLD` 26 because a bundle has
no icon routine compiled into the desktop. The keyboard selection (`launcher_sel`)
is drawn as an accent border, and the hint `"Super or Esc closes"` sits under the
panel. A click on a tile opens the app (or `wms_launch()`es the bundle) and closes
the launcher; a click elsewhere closes it; Esc closes it before it can reach the
focused window.

### 3.9 Lock screen (`src/main.c`, `src/sha256.c`)

Ctrl+Alt+L sets `locked` and clears the typed password (and drops the `l` byte of
the shortcut itself, which arrives in the same frame as a control character).
While `locked`, the main loop runs a separate branch before any global key
handling: bytes with `c >= 32` from `text[]` are appended to `pin`, evdev 14
removes the last character, evdev 28 calls `try_unlock()`, and the Enter that
unlocked is dropped from `text[]`/`keys[]` so it cannot fall through and execute
an empty command in a terminal. No window, taskbar, launcher or app receives
input, and the loop `continue`s instead of falling through.

The lock branch redraws only when something changed — a new minute on the clock or
input — rather than every 16 ms; the full-screen blur is the most expensive single
frame in the system. It still lets the world behind it make progress: every
iteration calls `wms_poll()`, reaps children with `waitpid(-1, …, WNOHANG)` and
`term_pump()`s each terminal, so an external app does not block on its socket and
shells do not pile up as zombies while the screen is locked.

`draw_lock_screen()` blurs the whole frame at a fixed radius of 40 when
`th->blur > 0`. Unlike the four direct backdrop blurs (top bar 18, window 14,
taskbar 20, launcher 22 — all multiplied by the theme's `blur`), this radius never
scales; the shadow blurs are fixed too, each using its own `spread`. It then dims
the frame with `0xB0000000` and paints a `ui_px(380) × ui_px(210)` panel
containing the UTC clock, the caption "Mystic OS is locked", a password field
showing one 8 px dot per character (up to 24), an optional message line and
"Enter to unlock".

`try_unlock()` reads the first line of `/etc/mystic/passwd` and verifies it with
`mystic_check_password()` from `src/sha256.c`. The file format is
`salt_hex:rounds:hash_hex` (e.g. `6ae89c69…:20000:0ba99560…`): the salt is
hex-decoded, `mystic_hash_password()` hashes `salt||password` with the in-house
SHA-256, then re-hashes the previous 32-byte digest together with the salt
`rounds - 1` more times, and the result is compared against the stored digest with
a full-length constant-time XOR loop. A mismatch shows `"wrong password"`; a
match clears `locked`.

With no password file at all the lock fails *open* rather than closed: any input
unlocks and the panel says `"no password file - any input unlocks"`. Deleting the
file used to make the session unrecoverable without a reboot.

The password file ships in the initramfs (`stage/etc/mystic/passwd`), so this
locks the *session*, not the machine: anyone who can read the image can also read
the salt and the digest. `mystic_make_password_line()` exists to produce such a
line but has no in-tree caller yet.

### 3.10 The start button (`draw_start_button`, `draw_eye`)

The Mystic mark in the top-left corner is geometry, not a glyph: two circular arcs
whose intersection is a lens (vesica), a filled iris, a smaller pupil, a
specular highlight and a lid outline drawn only where the lens coverage is
partial. The parameters are `EYE_CX 19`, `EYE_CY 17`, `EYE_RX 12`, `EYE_RY 7.6`.

Pressing it sets `blink_t0 = now_sec()` (`CLOCK_MONOTONIC`) and toggles the
launcher. `draw_start_button()` maps the elapsed time onto an eyelid opening:
closed over 90 ms, open again over the next 170 ms, then `blink_t0 = -1`. While
`blink_t0 >= 0` the frame loop forces `dirty`, so the animation is driven by the
normal recompose path; when it finishes, the loop goes idle again. The eye is not
scaled by `ui_px()` — it is the same 30×28 target at every text scale.

### 3.11 The taskbar (formerly a fixed dock)

The bar at the bottom is a list of **open windows**, not a launcher: there is one
tile per `used` window in `wins[]`, in slot order, and no tile at all when nothing
is open (`dock_rect()` returns 0 and the bar is not drawn). `taskbar_at(n)` maps a
tile index to its window, and the same function is used by the painter and by the
hit test, so a click always lands on the tile that was drawn.

Geometry: width `n*dock_isz() + (n-1)*dock_gap() + 2*dock_pad()`, height
`dock_isz() + 2*dock_pad()`, centred horizontally, `22` px above the bottom edge,
corner radius `0.32 * height`.

Magnification: when the pointer is within 110 px above (or 20 px below) the bar,
each tile is drawn at `dock_isz() * scale` with
`scale = 1 + 0.45*exp(-(d/62)²/2)`, where `d` is the horizontal distance from
the pointer to that tile's centre, and the grown tile is anchored towards the
top of the bar. The Gaussian, not a linear ramp, is what makes the neighbouring
tiles rise a little too.

Each tile draws the icon for its window's kind — `icon_for_window()` maps a
terminal to `icon_terminal()`, an in-desktop app to its `vt->id`
(`files`, `settings`, `monitor`, `notes`, `store`, `calc`) and a client window to
`clients[i].id`; `icon_for_id()` has a drawing for `clock`, `paint`, `life`,
`mandel` and `sysmon`, and falls back to `icon_gear()` for anything else — with a
hover tint, the accent colour for the focused window, a 45 %-alpha foreground for
a minimised one, a 14×3 accent pill under the focused one, and a tooltip with the
window title over the hovered one.

---

## 4. The app contract (`src/mystic.h`, `src/apps.c`)

### 4.1 The table

```c
typedef struct {
    const char *id;
    const char *title;
    void (*init)(App *a, int w, int h);
    void (*draw)(App *a, Surface *s, int x, int y, int w, int h, const Theme *th);
    void (*key)(App *a, const char *bytes, int n);
    void (*click)(App *a, int x, int y, int buttons);
    void (*resize)(App *a, int w, int h);
    void (*free)(App *a);
} AppVT;

struct App {
    const AppVT *vt;
    void *state;      /* app private data */
};
```

`id` is what the launcher and the F-keys name and what `app_new()` looks up;
`title` is the window title. The rest are the callbacks listed in §4.2.

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
    &app_vt_sysinfo,
```

`sysinfo` has no `#ifdef`: it is always built, and it is the reference
implementation of the table.

`tools/build-desktop.sh` walks `for app in files settings monitor notes store
calc`, adds `-DWITH_<NAME>` and the source file for each one that exists, and
links them all into the one binary. `src/mystic.h` declares the matching
`extern const AppVT app_vt_*` behind the same macros, so a file that is present
but not defined is simply not referenced.

| call | caller | when |
|---|---|---|
| `vt->init(a, w, h)` | `app_new()` | window opened; `w,h` are `w-2, h-42` |
| `vt->draw(a, s, x, y, w, h, th)` | `app_draw()` ← `draw_desktop()` | every composed frame, content rectangle `(x+1, y+title_h()+1, w-2, h-title_h()-2)` |
| `vt->key(a, bytes, n)` | `app_key()` ← `main()` | top window only: escape sequences per key, then the printable `text[]` once per frame |
| `vt->click(a, x, y, buttons)` | `app_click()` ← `handle_click()` | left press inside the body, window-local coordinates |
| `vt->resize(a, w, h)` | `main()` drag / snap / maximise handlers | called with `w-2, h-42` (the same size `init` got, which is one pixel taller than the draw rectangle when `title_h() == 40`) |
| `vt->free(a)` | `app_free()` ← `win_close()` | window closed |

`app_draw`, `app_key`, `app_click` and `app_free` are all null-safe: a missing
handler is a no-op, `app_title` falls back to `"App"`, and an unknown id makes
`app_new` return `NULL` (so the launcher can list an app that was not compiled in
and nothing breaks).

Registered today: `files`, `settings`, `monitor`, `notes`, `store` (`"Apps"`, a
browser for the installed bundles) and `calc` (`"Calculator"`, a recursive-descent
expression evaluator), plus `sysinfo`, which is the reference app that shows the
smallest useful implementation of the table. The launcher lists all seven plus one
tile per installed bundle; F1…F5 open terminal, files, settings, monitor and notes.

### 4.3 Why an in-desktop app cannot reach into the desktop

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

What it is **not**: process isolation, and not per-window clipping. In-desktop
apps are functions linked into `mystic-desktop` and run in its address space; a
stray pointer or an out-of-bounds write in an app can corrupt the compositor or
kill the whole desktop. `Surface` clipping is only to the surface bounds — the
compositor does not hand out a sub-surface or a clip rect — so an app that ignores
its rectangle will paint over neighbouring windows until the next recompose. Both
facts matter when reading the security claims in `README.md`; an app that needs a
real boundary is written as an external app instead (§4.5).

### 4.4 Apps read real data

No app invents numbers:

* `sysinfo` reads `/proc/meminfo`, `/proc/uptime`, `/proc/loadavg` and
  `/proc/<pid>/comm`.
* `monitor` walks `opendir("/proc")` and reads `/proc/<pid>/stat` (comm,
  utime/stime), `/proc/<pid>/statm` (RSS), `/proc/meminfo`, `/proc/loadavg` and
  `/proc/uptime`.
* `files` is a real file manager: `opendir`/`readdir`/`lstat` for the listing,
  `st_size`/`st_mode`/`st_mtime` for the columns, `statvfs()` for the free-space
  figure, `stat()` to resolve a symlink that points at a directory, and
  `strerror(errno)` when `opendir` fails. Its sort order, breadcrumbs and
  toolbar/row geometry are all derived from the fonts it draws with.
* `notes` reads and writes `/root/notes.txt` with `fsync` (§5.3).
* `settings` scans `/etc/mystic/themes` with `readdir`/`fopen`, reads the
  `name =` line of each `.theme` file (falling back to the file name when the
  line is missing) and writes the chosen `name` to `/etc/mystic/current-theme`.
* `store` reads `/usr/share/mystic/apps` with `readdir`, parses each `manifest`
  the way `wmsrv.c` does (`name`/`desc`/`exec`, one `=` per line) and stats the
  real payload file for the size it shows. It never forks; Enter writes the
  bundle id to `/run/mystic/launch.request` (§4.6).
* `calc` is pure: no file or `/proc` access at all. Its numbers come from the
  text the user typed and from the parser's own arithmetic.

### 4.5 External apps: a second kind of window (`src/ipc.h`, `src/wmsrv.c`, `src/client/`)

An **external app** is its own process. It connects to a unix socket, receives a
shared-memory pixel buffer, draws into it, and the desktop blits that buffer into
a window. `docs/IPC.md` is the protocol reference; what matters for the
architecture is:

* `wms_start()` binds `WM_SOCKET` (`/run/mystic/wm.sock`, creating the directory)
  once at startup; `wms_poll()` is called every frame, accepts connections
  without blocking and reads complete `\n`-terminated lines.
* On `HELLO <id> <w> <h> <title…>` the server creates
  `/dev/shm/mystic-<pid>-<n>` with `ftruncate` + `mmap`, zeroes it, and answers
  `OK <path> <stride>`; `stride == w`. `FRAME`, `TITLE <text>`, `CLOSE` and
  `EVENT <text>` (logged only) are the other client verbs.
* The desktop forwards input for the **focused** client window only:
  `KEY <code> <mods> <press>` on press and release, `TEXT <utf8>` once per frame,
  `MOUSE <x> <y> <buttons>` for clicks and motion, and `RESIZE <w> <h>` when the
  window is resized. Coordinates are relative to the content origin, and there is
  no partial damage: `FRAME` means "the whole window is new".
* The window is created by the frame loop, not by `win_open()`: as soon as a
  client has a buffer, a free `Window` is bound to it with `kind = WIN_CLIENT`,
  `w = cl->w + 2`, `h = cl->h + title_h() + 2`, a cascade position, and the
  client's title.
  The blit clips to *both* the client buffer and the screen, and shifts the source
  when the window's content origin is off-screen, because clamping only the
  right/bottom edges wrote before the buffer for a negative origin.
* Closing the window sends `CLOSE` to the app and then shuts the connection down;
  a client that disconnects or exits clears its slot, and the frame loop closes
  the window bound to that slot before the slot can be reused.
* Apps run as root, in the same VM, with no sandbox. The isolation is address
  space and crash containment, not privilege: a broken external app cannot corrupt
  the compositor, but it can do anything else root can.

### 4.6 Launching: one place does the `fork`

`wms_scan_apps()` runs once at startup and reads `WM_APP_DIR`
(`/usr/share/mystic/apps`): every subdirectory with a readable `manifest` becomes
a `WmApp { id, name, desc, exe }`, sorted by id so the launcher is stable. Because
it runs once, a bundle packaged while the desktop is running appears after the
next boot.

`wms_launch()` is the only place that starts an **external app**: `fork()`,
`setenv("MYSTIC_SOCKET")`/`setenv("MYSTIC_APP_ID")`, `execl(WM_APP_DIR/<id>/<exe>)`.
It is called from three places — a launcher tile, the Enter key in the launcher,
and the `/run/mystic/launch.request` polling in §3.6 — which is how the in-desktop
Apps window can start an external app without forking anything itself. The child
reaps through the per-frame `waitpid(-1, …, WNOHANG)` loop.

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
setup_disk() {
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
    else
        echo "disk: could not mount"
    fi
}
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
`/etc/mystic/themes/*.theme`, `/etc/mystic/ui.conf`, `/etc/mystic/wallpaper`
and `/etc/mystic/passwd` are unpacked from the cpio at boot, and writes to `/etc`
(the theme request file) disappear on reboot. `/run` and `/tmp` are ordinary directories in that ramfs;
`/dev/shm` is a tmpfs mounted by `/init`, so a client's shared pixel buffer is
released when the machine goes down — there is nothing to clean up on the disk.

### 5.3 fsync is the difference between saving and losing

`save()` in `src/apps/notes.c` never writes the target in place. It creates
`/root/notes.txt.tmp` in the same directory
(`open(O_WRONLY | O_CREAT | O_TRUNC, 0600)`), writes the whole buffer in a loop
that handles short writes and `EINTR`, then `fsync(fd)` (a failure aborts the
save, unlinks the temp and reports the real errno), then `close(fd)` — which is
also where a deferred write error surfaces — and finally
`rename(tmp, NOTES_PATH)`.

`write()` only reaches the kernel's page cache; with `-no-reboot` and a VM that is
killed rather than shut down, unsynced bytes are simply gone. `fsync` is what
forces ext4 to hand them to the virtio device. `rename()` inside one directory is
atomic, so a failure at any step leaves the previous `notes.txt` intact instead of
a half-written file, and the buffer stays dirty with the real `errno` in `err[]`.
The same reasoning is why probe mode runs `sync` before `poweroff -f`.

The buffer holds at most `NOTES_MAX - 1` (65535) bytes. If the file on disk is
larger than what was loaded, writing the buffer back would truncate it, so the app
states the two real sizes and asks for a second Ctrl+S before it overwrites.

Persistence is observable: probe mode increments `/root/boots.txt`, creates
`/root/permtest` with mode 600 and ownership 1000:1000 and prints what
`ls -l` says, and Notes keeps `/root/notes.txt` across boots.

---

## 6. The build

`image.sh` is the pipeline; the tools under `tools/` do the work. Everything runs
on the host (Windows or Linux) — nothing is built inside the guest.

### 6.1 `tools/fetch-kernel.sh` — kernel + exactly the modules

* Downloads `linux-virt-6.18.63-r0.apk` from Alpine v3.24 `main/x86_64` into
  `build/cache`, extracts it there, and copies the package's `boot/vmlinuz-virt`
  out as `build/cache/vmlinuz-virt`.
* Takes **whole module subtrees**, not a hand-picked file list:
  `kernel/drivers/{gpu,video,i2c,input,block,net}`, `kernel/net/core` and
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
       -o build/mystic-desktop $SRC $DEFS -lm -lutil
```

Sources: the twelve core files — `main.c`, `apps.c`, `term.c`, `sha256.c`,
`wmsrv.c`, `ui.c`, `wallpaper.c`, `gfx.c`, `font.c`, `fb.c`, `input.c`,
`theme.c` — plus `apps/sysinfo.c`, and every optional app that exists, each with
its `-DWITH_*` define (today: files, settings, monitor, notes, store, calc). `-static` plus musl
means the binary needs nothing but the kernel; `-lutil` is for `forkpty`. The
result is copied to `stage/usr/bin/mystic-desktop` (and `chmod +x`), and the font
files — the three Lato faces, `JetBrainsMono-Regular.ttf` and
`LICENSE.txt` — are copied from `assets/fonts` to `stage/usr/share/fonts`.

`image.sh` does **not** build app bundles. Each one is packaged separately with
`./tools/mkapp.sh <id>`, which compiles `src/client/apps/<id>.c` together with
`src/client/mystic_client.c`, `src/gfx.c`, `src/font.c` and `src/ui.c`, writes the
binary to `stage/usr/share/mystic/apps/<id>/bin` and creates a `manifest`
(`name`, `desc`, `exec = bin`) if none exists. Repack the initramfs afterwards for
the bundle to appear.

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
   links `S_IFLNK|0777` with the target text, regular files `S_IFREG` with the
   mode bits the tarball recorded and its contents. Later tarballs and later
   entries overwrite earlier ones, and `stage/` is applied last.
2. `stage/` is walked and overlaid on top. Modes are decided by path because
   NTFS has no executable bit: a file is `0755` when it is `init`, when it lives
   under `bin/`, `sbin/`, `usr/bin/`, `usr/sbin/`, `usr/local/bin/`, **or when
   its name is `bin`** — that last rule is what makes an app bundle's payload
   (`usr/share/mystic/apps/<id>/bin`) executable. Everything else is `0644`;
   directories from `stage/` are `0755`.
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
6. Boot with the argument array from §1.1, in one of four modes:
   `run` (a window), `probe` (serial facts, then poweroff),
   `shot N` (headless, QEMU monitor `screendump` after N seconds →
   `tools/ppm2png.py` → PNG), `themes` (one boot, `sendkey 2`, `sendkey 3`,
   three screendumps).

`build.sh` and `run.sh` are the stage-1 leftovers: they assemble `boot/boot.asm`
with nasm, verify it is exactly 512 bytes and ends in `0xAA55`, write it to a
1.44 MB floppy image and boot that with no kernel. `image.sh` does not use them.

### 6.5 `tools/smoke.sh` — does the image actually run?

`tools/smoke.sh` is the project's test: it boots the image that was just packed,
rather than testing functions in isolation.

* Two probe boots (`image.sh probe`'s mode, but driven directly) — the second one
  proves the ext4 `/root` survived and the boot counter moved — checking: QEMU
  present, kernel and initramfs present, disk present, kernel reached userspace,
  kernel version, `card0`, `/dev/fb0`, each expected module in `/proc/modules`,
  the input nodes, the boot-time budget from the probe's own `/proc/uptime`, no
  kernel/userspace crash in the log, and that `/dev/vda` mounted ext4.
* One desktop boot with `DESKTOP_WAIT` seconds of runtime against the packed
  image, checking that the desktop opened the display (the `display: … 32bpp`
  line printed by `disp_open()`), bound a keyboard (`input: keyboard`, printed by
  `input_open()`) and did not crash.
* Static checks that need no QEMU (`--quick` runs only these): the text scale
  (`stage/etc/mystic/ui.conf` exists, parses and is inside 0.8…1.6; `src/ui.c`
  defines `ui_scale_get`/`ui_scale_load`/`ui_px`; `main.c` loads the file;
  `font_get()` applies the scale), the monospace terminal (the `.ttf` exists and
  starts with a real sfnt magic, it is installed into `stage`, `font.c` names it,
  `term_draw()` is called with `FONT_MONO`), the taskbar (`taskbar_count`,
  `taskbar_at`, `dock_rect`'s "nothing open" guard, and that `dock_rect()` is used
  by both the painter and the hit test), and a stale-build guard: the mtime of
  `stage/usr/bin/mystic-desktop` against every `.c` file that
  `tools/build-desktop.sh` actually links plus the headers those files
  `#include`. A source the desktop does not link (a client app, or a module that
  has landed but is not wired in) is named on a `not part of the desktop build,
  ignored:` line instead of failing the run, so a pass never hides a stale
  binary.
* Compile checks: every `src/*.c`, `src/apps/*.c`, `src/client/*.c` and
  `src/client/apps/*.c` is compiled with the same `zig cc` flags and must be
  warning-free — the client apps are built by `tools/mkapp.sh`, which `image.sh`
  never calls, so nothing else would catch a broken one.

Every check prints `PASS`/`FAIL` and the script exits non-zero if anything failed,
so it is usable as a gate.

### 6.6 What git tracks, and what it does not

The repository holds sources, themes, manifests and scripts. Everything a build
produces is ignored:

```
stage/usr/bin/mystic-desktop
stage/usr/share/mystic/apps/*/bin
stage/lib/modules/
stage/usr/share/fonts/*.ttf
stage/sbin/
stage/usr/lib/
build/
```

So a fresh clone has the manifests (`stage/usr/share/mystic/apps/<id>/manifest`)
but no payloads: it needs `tools/fetch-kernel.sh` for the modules,
`tools/build-desktop.sh` for the desktop and fonts, and one `tools/mkapp.sh <id>`
per bundle before `image.sh` can pack an image that launches anything. `assets/`
keeps the `.ttf` files (and their OFL licence) as the source of the copies that
`build-desktop.sh` stages.

---

## Ours vs borrowed

| piece | origin | where |
|---|---|---|
| Compositor, layout, taskbar, launcher, window manager, cursor, event loop | ours | `src/main.c` |
| Global text scale (config parser, `ui_px`) | ours | `src/ui.c` |
| CPU renderer: SDF rounded rects, gradients, glow, blur, shadow, polygons | ours | `src/gfx.c` |
| Framebuffer access (`/dev/fb0`, stride, mmap, present) | ours | `src/fb.c` |
| evdev discovery, keymap, per-frame input state | ours | `src/input.c` |
| Glyph cache, text blending, UTF-8, face/size cache | ours | `src/font.c` |
| TrueType rasteriser | **stb_truetype.h** (public domain, Sean Barrett) | `src/stb_truetype.h` |
| Theme file format, parser, six shipped themes | ours | `src/theme.c`, `stage/etc/mystic/themes/*.theme` |
| Terminal emulator: pty, VT parser, cell grid, palette, scrollback | ours | `src/term.c` |
| `AppVT` contract and app registry | ours | `src/mystic.h`, `src/apps.c` |
| Seven in-desktop apps (files, settings, monitor, notes, store, calc, sysinfo) | ours | `src/apps/*.c` |
| Display protocol: messages, shm, fork/exec, bundle scan | ours | `src/ipc.h`, `src/wmsrv.c` |
| Client SDK for external apps | ours | `src/client/mystic_client.c`, `src/client/mystic_client.h` |
| External apps, one process each | ours | `src/client/apps/*.c` |
| Procedural wallpapers (four, theme-derived, two animated) | ours | `src/wallpaper.c`, `src/wallpaper.h` |
| SHA-256, iterated salted password hash, password-file check | ours | `src/sha256.c` |
| PID 1: mounts, module loading, network, disk setup, fbcon unbind, desktop exec | ours | `stage/init` |
| initramfs builder (newc writer) | ours | `tools/mkimage.py` |
| kernel/module fetcher, desktop build, app packager, boot harness, smoke test | ours | `tools/fetch-kernel.sh`, `tools/build-desktop.sh`, `tools/mkapp.sh`, `image.sh`, `tools/smoke.sh` |
| Screenshot converter (QEMU PPM → PNG) | ours | `tools/ppm2png.py` |
| 512-byte boot sector (stage-1 history, unused by `image.sh`) | ours | `boot/boot.asm`, `build.sh`, `run.sh` |
| Kernel 6.18.63-0-virt | Alpine `linux-virt` (GPL-2.0) | `build/cache/vmlinuz-virt` |
| DRM (`bochs`, `virtio-gpu`), evdev, psmouse, virtio_blk, virtio_net, ext4/jbd2, vfat modules | Alpine `linux-virt` | `stage/lib/modules/6.18.63-0-virt/` |
| Userland: `sh`/busybox (`mount`, `depmod`, `modprobe`, `udhcpc`, `ip`, `awk`, `ls`, `poweroff`, …), musl libc | Alpine minirootfs 3.24.4 | inside `build/cache/minirootfs.tar.gz`, packed into the cpio |
| `mkfs.ext4` and its shared libraries | Alpine e2fsprogs | `stage/sbin/mkfs.ext4`, `stage/usr/lib/*`, `stage/etc/mke2fs.conf` |
| Lato Regular/Bold/Black | SIL Open Font License | `assets/fonts/*.ttf` → `stage/usr/share/fonts` |
| JetBrains Mono Regular | SIL Open Font License | `assets/fonts/JetBrainsMono-Regular.ttf` → `stage/usr/share/fonts` |
| QEMU (build/test tool, not part of the OS) | QEMU project (GPL-2.0) | host |

---

## Known rough edges

These are behaviours of the current tree, not intentions:

* `title_h()` is `ui_px(40)` and the draw rectangle and the client-window size
  use it, but `win_open()` still passes an in-desktop app `h-42`, and the
  drag/snap/maximise/resize paths call `vt->resize(w-2, h-42)` — which is
  `title_h() + 2` only at scale 1.0. At 1.2 an app is told it has 8 px more height
  than is drawn (its last rows are clipped); at 0.8 it is told it has 8 px less (a
  strip is left empty).
* The drag clamp is still `y >= 34` while the bar it protects is `topbar_h()`, so
  above scale 1.0 a window can be dragged a few pixels into the top bar (the
  maximise path does use `topbar_h()` and leaves the taskbar its space).
* Everything that does not hold text keeps its 1.0-scale size: the traffic-light
  dots, the eye's 30×28 target, the 18 px resize corner and the lock panel's inner
  offsets. At 1.6 the eye sits in the upper half of the taller bar.
* `/run/mystic/launch.request` is only polled once per wall-clock second, so an
  Enter in the Apps window can take up to a second to start the app.
* `wms_scan_apps()` runs once at startup, so a bundle packaged while the desktop
  is running needs a reboot before it shows up in the launcher.
* `src/ipc.h`'s header comment documents `HELLO <id> <title…> <w> <h>`, but the
  server (`wmsrv.c`) and the SDK (`mystic_client.c`) both use
  `HELLO <id> <w> <h> <title…>`. `docs/IPC.md` has the right order.
* `image.sh`'s boot array separates the `-drive` and `-netdev` tokens with a
  literal `\n` (backslash + `n`), which bash turns into an extra argument `n`
  before `-netdev`. `bash -c` on that array shows the stray element, so the boot
  command needs that token removed before `-netdev`/`-device` mean anything.
* The wallpaper can be changed at runtime by writing `/etc/mystic/wallpaper`, but
  there is no in-desktop app that does it yet — it is a file and a text editor
  away, not a Settings row.
* The README's feature table still says the disk is FAT and that there are three
  themes and a dock; `setup_disk()` formats and mounts ext4, six `.theme` files
  ship, and the bottom bar is the taskbar of §3.11. The `fat`/`vfat` modules are
  loaded but never used.
* There is no per-window clip rectangle for in-desktop apps, so an app that draws
  outside its rectangle overpaints its neighbours until the next compose (§4.3).
* Resizing a terminal window does not update the pty `winsize`; the grid simply
  draws fewer cells than the window could hold.
* With `blur = 1.0` every recompose blurs the top bar, every visible window and
  the taskbar, plus a scratch surface per shadow, on one CPU core. Motion is what
  triggers it, so moving the pointer across the screen is the most expensive
  thing a user can do.
