#ifndef KH_HW_MEMMAP_H
#define KH_HW_MEMMAP_H

#include "kh_hw_map.h"

/* Microsecond timestamp of the last emulated VBlank start (0 before the first). */
extern volatile uint64_t kh_hw_vblank_start_us;

/* Power-on register values. Call before the game starts. */
void kh_hw_reset(void);

#endif
