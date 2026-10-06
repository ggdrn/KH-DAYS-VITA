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

/* The engine's graphics screen (BGs, sprites, 3D codes) whatever DISPCNT's display mode, before
 * master brightness: what the display capture's source A sees. Returns 1 when it has 3D pixels. */
int kh_gpu2d_render_graphics(int engine, uint32_t *fb);

/* An engine's DISPCNT to draw with instead of the register's (0: the register): the value
 * recorded with a dual-3D frame, the register having moved on to the next one. */
extern volatile uint32_t kh_gpu2d_dispcnt_override[2];
/* An engine drawn without its fades (master brightness, brighten/darken): the single screen's
 * panels while the game is paused, which the game dims behind its pause menu. */
extern volatile int kh_gpu2d_plain[2];
/* HUD size (gpu2d.c hud_codes): 1 counts the BG3 pixels of the top screen's bottom middle
 * (a dialogue box) into kh_gpu2d_center_bg3; 2 also gives the HUD's pixels in its corners codes
 * of their own (2D 0xfe, blended 0xe1, sprite-blended 0xa1-0xb0) that the composition draws
 * again smaller. */
extern volatile int kh_gpu2d_hud_mode;
extern volatile uint32_t kh_gpu2d_center_bg3;
/* the INFORMATION bar along the top: seen in the picture being drawn / the last one */
extern volatile int kh_gpu2d_banner_now, kh_gpu2d_banner_last;
/* per corner (top-left, bottom-left, bottom-right): the lines holding the HUD's layers in the
 * picture being drawn, and the lines (y0, y1) marked: the block joined to the screen's edge, as
 * found in the last picture */
extern volatile uint8_t kh_gpu2d_zone_rows[3][192];
extern volatile int kh_gpu2d_zone_lim[3][2];
/* the box the marked HUD fills in each corner (top-left, bottom-left, bottom-right: x0 y0 x1
 * y1) in the pictures since it was last reset to { 256, 192, 0, 0 } */
extern volatile int32_t kh_gpu2d_hud_box[3][4];

/* Diagnosis, with kh_gpu2d_profiling set: since the last call, per engine, the CPU time (us)
 * of the BGs, the sprites and whole lines, and the sprite lines drawn as tiles, bitmaps and
 * affine. */
extern volatile int kh_gpu2d_profiling;
void kh_gpu2d_take_profile(uint32_t us[2][3], uint32_t sprites[2][3]);

#endif
