#include "input.h"

#include "config.h"

#include "log.h"
#include "video.h"
#include "nitro/overlay.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>
#include <string.h>

/* kept by the game's lock-on code (Ov022_SetSelectionEnabled, Ov022_ReadSelectionInput) */
volatile int kh_lockon_active;
/* config r_toggle in the field: R does not turn the camera (Ov002_Camera_UpdateFollow) */
volatile int kh_r_one_click;

#define STICK_DEADZONE 48

/* the DS button each kh_config.button[] entry stands for */
static const uint16_t s_ds_key[KH_BTN_COUNT] = { DS_KEY_A, DS_KEY_B, DS_KEY_X, DS_KEY_Y,
                                                 DS_KEY_L, DS_KEY_R, DS_KEY_START, DS_KEY_SELECT };

static uint32_t s_prev_buttons;
static int s_front_down;  /* a finger is on the front panel */
static int s_front_swap;  /* that touch began on the small screen: it swaps, it is no tap */

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

/* Locked on (camera_stick on), the right stick's sideways push switches targets: a flick
 * left or right takes the nearest one on that side (Ov022_ReadSelectionInput, as Type B's L
 * and R), held it goes on every LOCKON_REPEAT_US; back near the middle it is armed again. Read
 * by the game thread once a game frame while locked on; a stick already pushed when the lock
 * begins waits for the middle first. */
#define LOCKON_PUSH 90  /* of 127: a flick */
#define LOCKON_REST 50  /* under it: back in the middle */
#define LOCKON_REPEAT_US 450000

int kh_vita_lockon_switch(void)
{
    static int armed;
    static uint64_t last_call, next_repeat;
    const uint64_t now = sceKernelGetProcessTimeWide();
    const int rx = s_rx, ax = rx < 0 ? -rx : rx;
    int dir = 0;
    if (now - last_call > 100000)
        armed = 0; /* not locked on just before: the stick may be held from the camera */
    last_call = now;
    if (!kh_config.camera_stick)
        return 0;
    if (ax < LOCKON_REST) {
        armed = 1;
    } else if (ax >= LOCKON_PUSH) {
        if (armed) {
            armed = 0;
            dir = rx < 0 ? -1 : 1;
            next_repeat = now + LOCKON_REPEAT_US;
        } else if (next_repeat && now >= next_repeat) {
            dir = rx < 0 ? -1 : 1;
            next_repeat = now + LOCKON_REPEAT_US;
        }
    }
    if (!armed && ax < LOCKON_PUSH)
        next_repeat = 0; /* let go part of the way: no repeat until the next flick */
    return dir;
}

