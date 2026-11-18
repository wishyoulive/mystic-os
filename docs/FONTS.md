# Fonts

How Mystic OS finds, chooses and draws fonts. Everything below is what
`src/font.c` actually does; where this text and the source disagree, the source
is right.

The desktop and every external app link the same `font.c`, so all of them use
the same loader, the same face list and the same role mapping. The only
difference is when they read `/etc/mystic/fonts.conf` (see §5).

---

## 1. Where fonts come from

Nothing is hard-coded to a file name any more. `font_init(dir)` scans three
directories for `*.ttf` and `*.otf` (extension matched case-insensitively):

| order | directory | purpose |
|---|---|---|
| 1 | `/root/fonts` | fonts the user installed; highest priority |
| 2 | the `dir` argument to `font_init()` | `main.c` passes `/usr/share/fonts` |
| 3 | `/usr/share/fonts` | fonts that ship in the image |

If the argument is `/root/fonts` or `/usr/share/fonts` it is not scanned twice.
A missing directory is normal and silent; a directory that exists but cannot be
opened is logged.

Every candidate is read whole, bounds-checked and parsed at startup. The face
list is then ordered user-fonts-first, so:

* A file in `/root/fonts` with the same family, weight and slant as one in
  `/usr/share/fonts` **wins**; the system copy is skipped with
  `font: shadowed <new path> (same family/weight as <kept path>)`.
* Different weights of one family are all kept — `Lato-Regular`,
  `Lato-Bold`, `Lato-Black` are three faces of the family `Lato`, and the
  regular/medium/semibold roles pick between them (§2).
* `readdir` order is sorted before loading, so the face list and every
  fallback choice are repeatable.

Within a directory the first 256 font files are considered, the face table
holds at most 128 faces, and a single file may be at most 64 MiB. Anything
past those limits is skipped and logged.

### What gets skipped

A file is rejected — never loaded, never able to crash the desktop — if it
fails any of these checks. The exact reason goes to stderr with the path:

```
font: skip /root/fonts/Broken.ttf: table 'GPOS' (210546 bytes at 436696)
      runs past the 300-byte file
```

* empty file, unreadable file, or a file over the 64 MiB limit;
* not an sfnt (`0x00010000`, `true`, `OTTO`) and not a TrueType collection;
* the sfnt table directory, or any table's declared offset/length, falls
  outside the file;
* a required table (`cmap`, `head`, `hhea`, `hmtx`, `maxp`) is missing or too
  short;
* `maxp` says zero glyphs, `hhea`'s long-metric count is zero or larger than
  the glyph count, or `hmtx` is too short for that count;
* the `name` table's records or string area are outside the table;
* a `cmap` subtable's offset or declared length is outside the `cmap` table;
* a TrueType `loca` table is too short, a `loca` entry points outside `glyf`,
  or **any glyph's** contour/instruction/flag/coordinate/component data runs
  past the end of that glyph's own outline;
* a CFF font's header size or its name/top-dict/string/global-subr INDEX does
  not fit inside the CFF table.

The glyph-outline check matters: stb_truetype is given no length and trusts
offsets inside the file. Validating every offset it can follow before handing
it the buffer is what makes a truncated file a logged skip instead of a read
past the end. It also means a file whose directory is intact but whose glyph
data is cut short is rejected at load, not at draw time.

`stbtt_InitFont()` is still the final authority: if it rejects the tables, the
file is skipped with `stb_truetype rejected the font tables`. Nothing about a
bad file is fatal; the loader keeps going and the roles fall back to whatever
did load. If *nothing* loads, `font_init()` returns `-1` and `font_get()`
returns `NULL`; `font_draw()` draws nothing and every layout still runs.

---

## 2. The four roles

The desktop draws with four roles (`FONT_REGULAR`, `FONT_MEDIUM`,
`FONT_SEMIBOLD`, `FONT_MONO` in `mystic.h`). A role is not a file; it is a
choice made at startup from the faces that parsed.

**UI family** — regular, medium and semibold all come from one family:

| role | wanted weight | how it is chosen |
|---|---|---|
| `FONT_REGULAR` | 400 | nearest `OS/2 usWeightClass` in the family |
| `FONT_MEDIUM` | 500 | nearest weight to 500 in the same family |
| `FONT_SEMIBOLD` | 600 | nearest weight to 600 in the same family |

