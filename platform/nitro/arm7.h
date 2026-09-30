#ifndef KH_NITRO_ARM7_H
#define KH_NITRO_ARM7_H

#include <stdint.h>

void kh_arm7_init(void);
/* Send a word to the ARM9 on a PXI tag (delivered at the next interrupt point). */
void kh_arm7_reply(int tag, uint32_t data, int err);
/* The touch panel's current sample, in its raw 12-bit ADC units (the display loop, per frame). */
void kh_arm7_touch(int touching, int raw_x, int raw_y);

#endif
