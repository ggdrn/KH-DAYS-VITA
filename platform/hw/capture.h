/* The DS display capture (DISPCAPCNT): see capture.c. */
#ifndef KH_HW_CAPTURE_H
#define KH_HW_CAPTURE_H

#include <stdint.h>

/* With DISPCAPCNT's enable bit set: capture this frame into its VRAM bank and clear the bit.
 * From the display loop, with tex3d the 3D layer drawn for it; returns 1 when it captured. */
int kh_capture_run(unsigned tex3d);
/* The same with DISPCAPCNT and engine A's DISPCNT as recorded earlier (dual 3D). */
int kh_capture_run_regs(unsigned tex3d, uint32_t cnt, uint32_t dispcnt);

/* Dual 3D: engine A's picture of this frame as displayed (fb its 2D, tex3d its 3D layer or 0,
 * master_bright what the GPU still applies to it) kept for the screen it is on (0 top,
 * 1 bottom), shown there while engine B has that screen (video_show_capture with
 * VIDEO_SCREEN_MEMORY + screen). */
void kh_capture_screen(const uint32_t *fb, unsigned tex3d, int screen, uint16_t master_bright);
/* Whether the last capture went to bank and is still what it holds: engine A showing that bank
 * (display mode 2) shows the capture (video_show_capture). */
int kh_capture_shown(int bank, int check_bytes);

/* captures since the last call, for the statistics */
uint32_t kh_capture_take_count(void);

#endif
