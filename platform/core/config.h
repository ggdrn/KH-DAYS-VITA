/* ux0:data/khdays/config.ini: key = value lines, '#' or ';' comments. Written with the defaults
 * when missing, so the user has something to edit; the port menu (L+R+Select) edits the same
 * settings and writes the file back. */
#ifndef KH_CONFIG_H
#define KH_CONFIG_H

#include <stdint.h>

/* the DS buttons a Vita button can be given to, in kh_config.button[] */
enum { KH_BTN_A, KH_BTN_B, KH_BTN_X, KH_BTN_Y, KH_BTN_L, KH_BTN_R, KH_BTN_START, KH_BTN_SELECT,
       KH_BTN_COUNT };

/* the single-screen panels (config panel): where each one is anchored on the Vita's display */
#define KH_PANELS 4
enum { KH_PANEL_TOP_LEFT, KH_PANEL_TOP_RIGHT, KH_PANEL_BOTTOM_LEFT, KH_PANEL_BOTTOM_RIGHT,
       KH_PANEL_TOP_CENTER, KH_PANEL_BOTTOM_CENTER };
enum { KH_PANEL_SX, KH_PANEL_SY, KH_PANEL_SW, KH_PANEL_SH, KH_PANEL_ANCHOR, KH_PANEL_DX,
       KH_PANEL_DY, KH_PANEL_SCALE, KH_PANEL_STYLE, KH_PANEL_RADIUS, KH_PANEL_AUTOHIDE,
       KH_PANEL_FIELDS };
/* a panel's colours: as on the DS, white turned black (the target's frame), or the greys
 * inverted (the map: black ground, white walls) */
enum { KH_STYLE_PLAIN, KH_STYLE_WHITE_TO_BLACK, KH_STYLE_INVERT_GREYS };
/* when a panel shows (autohide): always; 7 s when it changes, and while paused or pinned with
 * Select; only while paused or pinned; only while its top rows hold red (the TARGET tab, not the
 * world picture of a mission without a target) */
enum { KH_SHOW_ALWAYS, KH_SHOW_ON_CHANGE, KH_SHOW_ON_PAUSE, KH_SHOW_ON_RED };

/* how the large screen fills the 16:9 display */
enum { KH_ASPECT_WIDE, KH_ASPECT_STRETCH, KH_ASPECT_4_3 };

typedef struct {
    int render_scale;   /* 3D internal resolution: 1-4 times the DS's 256x192 (default 3) */
    int layout;         /* starting screen layout: 0 top main, 1 bottom main, 2 side by side */
    int inset_width;    /* width in pixels of the small screen (4:3), 128-480 */
    int aspect;         /* KH_ASPECT_*: widescreen 3D, the DS picture stretched, or 4:3 */
    int frame_interpolation; /* 3D at 60 fps with frames mixed in between: 0 off, 1 on */
    int texture_filter; /* 3D textures: 0 sharp as on the DS, 1 smoothed (bilinear) */
    int filter_2d;      /* the 2D (sprites, text, menus) scaled up: 0 pixels as they are,
                         * 1 sharp (square pixels, smoothed edges), 2 smooth (bilinear,
                         * the default) */
    int camera_stick;   /* the right stick turns the field camera: 0 off, 1 on */
    int camera_speed;   /* 1 slow, 2 normal, 3 fast */
    int camera_invert_x, camera_invert_y;
    int dpad_deck;      /* the d-pad moves the command deck's cursor in the field: 0 off, 1 on */
    int show_fps;       /* a frame-rate counter in a corner: 0 off, 1 on */
    int volume;         /* 0-100 % of the game's own volume */
    int hud;            /* widescreen field: 0 the 2D stretched over 16:9, 1 kept 4:3 in the middle */
    int screen_effect;  /* 0 none, 1 scanlines, 2 LCD grid */
    int inset_corner;   /* the small screen: 0 top-right, 1 top-left, 2 bottom-right, 3 bottom-left */
    int inset_opacity;  /* the small screen's opacity, 50-100 % */
    int rear_touch;     /* rear touchpad halves: 0 off, 1 L / R, 2 Select / Start */
    int stick_deadzone; /* 1 small, 2 normal, 3 large */
    int r_toggle;       /* the DS's R: 0 held as on the DS, 1 a press latches it until the next */
    int fast_forward;   /* the speed L+R+Square switches to: 2 or 3 times */
    int mission_balance; /* Mission Mode's enemies: 0 as the game has them (HP x3 and harder
                          * hits, made for four players, solo too), 1 balanced (HP x1.5, and
                          * damage taken as in the story, by the save's difficulty) */
    int single_screen;  /* experimental: in missions, the top screen alone over the whole display,
                         * with parts of the bottom screen as panels over it: 0 off, 1 on */
    int panel_opacity;  /* those panels' opacity, 50-100 % */
    /* the panels: the DS bottom screen's rectangle (sx, sy, sw, sh), where it goes on the Vita
     * (anchor KH_PANEL_*, dx/dy in Vita pixels from that corner or edge), its size (scale, %
     * of the DS pixel; 100 = one Vita pixel per DS pixel), its colours (style KH_STYLE_*), the
     * radius of its corners in Vita pixels, and whether it only shows for a while when its
     * contents change (autohide 1; Start and Select pin it). sw 0: unused. */
    int panel[KH_PANELS][KH_PANEL_FIELDS];
    int hud_size;       /* the field HUD's blocks (commands, HP, chain, target), 60-100 % */
    int language;       /* the game's language, read at boot: 1 English, 2 French, 3 German,
                         * 4 Italian, 5 Spanish (the European cartridge's five) */
    int debug;          /* debug hotkeys and the detailed log: 0 off (default), 1 on */
    uint32_t button[KH_BTN_COUNT]; /* the Vita button (SCE_CTRL_*) for each DS button */
} KhConfig;

extern KhConfig kh_config;

void config_load(void);
/* Write the current settings to config.ini (the port menu, when it closes). */
void config_save(void);
/* Every setting back to its default (not saved). */
void config_defaults(void);

/* The Vita buttons a DS button can be mapped to, for the menu: name and SCE_CTRL_* mask. */
#define KH_VITA_BUTTONS 8
const char *config_vita_button_name(int i);
uint32_t config_vita_button_mask(int i);
int config_vita_button_index(uint32_t mask);
/* config mission_balance, for the game's code (decomp, PLATFORM_VITA) */
int kh_vita_mission_balance(void);

#endif
