/* ux0:data/khdays/config.ini: key = value lines, '#' or ';' comments. Written with the defaults
 * when missing, so the user has something to edit. */
#ifndef KH_CONFIG_H
#define KH_CONFIG_H

#include <stdint.h>

/* the DS buttons a Vita button can be given to, in kh_config.button[] */
enum { KH_BTN_A, KH_BTN_B, KH_BTN_X, KH_BTN_Y, KH_BTN_L, KH_BTN_R, KH_BTN_START, KH_BTN_SELECT,
       KH_BTN_COUNT };

typedef struct {
    int render_scale;   /* 3D internal resolution: 1-4 times the DS's 256x192 (default 3) */
    int layout;         /* starting screen layout: 0 top main, 1 bottom main, 2 side by side */
    int inset_width;    /* width in pixels of the small screen (4:3), 128-480 */
    int debug;          /* debug hotkeys and the detailed log: 0 off (default), 1 on */
    int texture_filter; /* 3D textures: 0 sharp as on the DS, 1 smoothed (bilinear) */
    int widescreen;     /* 3D drawn for 16:9 when its screen fills the display: 0 off, 1 on */
    uint32_t button[KH_BTN_COUNT]; /* the Vita button (SCE_CTRL_*) for each DS button */
} KhConfig;

extern KhConfig kh_config;

void config_load(void);

#endif
