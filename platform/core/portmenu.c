/* The port menu (L+R+Select): the port's settings while the game runs, drawn on the overlay
 * layer, in tabs switched with L and R. The game is held while it is open (no VBlank reaches
 * it, see platform/hw/game.c); changes apply at once, and config.ini is written when the menu
 * closes. */
#include "portmenu.h"

#include "config.h"
#include "font8x8.h"
#include "video.h"

#include <psp2/ctrl.h>
#include <stdio.h>
#include <string.h>

/* what a change of a setting needs besides the value itself; the platform sets the hooks */
void (*portmenu_on_scale)(int scale);
void (*portmenu_on_texture_filter)(void);
void (*portmenu_on_volume)(int percent);

#define W VIDEO_OVERLAY_W
#define H VIDEO_OVERLAY_H

static uint32_t s_px[W * H];
/* s_px holds the frame-rate label (not the menu, which draws over it) */
static int s_px_fps;
static volatile int s_open;
static int s_boot_language; /* the language this run started with (config language) */
static int s_tab, s_sel[8];
static uint32_t s_prev;
static uint64_t s_repeat_at;
static int s_repeat_dir;
static char s_note[64]; /* feedback after an action ("Defaults restored.") */

enum {
    IT_PRESET, IT_SCALE, IT_FPS, IT_TEXFILTER, IT_FILTER2D, IT_EFFECT, IT_SHOWFPS,
    IT_ASPECT, IT_HUD, IT_LAYOUT, IT_INSET, IT_CORNER, IT_OPACITY, IT_SINGLE, IT_PANELOP, IT_HUDSIZE,
    IT_CAMERA, IT_CAMSPEED, IT_INVX, IT_INVY, IT_DEADZONE, IT_DPAD, IT_RMODE, IT_REAR,
    IT_BUTTON, /* + the DS button index */
    IT_LANGUAGE, IT_VOLUME, IT_FFWD, IT_MISSION, IT_DEFAULTS, IT_CLOSE,
};

typedef struct {
    int kind, arg;
    const char *label;
    const char *help;
} Item;

static const Item s_video[] = {
    { IT_PRESET, 0, "Quality preset", "Sets resolution, frame rate and filter at once." },
    { IT_SCALE, 0, "3D resolution", "Internal 3D resolution. 4x smooths edges (more GPU)." },
    { IT_FPS, 0, "3D frame rate", "30: original. 60 (experimental): may shake and glitch." },
    { IT_TEXFILTER, 0, "Texture filter", "Smooth: bilinear filtering. Sharp: as on the DS." },
    { IT_FILTER2D, 0, "2D filter", "Sprites and text. Sharp: square pixels, smooth edges." },
    { IT_EFFECT, 0, "Screen effect", "Scanlines or an LCD grid over the DS's pixels." },
    { IT_SHOWFPS, 0, "Show FPS", "A frame-rate counter in the top-left corner." },
};
static const Item s_screen[] = {
    { IT_ASPECT, 0, "Aspect ratio", "Widescreen: the 3D drawn for 16:9 in the field." },
    { IT_HUD, 0, "Widescreen HUD", "The 2D stretched to 16:9, or kept 4:3 in the middle." },
    { IT_LAYOUT, 0, "Screen layout", "Touch the small screen to swap the screens." },
    { IT_INSET, 0, "Small screen size", "Width of the small screen, in Vita pixels." },
    { IT_CORNER, 0, "Small screen corner", NULL },
    { IT_OPACITY, 0, "Small screen opacity", "See the game through the small screen." },
    { IT_HUDSIZE, 0, "HUD size", "Commands, HP, chain (and map, target) in the field." },
    { IT_SINGLE, 0, "Single screen (beta)", "Missions on one screen: map and gauge as panels." },
    { IT_PANELOP, 0, "Panel opacity", "Single screen: see the game through the panels." },
};
static const Item s_controls[] = {
    { IT_CAMERA, 0, "Right stick camera", "Turns the field camera; locked on, switches target." },
    { IT_CAMSPEED, 0, "Camera speed", "How far to tilt the stick for full speed." },
    { IT_INVX, 0, "Invert camera X", NULL },
    { IT_INVY, 0, "Invert camera Y", NULL },
    { IT_DEADZONE, 0, "Stick dead zone", "How far the sticks move before they count." },
    { IT_DPAD, 0, "D-pad on command deck", "On: the d-pad moves the command cursor." },
    { IT_RMODE, 0, "R button (lock-on)", "One click: a click locks on, the next lets go." },
    { IT_REAR, 0, "Rear touchpad", "The rear touchpad's halves as two more buttons." },
};
static const Item s_buttons[] = {
    { IT_BUTTON, KH_BTN_A, "A", NULL },         { IT_BUTTON, KH_BTN_B, "B", NULL },
    { IT_BUTTON, KH_BTN_X, "X", NULL },         { IT_BUTTON, KH_BTN_Y, "Y", NULL },
    { IT_BUTTON, KH_BTN_L, "L", NULL },         { IT_BUTTON, KH_BTN_R, "R", NULL },
    { IT_BUTTON, KH_BTN_START, "Start", NULL }, { IT_BUTTON, KH_BTN_SELECT, "Select", NULL },
};
static const Item s_system[] = {
    { IT_LANGUAGE, 0, "Language", "The game's language. Applied the next time it starts." },
    { IT_VOLUME, 0, "Volume", NULL },
    { IT_FFWD, 0, "Fast-forward speed", "L+R+Square turns fast-forward on and off." },
    { IT_MISSION, 0, "Mission balance", "Story: enemy HP and damage taken as in the story." },
    { IT_DEFAULTS, 0, "Restore defaults", "Press Cross to set every option to its default." },
    { IT_CLOSE, 0, "Save and return", "Press Cross (or L+R+Select) to save and go back." },
};

