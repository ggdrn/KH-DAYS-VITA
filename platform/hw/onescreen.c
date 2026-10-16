/* config single_screen 2 (onescreen.h). The pixel states are khdays-port's
 * port/host/render/onescreen.c (MIT): the INFORMATION banner and the mission panel found by
 * their colours on the lower screen, and the screens that show something. */
#include "hw/onescreen.h"
#include "hw/gpu3d.h"
#include "hw/situation_take.h"
#include "ui/composition.h"
#include "config.h"
#include "log.h"
#include "paths.h"
#include "video.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_MOD KH_DATA_DIR "/layouts/compositions.txt"
#define FILE_PORT "app0:layouts/compositions.txt"

static int s_loaded; /* 1 read, -1 none could be */
static const uint32_t *s_px[2];
static KhSituation s_told;
static int s_told_any;

static void say(const char *line)
{
    LOG("one screen: %s", line);
}

static char *read_file(const char *path)
{
    SceIoStat st;
    SceUID fd;
    char *text;
    if (sceIoGetstat(path, &st) < 0 || st.st_size <= 0 || st.st_size > 1024 * 1024)
        return NULL;
    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return NULL;
    text = malloc((size_t)st.st_size + 1);
    if (text && sceIoRead(fd, text, (SceSize)st.st_size) != (int)st.st_size) {
        free(text);
        text = NULL;
    }
    sceIoClose(fd);
    if (text)
        text[st.st_size] = '\0';
    return text;
}

static int backdrop_there(const char *name)
{
    char path[192];
    SceIoStat st;
    snprintf(path, sizeof(path), KH_DATA_DIR "/layouts/%s", name);
    return sceIoGetstat(path, &st) >= 0 && st.st_size > 0;
}

static void load(void)
{
    static const char *const files[2] = { FILE_MOD, FILE_PORT };
    int i;
    s_loaded = -1;
    kh_composition_set_backdrop_check(backdrop_there);
    for (i = 0; i < 2 && s_loaded < 0; i++) {
        char *text = read_file(files[i]);
        int sections;
        if (!text)
            continue;
        sections = kh_composition_read(files[i], text, say);
        free(text);
        if (sections < 0) {
            LOG("one screen: %s does not begin with `units port`", files[i]);
        } else {
            LOG("one screen: %s read, %d sections", files[i], sections);
            s_loaded = 1;
        }
    }
    if (s_loaded < 0)
        LOG("one screen: no compositions (%s, %s): the panels' single screen instead", FILE_MOD, FILE_PORT);
}

static uint32_t pixel(int screen, int x, int y)
{
    return s_px[screen][y * 256 + x] & 0xffffffu;
}

/* The screens that show something: a screen whose points of a 16 x 12 grid are not all alike. */
static uint32_t screens_content(void)
{
    uint32_t content = 0;
    int screen, gx, gy;
    for (screen = 0; screen < 2; screen++) {
        const uint32_t first = pixel(screen, 8, 8);
        for (gy = 0; gy < 12 && !(content & (1u << screen)); gy++)
            for (gx = 0; gx < 16; gx++)
                if (pixel(screen, 8 + gx * 16, 8 + gy * 16) != first) {
                    content |= 1u << screen;
                    break;
                }
    }
    return content;
}

static int near_white(uint32_t c)
{
    int n;
    for (n = 0; n < 3; n++)
        if (((c >> (8 * n)) & 0xffu) < 0xf0u)
            return 0;
    return 1;
}

static int near_grey(uint32_t c, uint32_t v)
{
    int n;
    for (n = 0; n < 3; n++) {
        const int d = (int)((c >> (8 * n)) & 0xffu) - (int)v;
        if (d < -12 || d > 12)
            return 0;
    }
    return 1;
}

/* "info-lower": the lower screen shows the field's INFORMATION banner, its white lines across at
 * y 23 and, right of its label, at y 8 */
static void info_lower(KhSituation *s)
{
    static const int below[] = { 16, 64, 128, 192, 236 }, above[] = { 128, 192, 236 };
    unsigned i;
    if (s->scene != KH_SIT_FIELD && s->scene != KH_SIT_FIELD_3D_BOTH && s->scene != KH_SIT_TUTORIAL &&
        s->scene != KH_SIT_PAUSE)
        return;
    for (i = 0; i < sizeof(below) / sizeof(below[0]); i++)
        if (!near_white(pixel(1, below[i], 23)))
            return;
    for (i = 0; i < sizeof(above) / sizeof(above[0]); i++)
        if (!near_white(pixel(1, above[i], 8)))
            return;
    s->states |= 1u << KH_ST_INFO_LOWER;
}

/* Whether the INFORMATION line holds a message: a pixel of its text rows that stands out from
 * its ground */
static int info_has_text(void)
{
    const uint32_t ground = pixel(1, 6, 15);
    int x, y, n;
    for (y = 11; y <= 19; y++)
        for (x = 8; x < 224; x++) {
            const uint32_t c = pixel(1, x, y);
            for (n = 0; n < 3; n++) {
                const int d = (int)((c >> (8 * n)) & 0xffu) - (int)((ground >> (8 * n)) & 0xffu);
                if (d > 40 || d < -40)
                    return 1;
            }
        }
    return 0;
}

/* "info-new": the INFORMATION line shows on one screen only for a while after its text changes
 * (its rows summed every frame: a new sum is a new message, shown for INFO_FRAMES VBlanks) */
#define INFO_FRAMES 300
#define INFO_SETTLE_FRAMES 30

