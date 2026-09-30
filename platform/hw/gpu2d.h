/* The DS 2D display engines (A and B): backgrounds, sprites, windows and colour effects,
 * rendered from the state the port keeps for them (I/O registers, palettes, OAM, VRAM). */
#ifndef KH_HW_GPU2D_H
#define KH_HW_GPU2D_H

#include <stdint.h>

#define KH_GPU2D_W 256
#define KH_GPU2D_H 192

enum { KH_ENGINE_A = 0, KH_ENGINE_B = 1 };

/* Render one engine's whole frame into fb (256x192, RGBA8888 in memory order R, G, B, A). */
void kh_gpu2d_render(int engine, uint32_t *fb);

#endif