static const struct {
    const char *name;
    const Item *items;
    int n;
} s_tabs[] = {
    { "VIDEO", s_video, sizeof(s_video) / sizeof(s_video[0]) },
    { "SCREEN", s_screen, sizeof(s_screen) / sizeof(s_screen[0]) },
    { "CONTROLS", s_controls, sizeof(s_controls) / sizeof(s_controls[0]) },
    { "BUTTONS", s_buttons, sizeof(s_buttons) / sizeof(s_buttons[0]) },
    { "SYSTEM", s_system, sizeof(s_system) / sizeof(s_system[0]) },
};
#define NTABS ((int)(sizeof(s_tabs) / sizeof(s_tabs[0])))

static const char *const s_vita_names[KH_VITA_BUTTONS] = { "Cross", "Circle", "Square",
                                                          "Triangle", "L", "R", "Start",
                                                          "Select" };

/* ---- presets ------------------------------------------------------------------------------ */

static const struct {
    const char *name;
    int scale, fps60, filter;
} s_presets[] = {
    { "Performance", 2, 0, 1 },
    { "Balanced", 3, 0, 1 },
    { "Quality", 4, 0, 1 },
};
#define NPRESETS 3

static int current_preset(void)
{
    int i;
    for (i = 0; i < NPRESETS; i++)
        if (kh_config.render_scale == s_presets[i].scale &&
            kh_config.frame_interpolation == s_presets[i].fps60 &&
            kh_config.texture_filter == s_presets[i].filter)
            return i;
    return -1;
}

static void apply_preset(int i)
{
    const int filter_changed = kh_config.texture_filter != s_presets[i].filter;
    kh_config.render_scale = s_presets[i].scale;
    kh_config.frame_interpolation = s_presets[i].fps60;
    kh_config.texture_filter = s_presets[i].filter;
    if (portmenu_on_scale)
        portmenu_on_scale(kh_config.render_scale);
    if (filter_changed && portmenu_on_texture_filter)
        portmenu_on_texture_filter();
}

/* ---- values ------------------------------------------------------------------------------- */

