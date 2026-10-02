/* The DS display capture (DISPCAPCNT): see capture.c. */
#ifndef KH_HW_CAPTURE_H
#define KH_HW_CAPTURE_H

#include <stdint.h>

/* With DISPCAPCNT's enable bit set: capture this frame into its VRAM bank and clear the bit.
 * From the display loop, after the 3D layer is drawn; returns 1 when it captured. */
int kh_capture_run(void);

/* captures since the last call, for the statistics */
uint32_t kh_capture_take_count(void);

#endif
