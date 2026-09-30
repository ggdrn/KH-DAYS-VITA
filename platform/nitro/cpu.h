#ifndef KH_NITRO_CPU_H
#define KH_NITRO_CPU_H

#include <stdint.h>

/* IE/IF bits */
#define KH_IRQ_VBLANK (1u << 0)
#define KH_IRQ_TIMER0 (1u << 3)
#define KH_IRQ_IPC_RECV (1u << 18)

/* The core the NitroSDK threads run on; the display loop keeps core 0, the IRQ thread core 2. */
#define KH_GAME_CPU_MASK 0x20000 /* SCE_KERNEL_CPU_MASK_USER_1 */

/* Counters for the watchdog and the on-screen status. */
extern volatile uint32_t kh_cpu_irqs_delivered, kh_cpu_preempted, kh_cpu_switches;

void kh_cpu_init(void);
/* The calling Vita thread becomes the one running NitroSDK code (the launcher thread). */
void kh_cpu_set_owner(void);

/* A hardware event: set IF bits (any thread). */
void kh_irq_raise(uint32_t bits);
void kh_irq_ack(uint32_t bits);

/* Run fn(a, b, c) on the running NitroSDK thread, in IRQ mode, at the next delivery point:
 * the completion of something the DS did asynchronously (a card read, an ARM7 reply). */
void kh_cpu_defer(void (*fn)(void *, void *, void *), void *a, void *b, void *c);

/* The same for a completion the DS delivered from a thread, not an interrupt (the CARD task
 * thread ends an asynchronous ROM read by calling its callback): it does not wait for IME/IE,
 * only for the CPU to be outside a critical section. The boot reads ov001, which is what
 * enables interrupts, before IME is ever set. */
void kh_cpu_defer_task(void (*fn)(void *, void *, void *), void *a, void *b, void *c);

/* Deliver whatever interrupts are pending and allowed (baton holder only). */
void kh_cpu_poll(void);
/* One log line with the interrupt and scheduling state (for the watchdog). */
void kh_cpu_log_state(void);
int kh_cpu_in_irq(void);

#endif