static void value(const Item *it, char *out, size_t n)
{
    static const char *const aspects[] = { "Widescreen 3D", "Stretched", "4:3 original" };
    static const char *const layouts[] = { "Top screen large", "Bottom screen large",
                                           "Side by side" };
    static const char *const speeds[] = { "", "Slow", "Normal", "Fast" };
    static const char *const effects[] = { "None", "Scanlines", "LCD grid" };
    static const char *const corners[] = { "Top right", "Top left", "Bottom right",
                                           "Bottom left" };
    static const char *const zones[] = { "", "Small", "Normal", "Large" };
    static const char *const rear[] = { "Off", "L / R", "Select / Start" };
    switch (it->kind) {
    case IT_PRESET: {
        const int p = current_preset();
        snprintf(out, n, "%s", p < 0 ? "Custom" : s_presets[p].name);
        break;
    }
    case IT_SCALE: snprintf(out, n, "%dx (%dx%d)", kh_config.render_scale,
                            256 * kh_config.render_scale, 192 * kh_config.render_scale); break;
    case IT_FPS: snprintf(out, n, "%s", kh_config.frame_interpolation ? "60 (experimental)" : "30"); break;
    case IT_TEXFILTER: snprintf(out, n, "%s", kh_config.texture_filter ? "Smooth" : "Sharp"); break;
    case IT_FILTER2D: {
        static const char *const f2d[] = { "Pixel", "Sharp", "Smooth" };
        snprintf(out, n, "%s", f2d[kh_config.filter_2d % 3]);
        break;
    }
    case IT_EFFECT: snprintf(out, n, "%s", effects[kh_config.screen_effect % 3]); break;
    case IT_SHOWFPS: snprintf(out, n, "%s", kh_config.show_fps ? "On" : "Off"); break;
    case IT_ASPECT: snprintf(out, n, "%s", aspects[kh_config.aspect % 3]); break;
    case IT_HUD: snprintf(out, n, "%s", kh_config.hud ? "Centered 4:3" : "Stretched"); break;
    case IT_LAYOUT: snprintf(out, n, "%s", layouts[kh_config.layout % 3]); break;
    case IT_INSET: snprintf(out, n, "%d", kh_config.inset_width); break;
    case IT_CORNER: snprintf(out, n, "%s", corners[kh_config.inset_corner & 3]); break;
    case IT_OPACITY: snprintf(out, n, "%d%%", kh_config.inset_opacity); break;
    case IT_SINGLE: snprintf(out, n, "%s", kh_config.single_screen ? "On" : "Off"); break;
    case IT_HUDSIZE: snprintf(out, n, "%d%%", kh_config.hud_size); break;
    case IT_PANELOP: snprintf(out, n, "%d%%", kh_config.panel_opacity); break;
    case IT_CAMERA: snprintf(out, n, "%s", kh_config.camera_stick ? "On" : "Off"); break;
    case IT_CAMSPEED: snprintf(out, n, "%s", speeds[kh_config.camera_speed & 3]); break;
    case IT_INVX: snprintf(out, n, "%s", kh_config.camera_invert_x ? "Yes" : "No"); break;
    case IT_INVY: snprintf(out, n, "%s", kh_config.camera_invert_y ? "Yes" : "No"); break;
    case IT_DEADZONE: snprintf(out, n, "%s", zones[kh_config.stick_deadzone & 3]); break;
    case IT_DPAD: snprintf(out, n, "%s", kh_config.dpad_deck ? "On" : "Off"); break;
    case IT_RMODE: snprintf(out, n, "%s", kh_config.r_toggle ? "One click" : "As on DS"); break;
    case IT_REAR: snprintf(out, n, "%s", rear[kh_config.rear_touch % 3]); break;
    case IT_BUTTON:
        snprintf(out, n, "%s", s_vita_names[config_vita_button_index(kh_config.button[it->arg])]);
        break;
    case IT_LANGUAGE: {
        static const char *const langs[] = { "English", "Francais", "Deutsch", "Italiano",
                                             "Espanol" };
        /* "(restart)" while it differs from the language this run started with */
        snprintf(out, n, "%s%s", langs[(kh_config.language - 1) % 5],
                 kh_config.language != s_boot_language ? " (restart)" : "");
        break;
    }
    case IT_VOLUME: snprintf(out, n, "%d%%", kh_config.volume); break;
    case IT_FFWD: snprintf(out, n, "%dx", kh_config.fast_forward); break;
    case IT_MISSION: snprintf(out, n, "%s", kh_config.mission_balance ? "Story" : "Original"); break;
    default: out[0] = 0; break;
    }
}

