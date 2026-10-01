#include "config.h"

#include "log.h"
#include "paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

KhConfig kh_config = { .render_scale = 2, .layout = 0, .inset_width = 224 };

static const char s_default[] =
    "# khdays-vita settings\n"
    "\n"
    "# 3D internal resolution, as a multiple of the DS's 256x192: 1, 2 or 3.\n"
    "# Higher is sharper and costs more GPU time.\n"
    "render_scale = 2\n"
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

void config_load(void)
{
    char line[256];
    FILE *f = fopen(KH_CONFIG_PATH, "r");
    if (!f) {
        f = fopen(KH_CONFIG_PATH, "w");
        if (f) {
            fputs(s_default, f);
            fclose(f);
        }
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
        if (!strcmp(k, "layout")) {
            kh_config.layout = !strcmp(v, "bottom") ? 1 : !strcmp(v, "side") ? 2 : 0;
        } else if (!strcmp(k, "inset_width")) {
            int w = atoi(v);
            kh_config.inset_width = w < 128 ? 128 : w > 480 ? 480 : w;
        } else if (!strcmp(k, "render_scale")) {
            int s = atoi(v);
            kh_config.render_scale = s < 1 ? 1 : s > 3 ? 3 : s;
        }
    }
    fclose(f);
    LOG("config: render_scale %d, layout %d, inset_width %d", kh_config.render_scale,
        kh_config.layout, kh_config.inset_width);
}
