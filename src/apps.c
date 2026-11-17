/* ============================================================
 *  Mystic OS - apps.c
 *  The app registry. An app is a pointer to a table of handlers;
 *  the window manager only ever talks to that table.
 * ============================================================ */
#include "mystic.h"

#include <stdlib.h>
#include <string.h>

/* Apps are added here as they are written. The desktop never needs to
 * know which ones exist - the dock asks for an id and gets NULL if the
 * app is missing. */
static const AppVT *REGISTRY[] = {
#ifdef WITH_FILES
    &app_vt_files,
#endif
#ifdef WITH_SETTINGS
    &app_vt_settings,
#endif
#ifdef WITH_MONITOR
    &app_vt_monitor,
#endif
#ifdef WITH_NOTES
    &app_vt_notes,
#endif
#ifdef WITH_STORE
    &app_vt_store,
#endif
    &app_vt_sysinfo,
};

App *app_new(const char *id, int w, int h) {
    for (unsigned i = 0; i < sizeof(REGISTRY) / sizeof(REGISTRY[0]); i++) {
        const AppVT *vt = REGISTRY[i];
        if (!vt || !vt->id) continue;
        if (strcmp(vt->id, id)) continue;
        App *a = calloc(1, sizeof(App));
        if (!a) return NULL;
        a->vt = vt;
        if (vt->init) vt->init(a, w, h);
        return a;
    }
    return NULL;
}

void app_free(App *a) {
    if (!a) return;
    if (a->vt && a->vt->free) a->vt->free(a);
    free(a);
}

void app_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    if (a && a->vt && a->vt->draw) a->vt->draw(a, s, x, y, w, h, th);
}

void app_key(App *a, const char *bytes, int n) {
    if (a && a->vt && a->vt->key) a->vt->key(a, bytes, n);
}

void app_click(App *a, int x, int y, int buttons) {
    if (a && a->vt && a->vt->click) a->vt->click(a, x, y, buttons);
}

const char *app_title(App *a) {
    if (a && a->vt && a->vt->title) return a->vt->title;
    return "App";
}