/* request bits for this camera tick: each axis pulsed in proportion to its deflection */
unsigned int kh_vita_camera_bits(void)
{
    static int acc_x, acc_y;
    unsigned int bits = 0;
    /* full speed (a turn every tick, even) from about 70% of the stick's travel: below it the
     * turns are spread over the ticks, which shows as an uneven camera at 60 fps */
    /* camera_speed: full speed from all of the travel (slow: never quite), 70% (normal), 40% */
    static const int full_at[4] = { 90, 200, 90, 50 };
    const int div = full_at[kh_config.camera_speed & 3];
    int rx = s_rx * 127 / div, ry = s_ry * 127 / div;
    if (!kh_config.camera_stick)
        return 0;
    /* locked on, sideways switches targets (kh_vita_lockon_switch): the camera only tilts */
    if (kh_lockon_active)
        rx = 0;
    rx = rx > 127 ? 127 : rx < -127 ? -127 : rx;
    ry = ry > 127 ? 127 : ry < -127 ? -127 : ry;
    if (kh_config.camera_invert_x)
        rx = -rx;
    if (kh_config.camera_invert_y)
        ry = -ry;
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

/* diagnostics: whether the camera takes manual turns where the stick is used. manual 1 it
 * turns, 0 it takes no manual turns, -1 the stream holds the camera (the stick not read), -2
 * the camera is in its own turn. Counted per camera tick while the stick is pushed, and
 * logged when that mix changes (at most 60 lines a run). */
void kh_vita_camera_state(int manual, unsigned int bits)
{
    static uint32_t n[4], ticks, logged;
    static int last = -9;
    int state;
    (void)bits;
    if (!(s_rx | s_ry))
        return;
    state = manual == 1 ? 0 : manual == 0 ? 1 : manual == -1 ? 2 : 3;
    n[state]++;
    if (++ticks >= 60 || state != last) {
        if (logged < 60 && (state != last || n[0] != ticks)) {
            logged++;
            LOG("input: right stick over %u camera ticks: turning %u, no manual turns %u, "
                "held by the stream %u, own turn %u%s", (unsigned)ticks, (unsigned)n[0],
                (unsigned)n[1], (unsigned)n[2], (unsigned)n[3],
                kh_lockon_active ? " (locked on)" : "");
        }
        last = state;
        ticks = 0;
        memset(n, 0, sizeof(n));
    }
}

static int stick_axis(int v)
{
    static const int dz[4] = { RSTICK_DEADZONE, 25, RSTICK_DEADZONE, 60 };
    const int d = dz[kh_config.stick_deadzone & 3];
    v -= 128;
    if (v > -d && v < d)
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
    for (i = 0; i < KH_BTN_COUNT; i++)
        if (pad.buttons & kh_config.button[i])
            held |= s_ds_key[i];

    s_rx = stick_axis(pad.rx);
    s_ry = stick_axis(pad.ry);

    /* The Vita's d-pad: the command deck's cursor while the deck is up (field and battle),
     * the DS d-pad everywhere else (menus). */
    {
        const uint64_t now = sceKernelGetProcessTimeWide();
        int dir = (pad.buttons & SCE_CTRL_DOWN) ? 1 : (pad.buttons & SCE_CTRL_UP) ? 2
                : (pad.buttons & SCE_CTRL_LEFT) ? 3 : (pad.buttons & SCE_CTRL_RIGHT) ? 4 : 0;
        if (kh_config.dpad_deck && now - s_deck_seen < DECK_SEEN_US) {
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

    /* Cross confirms and Circle cancels (config confirm_cross): the DS's A on Cross and B on
     * Circle, menus and field alike (0.4.18 kept the field's action where the buttons are
     * placed); the fonts' and the HUD's A and B icons follow (nitro/button_glyphs.c,
     * nitro/button_sprites.c) */
    if (kh_config.confirm_cross) {
        const uint16_t ab = held & (DS_KEY_A | DS_KEY_B);
        held = (uint16_t)((held & ~(DS_KEY_A | DS_KEY_B)) | ((ab & DS_KEY_A) ? DS_KEY_B : 0) |
                          ((ab & DS_KEY_B) ? DS_KEY_A : 0));
    }

    /* The left stick doubles as the d-pad (the DS has no stick), past the dead zone chosen */
    {
        static const int dz[4] = { STICK_DEADZONE, 30, STICK_DEADZONE, 72 };
        const int d = dz[kh_config.stick_deadzone & 3];
        if (pad.lx < 128 - d) held |= DS_KEY_LEFT;
        if (pad.lx > 128 + d) held |= DS_KEY_RIGHT;
        if (pad.ly < 128 - d) held |= DS_KEY_UP;
        if (pad.ly > 128 + d) held |= DS_KEY_DOWN;
    }

    /* the rear touchpad's halves as two more buttons (config rear_touch) */
    if (kh_config.rear_touch) {
        SceTouchData back;
        int i;
        sceTouchPeek(SCE_TOUCH_PORT_BACK, &back, 1);
        for (i = 0; i < (int)back.reportNum && i < 2; i++) {
            const int left = back.report[i].x < 960; /* the panel reports 1920 wide */
            if (kh_config.rear_touch == 1)
                held |= left ? DS_KEY_L : DS_KEY_R;
            else
                held |= left ? DS_KEY_SELECT : DS_KEY_START;
        }
    }

    /* One-click lock-on (config r_toggle), in the field (ov022): the game locks on, and lets
     * go, with a quick double tap of R (Ov022_ReadSelectionInput), and a single tap turns the
     * camera behind the player (Ov002_Camera_UpdateFollow, which kh_r_one_click turns off).
     * A click of R becomes that double tap, each step 3 VBlanks long so that the game sees it
     * at 30 fps too. */
    {
        static int was, step, steps;
        static uint8_t pattern[12];
        const int now_r = (held & DS_KEY_R) != 0;
        kh_r_one_click = kh_config.r_toggle && kh_overlay_loaded(22);
        if (kh_r_one_click) {
            if (now_r && !was && step >= steps) {
                /* the game locks on, and lets go, with a quick double tap of R (a single tap
                 * turned the camera: 0.1.9 sent one to lock on) */
                static const uint8_t double_tap[] = { 1, 1, 1, 0, 0, 0, 1, 1, 1 };
                steps = (int)sizeof(double_tap);
                memcpy(pattern, double_tap, (size_t)steps);
                step = 0;
            }
            held = (uint16_t)((held & ~DS_KEY_R) | (step < steps && pattern[step] ? DS_KEY_R : 0));
            if (step < steps)
                step++;
        } else {
            step = steps = 0;
        }
        was = now_r;
    }

    /* L+R+Square switches fast-forward on and off; the combination is swallowed. */
    if ((pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SQUARE)) ==
        (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SQUARE)) {
        out->fast_forward = (s_prev_buttons & SCE_CTRL_SQUARE) == 0;
        held = 0;
    }
    /* L+R+Select opens and closes the port menu; the combination is swallowed. */
    if ((pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT)) ==
        (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT)) {
        out->port_menu = (s_prev_buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT)) !=
                         (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT);
        held = 0;
    }
    out->vita_buttons = pad.buttons;
    /* the debug hotkeys, with debug = 1 in config.ini */
    if (kh_config.debug) {
        /* L+R+Start toggles the on-screen console; the combination is swallowed. */
        if ((pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) ==
            (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) {
            out->toggle_console = (s_prev_buttons & SCE_CTRL_START) == 0;
            held &= ~(DS_KEY_L | DS_KEY_R | DS_KEY_START);
        }
        /* L+R+Triangle dumps the 3D frame (textures, polygons) for diagnosis; swallowed. */
        if ((pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_TRIANGLE)) ==
            (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_TRIANGLE)) {
            out->dump_3d = (s_prev_buttons & SCE_CTRL_TRIANGLE) == 0;
            held &= ~(DS_KEY_L | DS_KEY_R | DS_KEY_X);
        }
        /* L+R+Circle cycles the 3D debug modes (hw/gpu3d.h); swallowed. */
        if ((pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_CIRCLE)) ==
            (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_CIRCLE)) {
            out->debug_cycle = (s_prev_buttons & SCE_CTRL_CIRCLE) == 0;
            held &= ~(DS_KEY_L | DS_KEY_R | DS_KEY_A);
        }
    }
    s_prev_buttons = pad.buttons;

    out->held = held;
    out->keyinput = (uint16_t)(~held & 0x03ff);
    /* X bit 10, Y bit 11, debug bit 13: set = released; hinge bit 15 clear = lid open */
    out->extkeys = (uint16_t)(0x2c00 & ~(((held >> 10) & 3) << 10));

    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    if (touch.reportNum > 0) {
        /* front panel reports 1920x1088 */
        const int px = touch.report[0].x / 2, py = touch.report[0].y / 2;
        int x, y;
        if (!s_front_down) {
            /* a touch that lands on the small screen swaps the screens; the whole touch,
             * until the finger lifts, then belongs to that gesture */
            s_front_down = 1;
            s_front_swap = video_on_inset(px, py);
            if (s_front_swap)
                out->swap_screens = 1;
        }
        if (!s_front_swap && video_map_touch(px, py, &x, &y)) {
            out->touching = 1;
            out->touch_x = x;
            out->touch_y = y;
        }
    } else {
        s_front_down = 0;
        s_front_swap = 0;
    }
}
