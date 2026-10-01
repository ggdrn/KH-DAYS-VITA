#include "input.h"

#include "log.h"
#include "video.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>
#include <string.h>

#define STICK_DEADZONE 48

/* Positional mapping, as on the DS: the right face button is A, the bottom one B. */
static const struct {
    uint32_t vita;
    uint16_t ds;
} s_map[] = {
    { SCE_CTRL_CIRCLE, DS_KEY_A },     { SCE_CTRL_CROSS, DS_KEY_B },
    { SCE_CTRL_TRIANGLE, DS_KEY_X },   { SCE_CTRL_SQUARE, DS_KEY_Y },
    { SCE_CTRL_SELECT, DS_KEY_SELECT }, { SCE_CTRL_START, DS_KEY_START },
    { SCE_CTRL_RTRIGGER, DS_KEY_R },   { SCE_CTRL_LTRIGGER, DS_KEY_L },
};

static uint32_t s_prev_buttons;
static uint64_t s_back_since; /* when the rear touchpad was first touched, 0 when untouched */
static int s_back_fired;      /* the current hold already swapped the screens */

#define BACK_HOLD_US 1000000

/* ---- game-side hooks (decomp patch, PLATFORM_VITA) ------------------------------------------
 * The right stick turns the field camera and the Vita's d-pad moves the command deck's
 * cursor, through the game's own request bits and cursor functions; the left stick stays the
 * DS d-pad. Written here by the input thread, read by the game thread. */

#define RSTICK_DEADZONE 40
#define NAV_FIRST_US 300000 /* d-pad auto-repeat: first repeat, then every NAV_NEXT_US */
#define NAV_NEXT_US 110000
#define DECK_SEEN_US 150000 /* the deck counts as up this long after the game last polled it */

static volatile int s_rx, s_ry;             /* right stick, -127..127 past the dead zone */
static volatile uint64_t s_deck_seen;       /* last kh_vita_command_nav call */
static volatile int s_nav_queue[8];
static volatile unsigned s_nav_head, s_nav_tail;
static int s_nav_dir;                       /* d-pad direction held: 1 down 2 up 3 left 4 right */
static uint64_t s_nav_next;

static void nav_push(int dir)
{
    unsigned h = s_nav_head;
    if (h - s_nav_tail < 8) {
        s_nav_queue[h & 7] = dir;
        __atomic_store_n(&s_nav_head, h + 1, __ATOMIC_RELEASE);
    }
}

int kh_vita_command_nav(void)
{
    unsigned t = s_nav_tail;
    s_deck_seen = sceKernelGetProcessTimeWide();
    if (t == __atomic_load_n(&s_nav_head, __ATOMIC_ACQUIRE))
        return 0;
    s_nav_tail = t + 1;
    return s_nav_queue[t & 7];
}

/* request bits for this camera tick: each axis pulsed in proportion to its deflection */
unsigned int kh_vita_camera_bits(void)
{
    static int acc_x, acc_y;
    unsigned int bits = 0;
    const int rx = s_rx, ry = s_ry;
    if (rx) {
        acc_x += rx < 0 ? -rx : rx;
        if (acc_x >= 127) {
            acc_x -= 127;
            bits |= rx > 0 ? 4 : 8;
        }
    } else {
        acc_x = 0;
    }
    if (ry) {
        acc_y += ry < 0 ? -ry : ry;
        if (acc_y >= 127) {
            acc_y -= 127;
            bits |= ry < 0 ? 2 : 1;
        }
    } else {
        acc_y = 0;
    }
    return bits;
}

/* diagnostics: whether the camera takes manual turns where the stick is used */
void kh_vita_camera_state(int manual, unsigned int bits)
{
    static int logged_on, logged_off;
    if (!(s_rx | s_ry))
        return;
    if (manual && bits && !logged_on++)
        LOG("input: right stick turning the camera");
    if (!manual && !logged_off++)
        LOG("input: right stick used while the camera takes no manual turns");
}

static int stick_axis(int v)
{
    v -= 128;
    if (v > -RSTICK_DEADZONE && v < RSTICK_DEADZONE)
        return 0;
    return v < -127 ? -127 : v > 127 ? 127 : v;
}

