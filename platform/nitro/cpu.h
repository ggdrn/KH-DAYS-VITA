#ifndef KH_NITRO_CPU_H
#define KH_NITRO_CPU_H

#include <stdint.h>

/* IE/IF bits */
#define KH_IRQ_VBLANK (1u << 0)
#define KH_IRQ_TIMER0 (1u << 3)
#define KH_IRQ_IPC_RECV (1u << 18)

void kh_cpu_init(void);
/* The calling Vita thread becomes the one running NitroSDK code (the launcher thread). */
void kh_cpu_set_owner(void);

/* A hardware event: set IF bits (any thread). */
void kh_irq_raise(uint32_t bits);
void kh_irq_ack(uint32_t bits);

/* Run fn(a, b, c) on the running NitroSDK thread, in IRQ mode, at the next delivery point:
 * the completion of something the DS did asynchronously (a card read, an ARM7 reply). */
void kh_cpu_defer(void (*fn)(void *, void *, void *), void *a, void *b, void *c);

/* Deliver whatever interrupts are pending and allowed (baton holder only). */
void kh_cpu_poll(void);
int kh_cpu_in_irq(void);

#endif
