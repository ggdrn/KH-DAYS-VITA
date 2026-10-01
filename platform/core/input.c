#include "input.h"

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
    { SCE_CTRL_RIGHT, DS_KEY_RIGHT },  { SCE_CTRL_LEFT, DS_KEY_LEFT },
    { SCE_CTRL_UP, DS_KEY_UP },        { SCE_CTRL_DOWN, DS_KEY_DOWN },
    { SCE_CTRL_RTRIGGER, DS_KEY_R },   { SCE_CTRL_LTRIGGER, DS_KEY_L },
};

static uint32_t s_prev_buttons;
static uint64_t s_back_since; /* when the rear touchpad was first touched, 0 when untouched */
static int s_back_fired;      /* the current hold already swapped the screens */

#define BACK_HOLD_US 1000000

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
