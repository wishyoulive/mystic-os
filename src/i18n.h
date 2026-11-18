/* ============================================================
 *  Mystic OS - i18n.h
 *
 *  The handful of Unicode facts that more than one module has to
 *  agree on. All of them are implemented in src/font.c, next to the
 *  face list they are answered from, and all of them are measurements
 *  of real data: the EastAsianWidth and combining tables in font.c,
 *  or stb_truetype's own cmap lookup for the installed faces.
 *
 *  These live in their own header for one reason: src/mystic.h is the
 *  desktop's contract and is edited by whoever owns the display work,
 *  while these calls are needed by the terminal (src/term.c) and by two
 *  applications (src/apps/files.c, src/client/apps/viewer.c) that are
 *  separate binaries. Nothing here depends on the desktop.
 *
 *  docs/ACCESSIBILITY.md says what is covered and what is not.
 * ============================================================ */
#ifndef MYSTIC_I18N_H
#define MYSTIC_I18N_H

#include <stddef.h>
#include "mystic.h"      /* u32 */

/* How many terminal cells the codepoint occupies: 2 for the East Asian
 * Wide/Fullwidth ranges and the emoji blocks, 0 for a combining mark or a
 * zero-width control, 1 for everything else. This is a property of the
 * character, never of the face that draws it. */
int font_cell_width(u32 cp);

/* One UTF-8 sequence out of s[0..len). Returns the codepoint, or -1 for bytes
 * that are not a valid sequence (overlong, surrogate, above U+10FFFF, or cut
 * short); *bytes is at least 1 either way, so a walk always advances. */
int font_utf8_next(const char *s, size_t len, int *bytes);

/* The largest k <= n for which s[0..k) holds whole codepoints only: the
 * longest prefix of s that may be kept when a string has to be shortened to
 * at most n bytes without splitting a character. */
size_t font_utf8_boundary(const char *s, size_t n);

/* The start of the codepoint that ends at byte n (0 < n <= strlen(s)): one
 * step back through a string, whole characters at a time. */
size_t font_utf8_prev(const char *s, size_t n);

/* How many codepoints of `text` the role's own face cannot draw and another
 * installed face supplies (fallback), and how many no installed face has at
 * all (missing). Counted with the same cmap lookup glyph_get() uses. */
void font_coverage(Font *f, const char *text, int *fallback, int *missing);

#endif
