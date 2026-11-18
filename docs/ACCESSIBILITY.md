# Accessibility and internationalisation

What Mystic OS actually does, what was already true before this work, and what
is **absent**. Every number here was measured on the host from the shipped
files by `.work/i18n/run.sh` (see *How to re-check* at the end); nothing in this
document is an estimate.

---

## 1. Text measurement: one Unicode width table for the whole OS

The width of a character in cells is a property of the character, not of the
face that draws it, so the tables live in `src/font.c` beside the other
character data and are shared through `src/i18n.h`:

| call | meaning |
|---|---|
| `font_cell_width(cp)` | 2 for East Asian Wide/Fullwidth and emoji, 0 for a combining mark or zero-width control, 1 otherwise |
| `font_utf8_next(s, len, &bytes)` | one strict UTF-8 sequence; rejects overlongs, surrogates, > U+10FFFF and cut-short sequences, and still advances at least one byte |
| `font_utf8_boundary(s, n)` | the longest prefix ≤ `n` bytes that holds whole characters only |
| `font_utf8_prev(s, n)` | one step back, whole characters at a time |

`CP_WIDE` and `CP_ZERO` in `src/font.c:186-341` are the range tables from
Markus Kuhn's public-domain `wcwidth()` (they encode Unicode `EastAsianWidth`
and the combining classes), and `font_cell_width()` (`src/font.c:342`) is a
binary search over them. Measured on the host:

| test | cells |
|---|---|
| `A`, `é`, `П`, `ї` | 1 |
| `日`, `あ`, `ア`, `Ａ`, `가`, `🎉`, `😀` | 2 |
| `e` + U+0301 combining acute | **1 in total** (the mark is 0) |
| U+200B, U+200D, U+FE0F, U+0301 alone, Hangul medial U+1161 | 0 |

The assignment's mixed line `Привіт café 日本語 🎉` is **17 codepoints and 21
cells** — measured, and asserted by `.work/i18n/grid.c` after pushing it
through the real terminal grid.

### 1.1 The terminal grid (was **not** true before)

`src/term.c` used to give every codepoint exactly one cell. A CJK glyph was
therefore drawn at its natural width (about two cells) on top of the following
character, and every column-aligned program (`ls`, a prompt, an editor) was
off by one per CJK character.

Now each cell carries a span (`src/term.c:65-79`) and `put_char()`
(`src/term.c:325`) uses `font_cell_width()`:

* **2 cells** — head cell `span = 1`, tail cell `span = 2`, cursor advances two.
  At the last column the pair starts on the next row (as xterm does), because a
  two-cell glyph cannot be split.
* **0 cells** — the mark is stored on the cell before the cursor
  (`Cell.mark`) and drawn over the base glyph at the same pen; one mark slot
  per cell (a second mark replaces the first).
* overwriting either half of a wide glyph releases the other half
  (`unspan()`, `src/term.c:296`), and a resize that cuts a pair in half blanks
  the survivor (`fix_row_edge()`, `src/term.c:879`). An orphaned span would
  either paint over a neighbour or leave a cell nothing ever paints.
* `term_draw()` skips the tail cell (`src/term.c:1393`) and draws the mark
  after the base (`src/term.c:1161-1180`).

Proven with the real `put_char()` (not a re-implementation) in
`.work/i18n/grid.c`: 日 advances two cells and produces spans 1/2; `e` + U+0301
advances zero and stores the mark; overwriting the tail releases the head; a
wide glyph in the last column starts on the next row; 40 × 日 fill an 80-column
row exactly; the mixed line ends at column 21.

### 1.2 Truncation never splits a character

Every place that shortens a string now cuts on a codepoint boundary. Before,
three of these cut by byte and left a bare lead byte in front of the `..`;
`font_draw()` then read that lead byte plus the first `.` as one 2-byte
sequence, drew a wrong glyph and swallowed the dots.

| where | file | fix |
|---|---|---|
| scrolled-back pill | `src/term.c:184` `fit_label()` | `font_utf8_prev()` per step, and the buffer cap is a boundary |
| file names / footer / dialogs | `src/apps/files.c:668` `text_fit()`, `:516` `cut_utf8()` | boundary-safe cut and a boundary-safe `snprintf` tail |
| breadcrumbs | `src/apps/files.c` `crumb_build()` | a segment longer than the buffer is cut on a boundary |
| viewer name / footer / path tail | `src/client/apps/viewer.c:2094` `tx_fit()`, `:2121` `tx_fit_tail()` | both the buffer-cap copy and the shorten loop step whole characters |
| notes labels and paths | `src/apps/notes.c` `clip_text()` | the "it fits the column" path also respected the buffer cap, which could cut a 2-byte letter in half; the tail is now moved back to a boundary |

