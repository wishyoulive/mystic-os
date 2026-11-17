# Themes

A theme is the whole colour identity of the desktop: the two-colour backdrop the
wallpaper is mixed with, the glass of the top bar and the launcher, the body of
every window, the text, and the accent that marks focus and selection.

Themes are data, not code. Anyone can write one with a text editor, drop it in a
directory, and see it in Settings without recompiling anything.

| Where | What |
|---|---|
| `stage/etc/mystic/themes/*.theme` | the sources, baked into the image at `/etc/mystic/themes` |
| `src/theme.c` | the parser (`theme_load`, `theme_load_all`) and the built-in defaults |
| `src/mystic.h:127-142` | the `Theme` struct every other file draws from |
| `src/main.c:32` | `THEME_DIR`, the directory the desktop reads |
| `src/apps/settings.c` | the Settings → Theme list |
| `tools/mkimage.py:94-103` | overlays all of `stage/` onto the rootfs, so a new `.theme` file needs no build-list edit |

The desktop loads the directory once at boot, in **filename order**
(`theme_load_all` sorts with `strcmp`), so the leading number in `1-obsidian.theme`
is what puts it first. `Ctrl+1` … `Ctrl+9` select the 1st … 9th theme in that
order (`src/main.c:1453`). Settings writes the chosen theme's `name` to
`/etc/mystic/current-theme`, which the desktop re-reads once a second; the match
is on the **`name =` field inside the file, not the filename**.

## The key set

Fifteen keys, one per line, `key = value`. All of them are optional: a file that
omits one keeps the built-in default from `defaults()` (`src/theme.c:68-86`), and
the parser says so on stderr.

| Key | Type | Range / form | Default | What it paints |
|---|---|---|---|---|
| `name` | text | ≤ 63 bytes, unique | `Default` | shown in Settings, written to `/etc/mystic/current-theme`; the only identity the desktop persists |
| `dark` | flag | `0` or `1` | `1` | tells the wallpaper which palette to use — shadows deepen at `1`, lift at `0` (`src/wallpaper.c:210-212`); it does **not** change text colours |
| `bg_top` | colour | opaque | `#0B0F1A` | top of the desktop gradient (the wallpaper is blended onto it) |
| `bg_bottom` | colour | opaque | `#141A2B` | bottom of that gradient |
| `accent` | colour | opaque | `#7AA2F7` | focus rings, selected rows, links, warnings; drawn on both `window_bg` and `panel` |
| `glow` | colour | opaque | `#3D59A0` | the secondary light in the wallpaper and the bloom behind the launcher |
| `glow_strength` | number | `0 … 1` | `0.55` | how much `glow` is mixed in |
| `panel` | colour | alpha allowed | `#8C1A2033` | top bar, launcher, lock panel, and in-window toolbars/footers; the alpha is the glass |
| `panel_border` | colour | alpha allowed | `#1AFFFFFF` | the 1 px edge of those surfaces |
| `text` | colour | opaque | `#E8ECF6` | every primary label, value, filename, terminal glyph |
| `text_dim` | colour | alpha allowed | `#99E8ECF6` | secondary text: hints, units, unselected rows, the clock |
| `window_bg` | colour | opaque (**forced**) | `#141928` | the body an app draws on, behind all of its pixels |
| `window_border` | colour | alpha allowed | `#22FFFFFF` | the 1 px edge of a window |
| `corner` | number | `0 … 48` px | `14` | corner radius of windows and panels |
| `blur` | number | `0 … 1` | `1.0` | backdrop blur strength; `0` skips the blur entirely (crisper, and the cheapest theme to compose) |

### `window_bg` is always opaque

Every value is taken as written except one: `theme_load()` forces the alpha of
`window_bg` to `FF` (`src/theme.c:321`). A window body sits *over* a blurred copy
of the wallpaper, so a translucent body lets wallpaper detail bleed through the
text and turns into haze. The colour is still the theme's; only the alpha is
ignored. Write `#RRGGBB` and there is nothing to know.

