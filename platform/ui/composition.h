/* The one-screen picture's compositions: for each situation, which screens it shows and the
 * shapes that place pieces of the screens in it, read from a compositions file
 * (ux0:data/khdays/layouts/compositions.txt, else the port's own, app0:layouts/compositions.txt).
 * Ported from khdays-port's port/ui/composition.c (MIT; the format: its docs/COMPOSITIONS.md,
 * assets/onescreen_ref). The animated pictures (`backdrop *.txt`) and the 3D shapes (`shape3d`)
 * are read and left out here. Plain C. */
#ifndef KH_UI_COMPOSITION_H
#define KH_UI_COMPOSITION_H

#include "situation.h"

#include <stdint.h>

enum {
    KH_SCREENS_UPPER,       /* the picture's ground: the 3D, the upper screen's backdrop */
    KH_SCREENS_LOWER,       /* the 3D, the lower screen's backdrop */
    KH_SCREENS_STACKED,     /* the two screens one above the other */
    KH_SCREENS_SIDE_BY_SIDE /* both whole, the upper on the left */
};

/* A shape's flags (khdays-port's gfx/onescreen.h KOF_*). */
#define KH_SHAPE_LOWER 0x1u       /* from the lower screen, else the upper one's 2D */
#define KH_SHAPE_MIRROR_X 0x4u
#define KH_SHAPE_MIRROR_Y 0x8u
#define KH_SHAPE_REPEAT_X 0x40u   /* its source repeated along its rows, across the picture */
#define KH_SHAPE_REPEAT_Y 0x80u
#define KH_SHAPE_FORCE 0x100u     /* its texels used where they are invisible too */
#define KH_SHAPE_ROUND 0x400u     /* its corners rounded */
#define KH_SHAPE_WEIGHT 0x800u    /* opacity and fade weigh it against what is under it */
#define KH_SHAPE_SEE_THROUGH 0x2000u
#define KH_SHAPE_DIM 0x4000u      /* its colours times `opacity`, toward black */
#define KH_SHAPE_3D 0x8000u       /* the picture's 3D where it is, not its source */

#define KH_SHAPE_KEYS 4

/* A shape built for this frame, in the picture's DS dots (C x 192). */
typedef struct KhShape {
    uint32_t flags;
    float src[4];  /* x, y, w, h: DS dots of its screen */
    float dst[4];  /* x1, y1, x2, y2: DS dots of the picture */
    float fade[4]; /* left, top, right, bottom: picture dots */
    float opacity; /* weight's or dim's factor */
    float round;   /* the corners' radius, source dots */
    uint32_t see_through; /* BGR555 (bits 0-14) drawn at an opacity (bits 16-23, of 255) */
    uint16_t keys[KH_SHAPE_KEYS]; /* BGR555 colours made transparent */
    int key_count;
} KhShape;

#define KH_COMPOSITION_SHAPES 32

typedef struct KhComposition {
    int screens;   /* KH_SCREENS_* */
    int brightness; /* 0 default, 1 upper, 2 lower, 3 halves, 4 own */
    int hide3d_all;
    int hide3d_hud; /* `hide3d hud`: the HUD's flat 3D pieces not drawn */
    /* `shape3d hud`: the HUD's flat 3D pieces wholly inside a rectangle of the DS's screen
     * (src: x, y, w, h, DS dots) moved, its corner to (x, y) of the picture, scaled (picture
     * dots a DS dot); the first one the composition gives */
    int hud3d;
    float hud3d_src[4], hud3d_x, hud3d_y, hud3d_scale;
    int shape_count;
    KhShape shapes[KH_COMPOSITION_SHAPES];
    /* a static picture behind everything (`backdrop <file>`, a PNG of the layouts folder):
     * its name ("" none), its anchor (0 left / top .. 1 right / bottom) and `fit` */
    char backdrop[128];
    float backdrop_ax, backdrop_ay;
    int backdrop_fit;
} KhComposition;

/* Reads a compositions file's text (modified in place); `file` names it in the reports. Returns
 * the sections read; the reports go to `say`. A text that does not start with `units port` is
 * refused (-1). */
int kh_composition_read(const char *file, char *text, void (*say)(const char *line));

/* The player's HUD factor (1 at the port's default size). */
void kh_composition_set_hud_factor(float f);

/* Whether a backdrop (its name as written) is there to show: the platform's answer. Names with
 * the title's state ({lang}...) and animated pictures (.txt) are never there here. */
void kh_composition_set_backdrop_check(int (*there)(const char *name));

/* The composition for a situation in a picture `columns` DS dots wide. */
void kh_composition_for(const KhSituation *situation, int columns, KhComposition *out);

#endif