Measured: `.work/i18n/harness.c` sweeps **every** width from 4 px to the full
string (`Документы/Проект-Япония-日本語のファイル名-Тест-é-ü-🎉-2026-final-version.txt`,
115 bytes / 67 codepoints / 77 cells) through all three helpers —
**2198 probes, 0 invalid UTF-8, 0 over-budget draws** — plus a 1080-byte string
into a 600-byte buffer, which now ends on a boundary.

### 1.3 The viewer counts cells, not codepoints

`line_cols()` and `build_display()` (`src/client/apps/viewer.c:1263,1294`) used
to count one column per codepoint, so a Japanese line fitted twice as many
glyphs as the mono face draws and ran into the line-number gutter. They now use
`font_cell_width()`, keep every UTF-8 sequence whole, never emit a wide glyph
in a column it does not fit, and copy a combining mark only when its base cell
is visible.

### 1.4 What is *not* done here

* No shaping (no HarfBuzz): Arabic/Devanagari letters are drawn in isolated
  forms, and there are no ligatures beyond the face's own.
* No grapheme clustering: a base plus several combining marks is one cell with
  **one** mark slot; emoji ZWJ sequences are separate glyphs, not one cluster.
* No bidi reordering (see §4).

---

## 2. Font fallback (was **not** true before)

`glyph_get()` used to ask the role's own face only. `stbtt_FindGlyphIndex()`
returning 0 is the index of glyph 0, `.notdef` — the box the owner saw where a
Cyrillic or Japanese letter belonged, even when an installed face had the
letter.

`face_pick()` (`src/font.c:1514`) now walks the whole installed face list in
scan order (sorted by file name, so the choice is repeatable), prefers a face
that puts **ink** on paper for that codepoint, and only accepts a face that
maps the codepoint to an empty outline (a colour-emoji layer, a space) when
nothing better exists. The fallback face is rasterised at the same pixel height
as the role's face, so the text stays on one baseline. The decision is cached
in the glyph entry, including "nowhere" (−1), so the walk happens once per
codepoint per size.

The count is **measured from the face list**, not estimated:

```c
void font_coverage(Font *f, const char *text, int *fallback, int *missing);
```

`fallback` counts codepoints the role's face cannot draw but another installed
face supplies; `missing` counts codepoints no installed face has at all (those
keep drawing the primary `.notdef`, so a gap stays visible instead of turning
into a silent blank).

### What the image ships, and what that means

`stage/usr/share/fonts` contains **Lato-Regular, Lato-Bold, Lato-Black and
JetBrains Mono Regular**. Between them: Latin (accented included), Greek and
Cyrillic. They do **not** contain CJK, Hangul, Thai, Arabic, Devanagari or
emoji — so on the shipped image those characters are still boxes, and
`font_coverage()` says exactly how many.

Measured with `.work/i18n/harness.c`, same string `Привіт café 日本語 🎉`:

| installed faces | fallback | missing |
|---|---|---|
| Lato + JetBrains Mono (what ships) | 0 | 4 (3 ideographs + the emoji) |
| Lato + JetBrains Mono + a CJK face | 3 (日本語 from the CJK face) | 1 (🎉) |

`日本語` alone: **3 fallback, 0 missing** with a CJK face installed.

The same line drawn by the real `font_draw()` exists as two renders:

* `.work/i18n/fallback-installed.png` — the shipped face list: the Cyrillic and
  accented Latin are correct, and the three ideographs and the emoji are boxes
  (`0 from another face · 4 missing`). This is the honest current state of the
  image.
* `.work/i18n/fallback-cjk.png` — the same faces plus one CJK face: the
  ideographs are drawn from it (`3 from another face · 1 missing`), no boxes;
  the emoji is still a box because no installed face can draw it.

Both renders are 560×40, produced by `.work/i18n/harness.c` (`run.sh fallback`).

The count is not only a harness number: the viewer prints it. A text file
loaded in the Viewer gets a **Glyphs** row in its `i` sheet when anything in it
came from another face or could not be drawn — measured once at load (never in
a draw path) from the face the reading column uses. Measured by
`.work/i18n/viewer_info.c` on a four-line file (`Привіт café`, `日本語のテキスト`,
`emoji: 🎉`, ASCII):

| installed faces | `i` sheet row |
|---|---|
| shipped | `Glyphs  0 from another face · 9 missing` |
| + a CJK face | `Glyphs  8 from another face · 1 missing` |
| a pure-ASCII file | no Glyphs row at all |

Controls, space separators and the zero-width formatting characters are not
counted (they never reach a font), so a file is not reported as broken just
because it has newlines.

