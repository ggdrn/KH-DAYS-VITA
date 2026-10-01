#include "config.h"

#include "log.h"
#include "paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

KhConfig kh_config = { .render_scale = 2 };

static const char s_default[] =
    "# khdays-vita settings\n"
    "\n"
    "# 3D internal resolution, as a multiple of the DS's 256x192: 1, 2 or 3.\n"
    "# Higher is sharper and costs more GPU time.\n"
    "render_scale = 2\n";

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
        if (!strcmp(k, "render_scale")) {
            int s = atoi(v);
            kh_config.render_scale = s < 1 ? 1 : s > 3 ? 3 : s;
        }
    }
    fclose(f);
    LOG("config: render_scale %d", kh_config.render_scale);
}
