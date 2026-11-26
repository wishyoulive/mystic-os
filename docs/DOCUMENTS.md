# Documents: viewing, creating and editing

`src/client/apps/docview.c` is the Documents window (`apps/docview`, title
"Document"). Until now it could only *show* a file that was handed to it, and
with no argument it searched four directories for something to display - which
meant there was no way to start a new document at all. That is fixed: the
window now opens an empty, editable buffer, and plain text and Markdown can be
typed into and saved.

Status at the time of writing:

| Piece | State |
|---|---|
| No file argument opens an empty editable "untitled" buffer | **yes**, `src/client/apps/docview.c` `main()` (the directory search is gone) |
| Typing, Enter, Backspace, Delete, arrows, Home/End, soft Tab | **yes**, code path complete; proven on the host by `tools/tests/docedit.c` |
| Ctrl+S saves under `/root/Documents`, never overwriting a new document | **yes**, `dv_save()` |
| The saved path is shown in the footer and logged to stderr | **yes**, `dv_draw_footer()`, `fprintf(stderr, "docview: saved %s\n", ...)` |
| `.docx`/`.xlsx`/`.pptx`/`.odt`/`.ods`/`.odp` stay read-only, footer says so | **yes**, unchanged reader path |
| The editing core asserted on exact resulting text | **yes**, 115 checks, 0 failures (section 7) |
| The *interactive* path in a running machine (real keys, real pixels, real `rename`) | **not run here** - there is no Linux userland on this host and QEMU is out of scope. Section 8 lists exactly what that leaves unproven |

---

## 1. What you can create and edit

Open the Documents tile (or run `docview`) with **no file argument**: you get a
window whose title is `untitled`, an empty buffer, and a caret at the top left.
Everything you type goes into that buffer. `Ctrl+S` writes it out.

Editable:

* `.txt`, `.text`, `.log`, `.md`, `.markdown` - and in fact **any file whose
  bytes decode as UTF-8 text** with no NUL in it. The check is
  `dv_is_text()`, not a list of extensions, so a note called `todo` with no
  suffix still edits.
* A Markdown file gets its `#`/`##`/`###` headings in the heading face,
  `- `/`* `/`+ ` lines as bullets, and `>` lines dimmed, while you edit the
  source. The markers stay on screen on purpose: this is a source editor, not
  a preview. A leading `# Title` line also supplies the window title.

Not editable, by decision:

| Format | Why |
|---|---|
| `.docx`, `.xlsx`, `.pptx` | OOXML is a ZIP of XML parts. Editing means parsing `word/document.xml`, applying the change to the XML, re-zipping and rebuilding the central directory. This reader has a ZIP *reader*, not a writer, and pretending otherwise would corrupt files. |
| `.odt`, `.ods`, `.odp` | Same shape: `content.xml` inside a ZIP. |
| `.doc`, `.xls`, `.ppt` | Legacy OLE2 binaries: detected and named, never decoded (unchanged). |
| `.rtf` | Detected, not parsed (unchanged). |
| Anything that is not UTF-8 text | Refused by name, with the reason in the window, rather than shown as mojibake. |

For every one of those the footer says **`read-only`** in the error colour and
the header repeats it next to the format name.

### How a file reaches this window

* The Documents tile runs the bundle with **no argument** (`wms_launch()` in
  `src/wmsrv.c` calls `execl(exe, "app", NULL)`), which is the new untitled
  path.
* `docview /root/notes/todo.txt`, or `MYSTIC_DOC=/root/notes/todo.txt`, opens
  that file - editable if it is text, read-only if it is an Office container.
* The Files app's "Open" for `.txt`/`.md` currently maps to the `viewer`
  bundle (`src/filetypes.c`, `FT_APP_VIEWER`), not to Documents, so a text file
  opened from Files lands in Viewer. Re-pointing that mapping (or adding an
  "Open with Documents" entry) is a change in `src/filetypes.c`, which is
  outside this one.
* `stage/usr/share/mystic/apps/docview/bin` is a build artefact. It has to be
  rebuilt (`./tools/mkapp.sh docview`) for a fresh image to contain this code;
  the source alone does nothing until the bundle is repacked.

---

## 2. Keys

Editing (`EDIT` in the top right):

| Key | Action |
|---|---|
| any printable character | insert at the caret (Shift gives the shifted character) |
| `Enter` | split the line at the caret |
| `Backspace` | delete the character before the caret; at the start of a line, join it to the line above |
| `Delete` | delete the character after the caret; on a newline, join the line below up |
| `Left` / `Right` | move one UTF-8 character (never into the middle of one) |
| `Up` / `Down` | move a line, keeping a goal column through short lines |
| `Home` / `End` | start / end of the current line |
| `Tab` | insert spaces up to the next 4-character stop (see section 6) |
| `Ctrl+S` | save |
| `Esc` | close; if there are unsaved changes it asks first (a second `Esc` discards them) |
| wheel / drag | scroll, never move the caret |

