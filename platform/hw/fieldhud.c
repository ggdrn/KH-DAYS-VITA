/* The single screen's field HUD (fieldhud.h). The pieces, their places and their effects are
 * khdays-port's [field] compositions (port_days.txt, MIT; measured there on missions 17 and 18),
 * in its units: the picture is the display's height in 192 DS pixels, C of them across, a
 * length scaled by the HUD's factor (here config hud_size / 100). The states are its own
 * checks: the mission's panel by its edge's greys on the bottom screen, a mission's start and a
 * person talking by the field's panel state, an event's text by its status, a challenge by the
 * mission's kind after the story's camp (its ui/situation_decide.c, host/render/onescreen.c). */
#include "hw/fieldhud.h"
#include "hw/gpu3d.h"
#include "nitro/overlay.h"
#include "config.h"
#include "video.h"

#include <string.h>

extern int kh_is_game_heap(unsigned long a, unsigned long n); /* nitro/os.c */
extern uint32_t data_ov002_0207f614;   /* ov002: the field's mission panel context */
extern uint32_t data_ov023_0208a784[]; /* ov023: the event's root, [1] its scene */
extern unsigned char data_0204c240;    /* the mission's kind: bit 1 a challenge */

#define MISSION_START_STATE 0x18cu
#define EVENT_STATUS 0x8758cu

/* BGR555 from 8-bit channels */
#define C555(r, g, b) ((uint16_t)(((r) >> 3) | ((g) >> 3) << 5 | ((b) >> 3) << 10))

/* the picture: k display pixels a DS pixel, C DS pixels across from x0 */
static float s_k, s_c, s_x0, s_f;

enum { AT_TOP_LEFT, AT_TOP, AT_TOP_RIGHT };

/* A piece of the field's HUD: its source (x y w h), placed at a corner of the picture with its
 * margins (left, top, right) and scale (sx, sy), as khdays-port's build_shape does */
static VideoPiece piece(int lower, float x, float y, float w, float h, int at, float ml, float mt, float mr,
                        float sx, float sy)
{
    VideoPiece p;
    const float pw = w * sx * s_f, ph = h * sy * s_f;
    float px = at == AT_TOP_LEFT ? 0.0f : at == AT_TOP ? (s_c - pw) / 2.0f : s_c - pw;
    px += (ml - mr) * s_f;
    memset(&p, 0, sizeof(p));
    p.lower = lower;
    p.src[0] = x, p.src[1] = y, p.src[2] = w, p.src[3] = h;
    p.dst[0] = s_x0 + px * s_k;
    p.dst[1] = mt * s_f * s_k;
    p.dst[2] = p.dst[0] + pw * s_k;
    p.dst[3] = p.dst[1] + ph * s_k;
    return p;
}

static void fade(VideoPiece *p, float l, float t, float r, float b)
{
    p->fade[0] = l * s_f * s_k, p->fade[1] = t * s_f * s_k;
    p->fade[2] = r * s_f * s_k, p->fade[3] = b * s_f * s_k;
}

static void see(VideoPiece *p, uint16_t colour, float opacity)
{
    p->see = 1;
    p->see_colour = colour;
    p->see_opacity = opacity;
}

/* "mission-panel": the bottom screen shows the mission's panel, its upper edge two rows across
 * (y 152 the grey 0x6d, y 153 the grey 0xae) */
static int near_grey(uint32_t c, int v)
{
    int n;
    for (n = 0; n < 3; n++) {
        const int d = (int)((c >> (8 * n)) & 0xffu) - v;
        if (d < -12 || d > 12)
            return 0;
    }
    return 1;
}

static int mission_panel(const uint32_t *lower)
{
    static const int xs[] = { 16, 64, 128, 192, 236 };
    unsigned i;
    for (i = 0; i < sizeof(xs) / sizeof(xs[0]); i++)
        if (!near_grey(lower[152 * 256 + xs[i]], 0x6d) || !near_grey(lower[153 * 256 + xs[i]], 0xae))
            return 0;
    return 1;
}

/* The HUD's flat 3D (the hearts counter, its "CHAIN" and its timer: the screen-space polygons at
 * the top left) moved under the mission's panel, at the 2D pieces' scale: khdays-port's
 * `shape3d hud -64 -64 191.5 141 at top-left margin -37.666667 -13.666667 0 0 scale 0.666667` */
static void hearts_3d(int on)
{
    static const float src[4] = { -64.0f, -64.0f, 191.5f, 141.0f };
    const ScreenRect r = video_screen_rect(0);
    KhGpu3dHudPlace p;
    const float s = 0.666667f * s_f, x = -37.666667f * s_f, y = -13.666667f * s_f;
    if (!on || r.w <= 0 || r.h <= 0) {
        kh_gpu3d_set_hud_place(NULL);
        return;
    }
    memset(&p, 0, sizeof(p));
    p.place = 1;
    p.src[0] = src[0], p.src[1] = src[1], p.src[2] = src[0] + src[2], p.src[3] = src[1] + src[3];
    p.mx = s * s_k * 2.0f / (float)r.w;
    p.cx = (s_x0 + (x - src[0] * s) * s_k - (float)r.x) * 2.0f / (float)r.w - 1.0f;
    p.my = -s * s_k * 2.0f / (float)r.h;
    p.cy = 1.0f - ((y - src[1] * s) * s_k - (float)r.y) * 2.0f / (float)r.h;
    kh_gpu3d_set_hud_place(&p);
}

