/* ============================================================
 *  docedit.c - host test for the Documents app's editing core
 *
 *  src/client/apps/docview.c compiles its editing core on its own
 *  when DOCEDIT_TEST is defined (no window server, no fonts, no
 *  filesystem), so this file includes it and drives exactly the
 *  functions the app runs when you type:
 *
 *      dv_text_set / dv_text_insert / dv_text_erase / dv_text_lines
 *      dv_edit_insert (typing), dv_edit_split (Enter),
 *      dv_edit_merge (join lines), dv_edit_backspace, dv_edit_delete
 *      dv_caret_left/right/up/down/home/end
 *      dv_md_line_style / dv_md_title / dv_tab_spaces
 *      dv_choose_save_name (with a fake "does it exist" probe)
 *
 *  Build and run:
 *
 *      zig cc -target x86_64-linux-musl -Isrc -Isrc/client -Wall -Wextra \
 *          -o build/docedit tools/tests/docedit.c && ./build/docedit
 *
 *  Prints "N checks, M failures" and exits non-zero when anything fails.
 *  What this cannot cover is stated in docs/DOCUMENTS.md: the interactive
 *  path (real keys, real pixels, the window server) needs a boot.
 * ============================================================ */
#define DOCEDIT_TEST 1
#include "../../src/client/apps/docview.c"

static int checks;
static int fails;

static void ck_str(const char *what, const DvText *t, const char *want)
{
    checks++;
    const char *got = t->p ? t->p : "";
    if (t->n != (int)strlen(want) || strcmp(got, want) != 0) {
        fails++;
        printf("FAIL %-44s got \"%s\" (%d bytes)  want \"%s\"\n",
               what, got, t->n, want);
    } else {
        printf("ok   %-44s \"%s\"\n", what, got);
    }
}

static void ck_cstr(const char *what, const char *got, const char *want)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        fails++;
        printf("FAIL %-44s got \"%s\"  want \"%s\"\n",
               what, got ? got : "(null)", want);
    } else {
        printf("ok   %-44s \"%s\"\n", what, got);
    }
}

static void ck_int(const char *what, int got, int want)
{
    checks++;
    if (got != want) {
        fails++;
        printf("FAIL %-44s got %d  want %d\n", what, got, want);
    } else {
        printf("ok   %-44s %d\n", what, got);
    }
}

/* Load a fresh buffer. The struct starts zeroed, as the app's calloc does. */
static void load(DvText *t, const char *s)
{
    dv_text_free(t);
    if (dv_text_set(t, s, (int)strlen(s)) != 0) {
        printf("FAIL could not allocate the test buffer\n");
        fails++;
        checks++;
    }
}

/* ---- the fake filesystem the name chooser probes ---- */

typedef struct {
    const char *name[8];
    int n;
} Taken;

static int probe_taken(void *ud, const char *path)
{
    Taken *k = (Taken *)ud;
    for (int i = 0; i < k->n; i++)
        if (strcmp(k->name[i], path) == 0) return 1;
    return 0;
}

/* ---- a fake font: every UTF-8 character is 10 px wide ---- */

static float probe_width(void *ud, char *s, int off, int len)
{
    (void)ud;
    (void)s;
    (void)off;
    (void)len;
    return 10.0f;                      /* one character, one advance */
}