static int wrap(int v, int lo, int hi, int d)
{
    v += d;
    return v < lo ? hi : v > hi ? lo : v;
}

/* a left/right on the item: the next or previous value, applied at once */
static void change(const Item *it, int d)
{
    switch (it->kind) {
    case IT_PRESET: {
        const int p = current_preset();
        apply_preset(p < 0 ? (d > 0 ? 0 : NPRESETS - 1) : wrap(p, 0, NPRESETS - 1, d));
        break;
    }
    case IT_SCALE:
        kh_config.render_scale = wrap(kh_config.render_scale, 1, 4, d);
        if (portmenu_on_scale)
            portmenu_on_scale(kh_config.render_scale);
        break;
    case IT_FPS: kh_config.frame_interpolation ^= 1; break;
    case IT_TEXFILTER:
        kh_config.texture_filter ^= 1;
        if (portmenu_on_texture_filter)
            portmenu_on_texture_filter();
        break;
    case IT_FILTER2D: kh_config.filter_2d = wrap(kh_config.filter_2d, 0, 2, d); break;
    case IT_EFFECT: kh_config.screen_effect = wrap(kh_config.screen_effect, 0, 2, d); break;
    case IT_SHOWFPS: kh_config.show_fps ^= 1; break;
    case IT_ASPECT:
        kh_config.aspect = wrap(kh_config.aspect, 0, 2, d);
        video_relayout();
        break;
    case IT_HUD: kh_config.hud ^= 1; break;
    case IT_LAYOUT:
        kh_config.layout = wrap(kh_config.layout, 0, 2, d);
        video_set_layout((ScreenLayout)kh_config.layout);
        break;
    case IT_INSET:
        kh_config.inset_width = wrap(kh_config.inset_width - kh_config.inset_width % 32, 128,
                                     480, d * 32);
        video_relayout();
        break;
    case IT_CORNER:
        kh_config.inset_corner = wrap(kh_config.inset_corner, 0, 3, d);
        video_relayout();
        break;
    case IT_OPACITY:
        kh_config.inset_opacity = wrap(kh_config.inset_opacity, 50, 100, d * 25);
        break;
    case IT_SINGLE: kh_config.single_screen ^= 1; break;
    case IT_HUDSIZE: kh_config.hud_size = wrap(kh_config.hud_size, 60, 100, d * 10); break;
    case IT_PANELOP:
        kh_config.panel_opacity = wrap(kh_config.panel_opacity, 50, 100, d * 10);
        break;
    case IT_CAMERA: kh_config.camera_stick ^= 1; break;
    case IT_CAMSPEED: kh_config.camera_speed = wrap(kh_config.camera_speed, 1, 3, d); break;
    case IT_INVX: kh_config.camera_invert_x ^= 1; break;
    case IT_INVY: kh_config.camera_invert_y ^= 1; break;
    case IT_DEADZONE: kh_config.stick_deadzone = wrap(kh_config.stick_deadzone, 1, 3, d); break;
    case IT_DPAD: kh_config.dpad_deck ^= 1; break;
    case IT_RMODE: kh_config.r_toggle ^= 1; break;
    case IT_REAR: kh_config.rear_touch = wrap(kh_config.rear_touch, 0, 2, d); break;
    case IT_BUTTON: {
        const int i = wrap(config_vita_button_index(kh_config.button[it->arg]), 0,
                           KH_VITA_BUTTONS - 1, d);
        kh_config.button[it->arg] = config_vita_button_mask(i);
        break;
    }
    case IT_LANGUAGE: kh_config.language = wrap(kh_config.language, 1, 5, d); break;
    case IT_VOLUME:
        kh_config.volume = wrap(kh_config.volume, 0, 100, d * 10);
        if (portmenu_on_volume)
            portmenu_on_volume(kh_config.volume);
        break;
    case IT_FFWD: kh_config.fast_forward = wrap(kh_config.fast_forward, 2, 3, d); break;
    case IT_MISSION: kh_config.mission_balance ^= 1; break;
    default: break;
    }
}

