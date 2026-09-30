/* Vita controls -> DS buttons and touch screen, in the form the DS hardware reported them. */
#ifndef KH_INPUT_H
#define KH_INPUT_H

#include <stdint.h>

/* DS button bits (KEYINPUT order, then X/Y/debug as the ARM7 reports them at 0x027fffa8). */
enum {
    DS_KEY_A = 1 << 0,
    DS_KEY_B = 1 << 1,
    DS_KEY_SELECT = 1 << 2,
    DS_KEY_START = 1 << 3,
    DS_KEY_RIGHT = 1 << 4,
    DS_KEY_LEFT = 1 << 5,
    DS_KEY_UP = 1 << 6,
    DS_KEY_DOWN = 1 << 7,
    DS_KEY_R = 1 << 8,
    DS_KEY_L = 1 << 9,
    DS_KEY_X = 1 << 10,
    DS_KEY_Y = 1 << 11,
};

typedef struct {
    uint16_t held;     /* DS_KEY_* bits, active high */
    uint16_t keyinput; /* REG_KEYINPUT value: bits 0-9, active low */
    uint16_t extkeys;  /* the ARM7's word at 0x027fffa8: X/Y/debug active low, hinge open */
    int touching;
    int touch_x, touch_y; /* DS pixels, 0..255 / 0..191 */
    int swap_layout;      /* port hotkey: cycle the screen layout (edge-triggered) */
} InputState;

void input_init(void);
void input_poll(InputState *out);

#endif