void input_init(void)
{
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
}

void input_poll(InputState *out)
{
    SceCtrlData pad;
    SceTouchData touch;
    uint16_t held = 0;
    size_t i;

    memset(out, 0, sizeof(*out));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    for (i = 0; i < sizeof(s_map) / sizeof(s_map[0]); i++)
        if (pad.buttons & s_map[i].vita)
            held |= s_map[i].ds;

    s_rx = stick_axis(pad.rx);
    s_ry = stick_axis(pad.ry);

    /* The Vita's d-pad: the command deck's cursor while the deck is up (field and battle),
     * the DS d-pad everywhere else (menus). */
    {
        const uint64_t now = sceKernelGetProcessTimeWide();
        int dir = (pad.buttons & SCE_CTRL_DOWN) ? 1 : (pad.buttons & SCE_CTRL_UP) ? 2
                : (pad.buttons & SCE_CTRL_LEFT) ? 3 : (pad.buttons & SCE_CTRL_RIGHT) ? 4 : 0;
        if (now - s_deck_seen < DECK_SEEN_US) {
            if (dir && dir != s_nav_dir) {
                nav_push(dir);
                s_nav_next = now + NAV_FIRST_US;
            } else if (dir && now >= s_nav_next) {
                nav_push(dir);
                s_nav_next = now + NAV_NEXT_US;
            }
        } else {
            if (pad.buttons & SCE_CTRL_RIGHT) held |= DS_KEY_RIGHT;
            if (pad.buttons & SCE_CTRL_LEFT) held |= DS_KEY_LEFT;
            if (pad.buttons & SCE_CTRL_UP) held |= DS_KEY_UP;
            if (pad.buttons & SCE_CTRL_DOWN) held |= DS_KEY_DOWN;
        }
        s_nav_dir = dir;
    }

    /* The left stick doubles as the d-pad (the DS has no stick). */
    if (pad.lx < 128 - STICK_DEADZONE) held |= DS_KEY_LEFT;
    if (pad.lx > 128 + STICK_DEADZONE) held |= DS_KEY_RIGHT;
    if (pad.ly < 128 - STICK_DEADZONE) held |= DS_KEY_UP;
    if (pad.ly > 128 + STICK_DEADZONE) held |= DS_KEY_DOWN;

    /* L+R+Select cycles the screen layout; the combination is swallowed. */
    if ((pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT)) ==
        (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT)) {
        out->swap_layout = (s_prev_buttons & SCE_CTRL_SELECT) == 0;
        held &= ~(DS_KEY_L | DS_KEY_R | DS_KEY_SELECT);
    }
    /* L+R+Start toggles the on-screen console; the combination is swallowed. */
    if ((pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) ==
        (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) {
        out->toggle_console = (s_prev_buttons & SCE_CTRL_START) == 0;
        held &= ~(DS_KEY_L | DS_KEY_R | DS_KEY_START);
    }
    s_prev_buttons = pad.buttons;

    out->held = held;
    out->keyinput = (uint16_t)(~held & 0x03ff);
    /* X bit 10, Y bit 11, debug bit 13: set = released; hinge bit 15 clear = lid open */
    out->extkeys = (uint16_t)(0x2c00 & ~(((held >> 10) & 3) << 10));

    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    if (touch.reportNum > 0) {
        /* front panel reports 1920x1088 */
        int x, y;
        if (video_map_touch(touch.report[0].x / 2, touch.report[0].y / 2, &x, &y)) {
            out->touching = 1;
            out->touch_x = x;
            out->touch_y = y;
        }
    }

    /* rear touchpad held for a second: swap the screens, once per hold */
    sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1);
    if (touch.reportNum > 0) {
        uint64_t now = sceKernelGetProcessTimeWide();
        if (!s_back_since)
            s_back_since = now;
        if (!s_back_fired && now - s_back_since >= BACK_HOLD_US) {
            out->swap_screens = 1;
            s_back_fired = 1;
        }
    } else {
        s_back_since = 0;
        s_back_fired = 0;
    }
}