Reading (unchanged): `j`/`k`, `Space`/`b`, `g`/`G`, arrows, `PageUp`/`PageDown`,
`r` for raw text, `i` for the information panel, `q` to quit the window.

In an editable window `q`, `j`, `r` and `i` are **text**, not commands - the
whole point is that typing goes into the document. Quitting is `Esc`.

**There is no selection yet.** Shift+arrows are not implemented at all (no
anchor, no highlight, no clipboard). The header says `no shift-selection yet`
rather than leaving it to be discovered. Selection is the obvious next step;
it needs the anchor-plus-caret model `src/term.c` already uses.

---

## 3. Where files are saved

New documents go to `/root/Documents`. The directory is created (mode 0755) on
the first save if it is missing (`dv_ensure_dir`).

The name is chosen by `dv_choose_save_name()`: `untitled.txt`, then
`untitled-2.txt`, `untitled-3.txt`, ... stopping at 9999. The chooser probes
the filesystem with `stat()` and takes the first name that does not exist, so
**a save can never overwrite an earlier document**. A Markdown document gets
`.md` instead of `.txt`.

The write itself is: `fopen("<path>.tmp", "wb")`, `fwrite`, `fclose`,
`rename(tmp, path)`. The rename means a full disk or a killed process leaves
the previous version of the file intact instead of a half-written one.

A file that was *opened* saves back to where it came from. `Ctrl+S` on
`/root/notes/todo.txt` writes `/root/notes/todo.txt` - that is what Save means,
and refusing to would make editing an existing file pointless. The footer shows
the exact target before you press it, so the difference between "untitled.txt,
chosen for me" and "the file I opened" is never hidden.

After a successful save:

* the footer shows `saved /root/Documents/untitled.txt (37 bytes)` and, from
  then on, keeps showing the path on the right (clipped from the left, because
  the tail of a path is the informative part);
* `docview: saved /root/Documents/untitled.txt` goes to stderr;
* the ` *` next to the title disappears (it is the unsaved-changes marker);
* an untitled window takes the basename as its title.

A failed save (no permission, no space, no free name) shows the real
`strerror(errno)` in the footer and changes nothing on disk.

---

## 4. How the editor is built

One buffer, not two. An editable window holds the text in a `DvText`
(`src/client/apps/docview.c`, "THE EDITING CORE"): a contiguous `char *` with
`int n` bytes and a NUL at `p[n]`, lines separated by `'\n'`, and a caret that
is a byte offset into it. `doc.text` stays NULL in this mode, so the text exists
exactly once and the reader's arena is untouched. Office files keep using the
`office.c` arena and are never edited.

Editing functions are pure: `dv_edit_insert`, `dv_edit_split`,
`dv_edit_merge`, `dv_edit_backspace`, `dv_edit_delete`, the caret movers, the
Markdown line classifier and the save-name chooser take a buffer and an offset
and touch nothing else. That is what makes them testable on the host, and it is
what `tools/tests/docedit.c` compiles when `DOCEDIT_TEST` is defined - the same
translation unit, not a copy of the logic.

**Re-layout policy: the whole buffer on every edit.** Patching only the changed
region means recomputing the wrap of one physical line and then shifting the
`y` of every later line - the same order of work as a full pass, plus the
chance of leaving a stale line behind when a word moves from one visual line to
the next. A full pass cannot disagree with the buffer, so that is what runs
(`dv_layout_edit`, called once per frame in which something was typed, not once
per character). To keep that affordable the editable size is capped at
**256 KiB** (`DV_TEXT_MAX`); a bigger file is refused with the cap in the
message instead of quietly becoming slow.

