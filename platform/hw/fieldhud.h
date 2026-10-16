/* The single screen's field HUD as khdays-port lays it out on one screen (its
 * port/ui/compositions/port_days.txt, MIT): the mission's panel at the top left with the hearts
 * under it, the enemy's health at the top right with the map and the sigils under it, a
 * challenge's label at the top. The deck and the party stay the panels' single screen's (config
 * hud_size); its map, target and gauge panels give way to these pieces. */
#ifndef KH_HW_FIELDHUD_H
#define KH_HW_FIELDHUD_H

#include <stdint.h>

/* The display thread, each frame. active: the panels' single screen in the field (not a
 * tutorial page); paused: the pause menu is up; dialog: a dialogue box is open (gpu2d's
 * centre BG3); lower: the bottom screen's last 2D picture (256x192, red first). */
void kh_fieldhud_frame(int active, int paused, int dialog, const uint32_t *lower);

#endif