int portmenu_is_open(void)
{
    return s_open;
}

static void close_menu(void)
{
    s_open = 0;
    config_save();
}

void portmenu_toggle(void)
{
    if (!s_boot_language)
        s_boot_language = kh_config.language;
    if (s_open) {
        close_menu();
        return;
    }
    s_open = 1;
    s_note[0] = 0;
    s_prev = 0xffffffffu; /* the buttons that opened it count as held */
}

void portmenu_input(uint32_t buttons, uint64_t now_us)
{
    const uint32_t pressed = buttons & ~s_prev;
    const uint32_t dirs = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT;
    const Item *items = s_tabs[s_tab].items;
    const int n = s_tabs[s_tab].n;
    int dir = 0, *sel = &s_sel[s_tab];

    if (!s_open)
        return;
    s_prev = buttons;
    /* L and R switch tabs (not while both are held: that is the way out, L+R+Select) */
    if ((pressed & SCE_CTRL_LTRIGGER) && !(buttons & SCE_CTRL_RTRIGGER)) {
        s_tab = (s_tab + NTABS - 1) % NTABS;
        s_note[0] = 0;
        return;
    }
    if ((pressed & SCE_CTRL_RTRIGGER) && !(buttons & SCE_CTRL_LTRIGGER)) {
        s_tab = (s_tab + 1) % NTABS;
        s_note[0] = 0;
        return;
    }
    /* the d-pad repeats when held */
    if (pressed & dirs) {
        dir = (int)(pressed & dirs);
        s_repeat_dir = dir;
        s_repeat_at = now_us + 350000;
    } else if ((buttons & (uint32_t)s_repeat_dir) && now_us >= s_repeat_at) {
        dir = s_repeat_dir;
        s_repeat_at = now_us + 90000;
    }
    if (dir & SCE_CTRL_UP)
        *sel = (*sel + n - 1) % n;
    if (dir & SCE_CTRL_DOWN)
        *sel = (*sel + 1) % n;
    if (dir & SCE_CTRL_LEFT)
        change(&items[*sel], -1);
    if (dir & SCE_CTRL_RIGHT)
        change(&items[*sel], 1);
    if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) {
        if (items[*sel].kind == IT_CLOSE) {
            close_menu();
        } else if (items[*sel].kind == IT_DEFAULTS) {
            config_defaults();
            if (portmenu_on_scale)
                portmenu_on_scale(kh_config.render_scale);
            if (portmenu_on_texture_filter)
                portmenu_on_texture_filter();
            if (portmenu_on_volume)
                portmenu_on_volume(kh_config.volume);
            video_set_layout((ScreenLayout)kh_config.layout);
            snprintf(s_note, sizeof(s_note), "Defaults restored.");
        } else {
            change(&items[*sel], 1);
        }
    }
}

/* ---- drawing ------------------------------------------------------------------------------ */

static void fill(int x0, int y0, int x1, int y1, uint32_t c)
{
    int x, y;
    for (y = y0 < 0 ? 0 : y0; y < y1 && y < H; y++)
        for (x = x0 < 0 ? 0 : x0; x < x1 && x < W; x++)
            s_px[y * W + x] = c;
}

static void text(int x, int y, const char *s, uint32_t fg)
{
    for (; *s && x + 8 <= W; s++, x += 8) {
        const unsigned char c = (unsigned char)*s;
        const uint8_t *g = font8x8_basic[(c >= 0x20 && c < 0x80) ? c - 0x20 : '?' - 0x20];
        int gy, gx;
        if (x < 0)
            continue;
        for (gy = 0; gy < 8; gy++)
            for (gx = 0; gx < 8; gx++)
                if (g[gy] & (1 << gx))
                    s_px[(y + gy) * W + x + gx] = fg;
    }
}

