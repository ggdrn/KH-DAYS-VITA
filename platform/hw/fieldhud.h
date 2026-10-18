/* The single screen's field HUD after khdays-port's one-screen layout (its
 * port/ui/compositions/port_days.txt, MIT): the hearts at the top left (their flat 3D moved with
 * their 2D), the map under the enemy's health at the top right, a challenge's label at the top.
 * The deck, the party, the enemy's health (and the rows under it) and the mission's panel
 * (config panel 4, at the bottom, shown when it changes) stay the panels' single screen's; its
 * map and target panels give way to these pieces. */
#ifndef KH_HW_FIELDHUD_H
#define KH_HW_FIELDHUD_H

#include <stdint.h>

/* The display thread, each frame. active: the panels' single screen in the field (not a
 * tutorial page); paused: the pause menu is up; dialog: a dialogue box is open (gpu2d's
 * centre BG3); lower: the bottom screen's last 2D picture (256x192, red first). */
void kh_fieldhud_frame(int active, int paused, int dialog, const uint32_t *lower);

#endif
