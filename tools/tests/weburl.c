/* ============================================================
 *  weburl.c - host test for the browser's URL resolution
 *
 *  src/client/apps/web.c keeps the address parser and the relative-link
 *  resolver that decide which bytes the browser puts on the wire. Both
 *  had defects found by differential testing against RFC 3986 (see
 *  .work/hunt/browser.md, findings F2 and F3), so this file pins the
 *  behaviour down:
 *
 *      path_clean()  keeps a trailing slash (a directory stays a
 *                    directory) and still collapses "." and "..";
 *      url_resolve() resolves against the base *path*, not the base
 *                    path-and-query, and a reference that starts with
 *                    "?" replaces only the query.
 *
 *  Build and run:
 *
 *      zig cc -Isrc -Isrc/client -o build/weburl tools/tests/weburl.c \
 *          && ./build/weburl
 *
 *  Prints "N checks, M failures" and exits non-zero when anything fails.
 *  What this cannot cover: the bytes actually sent (a socket is not
 *  opened here) - that is build_request()'s job and web.c's own footer
 *  numbers, checked on a real boot.
 * ============================================================ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../../src/img.h"
#define WEB_HOST_TEST 1
#include "../../src/client/apps/web.c"
#include "../../src/css.c"

/* The two OS symbols the host-test slice still references. */
int ui_px(int base) { return base < 1 ? 1 : base; }
void img_free(Img *im) { if (im && im->px) { free(im->px); im->px = NULL; } }

static int checks, fails;

static void ck_resolve(const char *base, const char *href, const char *want)
{
    Url u;
    char err[160];
    err[0] = 0;
    checks++;
    if (url_parse(base, &u, err, sizeof err) != 0) {
        printf("FAIL %-30s + %-14s  base did not parse: %s\n", base, href, err);
        fails++;
        return;
    }
    char out[WEB_PATH_MAX * 2], e2[160];
    e2[0] = 0;
    url_resolve(&u, href, out, sizeof out, e2, sizeof e2);
    int ok = (strcmp(out, want) == 0);
    if (!ok) fails++;
    printf("%s %-30s + %-14s -> %s%s%s\n", ok ? "ok  " : "FAIL", base, href,
           out[0] ? out : "(ignored)",
           ok ? "" : "   want ", ok ? "" : want);
}

static void ck_parse_path(const char *in, const char *want)
{
    Url u;
    char err[160];
    err[0] = 0;
    checks++;
    int rc = url_parse(in, &u, err, sizeof err);
    int ok = (rc == 0 && strcmp(u.path, want) == 0);
    if (!ok) fails++;
    printf("%s parse %-38s -> path %s%s%s\n", ok ? "ok  " : "FAIL", in,
           rc == 0 ? u.path : "(refused)",
           ok ? "" : "   want ", ok ? "" : want);
}

/* img_url() is reachable from this translation unit only after the F3b fix
 * moved it out of the app-only section; building this file against a
 * pre-fix copy of web.c (the "before" demo in .work/hunt/web-cases) is done
 * with -DWEB_NO_IMG_TESTS. */
#ifndef WEB_NO_IMG_TESTS
static void ck_img(const char *base, const char *src, const char *want)
{
    Url u;
    char err[160];
    err[0] = 0;
    checks++;
    if (url_parse(base, &u, err, sizeof err) != 0) {
        printf("FAIL %-30s + %-18s  base did not parse: %s\n", base, src, err);
        fails++;
        return;
    }
    char out[WEB_PATH_MAX * 2];
    img_url(&u, src, out, sizeof out);
    int ok = (strcmp(out, want) == 0);
    if (!ok) fails++;
    printf("%s img %-30s + %-18s -> %s%s%s\n", ok ? "ok  " : "FAIL", base, src, out,
           ok ? "" : "   want ", ok ? "" : want);
}
#endif /* !WEB_NO_IMG_TESTS */

