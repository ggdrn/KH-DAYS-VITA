/* Presentation: the two finished 256x192 DS screens are uploaded as textures and drawn scaled on
 * the 960x544 display. How the DS image itself is produced (2D engines, 3D engine) lives in
 * platform/hw; this module only puts pixels on the screen. */
#ifndef KH_VIDEO_H
#define KH_VIDEO_H

#include <stdint.h>

#define DS_SCREEN_W 256
#define DS_SCREEN_H 192

typedef enum {
    LAYOUT_SIDE_BY_SIDE = 0, /* both screens, 480x360 each */
    LAYOUT_TOP_FOCUS,        /* top screen large, touch screen small on the right */
    LAYOUT_BOTTOM_FOCUS,     /* the reverse */
    LAYOUT_COUNT
} ScreenLayout;

typedef struct {
    int x, y, w, h;
} ScreenRect;

void video_init(void);
void video_set_layout(ScreenLayout layout);
ScreenLayout video_layout(void);
/* Where the bottom (touch) screen currently is on the display, for mapping touch input. */
ScreenRect video_bottom_rect(void);
/* top/bottom: 256x192 RGBA8888 pixels. */
void video_present(const uint32_t *top, const uint32_t *bottom);

#endif
