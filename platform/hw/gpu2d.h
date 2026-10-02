/* The DS 2D display engines (A and B): backgrounds, sprites, windows and colour effects,
 * rendered from the state the port keeps for them (I/O registers, palettes, OAM, VRAM). */
#ifndef KH_HW_GPU2D_H
#define KH_HW_GPU2D_H

#include <stdint.h>

#define KH_GPU2D_W 256
#define KH_GPU2D_H 192

enum { KH_ENGINE_A = 0, KH_ENGINE_B = 1 };

/* The alpha byte of an output pixel: 255 for a finished 2D pixel. With the 3D layer on
 * (engine A, DISPCNT bit 3), a pixel whose frontmost layer is 3D holds the colour under it and
 * one of these codes, the low 5 bits being EVY for the brightness effects. */
enum {
    KH_GPU2D_2D = 0xff,
    KH_GPU2D_3D = 0x00,
    KH_GPU2D_3D_BRIGHTEN = 0x40,
    KH_GPU2D_3D_DARKEN = 0x80,
    /* the pixel's colour is a 2D layer blended over the 3D one: OVER_3D | EVA (1-16, EVB the
     * rest, a bitmap sprite's alpha), or BLEND_3D with BLDALPHA's EVA/EVB */
    KH_GPU2D_OVER_3D = 0xc0,
    KH_GPU2D_BLEND_3D = 0xe0,
};

/* Render one engine's whole frame into fb (256x192, RGBA8888 in memory order R, G, B, A).
 * Returns 1 when the frame has 3D pixels to composite (codes above); master brightness is then
 * left to the composition. */
int kh_gpu2d_render(int engine, uint32_t *fb);

/* Lines y0..y1-1 only (fb is still the whole screen), from any thread: bands of one frame can
 * be rendered in parallel. kh_gpu2d_init first. */
void kh_gpu2d_init(void);
int kh_gpu2d_render_lines(int engine, uint32_t *fb, int y0, int y1);

/* Diagnosis: one engine's BGs and sprites, each on its own (256x192 RGBA, alpha 255 where the
 * layer has a pixel). */
void kh_gpu2d_dump_layers(int engine, uint32_t *bg[4], uint32_t *obj);

#endif