### Colours

* `#RRGGBB` — opaque.
* `#AARRGGBB` — with alpha, `AA` first (this is the same `0xAARRGGBB` word the
  compositor uses). `panel` and `text_dim` are the two that normally use it.
* The leading `#` is optional, and `0x` works too: `#7AA2F7`, `7AA2F7` and
  `0x7AA2F7` are the same colour.
* The **whole token** must be hex. `#12345` (five digits), `#0B0F1A;` and `nothex`
  are errors, not approximations.
* A `#` **at the start of a value** is the colour prefix. A `#` **after white
  space** starts a comment, so this line is one colour and one comment:

  ```
  accent = #FF00FF  # magenta, for the warning state
  ```

* A line whose first non-blank character is `#` is a comment, and so is a blank
  line.

### Numbers

Plain decimal. `glow_strength` and `blur` are clamped to `0 … 1`, `corner` to
`0 … 48`. Out-of-range values are reported and clamped rather than rejected.

## Contrast

Text has to be readable, and "readable" is measurable. Every theme shipped with
the system is held to these targets:

| Text | Measured against | Target |
|---|---|---|
| `text` (body) | `window_bg`, and `panel` composited over both the desktop and the window body | **≥ 7 : 1** |
| `text_dim` (secondary) | the same two surfaces | **≥ 4.5 : 1** |
| `accent` (selection, focus, errors) | the same two surfaces | ≥ 4.5 : 1 |

`accent` is used for small selected-row labels, so 4.5 : 1 is the floor here
rather than the 3 : 1 WCAG asks of large text and non-text UI.

### The formula

WCAG 2.1 relative luminance and contrast ratio, on the composited sRGB values:

```
c  = channel / 255
L  = 0.2126 * f(R) + 0.7152 * f(G) + 0.0722 * f(B)
f(c) = c / 12.92                      if c <= 0.04045
     = ((c + 0.055) / 1.055) ^ 2.4    otherwise
ratio = (Lmax + 0.05) / (Lmin + 0.05)
```

Two details that decide whether the numbers are honest:

* **Alpha is composited first**, with the same arithmetic the compositor uses
  (`dst = src*a + dst*(1-a)`, in 8-bit sRGB): `text_dim` at `#99…` over a light
  body is a mid grey, not the dark colour written in the file.
* **`panel` is checked twice**, because it is drawn over two different things:
  over `bg_top` for the top bar and the launcher, and over `window_bg` for the
  toolbar and footer inside a window (`surf_blend(..., th->panel)` in
  `src/apps/files.c`). The worse of the two is the number that matters.

### The four new themes, measured

| Theme | `text` on `window_bg` | `text` on `panel` (bar / in window) | `text_dim` on `window_bg` | `text_dim` on `panel` (bar / in window) | `accent` on `window_bg` | `accent` on `panel` |
|---|---|---|---|---|---|---|
| **Neon** | 19.30 : 1 | 19.17 / 19.04 | 6.98 : 1 | 6.93 / 6.89 | 14.36 : 1 | 14.27 / 14.18 |
| **Forest** | 15.18 : 1 | 14.55 / 14.25 | 7.81 : 1 | 7.49 / 7.33 | 9.39 : 1 | 9.00 / 8.81 |
| **Rose** | 13.48 : 1 | 13.20 / 13.21 | 5.80 : 1 | 5.68 / 5.69 | 6.69 : 1 | 6.55 / 6.56 |
| **Mono** | 13.92 : 1 | 12.94 / 12.80 | 7.16 : 1 | 6.66 / 6.59 | 18.73 : 1 | 17.40 / 17.22 |

Every value clears its target; the tightest is Rose's dim text at 5.68 : 1.

How the four differ is a design decision, not a hue rotation:

* **Neon** — `bg_top #05040A` is near-black so the cyan accent (`#22F0FF`) and the
  magenta glow (`#FF2BD6`) carry the screen. Tight 10 px corners, full blur.