**How to get CJK text:** drop a `.ttf`/`.otf` CJK face into `/root/fonts` (it
is scanned first and wins over the system copy) or `/usr/share/fonts`, then
log out or switch theme/font in Settings. No configuration is needed: the
loader discovers any `.ttf`/`.otf` in those directories and the fallback chain
picks it up. Caveat: the scanner only accepts the `.ttf` and `.otf`
extensions — a `.ttc` collection (Microsoft YaHei, Noto CJK ship as
collections) must be converted or extracted to a single `.ttf` first.
`src/font.c` understands a `ttcf` collection when one is presented under a
`.ttf` name (first face is used), which is how the harness above tested it.

---

## 3. Keyboard operation

Everything a mouse can do has a key. The desktop half was **already true**;
the per-app part in Files is what this work added.

### 3.1 Already true (verified by reading `src/shortcuts.c` and `src/main.c`)

| key | action |
|---|---|
| `Super` (tap) / `Super+Space` | launcher; arrows move, `Enter` launches |
| `Super`+`1…9` | focus window by dock number |
| `Alt+Tab` / `Alt+Shift+Tab` | next / previous window (works through modals) |
| `Ctrl+W`, `Ctrl+Q` | close window |
| `Ctrl+M`, `Ctrl+Shift+M` | minimise, maximise/restore |
| `F11`, `Super+F` | full screen |
| `Ctrl+Alt+←/→/↑` | move window; `+Shift` resizes |
| `Super+←/→/↑` | snap left / right / up; `Super+=`/`-` grow/shrink |
| `Super+E` Files, `Super+I` Settings, `Super+M` Monitor, `Super+O` Notes, `Super+S` Store, `Super+K` Calc, `Super+B` Browser | open the app |
| `F2…F8` | the same apps by function key; `F9` System info |
| `Ctrl+Alt+Enter` | terminal |
| `Ctrl+=`, `Ctrl+-`, `Ctrl+0` | text larger / smaller / normal |
| `Ctrl+Alt+1…9` | theme by number; `Ctrl+Alt+T` next theme, `Ctrl+Alt+W` wallpaper |
| `Super+?`, or `F1` when no window is focused | shortcut panel (page up/down, `Home`) |
| `Ctrl+Alt+L` | lock screen; type the password, `Enter`, `Backspace` |
| `Print` / `Alt+Print` | screenshot screen / window |
| `Super+N` | do not disturb |
| `Esc` | cancel any modal (always handled, even behind a menu) |

Every in-tree app defines a key handler (`.key` in its `AppVT`: Files,
Settings, Monitor, Notes, Store, Calc, Sysinfo), the Viewer handles keys in its
own loop, and `src/main.c` hands the focused window its keys either way — so no
app needs a pointer to be opened or driven. Files: `j`/`k` or arrows, `Enter` open, `Backspace`/`←` up,
`Space` mark, `v` range, `s` sort, `r` refresh, `b` back, `h` home, `n` new
folder, `m` rename, `d` delete, `i` properties, `H` hidden, `Ctrl+A/C/X/V/Z`,
`Delete`, `Home`/`End`, `PgUp`/`PgDn`, and the `Tab` focus ring of §3.2.
Viewer: arrows, `PgUp/PgDn`,
`Home`/`End`, `+`/`-`/`0`/`f` zoom, `i` info sheet, `q`/`Esc` quit.
Terminal: `Ctrl+L` clear, `Ctrl+Shift+C`/`V` copy/paste, `Ctrl+Shift+=`/`-`
font size, `Shift+PgUp/PgDn` scroll.

### 3.2 Added: a focus ring and Tab order in Files

`src/apps/files.c`: `Files.focus` (`FOCUS_LIST|BACK|UP|ACTION`), `focus_next()`
(`:899`) and `draw_focus()` (`:914`).

* `Tab` / `Shift+Tab` walk **list → back → up → header action** and wrap;
  controls that are not on screen in a short window are skipped.
* `Enter` and `Space` activate the focused control; `Esc` returns the keys to
  the list; a mouse click puts the focus where it landed.
* The focused control is outlined with **two device pixels of the theme's
  accent** just outside its rect. The list keeps its selection pill and only
  gains the ring once a key has moved the focus, so a mouse user's screen is
  unchanged.

Measured in pixels by `.work/i18n/files_focus.c` (real `files_draw()` at
520×360, three frames compared against one with no ring):

| focused control | pixels changed in its outline band |
|---|---|
| header action ("New folder") | 266 |
| back button | 64 |
| selected list row | 960 |
| the un-focused `up` button | **0** |
| the band one header above the back button | **0** |

