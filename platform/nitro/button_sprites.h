/* The Vita's buttons in the game's graphics (nitro/button_sprites.c). */
#ifndef KH_BUTTON_SPRITES_H
#define KH_BUTTON_SPRITES_H

#include <stdint.h>

/* A texture's texels (n bytes) about to be decoded: when they are one the port redraws (the
 * combo prompt), the redrawn copy into out and 1; else 0. */
int kh_button_texture(const uint8_t *texels, uint32_t n, uint8_t *out);

#endif