void kh_fieldhud_frame(int active, int paused, int dialog, const uint32_t *lower)
{
    static int story_camp = 1;
    VideoPiece pieces[VIDEO_PIECES];
    float holes[VIDEO_HOLES][4];
    int n = 0, nh = 0, start = -1, info_upper, challenge, hud, panel;
    /* the last camp loaded: the story's (ov025) or Mission Mode's (ov008); a challenge's mark
     * is only the story's */
    if (kh_overlay_loaded(25))
        story_camp = 1;
    else if (kh_overlay_loaded(8))
        story_camp = 0;
    if (!active) {
        video_set_field_hud(NULL, 0, NULL, 0, 0, 0);
        hearts_3d(0);
        return;
    }
    s_k = 544.0f / 192.0f;
    s_c = kh_config.aspect == KH_ASPECT_4_3 ? 256.0f : 960.0f / s_k;
    s_x0 = (960.0f - s_c * s_k) / 2.0f;
    s_f = (float)kh_config.hud_size / 100.0f;
    /* the field's panel: 3-6 a mission's start (its information on the top screen), 10 a
     * person talking; an event's text (ov023's status bit 1) */
    if (kh_overlay_loaded(2) && kh_is_game_heap(data_ov002_0207f614, MISSION_START_STATE + 4))
        start = (int)*(const volatile uint32_t *)(uintptr_t)(data_ov002_0207f614 + MISSION_START_STATE);
    if (kh_overlay_loaded(23)) {
        const uintptr_t scene = data_ov023_0208a784[1];
        if (kh_is_game_heap(scene, EVENT_STATUS + 2) && (*(const volatile uint16_t *)(scene + EVENT_STATUS) & 2))
            dialog = 1;
    }
    info_upper = start >= 3 && start <= 6;
    dialog = dialog || start == 10;
    challenge = story_camp && (data_0204c240 & 2u);
    hud = !paused && !dialog && !info_upper;
    panel = !dialog && !info_upper && !challenge && mission_panel(lower);
    if (panel) {
        /* the mission's panel (the bottom screen's 4,152-252,192) at the top left, the ground
         * around its bevelled corners (36 36 36) not drawn, its black half seen through; its
         * lower edge, cut by the DS's screen, is its upper edge turned over */
        VideoPiece p = piece(1, 4, 152, 248, 40, AT_TOP_LEFT, 5, 5, 0, 0.5f, 0.5f);
        p.keys[p.key_count++] = C555(36, 36, 36);
        see(&p, C555(0, 0, 0), 0.65f);
        pieces[n++] = p;
        p = piece(1, 4, 152, 248, 4, AT_TOP_LEFT, 5, 25, 0, 0.5f, 0.5f);
        p.mirror_y = 1;
        p.keys[p.key_count++] = C555(36, 36, 36);
        see(&p, C555(0, 0, 0), 0.65f);
        pieces[n++] = p;
    }
    if (hud) {
        /* the enemy's health at the top right, above the map */
        pieces[n++] = piece(0, 163, 0, 93, 22, AT_TOP_RIGHT, 0, 5, 5, 0.666667f, 0.666667f);
        holes[nh][0] = 163, holes[nh][1] = 0, holes[nh][2] = 256, holes[nh++][3] = 22;
    }
    if (panel) {
        /* the map, as Kingdom Hearts II has it: a 104 x 64 window of its box, its white ground not
         * drawn, its floor (239 239 247) seen through, fading at its edges; the panel's black
         * stretched behind it, half seen through */
        VideoPiece p = piece(1, 112, 64, 104, 64, AT_TOP_RIGHT, 0, 5, 5, 0.6f, 0.6f);
        p.keys[p.key_count++] = C555(248, 248, 248);
        see(&p, C555(232, 232, 240), 0.55f);
        fade(&p, 4, 4, 4, 4);
        pieces[n++] = p;
        p = piece(1, 186, 156, 2, 12, AT_TOP_RIGHT, 0, 3, 3, 33.2f, 3.533333f);
        see(&p, C555(0, 0, 0), 0.5f);
        fade(&p, 6, 6, 6, 6);
        pieces[n++] = p;
    }
    if (hud) {
        /* the hearts' item notifications and timer (the 2D at the top left) under the panel */
        pieces[n++] = piece(0, 0, 0, 108, 86, AT_TOP_LEFT, 0, 29, 0, 0.666667f, 0.666667f);
        holes[nh][0] = 0, holes[nh][1] = 0, holes[nh][2] = 108, holes[nh++][3] = 86;
        /* the sigils and the death counter under the map */
        pieces[n++] = piece(0, 163, 25, 93, 30, AT_TOP_RIGHT, 0, 47.333333f, 5, 0.666667f, 0.666667f);
        holes[nh][0] = 163, holes[nh][1] = 25, holes[nh][2] = 256, holes[nh++][3] = 55;
        if (challenge) {
            /* a challenge's label (the top screen's rows 0-13) at the top, under the countdown */
            pieces[n++] = piece(0, 0, 0, 132, 14, AT_TOP, 0, 22, 0, 0.666667f, 0.666667f);
            holes[nh][0] = 0, holes[nh][1] = 0, holes[nh][2] = 132, holes[nh++][3] = 14;
        }
    }
    /* the panels of the map (2), the target (3) and the mission's gauge (4) give way to these */
    video_set_field_hud(pieces, n, (const float(*)[4])holes, nh, hud, 0xeu);
    hearts_3d(hud);
}
