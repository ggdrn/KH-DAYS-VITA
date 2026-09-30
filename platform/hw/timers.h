/* The four ARM9 timers (TMxCNT at 0x04000100), run from the Vita's clock. */
#ifndef KH_HW_TIMERS_H
#define KH_HW_TIMERS_H

#include <stdint.h>

/* Bus clock of the DS, the timers' base frequency. */
#define KH_DS_BUS_HZ 33513982u

/* Notice writes to the timer registers, bring the counters up to date and raise overflow
 * interrupts. KH_HW(0x0400010x) calls it before every access; the CPU calls it at delivery
 * points. */
void kh_timers_update(void);
/* Microseconds until the next overflow that raises an interrupt (UINT32_MAX if none). */
uint32_t kh_timers_next_event_us(void);

#endif
