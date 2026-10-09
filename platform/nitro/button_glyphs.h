/* The Vita's face buttons in place of the DS's A, B, X and Y icons in the game's fonts. */
#ifndef KH_BUTTON_GLYPHS_H
#define KH_BUTTON_GLYPHS_H

/* After kh_romfs_init and before the game reads its fonts: config button_icons. */
void kh_button_glyphs_init(void);
/* The fonts' A and B glyphs drawn as Cross and Circle (swap 1: the menus confirm with Cross)
 * or as Circle and Cross (0), in the fonts the game has in memory. */
void kh_button_glyphs_menu(int swap);

#endif