With Lato installed that means Regular (400) → Regular/Bold → Bold (700).
With a single-weight family all three roles are the same face, which is
correct: the roles exist so an installed family can express weight, not so the
loader invents it. Weight comes from the real `OS/2` table; if that is missing
it comes from the style name in the `name` table (`Bold`, `SemiBold`,
`Black`, …) and finally defaults to 400.

**Mono family** — `FONT_MONO` wants a genuinely monospaced face. A face counts
as monospaced when the real `hmtx` advances of `i`, `W`, `0` and `m` are all
equal; `post.isFixedPitch` is the fallback for a font without those glyphs.
The picker prefers the shipped `JetBrainsMono-Regular.ttf`, then any
monospaced face, then any face at all. If no monospaced face is installed the
terminal falls back to the UI family and the log says so:

```
font: no monospaced face installed; the terminal falls back to Lato
```

The terminal then draws proportional glyphs in fixed cells — readable, but
columns will not line up. Install a monospaced font to fix it.

### Italic

**No italic is ever synthesised.** There is no slant, shear or faux-oblique
anywhere in `font.c`. Italic faces are kept in the face list (they are real
fonts and someone may install a family that only has an italic), but every
role choice adds a large penalty to an italic face, so an upright face of the
same family always wins. A family that only ships an italic is drawn as that
italic, unmodified. Bold is *not* synthesised either: `FONT_SEMIBOLD` maps to
the nearest installed weight, and a family with only a regular face uses it.

### Per-codepoint fallback

There is none. If the chosen face has no glyph for a codepoint, the font's own
`.notdef` box is drawn (the same behaviour `stbtt_FindGlyphIndex()` returns for
a missing glyph). A missing glyph never crashes and never switches fonts.

---

## 3. `/etc/mystic/fonts.conf`

Optional. Without it, the defaults in §2 apply: Lato for UI and JetBrains Mono
for the terminal when they are installed.

The file is plain text, one directive per line:

```
# /etc/mystic/fonts.conf
# role = family

ui   = Lato
mono = JetBrains Mono
```

Rules, exactly as implemented:

* The only role names are `ui` and `mono` (case-insensitive).
  `ui` drives `FONT_REGULAR`, `FONT_MEDIUM` and `FONT_SEMIBOLD`; `mono` drives
  `FONT_MONO`.
* Lines whose first non-space character is `#` or `;` are comments. Comments
  are whole-line only — a `#` inside a family name is part of the name.
* Leading/trailing whitespace around the key and the value is ignored.
* The value may be wrapped in matching single or double quotes.
* The value is matched, case-insensitively, against, in order:
  1. the family name from the font's `name` table (`Lato`, `JetBrains Mono`);
  2. the file name (`Lato-Regular.ttf`);
  3. the file name without its extension (`Lato-Regular`).
  The family name is what most fonts use and what `font_family_name()` returns.
* If a family matches, the face nearest the role's wanted weight is used. If
  the value matches nothing, the loader logs the reason and falls back to the
  default family; it never refuses to start:

  ```
  font: /etc/mystic/fonts.conf: no installed face matches ui family "Inter"; using the default
  ```
* An unknown role is logged with its line number and ignored:
  `font: /etc/mystic/fonts.conf:4: unknown role "sans" (use ui or mono)`.
* `mono = <proportional family>` is honoured but warned about:
  `font: fonts.conf: mono family "Lato" is not monospaced; the terminal will use it anyway`.
* The file is read by `font_init()` / `font_reload()`. Changing it on disk has
  no effect until one of those runs.

The loader logs the result once per init so the running configuration is
visible in the boot log:

```
font: 4 faces: ui=Lato mono=JetBrains Mono
```

No font is installed by this loader. `/root/fonts` is the user's directory;
the image ships its fonts in `/usr/share/fonts` (`tools/build-desktop.sh`
copies `assets/fonts/*.ttf` there).

---

## 4. Glyph cache and LRU

Each `Font` object is one `(role, size)` pair. `font_get()` looks up the pair
and applies the global text scale first:

```
requested size × ui_scale_get(), snapped to 0.5 px, clamped to 5..96 px
```