int main(void)
{
    printf("-- a relative reference that names a directory keeps its slash\n");
    ck_resolve("https://example.com/a/b/", "sub/",   "https://example.com/a/b/sub/");
    ck_resolve("https://example.com/a/b/", "./",     "https://example.com/a/b/");
    ck_resolve("https://example.com/a/b/", "..",     "https://example.com/a/");
    ck_resolve("https://example.com/a/b/", "../",    "https://example.com/a/");
    ck_resolve("https://example.com/a/b/", "../../", "https://example.com/");
    ck_resolve("https://example.com/a/b/", "sub",    "https://example.com/a/b/sub");
    ck_resolve("https://example.com/a/b",  "sub/",   "https://example.com/a/sub/");

    printf("-- dot segments still collapse\n");
    ck_resolve("https://example.com/a/b/", "a/../b",  "https://example.com/a/b/b");
    ck_resolve("https://example.com/a/b/", "/./x",    "https://example.com/x");
    ck_resolve("https://example.com/a/b/", "/../x",   "https://example.com/x");
    ck_resolve("https://example.com/a/b/", "//x//y//", "https://x//y//");   /* protocol-relative */
    ck_resolve("https://example.com/a/b/", "/a//b//",  "https://example.com/a/b/");
    ck_resolve("https://example.com/a/b/", "../../../../x", "https://example.com/x");
    ck_resolve("https://example.com/a/b/", "/",        "https://example.com/");
    ck_resolve("https://example.com/a/b/", "/a/./././", "https://example.com/a/");

    printf("-- a reference that starts with '?' replaces only the query\n");
    ck_resolve("https://example.com/a/b?x=1",  "?page=2", "https://example.com/a/b?page=2");
    ck_resolve("https://example.com/a/b",      "?page=2", "https://example.com/a/b?page=2");
    ck_resolve("https://example.com/a/b?x=1",  "c?d=2",   "https://example.com/a/c?d=2");

    printf("-- the base query is not part of the base path\n");
    ck_resolve("https://example.com/a/b", "x?u=https://example.com/a",
               "https://example.com/a/x?u=https://example.com/a");
    ck_resolve("https://example.com/a/b", "x?p=/../y", "https://example.com/a/x?p=/../y");
    ck_resolve("https://example.com/a/b", "x?p=a//b", "https://example.com/a/x?p=a//b");
    ck_resolve("https://example.com/a/b", "x?a=1#f", "https://example.com/a/x?a=1#f");
    ck_resolve("https://example.com/a/b", "x#f", "https://example.com/a/x#f");
    ck_resolve("https://example.com/a/b", "/p?q=1", "https://example.com/p?q=1");
    ck_resolve("https://example.com/a/b", "?u=https://e/x",
               "https://example.com/a/b?u=https://e/x");
    ck_resolve("https://example.com/p?next=/x/y/", "img.png",
               "https://example.com/img.png");
    ck_resolve("https://example.com/p?next=/x/y/", "/img.png",
               "https://example.com/img.png");
    ck_resolve("https://example.com/d/a?u=/q/r/", "sub/",
               "https://example.com/d/sub/");

    printf("-- schemes, ports and things we do not fetch\n");
    ck_resolve("https://example.com/a/b", "//other.example/x", "https://other.example/x");
    ck_resolve("http://example.com:8080/a/b", "c", "http://example.com:8080/a/c");
    ck_resolve("https://example.com/a/b", "https://o.example/p", "https://o.example/p");
    ck_resolve("https://example.com/a/b", "mailto:a@b", "");
    ck_resolve("https://example.com/a/b", "javascript:void(0)", "");
    ck_resolve("https://example.com/a/b", "#frag", "");

    printf("-- what the address bar keeps\n");
    ck_parse_path("https://example.com/docs/guide/", "/docs/guide/");
    ck_parse_path("example.com/docs/", "/docs/");
    ck_parse_path("https://example.com/a/b?x=1", "/a/b?x=1");

#ifndef WEB_NO_IMG_TESTS
    printf("-- <img src> uses the same resolver\n");
    ck_img("https://example.com/docs/guide/page.html", "pics/logo.png",
           "https://example.com/docs/guide/pics/logo.png");
    ck_img("https://example.com/docs/guide/page.html", "//cdn.example/x.png",
           "https://cdn.example/x.png");
    ck_img("https://example.com/docs/guide/page.html", "/root.png",
           "https://example.com/root.png");
    ck_img("https://example.com/docs/guide/page.html", "../up.png",
           "https://example.com/docs/up.png");
    ck_img("https://example.com/view?file=/a/b/", "pic.png",
           "https://example.com/pic.png");
    ck_img("http://example.com:8080/a/b", "x.png",
           "http://example.com:8080/a/x.png");
    ck_img("https://example.com/a/b", "https://other.example/y.png",
           "https://other.example/y.png");
#endif /* !WEB_NO_IMG_TESTS */

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