* **Forest** — deep moss (`#07110A` → `#122A1A`) with warm parchment text
  (`#F1EAD9`) and an amber accent; `glow_strength` drops to 0.35 so nothing
  glares, and the 20 px corners are the softest of the set.
* **Rose** — the light theme: warm paper (`#FDF4EF`), sand (`#F2DDD4`), deep plum
  text (`#3B2530`) and a rose-madder accent. Its `text_dim` is **opaque**
  `#7A5A63` on purpose: a 60 %-alpha grey over a light body measures 4.1 : 1
  (that is what the older Porcelain theme does), while the opaque warm grey holds
  5.8 : 1.
* **Mono** — every channel equal in every colour, so there is no hue at all:
  `#0E0E0E`, `#121212`, `#DEDEDE`, `#A0A0A0`, accent pure white. `blur = 0.0` and
  `glow_strength = 0.25`, because flat surfaces and crisp glyph edges are what
  maximum legibility means; it is also the cheapest theme to compose. The only
  colour left on the screen is the three window buttons, which `src/main.c:793-797`
  paints at fixed RGB instead of taking them from the theme.

### The two space themes, measured

Both are built for the two space wallpapers (`Black Hole` and `Nebula` in
`src/wallpaper.c`), which take their colours from the theme: the accretion disk
runs hot-white at the ISCO through `accent` to `glow` at the rim, and the
nebula's two cloud hues are `accent` and `glow` again. Neither theme needs a
different key set — they are ordinary `.theme` files.

| Theme | `text` on `window_bg` | `text` on `panel` (bar / in window) | `text_dim` on `window_bg` | `text_dim` on `panel` (bar / in window) | `accent` on `window_bg` | `accent` on `panel` |
|---|---|---|---|---|---|---|
| **Nebula** | 16.27 : 1 | 16.67 / 16.44 | 6.57 : 1 | 6.74 / 6.64 | 9.04 : 1 | 9.27 / 9.14 |
| **Eclipse** | 18.63 : 1 | 18.74 / 18.52 | 6.29 : 1 | 6.32 / 6.25 | 11.31 : 1 | 11.37 / 11.24 |

* **Nebula** — violet and indigo: `bg_top #0A0618` is almost black with a blue
  cast, `bg_bottom #170D33` is the nebula's own light, the accent is a pale
  lavender (`#B9A6FF`) and the glow an indigo (`#6B3FD4`) at
  `glow_strength 0.65`. 16 px corners; the softest theme that is still dark.
