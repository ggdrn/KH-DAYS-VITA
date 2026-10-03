#include "config.h"

#include "log.h"
#include "paths.h"

#include <psp2/ctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_VERSION 8

static const KhConfig s_defaults = {
    .render_scale = 3, .layout = 0, .inset_width = 224, .aspect = KH_ASPECT_WIDE,
    .frame_interpolation = 1, .texture_filter = 1, .hud = 0, .screen_effect = 0,
    .inset_corner = 0, .inset_opacity = 100, .rear_touch = 0, .stick_deadzone = 2, .r_toggle = 0,
    .fast_forward = 2, .camera_stick = 1, .camera_speed = 2,
    .camera_invert_x = 0, .camera_invert_y = 0, .dpad_deck = 1, .show_fps = 0, .volume = 100,
    .debug = 0,
    /* positional, as on the DS: the right face button is A, the bottom one B */
    .button = { SCE_CTRL_CIRCLE, SCE_CTRL_CROSS, SCE_CTRL_TRIANGLE, SCE_CTRL_SQUARE,
                SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER, SCE_CTRL_START, SCE_CTRL_SELECT },
};

KhConfig kh_config;

static const struct {
    const char *name;
    uint32_t mask;
} s_vita_buttons[KH_VITA_BUTTONS] = {
    { "cross", SCE_CTRL_CROSS }, { "circle", SCE_CTRL_CIRCLE }, { "square", SCE_CTRL_SQUARE },
    { "triangle", SCE_CTRL_TRIANGLE }, { "l", SCE_CTRL_LTRIGGER }, { "r", SCE_CTRL_RTRIGGER },
    { "start", SCE_CTRL_START }, { "select", SCE_CTRL_SELECT },
};
static const char *const s_ds_buttons[KH_BTN_COUNT] = { "button_a", "button_b", "button_x",
    "button_y", "button_l", "button_r", "button_start", "button_select" };
static const char *const s_layouts[] = { "top", "bottom", "side" };
static const char *const s_aspects[] = { "wide", "stretch", "4:3" };

const char *config_vita_button_name(int i) { return s_vita_buttons[i].name; }
uint32_t config_vita_button_mask(int i) { return s_vita_buttons[i].mask; }

int config_vita_button_index(uint32_t mask)
{
    int i;
    for (i = 0; i < KH_VITA_BUTTONS; i++)
        if (s_vita_buttons[i].mask == mask)
            return i;
    return 0;
}