int main(void)
{
    DvText t;
    memset(&t, 0, sizeof(t));
    int caret;

    /* ---- opening and replacing text ---- */
    dv_text_set(&t, "", 0);
    ck_str("empty buffer", &t, "");
    ck_int("empty buffer length", t.n, 0);
    ck_int("empty buffer has one line", dv_text_lines(&t), 1);

    load(&t, "hello\nworld");
    ck_str("text_set replaces the contents", &t, "hello\nworld");
    ck_int("two lines", dv_text_lines(&t), 2);

    load(&t, "a\n");
    ck_int("a trailing newline starts an empty line", dv_text_lines(&t), 2);

    /* ---- typing ---- */
    load(&t, "");
    caret = 0;
    ck_int("type into empty: changed", dv_edit_insert(&t, &caret, "hello", 5), 1);
    ck_str("type into empty", &t, "hello");
    ck_int("caret after typing", caret, 5);

    load(&t, "world");
    caret = 0;
    dv_edit_insert(&t, &caret, "hello ", 6);
    ck_str("insert at start", &t, "hello world");
    ck_int("caret after insert at start", caret, 6);

    load(&t, "helo");
    caret = 3;
    dv_edit_insert(&t, &caret, "l", 1);
    ck_str("insert in the middle", &t, "hello");
    ck_int("caret after insert in the middle", caret, 4);

    load(&t, "abc");
    caret = 3;
    dv_edit_insert(&t, &caret, "def", 3);
    ck_str("insert at end", &t, "abcdef");
    ck_int("caret after insert at end", caret, 6);

    load(&t, "abc");
    caret = 99;
    dv_edit_insert(&t, &caret, "!", 1);
    ck_str("insert past the end clamps", &t, "abc!");
    ck_int("caret clamped into the buffer", caret, 4);

    /* ---- Backspace ---- */
    load(&t, "abc");
    caret = 3;
    ck_int("backspace at end: changed", dv_edit_backspace(&t, &caret), 1);
    ck_str("backspace at end", &t, "ab");
    ck_int("caret after backspace", caret, 2);

    load(&t, "abc");
    caret = 0;
    ck_int("backspace at start is a no-op", dv_edit_backspace(&t, &caret), 0);
    ck_str("backspace at start leaves the text", &t, "abc");
    ck_int("backspace at start leaves the caret", caret, 0);

    load(&t, "caf\xC3\xA9");                 /* "cafe" with an acute accent */
    caret = t.n;
    dv_edit_backspace(&t, &caret);
    ck_str("backspace removes a whole UTF-8 character", &t, "caf");
    ck_int("caret after a UTF-8 backspace", caret, 3);

    load(&t, "ab\ncd");
    caret = 3;                              /* start of the second line */
    ck_int("backspace at line start merges", dv_edit_backspace(&t, &caret), 1);
    ck_str("merge joins the two lines", &t, "abcd");
    ck_int("caret after the merge", caret, 2);

    /* ---- Delete ---- */
    load(&t, "abc");
    caret = 3;
    ck_int("delete at end is a no-op", dv_edit_delete(&t, &caret), 0);
    ck_str("delete at end leaves the text", &t, "abc");
    ck_int("delete at end leaves the caret", caret, 3);

    load(&t, "abc");
    caret = 1;
    ck_int("delete in the middle: changed", dv_edit_delete(&t, &caret), 1);
    ck_str("delete in the middle", &t, "ac");
    ck_int("delete leaves the caret", caret, 1);

    load(&t, "ab\ncd");
    caret = 2;                              /* on the newline */
    dv_edit_delete(&t, &caret);
    ck_str("delete on a newline joins downward", &t, "abcd");
    ck_int("delete on a newline keeps the caret", caret, 2);

    /* ---- Enter: split a line ---- */
    load(&t, "abcd");
    caret = 2;
    ck_int("enter in the middle: changed", dv_edit_split(&t, &caret), 1);
    ck_str("enter in the middle of a line", &t, "ab\ncd");
    ck_int("caret after enter in the middle", caret, 3);

    load(&t, "ab");
    caret = 2;
    dv_edit_split(&t, &caret);
    ck_str("enter at the end of a line", &t, "ab\n");
    ck_int("caret after enter at the end", caret, 3);

    load(&t, "ab");
    caret = 0;
    dv_edit_split(&t, &caret);
    ck_str("enter at the start of a line", &t, "\nab");
    ck_int("caret after enter at the start", caret, 1);

    /* ---- merge, called directly ---- */
    load(&t, "ab\ncd");
    caret = 3;
    ck_int("merge at a line start: changed", dv_edit_merge(&t, &caret), 1);
    ck_str("merge joins the lines", &t, "abcd");
    ck_int("caret after merge", caret, 2);

    load(&t, "abcd");
    caret = 2;
    ck_int("merge away from a line start is refused", dv_edit_merge(&t, &caret), 0);
    ck_str("refused merge leaves the text", &t, "abcd");
    ck_int("refused merge leaves the caret", caret, 2);

    /* ---- caret movement ---- */
    load(&t, "abcdef\nxy");
    ck_int("up from the second line keeps the column",
           dv_caret_up(&t, 8, NULL), 1);
    ck_int("down from the first line clamps to the short line",
           dv_caret_down(&t, 5, NULL), 9);
    ck_int("up from column 0 of line 2", dv_caret_up(&t, 7, NULL), 0);
    ck_int("down on the last line is a no-op",
           dv_caret_down(&t, 9, NULL), 9);
    {
        int goal = 4;
        ck_int("up honours the goal column", dv_caret_up(&t, 9, &goal), 4);
        ck_int("the goal column survives the move", goal, 4);
    }
    ck_int("home goes to the start of the line", dv_caret_home(&t, 8), 7);
    ck_int("end goes to the end of the line", dv_caret_end(&t, 7), 9);
    ck_int("line_start at a newline is its own line",
           dv_line_start(&t, 6), 0);
    ck_int("line_end at a newline is the newline", dv_line_end(&t, 6), 6);

    load(&t, "a\xC3\xA9z");                  /* a, e-acute, z */
    ck_int("left steps over the whole character", dv_caret_left(&t, 4), 3);
    ck_int("left again reaches the ASCII letter", dv_caret_left(&t, 3), 1);
    ck_int("right steps over the character forward", dv_caret_right(&t, 1), 3);
    ck_int("right at the end stays at the end", dv_caret_right(&t, 4), 4);
    ck_int("left at the start stays at the start", dv_caret_left(&t, 0), 0);

    /* ---- Tab is soft: it inserts spaces, never a literal tab ---- */
    load(&t, "abc");
    ck_int("tab at column 3 moves one stop", dv_tab_spaces(&t, 3), 1);
    ck_int("tab at column 4 moves a whole stop", dv_tab_spaces(&t, 4), 4);
    ck_int("tab at column 0 moves a whole stop", dv_tab_spaces(&t, 0), 4);

    /* ---- Markdown line styles ---- */
    {
        struct { const char *line; int style; } md[] = {
            { "# Title",        OFFICE_STYLE_H1 },
            { "## Section",     OFFICE_STYLE_H2 },
            { "### Detail",     OFFICE_STYLE_H3 },
            { "#### Deeper",    OFFICE_STYLE_H3 },
            { "- item",         OFFICE_STYLE_BULLET },
            { "* item",         OFFICE_STYLE_BULLET },
            { "+ item",         OFFICE_STYLE_BULLET },
            { "> quoted",       OFFICE_STYLE_QUOTE },
            { "plain words",    OFFICE_STYLE_BODY },
            { "#nospace",       OFFICE_STYLE_BODY },
            { "",               OFFICE_STYLE_BODY },
            { "   ## indented", OFFICE_STYLE_H2 },
        };
        for (size_t i = 0; i < sizeof(md) / sizeof(md[0]); i++) {
            char what[80];
            snprintf(what, sizeof(what), "markdown style of \"%s\"", md[i].line);
            ck_int(what, dv_md_line_style(md[i].line, (int)strlen(md[i].line)),
                   md[i].style);
        }
    }

    /* ---- the title a Markdown file declares ---- */
    {
        char title[64];

        load(&t, "# Hello world\nbody text\n");
        dv_md_title(&t, title, sizeof(title));
        ck_cstr("markdown title from '# Title'", title, "Hello world");

        load(&t, "## Section heading\n");
        dv_md_title(&t, title, sizeof(title));
        ck_cstr("markdown title from '## Title'", title, "Section heading");

        load(&t, "# Closed heading ###\n");
        dv_md_title(&t, title, sizeof(title));
        ck_cstr("markdown title drops the closing #s", title, "Closed heading");

        load(&t, "no heading here\n");
        dv_md_title(&t, title, sizeof(title));
        ck_cstr("no markdown title", title, "");
    }

    /* ---- the save-name chooser ---- */
    {
        Taken none = { { 0 }, 0 };
        Taken two = { { "/root/Documents/untitled.txt",
                        "/root/Documents/untitled-2.txt" }, 2 };
        char out[256];

        ck_int("choosing untitled.txt succeeds",
               dv_choose_save_name(probe_taken, &none, "/root/Documents",
                                   "untitled", ".txt", out, sizeof(out)), 0);
        ck_cstr("untitled.txt path", out, "/root/Documents/untitled.txt");

        ck_int("choosing past two taken names succeeds",
               dv_choose_save_name(probe_taken, &two, "/root/Documents",
                                   "untitled", ".txt", out, sizeof(out)), 0);
        ck_cstr("two taken names roll over to -3", out,
                "/root/Documents/untitled-3.txt");

        ck_int("choosing a markdown name succeeds",
               dv_choose_save_name(probe_taken, &none, "/root/Documents/",
                                   "untitled", ".md", out, sizeof(out)), 0);
        ck_cstr("a trailing slash is not doubled", out,
                "/root/Documents/untitled.md");

        ck_int("a buffer too small to hold the name is refused",
               dv_choose_save_name(probe_taken, &none, "/root/Documents",
                                   "untitled", ".txt", out, 5), -1);

        ck_int("join_path with a plain directory",
               dv_join_path(out, sizeof(out), "/root", "a.txt"), 0);
        ck_cstr("join_path result", out, "/root/a.txt");
        ck_int("join_path with a trailing slash",
               dv_join_path(out, sizeof(out), "/root/", "a.txt"), 0);
        ck_cstr("join_path result with a slash", out, "/root/a.txt");
    }

    /* ---- the size cap, and that erase clamps ---- */
    {
        char chunk[1024];
        memset(chunk, 'x', sizeof(chunk));
        ck_int("a fresh buffer for the cap test", dv_text_set(&t, "", 0), 0);
        while (t.n + (int)sizeof(chunk) <= DV_TEXT_MAX) {
            if (dv_text_insert(&t, t.n, chunk, (int)sizeof(chunk)) != 0) break;
        }
        ck_int("filled the buffer to the cap", t.n, DV_TEXT_MAX);
        ck_int("one byte past the cap is refused",
               dv_text_insert(&t, t.n, "y", 1), -1);
        ck_int("a refused insert leaves the length alone", t.n, DV_TEXT_MAX);

        load(&t, "abc");
        ck_int("erase past the end clamps", dv_text_erase(&t, 2, 99), 1);
        ck_str("erase past the end", &t, "ab");
        ck_int("erase at the end is a no-op", dv_text_erase(&t, 2, 1), 0);
    }

    /* ---- a short typing session, in the order the window applies it ---- */
    load(&t, "");
    caret = 0;
    for (const char *c = "Hello"; *c; c++) dv_edit_insert(&t, &caret, c, 1);
    dv_edit_split(&t, &caret);
    for (const char *c = "world"; *c; c++) dv_edit_insert(&t, &caret, c, 1);
    ck_str("a typing session", &t, "Hello\nworld");
    ck_int("caret after the typing session", caret, 11);

    /* ---- wrapping one visual line (dv_fit_span, 10 px per character) ---- */
    {
        char line[] = "hello world again";   /* 17 bytes */
        int n = (int)strlen(line);

        ck_int("a line that fits is not broken",
               dv_fit_span(line, n, 0, n, 1000, probe_width, NULL), n);
        /* 10 px each: maxw 50 fits "hello" exactly. There is no space inside
         * the fit, and the character that overflows IS the space, so the
         * break is at 5 and that space begins the next visual line. Every
         * byte belongs to exactly one line, which is what keeps a caret
         * position mapping to one line without a special case. */
        ck_int("the space that does not fit starts the next line",
               dv_fit_span(line, n, 0, n, 50, probe_width, NULL), 5);
        ck_int("the rest continues at that offset",
               dv_fit_span(line, n, 6, n, 1000, probe_width, NULL), n);
        /* no space to break on: a hard cut exactly at the width */
        ck_int("a word wider than the body is cut on a character",
               dv_fit_span((char *)"abcdefg", 7, 0, 7, 30, probe_width, NULL), 3);
        /* one glyph wider than the whole body still makes progress */
        ck_int("a single glyph wider than the body still advances",
               dv_fit_span((char *)"abcdefg", 7, 0, 7, 4, probe_width, NULL), 1);
        /* a UTF-8 character is never split by the hard cut */
        {
            char u[] = "a\xC3\xA9\xC3\xA9";       /* a, e-acute, e-acute: 5 bytes */
            ck_int("a hard cut never splits a UTF-8 character",
                   dv_fit_span(u, 5, 0, 5, 15, probe_width, NULL), 1);
        }
        ck_int("an empty line yields an empty span",
               dv_fit_span(line, n, 3, 3, 50, probe_width, NULL), 3);
        ck_int("a line that ends on a space breaks there",
               dv_fit_span((char *)"ab cd", 5, 0, 5, 35, probe_width, NULL), 3);
    }

    /* ---- free resets the buffer ---- */
    dv_text_free(&t);
    ck_int("free drops the length", t.n, 0);
    checks++;
    if (t.p != NULL) {
        fails++;
        printf("FAIL %-44s pointer is still %p\n", "free drops the pointer",
               (void *)t.p);
    } else {
        printf("ok   %-44s NULL\n", "free drops the pointer");
    }
    ck_int("the buffer can be used again after free",
           dv_text_set(&t, "again", 5), 0);
    ck_str("text after a free and reuse", &t, "again");
    dv_text_free(&t);

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
