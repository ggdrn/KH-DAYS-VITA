/* Fault reporting through kubridge (optional plugin): data aborts, prefetch aborts and undefined
 * instructions are written to the log with every register before the default handler takes
 * the process down (and the system writes its core dump).
 *
 * The log line carries the run-time address of main(), because the eboot is relocated when it
 * loads: tools/symbolize.py uses it to map PC/LR back to the ELF and to file and line.
 * Without the plugin kuKernelRegisterExceptionHandler resolves to a weak stub and this is a
 * no-op. */
#include <stdint.h>
#include "fault.h"

#include "log.h"

#include <kubridge.h>
#include <psp2/vshbridge.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>

extern int main(void);

static KuKernelExceptionHandler s_prev[3];

static void report(KuKernelExceptionContext *c)
{
    static const char *const kinds[] = { "data abort", "prefetch abort", "undefined instruction" };
    static uint32_t s_last_pc, s_repeats;
    char buf[512];
    SceKernelThreadInfo ti = { .size = sizeof(ti) };
    int n;

    /* a fault the default handler returns from is retried: report it a few times, not forever */
    if (c->pc == s_last_pc && ++s_repeats >= 3) {
        if (s_repeats == 3)
            LOG("fault: pc=%08x keeps faulting; not reporting it again", (unsigned)c->pc);
        return;
    }
    if (c->pc != s_last_pc)
        s_last_pc = c->pc, s_repeats = 0;
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti);
    n = snprintf(buf, sizeof(buf),
                 "FAULT %s in thread %s: pc=%08x lr=%08x sp=%08x far=%08x fsr=%08x main=%08x\n"
                 "  r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x\n"
                 "  r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x spsr=%08x\n",
                 c->exceptionType < 3 ? kinds[c->exceptionType] : "exception", ti.name,
                 (unsigned)c->pc, (unsigned)c->lr, (unsigned)c->sp, (unsigned)c->FAR,
                 (unsigned)c->FSR, (unsigned)(uintptr_t)main, (unsigned)c->r0, (unsigned)c->r1,
                 (unsigned)c->r2, (unsigned)c->r3, (unsigned)c->r4, (unsigned)c->r5,
                 (unsigned)c->r6, (unsigned)c->r7, (unsigned)c->r8, (unsigned)c->r9,
                 (unsigned)c->r10, (unsigned)c->r11, (unsigned)c->r12, (unsigned)c->SPSR);
    log_write_raw(buf, n);
    /* the top of the stack: the return addresses in it give the callers (symbolize.py picks
     * the words that point into the code) */
    {
        const uint32_t *sp = (const uint32_t *)(uintptr_t)(c->sp & ~3u);
        const uintptr_t top = (uintptr_t)ti.stack + (uintptr_t)ti.stackSize;
        int words = 128, i, j;
        if ((uintptr_t)sp < (uintptr_t)ti.stack || (uintptr_t)sp >= top)
            words = 0; /* sp is not in this thread's stack: do not touch it */
        else if ((top - (uintptr_t)sp) / 4 < (uintptr_t)words)
            words = (int)((top - (uintptr_t)sp) / 4);
        for (i = 0; i + 8 <= words; i += 8) {
            n = snprintf(buf, sizeof(buf), "  stack+%03x:", i * 4);
            for (j = 0; j < 8; j++)
                n += snprintf(buf + n, sizeof(buf) - n, " %08x", (unsigned)sp[i + j]);
            buf[n++] = '\n';
            log_write_raw(buf, n);
        }
    }
}

/* A call through a NULL pointer: on the DS address 0 is the ITCM mirror, where code sits, so
 * the game's own slips of that kind (Ov025_TickTagTrackerNodes's uninitialised `swap`) run
 * something harmless and come back. Here the jump is made to return at once, as an empty
 * function would; each call site is logged once. */
static int null_call(KuKernelExceptionContext *c)
{
    static uint32_t s_seen[16];
    uint32_t i;
    if (c->exceptionType != 1 || c->pc >= 0x1000 || c->lr < 0x80000000u)
        return 0;
    for (i = 0; i < 16 && s_seen[i] && s_seen[i] != c->lr; i++)
        ;
    if (i < 16 && !s_seen[i]) {
        s_seen[i] = c->lr;
        LOG("fault: call to %08x from lr=%08x (main=%08x) returned as a no-op", (unsigned)c->pc,
            (unsigned)c->lr, (unsigned)(uintptr_t)main);
    }
    c->r0 = 0;
    c->SPSR = (c->SPSR & ~0x20u) | ((c->lr & 1) << 5); /* back in the caller's ARM/Thumb state */
    c->pc = c->lr & ~1u;
    return 1;
}

#define HANDLER(i)                                                                 \
    static void handler##i(KuKernelExceptionContext *c)                            \
    {                                                                              \
        if (null_call(c))                                                          \
            return;                                                                \
        report(c);                                                                 \
        if (s_prev[i])                                                             \
            s_prev[i](c);                                                          \
    }
HANDLER(0)
HANDLER(1)
HANDLER(2)

void fault_init(void)
{
    static const KuKernelExceptionHandler h[3] = { handler0, handler1, handler2 };
    int i, ok = 0, search[2] = { 0, 0 };
    /* kubridge is optional (crash reports only). Without the plugin its weak import does not
     * return an error as fault.c assumed: the call jumps to address 0 and the port died at boot
     * (GitHub issue, v0.1.0). Asked for by name first. */
    if (_vshKernelSearchModuleByName("kubridge", search) < 0) {
        LOG("fault: kubridge not loaded, no crash reports (main=%08x)", (unsigned)(uintptr_t)main);
        return;
    }
    for (i = 0; i < 3; i++)
        ok += kuKernelRegisterExceptionHandler(i, h[i], &s_prev[i], NULL) >= 0;
    LOG("fault: %s (main=%08x)", ok == 3 ? "kubridge handlers installed" : "kubridge not available",
        (unsigned)(uintptr_t)main);
}

/* Whether a is an address in the eboot's code (the main module's .text through the overlays'),
 * for game code that is about to call a value it computed. */
extern char _init[], kh_ov_text_all_end[];
int kh_is_code(unsigned long a)
{
    return a >= (uintptr_t)_init && a < (uintptr_t)kh_ov_text_all_end && !(a & 3);
}