void config_defaults(void)
{
    const int debug = kh_config.debug; /* a diagnostic switch, not a preference */
    kh_config = s_defaults;
    kh_config.debug = debug;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* The file, comments included: what the user reads when editing it by hand. */
void config_save(void)
{
    FILE *f = fopen(KH_CONFIG_PATH, "w");
    int b;
    if (!f)
        return;
    fprintf(f, "# khdays-vita settings. The port menu (L+R+Select in the game) edits them too.\n");
    fprintf(f, "config_version = %d\n\n", CONFIG_VERSION);
    fprintf(f, "# 3D internal resolution, as a multiple of the DS's 256x192: 1, 2, 3 or 4.\n"
               "# 3 (768x576) covers the Vita's 544 lines; 4 smooths edges for more GPU time.\n"
               "render_scale = %d\n\n", kh_config.render_scale);
    fprintf(f, "# 3D frame rate: 60 (frames mixed in between, one Vita frame later) or 30.\n"
               "fps = %d\n\n", kh_config.frame_interpolation ? 60 : 30);
    fprintf(f, "# How the large screen fills the display: wide (the 3D drawn for 16:9 in the\n"
               "# field, true proportions), stretch (the DS picture stretched), 4:3 (as on the\n"
               "# DS, with borders).\n"
               "aspect = %s\n\n", s_aspects[clampi(kh_config.aspect, 0, 2)]);
    fprintf(f, "# Screen layout: top (top screen large, touch screen small in the top-right\n"
               "# corner), bottom (the reverse), side (both side by side). Touching the small\n"
               "# screen swaps them.\n"
               "layout = %s\n\n", s_layouts[kh_config.layout % 3]);
    fprintf(f, "# Width in pixels of the small screen (it keeps the DS's 4:3), 128 to 480.\n"
               "inset_width = %d\n\n", kh_config.inset_width);
    fprintf(f, "# 3D textures: 0 sharp as on the DS, 1 smoothed (bilinear filtering).\n"
               "texture_filter = %d\n\n", kh_config.texture_filter);
    fprintf(f, "# Right stick turns the field camera (1) or does nothing (0); its speed 1 slow,\n"
               "# 2 normal, 3 fast; each axis inverted with 1.\n"
               "camera_stick = %d\ncamera_speed = %d\ncamera_invert_x = %d\ncamera_invert_y = %d\n\n",
            kh_config.camera_stick, kh_config.camera_speed, kh_config.camera_invert_x,
            kh_config.camera_invert_y);
    fprintf(f, "# 1: in the field the d-pad moves the command deck's cursor (the left stick\n"
               "# walks); 0: the d-pad is the DS d-pad everywhere.\n"
               "dpad_deck = %d\n\n", kh_config.dpad_deck);
    fprintf(f, "# A frame-rate counter in the top-left corner: 1 on, 0 off.\n"
               "show_fps = %d\n\n", kh_config.show_fps);
    fprintf(f, "# Sound volume, 0 to 100 (%% of the game's own).\n"
               "volume = %d\n\n", kh_config.volume);
    fprintf(f, "# Widescreen field: hud = 0 stretches the 2D (HUD) over 16:9, 1 keeps it 4:3 in\n"
               "# the middle over the wide 3D.\n"
               "hud = %d\n\n", kh_config.hud);
    fprintf(f, "# Screen effect: 0 none, 1 scanlines, 2 LCD grid.\n"
               "screen_effect = %d\n\n", kh_config.screen_effect);
    fprintf(f, "# Small screen: corner 0 top-right, 1 top-left, 2 bottom-right, 3 bottom-left;\n"
               "# opacity 50 to 100.\n"
               "inset_corner = %d\ninset_opacity = %d\n\n", kh_config.inset_corner,
            kh_config.inset_opacity);
    fprintf(f, "# Rear touchpad: 0 off, 1 left half L / right half R, 2 Select / Start.\n"
               "rear_touch = %d\n\n", kh_config.rear_touch);
    fprintf(f, "# Analog stick dead zone: 1 small, 2 normal, 3 large.\n"
               "stick_deadzone = %d\n\n", kh_config.stick_deadzone);
    fprintf(f, "# The DS's R (lock-on): 0 held as on the DS, 1 toggled by each press.\n"
               "r_toggle = %d\n\n", kh_config.r_toggle);
    fprintf(f, "# Fast-forward speed, switched on and off with L+R+Square: 2 or 3.\n"
               "fast_forward = %d\n\n", kh_config.fast_forward);
    fprintf(f, "# Controls: the Vita button for each DS button. Names: cross circle square\n"
               "# triangle l r start select. The default is positional, as on the DS.\n");
    for (b = 0; b < KH_BTN_COUNT; b++)
        fprintf(f, "%s = %s\n", s_ds_buttons[b],
                s_vita_buttons[config_vita_button_index(kh_config.button[b])].name);
    fprintf(f, "\n# Diagnostics: 1 turns on the debug hotkeys (L+R+Start on-screen log,\n"
               "# L+R+Triangle frame dump, L+R+Circle 3D debug modes) and the detailed log.\n"
               "debug = %d\n", kh_config.debug);
    fclose(f);
}

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

static void set(const char *k, const char *v, int version)
{
    int b, i;
    if (!strcmp(k, "render_scale"))
        kh_config.render_scale = clampi(atoi(v), 1, 4);
    else if (!strcmp(k, "layout"))
        kh_config.layout = !strcmp(v, "bottom") ? 1 : !strcmp(v, "side") ? 2 : 0;
    else if (!strcmp(k, "inset_width"))
        kh_config.inset_width = clampi(atoi(v), 128, 480);
    else if (!strcmp(k, "aspect"))
        kh_config.aspect = !strcmp(v, "stretch") ? KH_ASPECT_STRETCH
                           : !strcmp(v, "4:3") ? KH_ASPECT_4_3 : KH_ASPECT_WIDE;
    else if (!strcmp(k, "widescreen") && version < 7) /* before the aspect setting */
        kh_config.aspect = atoi(v) ? KH_ASPECT_WIDE : KH_ASPECT_STRETCH;
    else if (!strcmp(k, "fps"))
        kh_config.frame_interpolation = atoi(v) >= 60;
    else if (!strcmp(k, "frame_interpolation") && version < 7)
        kh_config.frame_interpolation = atoi(v) != 0;
    else if (!strcmp(k, "texture_filter"))
        kh_config.texture_filter = atoi(v) != 0;
    else if (!strcmp(k, "camera_stick"))
        kh_config.camera_stick = atoi(v) != 0;
    else if (!strcmp(k, "camera_speed"))
        kh_config.camera_speed = clampi(atoi(v), 1, 3);
    else if (!strcmp(k, "camera_invert_x"))
        kh_config.camera_invert_x = atoi(v) != 0;
    else if (!strcmp(k, "camera_invert_y"))
        kh_config.camera_invert_y = atoi(v) != 0;
    else if (!strcmp(k, "dpad_deck"))
        kh_config.dpad_deck = atoi(v) != 0;
    else if (!strcmp(k, "show_fps"))
        kh_config.show_fps = atoi(v) != 0;
    else if (!strcmp(k, "volume"))
        kh_config.volume = clampi(atoi(v), 0, 100);
    else if (!strcmp(k, "hud"))
        kh_config.hud = atoi(v) != 0;
    else if (!strcmp(k, "screen_effect"))
        kh_config.screen_effect = clampi(atoi(v), 0, 2);
    else if (!strcmp(k, "inset_corner"))
        kh_config.inset_corner = clampi(atoi(v), 0, 3);
    else if (!strcmp(k, "inset_opacity"))
        kh_config.inset_opacity = clampi(atoi(v), 50, 100);
    else if (!strcmp(k, "rear_touch"))
        kh_config.rear_touch = clampi(atoi(v), 0, 2);
    else if (!strcmp(k, "stick_deadzone"))
        kh_config.stick_deadzone = clampi(atoi(v), 1, 3);
    else if (!strcmp(k, "r_toggle"))
        kh_config.r_toggle = atoi(v) != 0;
    else if (!strcmp(k, "fast_forward"))
        kh_config.fast_forward = clampi(atoi(v), 2, 3);
    else if (!strcmp(k, "debug"))
        kh_config.debug = atoi(v) != 0;
    else if (!strncmp(k, "button_", 7))
        for (b = 0; b < KH_BTN_COUNT; b++)
            if (!strcmp(k, s_ds_buttons[b]))
                for (i = 0; i < KH_VITA_BUTTONS; i++)
                    if (!strcmp(v, s_vita_buttons[i].name))
                        kh_config.button[b] = s_vita_buttons[i].mask;
}

void config_load(void)
{
    char line[256];
    int version = 0;
    FILE *f = fopen(KH_CONFIG_PATH, "r");

    kh_config = s_defaults;
    if (!f) {
        config_save();
        LOG("config: %s written with the defaults", KH_CONFIG_PATH);
    } else {
        /* the version first: older files name some settings differently */
        while (fgets(line, sizeof(line), f)) {
            char *k = trim(line), *v = strchr(k, '=');
            if (*k == '#' || *k == ';' || !v)
                continue;
            *v++ = 0;
            if (!strcmp(trim(k), "config_version"))
                version = atoi(trim(v));
        }
        rewind(f);
        while (fgets(line, sizeof(line), f)) {
            char *k = trim(line), *v = strchr(k, '=');
            if (*k == '#' || *k == ';' || !v)
                continue;
            *v++ = 0;
            set(trim(k), trim(v), version);
        }
        fclose(f);
        if (version < CONFIG_VERSION) {
            /* a file from an older build: the 3D default went from 2x to 3x (the Vita's full
             * height), and the newer settings get written out with it */
            if (version < 2 && kh_config.render_scale == 2)
                kh_config.render_scale = 3;
            /* the smooth texture filter became the default with the port menu's tabs */
            if (version < 8)
                kh_config.texture_filter = 1;
            config_save();
            LOG("config: upgraded from version %d", version);
        }
    }
    kh_log_verbose = kh_config.debug;
    LOG("config: render_scale %d, fps %d, aspect %d, layout %d, inset %d, texture_filter %d, "
        "camera %d/%d, dpad_deck %d, volume %d, debug %d", kh_config.render_scale,
        kh_config.frame_interpolation ? 60 : 30, kh_config.aspect, kh_config.layout,
        kh_config.inset_width, kh_config.texture_filter, kh_config.camera_stick,
        kh_config.camera_speed, kh_config.dpad_deck, kh_config.volume, kh_config.debug);
}