`Esc` returns the focus, `Enter` on the focused action really opens New folder,
and the ring renders at 320×240 / 520×360 / 900×600 × scale 1.0 / 1.2 / 1.6
with 0 clip escapes.

### 3.3 What is *not* done here

No other app got a focus ring in this pass (they are single-pane and their
selection or caret already shows where the keys go); `docs/` should not be read
as saying they have one.

---

## 4. High contrast and text size

### Text size — already true

`ui.conf`/`ui_scale_get()` scale every font and every spacing unit through
`ui_px()`, and `Ctrl+=` / `Ctrl+-` / `Ctrl+0` change it live. Settings → Text
size offers the same range, and each theme can carry its own `font_scale`.
Layouts are built from `font_height()` and `ui_px()`, so row pitch follows the
scale (this was the subject of an earlier layout pass; nothing here regressed
it — the same window with the focus ring on renders at 320×240, 520×360 and
900×600 times scale 1.0/1.2/1.6 in `.work/i18n/files_focus.c` with
**0 clip escapes, 0 layout failures**, and at 900×600 the whole list still fits
its columns).

### Mono theme — already true, now measured

`stage/etc/mystic/themes/mono.theme` is greyscale, has `blur = 0`, `anim =
none` and `anim_speed = 0` (no motion), and its own comments claim measured
contrast. `.work/i18n/contrast.c` recomputes those numbers with the WCAG 2.1
formula from the loaded theme values:

| pair | ratio | floor |
|---|---|---|
| text on window_bg | **13.92 : 1** | 4.5 : 1 |
| text_dim on window_bg | **7.16 : 1** | 4.5 : 1 |
| text on panel | **12.25 : 1** | 4.5 : 1 |
| text_dim on panel | **6.30 : 1** | 4.5 : 1 |
| accent on window_bg | **18.73 : 1** | 3 : 1 |
| window_border on window_bg | **3.78 : 1** | 3 : 1 (1.4.11 non-text) |

The same harness checks the other shipped themes. All pass except
**Porcelain (`2-porcelain.theme`), whose `text_dim` is 4.15 : 1** against its
`window_bg` — below the 4.5 : 1 body-text floor that `docs/THEMES.md` claims
for every shipped theme. That palette file is not part of this work and was
left alone; the fix is one line (darken `text_dim` in that file) and the
harness will confirm it.

---

## 5. Absent: screen reader

There is none. Mystic OS has no accessibility bridge: no AT-SPI, no
`org.a11y` bus, no way to enumerate the widget tree of a window, no way to read
the focused control's name, role or value, and no notification when it
changes. A blind user cannot use this desktop today. The focus ring in §3.2
and the contrast in §4 help low-vision and keyboard-only users; they are not a
substitute.

## 6. Absent: right-to-left layout

There is none. Text is drawn left-to-right in logical order: the bidi
embedding/override and isolate controls are consumed as zero-width (so they
never shift a column) but **no reordering happens**, and the terminal grid,
the files list, the viewer and the editor all assume the first byte is the
leftmost glyph. Arabic and Hebrew render right-to-left *letters* in
left-to-right *order* — unreadable as a sentence. Adding RTL needs a bidi
implementation (UAX #9) in the text layer plus mirroring for the layout, not a
flag.

## 7. Absent: locale-specific formatting

There is none. The OS has no locale files, no `LC_*` handling beyond what the C
library defaults to, and no `setlocale()` call in the desktop. Consequences
that are visible: dates are formatted `%Y-%m-%d %H:%M` (ISO, not localised),
sizes use `B`/`KB`/`MB`/`GB` with a dot as the decimal separator, weekdays and
month names are never printed, the decimal separator is always `.`, and the
keyboard layout is fixed US. All user-visible text is English. Translating the
UI means adding a message catalogue and a language choice; it has not been
done, and nothing in this work pretends otherwise.

---

## How to re-check

```sh
.work/i18n/run.sh            # everything below
.work/i18n/run.sh grid       # terminal cell grid, real put_char
.work/i18n/run.sh widths     # the width table, per codepoint
.work/i18n/run.sh fit        # 2198 truncation probes, real helpers
.work/i18n/run.sh fallback   # face list with/without CJK, render to PPM
.work/i18n/run.sh focus      # Files focus ring, measured in pixels
.work/i18n/run.sh contrast   # WCAG numbers from the shipped theme files
```

The harnesses compile the shipped sources (`src/font.c` in full; the grid and
truncation helpers are lifted verbatim from `src/term.c`, `src/apps/files.c`
and `src/client/apps/viewer.c` between their `@...` markers) and print the
sha256 of each lifted block, so what is tested is the text that ships. No
executable is left in the tree.
