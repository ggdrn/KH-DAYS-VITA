#include "config.h"

#include "log.h"
#include "paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/ctrl.h>

KhConfig kh_config = {
    .render_scale = 3, .layout = 0, .inset_width = 224, .debug = 0, .texture_filter = 0,
    .widescreen = 1, .frame_interpolation = 1,
    /* positional, as on the DS: the right face button is A, the bottom one B */
    .button = { SCE_CTRL_CIRCLE, SCE_CTRL_CROSS, SCE_CTRL_TRIANGLE, SCE_CTRL_SQUARE,
                SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER, SCE_CTRL_START, SCE_CTRL_SELECT },
};

static const struct {
    const char *name;
    uint32_t mask;
} s_vita_buttons[] = {
    { "cross", SCE_CTRL_CROSS }, { "circle", SCE_CTRL_CIRCLE }, { "square", SCE_CTRL_SQUARE },
    { "triangle", SCE_CTRL_TRIANGLE }, { "l", SCE_CTRL_LTRIGGER }, { "r", SCE_CTRL_RTRIGGER },
    { "start", SCE_CTRL_START }, { "select", SCE_CTRL_SELECT },
};
static const char *const s_ds_buttons[KH_BTN_COUNT] = { "button_a", "button_b", "button_x",
    "button_y", "button_l", "button_r", "button_start", "button_select" };

static const char *button_name(uint32_t mask)
{
    size_t i;
    for (i = 0; i < sizeof(s_vita_buttons) / sizeof(s_vita_buttons[0]); i++)
        if (s_vita_buttons[i].mask == mask)
            return s_vita_buttons[i].name;
    return "none";
}

#define CONFIG_VERSION 6

static const char s_default[] =
    "# khdays-vita settings\n"
    "config_version = 6\n"
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
    "inset_width = 224\n"
    "\n"
    "# 1: the 3D is drawn for 16:9 when its screen fills the display (a wider view, characters\n"
    "# in their true proportions); 0: the DS's 4:3 picture stretched to the display.\n"
    "widescreen = 1\n"
    "\n"
    "# 1: the 3D moves at 60 fps (the game draws 30; a frame mixed from two is shown in\n"
    "# between, one Vita frame later); 0: the DS's 30 fps.\n"
    "frame_interpolation = 1\n"
    "\n"
    "# 3D textures: 0 sharp as on the DS, 1 smoothed (bilinear filtering).\n"
    "texture_filter = 0\n"
    "\n"
    "# Controls: the Vita button for each DS button. Names: cross circle square triangle l r\n"
    "# start select. The default is positional, as on the DS (the right face button is A).\n"
    "button_a = circle\n"
    "button_b = cross\n"
    "button_x = triangle\n"
    "button_y = square\n"
    "button_l = l\n"
    "button_r = r\n"
    "button_start = start\n"
    "button_select = select\n"
    "\n"
    "# Diagnostics: 1 turns on the debug hotkeys (L+R+Start on-screen log, L+R+Triangle 3D frame\n"
    "# dump, L+R+Circle 3D debug modes) and the detailed log (card reads, the game's traces,\n"
    "# per-subsystem statistics). 0 for normal play.\n"
    "debug = 0\n";

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
        else if (!strncmp(line, "debug", 5))
            fprintf(f, "debug = %d\n", kh_config.debug ? 1 : 0);
        else if (!strncmp(line, "widescreen", 10))
            fprintf(f, "widescreen = %d\n", kh_config.widescreen ? 1 : 0);
        else if (!strncmp(line, "frame_interpolation", 19))
            fprintf(f, "frame_interpolation = %d\n", kh_config.frame_interpolation ? 1 : 0);
        else if (!strncmp(line, "texture_filter", 14))
            fprintf(f, "texture_filter = %d\n", kh_config.texture_filter ? 1 : 0);
        else if (!strncmp(line, "button_", 7)) {
            int b;
            for (b = 0; b < KH_BTN_COUNT; b++)
                if (!strncmp(line, s_ds_buttons[b], strlen(s_ds_buttons[b])) &&
                    line[strlen(s_ds_buttons[b])] == ' ')
                    fprintf(f, "%s = %s\n", s_ds_buttons[b], button_name(kh_config.button[b]));
        } else
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
        } else if (!strcmp(k, "debug")) {
            kh_config.debug = atoi(v) != 0;
        } else if (!strcmp(k, "widescreen")) {
            kh_config.widescreen = atoi(v) != 0;
        } else if (!strcmp(k, "frame_interpolation")) {
            kh_config.frame_interpolation = atoi(v) != 0;
        } else if (!strcmp(k, "texture_filter")) {
            kh_config.texture_filter = atoi(v) != 0;
        } else if (!strncmp(k, "button_", 7)) {
            int b;
            size_t i;
            for (b = 0; b < KH_BTN_COUNT; b++)
                if (!strcmp(k, s_ds_buttons[b]))
                    for (i = 0; i < sizeof(s_vita_buttons) / sizeof(s_vita_buttons[0]); i++)
                        if (!strcmp(v, s_vita_buttons[i].name))
                            kh_config.button[b] = s_vita_buttons[i].mask;
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
    kh_log_verbose = kh_config.debug;
    LOG("config: render_scale %d, layout %d, inset_width %d, debug %d, widescreen %d, "
        "frame_interpolation %d, texture_filter %d", kh_config.render_scale, kh_config.layout,
        kh_config.inset_width, kh_config.debug, kh_config.widescreen,
        kh_config.frame_interpolation, kh_config.texture_filter);
}
