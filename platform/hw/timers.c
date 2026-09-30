/* ARM9 timers 0-3 on the Vita's microsecond clock.
 *
 * The registers are plain memory (KH_HW), so a timer notices how it was programmed when it is
 * next updated: an enable bit that came on starts it with the reload value then in TMxCNT_L; one
 * that went off stops it. Every access to the timer registers updates first, and so does every
 * interrupt delivery point, so a stop-reload-start sequence is always seen in order.
 *
 * While a timer runs, TMxCNT_L holds its current count. A value there that is not the one the
 * port last wrote is a new reload written by the game, and restarts the period.
 * Count-up (cascade) mode is not emulated: the NitroSDK does not use it. */
#include "hw/timers.h"

#include "hw/io.h"
#include "log.h"
#include "nitro/cpu.h"

#include <psp2/kernel/processmgr.h>

typedef struct {
    int running;
    uint16_t reload;
    uint16_t last_written;
    int shift;
    uint64_t start_us;
    uint64_t overflows_seen;
} Timer;

static Timer s_timers[4];

static const int s_shift[4] = { 0, 6, 8, 10 };

static uint64_t ticks_since(const Timer *t, uint64_t now)
{
    return (now - t->start_us) * KH_DS_BUS_HZ / (1000000ull << t->shift);
}

void kh_timers_update(void)
{
    uint64_t now = sceKernelGetProcessTimeWide();
    int n;

    for (n = 0; n < 4; n++) {
        Timer *t = &s_timers[n];
        uint32_t l = 0x04000100 + n * 4, h = l + 2;
        uint16_t ctl = KH_IO16(h);

        if ((ctl & 0x80) && !t->running) {
            t->running = 1;
            t->reload = KH_IO16(l);
            t->shift = s_shift[ctl & 3];
            t->start_us = now;
            t->overflows_seen = 0;
            if (ctl & 4) {
                static int warned;
                if (!warned++)
                    LOG("timers: count-up mode requested on timer %d (not emulated)", n);
            }
        } else if (!(ctl & 0x80) && t->running) {
            t->running = 0;
            continue;
        } else if (t->running && KH_IO16(l) != t->last_written) {
            t->reload = KH_IO16(l); /* the game wrote a new reload */
            t->start_us = now;
            t->overflows_seen = 0;
        }
        if (!t->running)
            continue;
        {
            uint64_t period = 0x10000u - t->reload, ticks = ticks_since(t, now), ovf = 0;
            uint16_t count;
            if (ticks < period) {
                count = (uint16_t)(t->reload + ticks);
            } else {
                ovf = 1 + (ticks - period) / period;
                count = (uint16_t)(t->reload + (ticks - period) % period);
            }
            KH_IO16(l) = count;
            t->last_written = count;
            if (ovf > t->overflows_seen) {
                t->overflows_seen = ovf;
                if (ctl & 0x40)
                    kh_irq_raise(KH_IRQ_TIMER0 << n);
            }
        }
    }
}

uint32_t kh_timers_next_event_us(void)
{
    uint64_t now = sceKernelGetProcessTimeWide(), best = UINT32_MAX;
    int n;
    for (n = 0; n < 4; n++) {
        const Timer *t = &s_timers[n];
        uint64_t period, ticks, left, us;
        if (!t->running || !(KH_IO16(0x04000102 + n * 4) & 0x40))
            continue;
        period = 0x10000u - t->reload;
        ticks = ticks_since(t, now);
        left = ticks < period ? period - ticks : period - (ticks - period) % period;
        us = left * (1000000ull << t->shift) / KH_DS_BUS_HZ + 1;
        if (us < best)
            best = us;
    }
    return (uint32_t)best;
}