* **Eclipse** — near-black with a single warm light: `bg_top #070404`,
  `bg_bottom #170C05` (the corona's afterglow), accent amber `#FFB454`, glow a
  burnt ember `#8A3E0C` at `0.7`. 12 px corners. The accent is the only
  saturated colour on the screen, which is what makes the black hole's beamed
  side read as a highlight rather than as decoration.

### The six older themes, measured

Same formula, same method, for the record:

| Theme | `text` | `text_dim` | Note |
|---|---|---|---|
| Obsidian | 14.81 : 1 | 6.06 : 1 | |
| Porcelain | 15.04 : 1 | **4.11 : 1** | below the 4.5 target: `text_dim` is `#992A2620`, and 60 % alpha over a white body lands at 4.11 (4.07 on a panel). `text_dim = #B32A2620` would measure 5.65. Not changed here — that file belongs to a different change. |
| Aurora | 16.36 : 1 | 6.59 : 1 | |
| Basalt | 13.96 : 1 | 5.75 : 1 | |
| Sandstone | 15.35 : 1 | 4.57 : 1 | just over the line (4.55 on a panel) |
| Carbon | 19.60 : 1 | 12.46 : 1 | |

## Adding a theme

1. Copy a file you like, e.g. `cp 1-obsidian.theme 7-mine.theme`. The leading
   number decides where it appears in the list.
2. Set `name` to something unique. Settings and `/etc/mystic/current-theme`
   identify a theme by this string, and two themes with the same name cannot be
   told apart.
3. Fill in all nine that matter: `name`, `dark`, `bg_top`, `bg_bottom`, `accent`,
   `panel`, `text`, `text_dim`, `window_bg`. The other six have defaults that work
   on any background.
4. Measure. The two rules that catch people out: composited alpha, and `panel`
   over both `bg_top` and `window_bg`. `text` must be ≥ 7 : 1 and `text_dim`
   ≥ 4.5 : 1 on both.
5. Put the file in `stage/etc/mystic/themes/` (that directory *is*
   `/etc/mystic/themes` in the image; `tools/mkimage.py` overlays all of `stage/`,
   so there is no build list to add it to), rebuild the image, boot, and look at
   stderr: a clean theme prints nothing.
6. Switch to it with Settings → Theme, or `Ctrl+1` … `Ctrl+9` for the 1st … 9th
   file in list order.

## When a theme is wrong

`theme_load()` never fails because of a bad value: it keeps the default for that
key, keeps going, and prints one line per problem naming the file, the line (when
there is one) and the key. All of the following are exercised by the host test
harness, not just documented:

| In the file | Result | Logged as |
|---|---|---|
| `bg_top = #12345` | default kept | `theme: FILE:3: key 'bg_top': malformed colour '#12345' (want #RRGGBB or #AARRGGBB), keeping #FF0B0F1A` |
| `glow_strength = 2.5` | clamped to `1` | `theme: FILE:6: key 'glow_strength': 2.5 is outside 0..1, using 1` |
| `blur = nan` | default kept | `theme: FILE:8: key 'blur': not a number, keeping 1` |
| `accent` missing entirely | default kept | `theme: FILE: key 'accent': missing, keeping the built-in default` |
| `acccent = #123456` | line ignored | `theme: FILE:12: unknown key 'acccent', ignored (see docs/THEMES.md)` |
| the same key twice | last value wins | `theme: FILE:13: key 'panel': appears more than once, the last value wins` |
| a line over 511 bytes | line ignored | `theme: FILE:3: line longer than 511 bytes, ignored` |
| a file that cannot be opened | skipped, next file fills the slot | `theme: PATH: cannot read (No such file or directory), skipped` |

Two smaller kindnesses: a UTF-8 BOM at the start of the file is stripped (a text
editor on Windows adds one), and CRLF line endings are accepted. Names are capped
at 63 bytes and a longer one is truncated with a warning, because the name is
what the desktop persists. After 32 problems in one file the parser prints
`too many problems in this file, further warnings suppressed` instead of burying
the boot log.

## Limits

`src/theme.c` will scan 256 directory entries, load at most `THEME_MAX` (32) into
a caller's array, and warn when the caller asks for more than 32
(`src/theme.c:36-39`). It also warns when the directory holds more `.theme` files
than the caller has slots, and names the ones it had to drop.

**That limit is currently 8, and it is a real bug.** With ten themes shipped:

* `src/main.c:99` — `static Theme themes[8];`
* `src/main.c:1367` — `theme_count = theme_load_all(themes, 8, THEME_DIR);`
* `src/apps/settings.c` — `#define MAX_THEMES 8`, deliberately matched to
  `main.c` so Settings never offers a row the desktop cannot apply.

So `neon.theme` and `rose.theme` sort after the eighth name and are **not loaded**;
the boot log now says exactly that:

```
theme: /etc/mystic/themes: 2 of 10 theme file(s) not loaded - the caller only has 8 slots (raise the Theme array, see docs/THEMES.md): neon.theme rose.theme
```

The fix is three numbers in files this document does not own — raise `themes[8]`
to `themes[16]`, pass `16`, and set `MAX_THEMES` to `16`. `theme.c` is already
sized for it (`THEME_MAX 32`), and so is the rest of the desktop: `Ctrl+1..9`
addresses nine themes, and Settings scrolls.