so the cache key is effectively `(role, scaled size)` — two sizes of the same
face are two `Font` objects, each with its own 2048-entry glyph table. The same
pair always returns the same object until the role is remapped or
`font_reload()`/`font_shutdown()` frees it.

Each glyph table is open-addressed but never stops a probe at an empty slot
(eviction can free a slot in the middle of a probe run), and every hit bumps a
monotonic `use_clock` stamp. When all 2048 slots are used, the entry with the
smallest stamp is freed — **including its bitmap** — and reused. The stamp
halves every entry when the counter approaches `UINT_MAX`, so ordering
survives a four-billion-access wrap.

Consequences worth knowing:

* The first 2048 distinct codepoints a size draws stay; the 2049th evicts the
  least recently used one. There is no "invisible glyph after the table fills"
  failure mode.
* Bitmaps are `malloc`ed per glyph and freed on eviction, on `font_reload()`
  and on `font_shutdown()`. The host harness proves no orphan bitmap is left
  behind across 6000 codepoints (§7c).
* Face file bytes are loaded **once per file** and shared by every size and
  role that points at that face. Eight sizes of Lato keep one copy of Lato.
* `font_shutdown()` frees every face byte and every cache entry; the harness
  checks that the allocation count returns to zero.

---

## 5. Switching a font at runtime (for `src/apps/settings.c`)

`src/mystic.h` is owned by the desktop and does not declare these, so a
settings screen declares them itself. They are real functions in `src/font.c`:

```c
/* add this block to src/apps/settings.c */
extern int  font_family_count(void);
extern const char *font_family_name(int index);
extern int  font_face_count(void);
extern int  font_role_family(int role, char *out, size_t cap);
extern int  font_set_ui_family(const char *family);
extern int  font_set_mono_family(const char *family);
extern int  font_set_role_family(int role, const char *family);
extern int  font_save_config(const char *path);
extern int  font_reload(void);
extern void font_shutdown(void);
```

What each one does:

| call | result |
|---|---|
| `font_family_count()` | number of distinct family names currently loaded |
| `font_family_name(i)` | family name for the picker, `NULL` past the end |
| `font_face_count()` | number of font files loaded (weights included) |
| `font_role_family(role, buf, cap)` | family the role draws with, `-1` if none |
| `font_set_ui_family("Inter")` | remaps `REGULAR`/`MEDIUM`/`SEMIBOLD` immediately; `0` on success, `-1` and a log line if no loaded face matches |
| `font_set_mono_family("JetBrains Mono")` | remaps `FONT_MONO` immediately; warns if the face is proportional |
| `font_set_role_family(role, "Inter")` | `MONO` → `font_set_mono_family`, the other three → `font_set_ui_family` |
| `font_save_config("/etc/mystic/fonts.conf")` | writes the current UI/mono mapping back as the file in §3 |
| `font_reload()` | rescans the directories and re-reads `fonts.conf`; returns `0`/`-1` exactly like `font_init()` |
| `font_shutdown()` | frees all faces and caches (only for a process that is finished) |

### The two ways a settings screen can apply a change

**A. Immediate switch, then persist** (no restart, no file round-trip):

```c
if (font_set_ui_family(chosen_family) == 0) {   /* desktop text changes next frame */
    font_save_config("/etc/mystic/fonts.conf"); /* remembered across reboots   */
}
```

The three UI roles and the mono role are independent; call
`font_set_mono_family()` as well if the screen has a terminal-font row.

**B. Write the file, then reload** (if the screen already has a "write config"
path, the way it writes `/etc/mystic/ui.conf`):

```c
/* write "ui = Inter\nmono = JetBrains Mono\n" to /etc/mystic/fonts.conf */
font_reload();     /* re-reads the file and rescans the directories */
```

`font_reload()` is the only call that frees live `Font` objects, so it must be
made from the click handler, never in the middle of a draw. The `font_set_*`
calls do **not** free anything: the existing `(role, size)` cache entries are
re-pointed at the new face on their next `font_get()`, so a `Font *` that some
layout is still holding stays valid and simply draws the new face.

External app processes (the ones speaking to `wmsrv.c`) read
`/etc/mystic/fonts.conf` when they start, because they link their own copy of
`font.c`. They keep their fonts until they are restarted; changing the desktop
font does not reach into an already-running app.

