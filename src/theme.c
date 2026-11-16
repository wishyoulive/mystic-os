/* ============================================================
 *  Mystic OS - theme.c
 *  Themes are plain text files, one setting per line, so anyone
 *  can write one without touching code:
 *
 *      name    = Obsidian
 *      bg_top  = #0B0F1A
 *      accent  = #7AA2F7
 *      corner  = 14
 * ============================================================ */
#include "mystic.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 parse_color(const char *v, u32 fallback) {
    while (*v == ' ' || *v == '\t') v++;
    if (*v == '#') v++;
    else if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) v += 2;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 16);
    if (end == v) return fallback;
    if (strlen(v) >= 8) return (u32)n;              /* AARRGGBB */
    return WITH_A((u32)n, 0xFF);                    /* RRGGBB   */
}

static float parse_float(const char *v, float fallback) {
    char *end = NULL;
    float f = strtof(v, &end);
    return (end == v) ? fallback : f;
}

static void defaults(Theme *t) {
    memset(t, 0, sizeof(*t));
    snprintf(t->name, sizeof(t->name), "Default");
    t->bg_top = RGB(0x0B, 0x0F, 0x1A);
    t->bg_bottom = RGB(0x14, 0x1A, 0x2B);
    t->accent = RGB(0x7A, 0xA2, 0xF7);
    t->glow = RGB(0x3D, 0x59, 0xA0);
    t->glow_strength = 0.55f;
    t->panel = WITH_A(RGB(0x1A, 0x20, 0x33), 0x8C);
    t->panel_border = WITH_A(RGB(0xFF, 0xFF, 0xFF), 0x1A);
    t->text = RGB(0xE8, 0xEC, 0xF6);
    t->text_dim = WITH_A(RGB(0xE8, 0xEC, 0xF6), 0x99);
    t->window_bg = WITH_A(RGB(0x14, 0x19, 0x28), 0xD9);
    t->window_border = WITH_A(RGB(0xFF, 0xFF, 0xFF), 0x22);
    t->corner = 14.0f;
    t->blur = 1.0f;
    t->dark = 1;
}

int theme_load(Theme *t, const char *path) {
    defaults(t);
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        /* a '#' may start a colour, so only treat it as a comment when
           it is not immediately followed by 6 or 8 hex digits */
        if (hash) {
            int hex = 1;
            for (int i = 1; i <= 8; i++) {
                char c = hash[i];
                if (c == 0 || c == '\n') break;
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                    hex = 0; break;
                }
            }
            if (!hex) *hash = 0;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line;
        char *val = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        char *kend = key + strlen(key);
        while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t')) *--kend = 0;
        while (*val == ' ' || *val == '\t') val++;
        char *vend = val + strlen(val);
        while (vend > val && (vend[-1] == '\n' || vend[-1] == '\r' || vend[-1] == ' ')) *--vend = 0;

        if      (!strcmp(key, "name"))          snprintf(t->name, sizeof(t->name), "%s", val);
        else if (!strcmp(key, "bg_top"))        t->bg_top = parse_color(val, t->bg_top);
        else if (!strcmp(key, "bg_bottom"))     t->bg_bottom = parse_color(val, t->bg_bottom);
        else if (!strcmp(key, "accent"))        t->accent = parse_color(val, t->accent);
        else if (!strcmp(key, "glow"))          t->glow = parse_color(val, t->glow);
        else if (!strcmp(key, "glow_strength")) t->glow_strength = parse_float(val, t->glow_strength);
        else if (!strcmp(key, "panel"))         t->panel = parse_color(val, t->panel);
        else if (!strcmp(key, "panel_border"))  t->panel_border = parse_color(val, t->panel_border);
        else if (!strcmp(key, "text"))          t->text = parse_color(val, t->text);
        else if (!strcmp(key, "text_dim"))      t->text_dim = parse_color(val, t->text_dim);
        else if (!strcmp(key, "window_bg"))     t->window_bg = parse_color(val, t->window_bg);
        else if (!strcmp(key, "window_border")) t->window_border = parse_color(val, t->window_border);
        else if (!strcmp(key, "corner"))        t->corner = parse_float(val, t->corner);
        else if (!strcmp(key, "blur"))          t->blur = parse_float(val, t->blur);
        else if (!strcmp(key, "dark"))          t->dark = (int)parse_float(val, (float)t->dark);
    }
    fclose(f);
    return 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Read every *.theme in a directory, in filename order so the numbering
 * in the filenames is the order the user sees. */
int theme_load_all(Theme *list, int max, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;

    char *names[64];
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) && count < 64) {
        size_t len = strlen(e->d_name);
        if (len < 7 || strcmp(e->d_name + len - 6, ".theme")) continue;
        names[count++] = strdup(e->d_name);
    }
    closedir(d);
    if (count == 0) return 0;

    qsort(names, (size_t)count, sizeof(names[0]), cmp_str);

    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        char path[700];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (theme_load(&list[n], path) == 0) n++;
        free(names[i]);
    }
    return n;
}
