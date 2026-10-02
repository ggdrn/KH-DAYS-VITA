#include "config.h"

#include "log.h"
#include "paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

KhConfig kh_config = { .render_scale = 3, .layout = 0, .inset_width = 224 };

#define CONFIG_VERSION 3

static const char s_default[] =
    "# khdays-vita settings\n"
    "config_version = 3\n"
    "\n"
    "# 3D internal resolution, as a multiple of the DS's 256x192: 1, 2, 3 or 4.\n"
    "# 3 (768x576) covers the Vita's 544 lines; 4 (1024x768) is above the screen both ways,\n"
    "# which smooths edges (the game turns on the DS's anti-aliasing) for more GPU time.\n"
    "render_scale = 3\n"
    "\n"
    "# Starting screen layout: top (top screen over the whole display, the touch screen small\n"
    "# in the top-right corner), bottom (the reverse), side (both side by side).\n"
    "# Touching the small screen swaps top and bottom; L+R+Select cycles the layouts.\n"
    "layout = top\n"
    "\n"
    "# Width in pixels of the small screen (it keeps the DS's 4:3), 128 to 480.\n"
    "inset_width = 224\n";

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

/* The template, line by line, with the current values in place of the defaults. */
static void write_config(void)
{
    static const char *const layouts[] = { "top", "bottom", "side" };
    const char *line = s_default;
    FILE *f = fopen(KH_CONFIG_PATH, "w");
    if (!f)
        return;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        if (!strncmp(line, "render_scale", 12))
            fprintf(f, "render_scale = %d\n", kh_config.render_scale);
        else if (!strncmp(line, "layout", 6))
            fprintf(f, "layout = %s\n", layouts[kh_config.layout % 3]);
        else if (!strncmp(line, "inset_width", 11))
            fprintf(f, "inset_width = %d\n", kh_config.inset_width);
        else
            fprintf(f, "%.*s\n", (int)n, line);
        line += n + (end != NULL);
    }
    fclose(f);
}

void config_load(void)
{
    char line[256];
    int version = 0;
    FILE *f = fopen(KH_CONFIG_PATH, "r");
    if (!f) {
        write_config();
        LOG("config: %s written with the defaults", KH_CONFIG_PATH);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *k = trim(line), *v = strchr(k, '=');
        if (*k == '#' || *k == ';' || !v)
            continue;
        *v++ = 0;
        k = trim(k);
        v = trim(v);
        if (!strcmp(k, "config_version")) {
            version = atoi(v);
        } else if (!strcmp(k, "layout")) {
            kh_config.layout = !strcmp(v, "bottom") ? 1 : !strcmp(v, "side") ? 2 : 0;
        } else if (!strcmp(k, "inset_width")) {
            int w = atoi(v);
            kh_config.inset_width = w < 128 ? 128 : w > 480 ? 480 : w;
        } else if (!strcmp(k, "render_scale")) {
            int s = atoi(v);
            kh_config.render_scale = s < 1 ? 1 : s > 4 ? 4 : s;
        }
    }
    fclose(f);
    if (version < CONFIG_VERSION) {
        /* a file from an older build: the 3D default went from 2x to 3x (the Vita's full
         * height), and the newer settings get written out with it */
        if (version < 2 && kh_config.render_scale == 2)
            kh_config.render_scale = 3;
        write_config();
        LOG("config: upgraded from version %d", version);
    }
    LOG("config: render_scale %d, layout %d, inset_width %d", kh_config.render_scale,
        kh_config.layout, kh_config.inset_width);
}
