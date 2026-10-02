/* The port menu (L+R+Select): the port's settings while the game runs. See portmenu.c. */
#ifndef KH_PORTMENU_H
#define KH_PORTMENU_H

#include <stdint.h>

int portmenu_is_open(void);
/* L+R+Select: open, or close and write config.ini. */
void portmenu_toggle(void);
/* The Vita's buttons as held (SCE_CTRL_*), every VBlank while the menu is open. */
void portmenu_input(uint32_t buttons, uint64_t now_us);
/* The menu drawn on the overlay layer (VIDEO_OVERLAY_W x VIDEO_OVERLAY_H). */
const uint32_t *portmenu_render(void);
/* The overlay with only a frame-rate counter in the top-left corner. */
const uint32_t *portmenu_render_fps(const char *label);

/* Set by the platform: what a change needs beyond the setting (a new 3D target, textures
 * decoded again, the mixer's volume). */
extern void (*portmenu_on_scale)(int scale);
extern void (*portmenu_on_texture_filter)(void);
extern void (*portmenu_on_volume)(int percent);

#endif
