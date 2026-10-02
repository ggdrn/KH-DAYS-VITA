/* Presentation: the two finished 256x192 DS screens are uploaded as textures and drawn scaled on
 * the 960x544 display. How the DS image itself is produced (2D engines, 3D engine) lives in
 * platform/hw; this module only puts pixels on the screen. */
#ifndef KH_VIDEO_H
#define KH_VIDEO_H

#include <stdint.h>

#define DS_SCREEN_W 256
#define DS_SCREEN_H 192

typedef enum {
    LAYOUT_TOP_MAIN = 0,  /* top screen over the whole display (16:9), touch screen small */
    LAYOUT_BOTTOM_MAIN,   /* touch screen large (4:3), top screen small */
    LAYOUT_SIDE_BY_SIDE,  /* both screens, 480x360 each */
    LAYOUT_COUNT
} ScreenLayout;

typedef struct {
    int x, y, w, h;
} ScreenRect;

void video_init(void);
void video_set_layout(ScreenLayout layout);
ScreenLayout video_layout(void);
/* Where the bottom (touch) screen currently is on the display. */
ScreenRect video_bottom_rect(void);
/* Whether display pixel (960x544) lies on the small screen (touching it swaps the screens). */
int video_on_inset(int px, int py);
/* Display pixel (960x544) -> DS touch-screen pixel; 0 when it is not on the touch screen. */
int video_map_touch(int px, int py, int *x, int *y);
/* The screen drawn small (0 top, 1 bottom), -1 when both are full size. */
int video_inset_screen(void);
/* Swap which screen is the large one (top-main <-> bottom-main). */
void video_swap_screens(void);
/* A 480x272 RGBA layer drawn over everything, scaled to the full display (NULL: none). */
#define VIDEO_OVERLAY_W 480
#define VIDEO_OVERLAY_H 272
void video_set_overlay(const uint32_t *pixels);

/* For the next video_present: screen (0 top, 1 bottom, -1 none) carries engine A's 3D layer
 * (alpha codes from hw/gpu2d.h) to lay in from the GL texture tex, then the engine's master
 * brightness (its 0x0400006c value) applied after. */
/* hofs: the 3D layer's horizontal scroll (BG0HOFS, G3X_SetHOffset), in DS pixels. */
/* bldalpha: engine A's BLDALPHA, backdrop: its BGR555 backdrop colour, for 2D layers
 * blended over the 3D one */
void video_set_3d(int screen, unsigned tex, uint16_t master_bright, int hofs, uint16_t bldalpha,
                  uint16_t backdrop);

/* A CG shader program with attribs[i] at location i; 0 (logged) on failure. */
unsigned video_build_program(const char *vs, const char *fs, const char *const *attribs,
                             int nattribs);

/* top/bottom: 256x192 RGBA8888 pixels. */
void video_present(const uint32_t *top, const uint32_t *bottom);

#endif
