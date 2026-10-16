/* config single_screen 2: the one-screen picture of khdays-port's compositions. Each display
 * frame: the situation (ui/situation.h) from the game's data and the screens' pixels, then its
 * composition (ui/composition.h) handed to video.c. */
#ifndef KH_HW_ONESCREEN_H
#define KH_HW_ONESCREEN_H

#include <stdint.h>

/* The display thread, each frame. active: the compositions are wanted (config single_screen 2,
 * not a dual-3D scene the layouts handle); movie: a MobiClip movie plays; dual: the game shows
 * its 3D on both screens; upper/lower: the screens' last 2D pictures (256x192, red first);
 * vblank: the VBlank count. Returns 1 when a composition is drawn this frame. */
int kh_onescreen_frame(int active, int movie, int dual, const uint32_t *upper, const uint32_t *lower,
                       uint32_t vblank);

#endif