Layout reuses the reader's machinery: the same `DvLine` array, the same
`scroll`/`dv_clamp_scroll`, the same body band, footer percentage and scrollbar.
Only the wrap differs - `dv_fit_span` keeps every byte of a line (leading and
repeated spaces are the user's text, not layout to be swallowed) and prefers to
break after a space, whereas the reader's paragraph wrapper collapses leading
spaces and works on paragraphs. The caret is then a bar at
`line->x + width(line start .. caret)`, measured with the same face the line was
drawn with, and `dv_caret_reveal` keeps the caret's line inside the body while
you type.

---

## 5. What the Documents window draws

* **Editable:** header `title *` (the star means unsaved), subtitle
  `Plain text · 12 lines · editing · no shift-selection yet` (or `Markdown ·
  ...`), `EDIT` top right in the accent colour; body = the source text with the
  caret; footer = the key hint (or the last save message), the save path, and
  the scroll percentage. `i` is not available in an editable window because `i`
  has to be a letter you can type.
* **Read-only:** header `FORMAT · author · read-only`, `READ-ONLY` top right in
  the error colour; footer begins with `read-only`; `i` opens a panel that now
  also states `Editing: no - <format> is shown read-only here`.
* **No file at all** (open failed): the message says what happened, that `.txt`
  and `.md` are editable up to 256 KiB, and that a window with no file argument
  opens an empty document.

---

## 6. Two honest limits

* **Tab inserts spaces.** `font_width()`/`font_draw()` ask the font for the
  advance of U+0009, and these faces have no tab glyph, so a literal tab draws
  with zero width and the caret would sit on top of the previous character.
  Typing Tab therefore inserts 1-4 spaces to the next 4-character stop, which
  is visible and behaves the same on screen as what gets saved. A tab that was
  already in a file keeps drawing with no advance - the caret still lands where
  the (invisible) character is, because both measure with the same call.
* **No mouse caret placement.** Clicking and dragging still scrolls the page,
  as it did for reading; the caret moves by keyboard only. Click-to-place needs
  a hit test from a pixel back to a byte through `font_width`, which is a
  separate, testable piece of work.
* **Up/Down keep a byte column, not a pixel column.** The goal column is
  counted in bytes from the start of the line, so on a line that mixes wide
  and narrow characters the caret can land a few pixels off the column it left.
  On ASCII text (the common case) the two are identical. Making it pixels means
  measuring every line's prefix on every move; it is not done yet.

---

## 7. The host test

`tools/tests/docedit.c` includes `docview.c` with `DOCEDIT_TEST` and asserts the
exact text, caret and return value of the editing functions: 115 checks over
insert at start / middle / end, insert past the end, backspace at start and at
end, a UTF-8-aware backspace, Delete at end / middle / on a newline, Enter at
start / middle / end of a line, merge accepted and refused, caret up/down with a
goal column, Home/End, UTF-8 caret steps, soft-Tab widths, twelve Markdown line
styles, the Markdown title, the save-name chooser against a fake "already
exists" set, the 256 KiB cap, erase clamping, reuse after free, a short typing
session (type, Enter, type) asserted as one resulting buffer, and the wrap
arithmetic itself - `dv_fit_span` is given a fake 10 px font and asked for the
exact break offset of eight lines (fits, breaks at a space, space that does not
fit, word wider than the body, one glyph wider than the body, a UTF-8 character
that must not be split, an empty line, a line ending on a space).

On a POSIX host:

    mkdir -p build
    zig cc -target x86_64-linux-musl -Isrc -Isrc/client -Wall -Wextra \
        -o build/docedit tools/tests/docedit.c && ./build/docedit

On this Windows host the same source is built natively and run, because a
musl-Linux binary cannot execute here:

    mkdir -p build
    zig cc -Wall -Wextra -o build/docedit.exe tools/tests/docedit.c && ./build/docedit.exe

Result on this machine:

    115 checks, 0 failures

That run found one real bug before it shipped: `dv_edit_insert` clamped the
insert position into the buffer but then set the caret from the unclamped
value, so typing with a caret beyond the end left the caret there. It is fixed
in the core, and the test asserts the clamped caret.

The app itself was compiled with the assignment's command and linked with the
bundle list from `tools/mkapp.sh`:

    zig cc -target x86_64-linux-musl -Os -static -Isrc -Isrc/client \
        -Wall -Wextra -Wno-unused-parameter -c src/client/apps/docview.c -o build/docview.o
    zig cc -target x86_64-linux-musl -Os -static -Isrc -Isrc/client \
        -o build/docview src/client/apps/docview.c \
        src/client/mystic_client.c src/gfx.c src/font.c src/ui.c \
        src/filetypes.c src/audio.c src/img.c -lm

Both silent: no warnings, no output.

---

## 8. What still needs a boot to confirm

These cannot be proven on this host, and are **not** claimed as verified:

1. **That the keys arrive as assumed.** The code trusts `src/main.c`'s contract:
   modifier bits `shift=1, ctrl=2, alt=4` on `MC_KEY`, Enter as `'\n'` and
   Backspace as `'\b'` in `MC_TEXT`, `Ctrl+S` as key 31 with bit 2 (and also as
   byte 0x13, which is handled a second time so either encoding saves). Every
   one of those is read out of `src/main.c`/`src/input.c`, not observed in a
   running VM.
2. **That `Ctrl+S` really writes the file in the guest.** `mkdir("/root/Documents")`,
   the temporary file and `rename()` are ordinary POSIX calls against the
   guest's filesystem; the host test cannot exercise them.
3. **That the caret and the wrapped lines land on the pixels described.** Font
   metrics, `dv_fit_span` and `dv_draw_caret` need the real font files and a
   surface; there is no way to run them here (no Linux userland, no QEMU).
4. **That the window is usable at the app's 780x620 size** - in particular that
   the footer's path plus percentage do not collide. They are clipped to their
   own bands, but "clipped" is not "pretty".
5. **That the launcher tile with no argument produces the untitled window.** The
   manifest (`stage/usr/share/mystic/apps/docview/manifest`, `exec = bin`) starts
   the app with no argv, which is exactly the new path in `main()`, but the
   launcher itself was not run here.

The next boot should therefore check, in this order: open the tile, type a few
lines, press Enter in the middle of one, `Ctrl+S`, then `ls /root/Documents` and
`cat` the file; then do it again and confirm the second save is
`untitled-2.txt`; then open a `.docx` and confirm the footer still says
`read-only`; then type after a save and press `Esc` once (it must warn, not
close) and twice (it must close).