`font_set_ui_family()` takes a family name, file name or file stem, exactly
like the config file. An unknown name is refused and logged; the current
mapping is left untouched.

---

## 6. What a caller sees

Unchanged from before:

* `font_get(role, nominal_size)` returns a `Font *` or `NULL`. The global text
  scale is applied inside this call, so no layout can forget it.
* `font_height(f)` is `size * 1.4`; `font_ascent(f)` is the real ascender in
  pixels; `font_draw(s, f, x, y, utf8, colour)` treats `y` as the ascender
  line and clips to the `Surface`; `font_width(f, utf8)` is the real advance
  sum.
* UTF-8 is decoded by `utf8_next()`, which tolerates truncated sequences by
  consuming what it can.

---

## 7. Verification

A host harness lives in `.work/fonts/`. It `#include`s the real `src/font.c`,
uses the real `stb_truetype.h`, and routes every allocation in the loader
through a counting wrapper, so "no leak" is an exact count rather than a guess
at RSS. It is not part of the image.

```
cd .work/fonts && bash run.sh
```

The harness was run on 2026-11-17 against the shipped Lato/JetBrains Mono
files; `RESULTS.txt` is its output. Required proofs:

**(a) one valid TTF + one truncated file → one face, one logged skip.**
A directory holding a copy of `Lato-Regular.ttf` and a 300-byte truncation of
it yields `face_count == 1`, the face is the valid file, `font_get()` draws
text, and stderr contains exactly one line:

```
font: skip .../Broken-Truncated.ttf: table 'GPOS' (210546 bytes at 436696)
      runs past the 300-byte file
```

**(b) two sizes of one face → two cache entries.**
`font_get(FONT_REGULAR, 13)` and `font_get(FONT_REGULAR, 18)` are different
objects, both `source` the same face file, both own their `M` bitmap, and the
18 px bitmap is wider. A repeated request for 13 px returns the cached object.

**(c) 3000 distinct codepoints → LRU eviction, no leak.**
After 3000 codepoints: 2048/2048 slots used, 2048 live bitmaps, and
`live_blocks == faces + 1 Font + live bitmaps` (2053 blocks) — one leaked
bitmap per eviction would break that equality. The oldest codepoint is gone,
the newest is present, and the table is still exactly full after 6000. Then
`font_shutdown()` returns both the block and byte counters to zero. The same
check runs for every fixture, including the broken ones.

Extra checks in the same run:

* 17 truncation points from 0 bytes to one byte short of the whole file: all
  rejected with a reason, no crash;
* 200 randomly mutated Lato files: 141 still parsed and drew 1024 codepoints
  each, 59 were rejected, zero crashes;
* empty file, a text file named `.otf`, and a directory named `.ttf`: all
  skipped and logged, the one valid font in the directory still loads;
* `/root/fonts` copy shadows the `/usr/share/fonts` copy (and is logged);
* `ui = JetBrains Mono` / `mono = Lato` in `/etc/mystic/fonts.conf` select
  those families, medium/semibold follow UI, unknown roles are logged;
* `font_set_ui_family()` re-points the live cache entry without freeing the
  object, `font_save_config()` round-trips the mapping, `font_reload()`
  releases old caches and reloads.

Independent compile check for this file, from the repository root:

```
zig cc -target x86_64-linux-musl -Os -static -Isrc -Wall -Wextra \
       -Wno-unused-parameter -c src/font.c -o /tmp/font.o
```

It is silent. The desktop and external apps still link against the unchanged
`mystic.h` API: the only additions are the extern functions in §5, which no
existing caller uses.

---

## 8. Limits, deliberately

* One face per family/weight/slant after shadowing; there is no style picker
  beyond the four roles.
* TrueType collections (`.ttc`) are not scanned; a collection renamed to
  `.ttf` loads its first face only.
* CFF (PostScript-flavoured `.otf`) is supported through stb_truetype. The
  loader checks the CFF header and leading INDEX chain fit the table; the
  per-glyph charstring programs are interpreted by stb_truetype. TrueType
  outlines get the stricter per-glyph bounds check described in §1.
* No synthetic bold or italic, no per-codepoint fallback, no colour or
  variable-font axes.
* Loading happens once at process start (or on `font_reload()`); new files in
  `/root/fonts` are picked up by a reload or a restart, not by watching the
  directory.