static void info_new(KhSituation *s, uint32_t frame)
{
    static uint32_t last_sum, changed, still_since;
    static int seen, after_start;
    uint32_t sum = 2166136261u;
    int x, y;
    if (s->states & (1u << KH_ST_INFO_UPPER))
        after_start = 1; /* the mission's start shows its information: its first message is seen */
    if (!(s->states & (1u << KH_ST_INFO_LOWER))) {
        seen = 0;
        return;
    }
    if (!info_has_text()) {
        seen = 0;
        changed = frame - INFO_FRAMES;
        return;
    }
    for (y = 10; y <= 20; y += 2)
        for (x = 4; x < 252; x++)
            sum = (sum ^ pixel(1, x, y)) * 16777619u;
    if (after_start) {
        if (!seen || sum != last_sum) {
            seen = 1;
            last_sum = sum;
            still_since = frame;
        }
        changed = frame - INFO_FRAMES;
        after_start = frame - still_since < INFO_SETTLE_FRAMES;
    } else if (!seen || sum != last_sum) {
        seen = 1;
        last_sum = sum;
        changed = frame;
    }
    if (frame - changed < INFO_FRAMES)
        s->states |= 1u << KH_ST_INFO_NEW;
}

/* "mission-panel": the lower screen shows the field's mission panel, its upper edge two rows
 * across (y 152 the grey 0x6d, y 153 the grey 0xae) */
static void mission_panel(KhSituation *s)
{
    static const int xs[] = { 16, 64, 128, 192, 236 };
    unsigned i;
    for (i = 0; i < sizeof(xs) / sizeof(xs[0]); i++)
        if (!near_grey(pixel(1, xs[i], 152), 0x6du) || !near_grey(pixel(1, xs[i], 153), 0xaeu))
            return;
    s->states |= 1u << KH_ST_MISSION_PANEL;
}

/* Where the wider picture shows more of what the game shows: its 3D is the field's world (or the
 * title's logo over white); the camp's menus and the world selector keep the DS's columns. */
static int widens(const KhSituationInputs *in)
{
    if (in->movie || ((in->overlays >> 11) & 1u) ||
        (in->camp_screen != KH_CAMP_NONE && (in->scene == 19 || in->camp_story)))
        return 0;
    return in->scene == 2 || in->scene == 1;
}

/* The HUD's flat 3D (`shape3d hud`, `hide3d hud`): from a DS position to the 3D's NDC, where the
 * 3D covers the ground's rectangle of the display, through the picture's place for it. */
static void hud_place(const KhComposition *c, int columns)
{
    KhGpu3dHudPlace p;
    const ScreenRect r = video_screen_rect(0);
    const float k = 544.0f / 192.0f, x0 = (960.0f - (float)columns * k) / 2.0f;
    memset(&p, 0, sizeof(p));
    if (c->screens == KH_SCREENS_UPPER || c->screens == KH_SCREENS_LOWER) {
        p.hide = c->hide3d_hud;
        if (c->hud3d && r.w > 0 && r.h > 0) {
            const float s = c->hud3d_scale;
            p.place = 1;
            p.src[0] = c->hud3d_src[0];
            p.src[1] = c->hud3d_src[1];
            p.src[2] = c->hud3d_src[0] + c->hud3d_src[2];
            p.src[3] = c->hud3d_src[1] + c->hud3d_src[3];
            p.mx = s * k * 2.0f / (float)r.w;
            p.cx = (x0 + (c->hud3d_x - c->hud3d_src[0] * s) * k - (float)r.x) * 2.0f / (float)r.w - 1.0f;
            p.my = -s * k * 2.0f / (float)r.h;
            p.cy = 1.0f - ((c->hud3d_y - c->hud3d_src[1] * s) * k - (float)r.y) * 2.0f / (float)r.h;
        }
    }
    kh_gpu3d_set_hud_place(p.place || p.hide ? &p : NULL);
}

static void tell(const KhSituation *s, uint32_t frame)
{
    char states[160] = "";
    int i, n = 0;
    for (i = 0; i < KH_STATES; i++)
        if (s->states & (1u << i))
            n += snprintf(states + n, sizeof(states) - (size_t)n, " %s", kh_situation_state_name(i));
    LOG("one screen: vblank %u, %s%s%s", (unsigned)frame, kh_situation_name(s->scene), n ? ":" : "", states);
}

int kh_onescreen_frame(int active, int movie, int dual, const uint32_t *upper, const uint32_t *lower,
                       uint32_t vblank)
{
    static KhComposition comp;
    KhSituationInputs in;
    KhSituation now;
    int columns;
    if (active && !s_loaded)
        load();
    if (!active || s_loaded < 0) {
        video_set_composition(NULL, 256, 0);
        kh_gpu3d_set_hud_place(NULL);
        s_told_any = 0;
        return 0;
    }
    s_px[0] = upper;
    s_px[1] = lower;
    kh_situation_take(&in, movie, dual);
    kh_situation_decide(&in, &now);
    if (kh_situation_hud_mode() == KH_HUD_COMPLETE) {
        info_lower(&now);
        info_new(&now, vblank);
    }
    mission_panel(&now);
    now.content = screens_content();
    if (!s_told_any || now.scene != s_told.scene || now.states != s_told.states) {
        tell(&now, vblank);
        s_told = now;
        s_told_any = 1;
    }
    kh_composition_set_hud_factor((float)kh_config.hud_size / 100.0f);
    /* the picture: the display's height is 192 DS dots, its width C of them (4:3: the DS's) */
    columns = kh_config.aspect == KH_ASPECT_4_3 ? 256 : (int)(192.0f * 960.0f / 544.0f + 0.5f);
    kh_composition_for(&now, columns, &comp);
    video_set_composition(&comp, columns, widens(&in));
    hud_place(&comp, columns);
    return 1;
}