/* colours: 0xAABBGGRR */
#define C_PANEL 0xe0180c08u
#define C_BAR 0xff402018u
#define C_TAB 0xff302010u
#define C_TAB_ON 0xff6a3a20u
#define C_SEL 0xff6a3a20u
#define C_TITLE 0xff80e0ffu
#define C_TEXT 0xffffffffu
#define C_VALUE 0xffc0c0c0u
#define C_VALUE_ON 0xff60ffffu
#define C_HELP 0xffe0e0e0u
#define C_HINT 0xffa0a0a0u

#define LIST_Y 46
#define ROW_H 14

const uint32_t *portmenu_render(void)
{
    const Item *items = s_tabs[s_tab].items;
    const int n = s_tabs[s_tab].n, sel = s_sel[s_tab];
    char v[48], line[64];
    int i, x;

    fill(0, 0, W, H, C_PANEL);
    fill(0, 0, W, 16, C_BAR);
    text(8, 4, "KINGDOM HEARTS 358/2 DAYS - PORT SETTINGS", C_TITLE);

    /* the tabs, with the L / R hint at the ends */
    fill(0, 18, W, 34, C_TAB);
    text(4, 22, "L", C_HINT);
    text(W - 12, 22, "R", C_HINT);
    for (i = 0, x = 20; i < NTABS; i++) {
        const int w = (int)strlen(s_tabs[i].name) * 8 + 16;
        if (i == s_tab)
            fill(x, 18, x + w, 34, C_TAB_ON);
        text(x + 8, 22, s_tabs[i].name, i == s_tab ? C_TITLE : C_VALUE);
        x += w + 4;
    }

    for (i = 0; i < n; i++) {
        const Item *it = &items[i];
        const int y = LIST_Y + i * ROW_H;
        if (i == sel)
            fill(12, y - 3, W - 12, y + 11, C_SEL);
        if (it->kind == IT_BUTTON)
            snprintf(line, sizeof(line), "DS %s button", it->label);
        else
            snprintf(line, sizeof(line), "%s", it->label);
        text(24, y, line, C_TEXT);
        value(it, v, sizeof(v));
        if (v[0]) {
            snprintf(line, sizeof(line), "< %s >", v);
            text(W - 24 - (int)strlen(line) * 8, y, line, i == sel ? C_VALUE_ON : C_VALUE);
        }
    }

    fill(0, H - 34, W, H, C_BAR);
    if (s_note[0])
        text(8, H - 30, s_note, 0xff80ff80u);
    else if (items[sel].kind == IT_BUTTON)
        text(8, H - 30, "The Vita button that acts as this DS button.", C_HELP);
    else if (items[sel].help)
        text(8, H - 30, items[sel].help, C_HELP);
    text(8, H - 14, "L/R tab  Up/Down pick  Left/Right change  L+R+Select exit", C_HINT);
    video_overlay_changed();
    s_px_fps = 0;
    return s_px;
}

/* the frame-rate counter alone: the rest of the layer clear */
const uint32_t *portmenu_render_fps(const char *label)
{
    static char last[48];
    /* redrawn when the label changes (once a second) or the menu was drawn: before, the
     * label's own background at (0, 0) made it redrawn and uploaded every frame, and vitaGL
     * copied the whole overlay texture for each upload, 10 ms a frame (0.1.27's log) */
    if (strcmp(last, label) != 0 || !s_px_fps) {
        s_px_fps = 1;
        snprintf(last, sizeof(last), "%s", label);
        memset(s_px, 0, sizeof(s_px));
        fill(0, 0, 8 + (int)strlen(label) * 8, 12, 0x90000000u);
        text(4, 2, label, 0xff60ff60u);
        video_overlay_changed();
    }
    return s_px;
}
