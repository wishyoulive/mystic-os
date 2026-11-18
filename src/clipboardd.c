/* ============================================================
 *  Mystic OS - clipboardd.c
 *
 *  The standalone clipboard daemon. src/clipboard.c is the whole
 *  service - the store, the line protocol and the server loop all
 *  live there - and it already exposes the loop as clipd_main()
 *  (declared in clipboard.h). This file is only the process entry
 *  point, so `clipboardd` on disk is a real binary with a real
 *  main() instead of code that never runs.
 *
 *  /etc/init starts it before the desktop (stage/init), and
 *  mystic-desktop also hosts the same loop when it starts without
 *  one, so the clipboard exists either way. Both paths end in
 *  clipd_main(); the flock in clipboard.c makes the second one
 *  exit quietly instead of fighting over the store.
 *
 *  Build:
 *    zig cc -target x86_64-linux-musl -Os -static -Isrc \
 *        src/clipboardd.c src/clipboard.c -o clipboardd
 * ============================================================ */
#include "clipboard.h"

int main(int argc, char **argv) {
    return clipd_main(argc, argv);
}
