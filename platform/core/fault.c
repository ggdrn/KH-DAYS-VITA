/* Fault reporting through kubridge (optional plugin): data aborts, prefetch aborts and undefined
 * instructions are written to the log with every register before the default handler takes
 * the process down (and the system writes its core dump).
 *
 * The log line carries the run-time address of main(), because the eboot is relocated when it
 * loads: tools/symbolize.py uses it to map PC/LR back to the ELF and to file and line.
 * Without the plugin kuKernelRegisterExceptionHandler resolves to a weak stub and this is a
 * no-op. */
#include "fault.h"

#include "log.h"

#include <kubridge.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>

extern int main(void);

static KuKernelExceptionHandler s_prev[3];

static void report(KuKernelExceptionContext *c)
{
    static const char *const kinds[] = { "data abort", "prefetch abort", "undefined instruction" };
    char buf[512];
    SceKernelThreadInfo ti = { .size = sizeof(ti) };
    int n;

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
}

#define HANDLER(i)                                                                 \
    static void handler##i(KuKernelExceptionContext *c)                            \
    {                                                                              \
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
    int i, ok = 0;
    for (i = 0; i < 3; i++)
        ok += kuKernelRegisterExceptionHandler(i, h[i], &s_prev[i], NULL) >= 0;
    LOG("fault: %s (main=%08x)", ok == 3 ? "kubridge handlers installed" : "kubridge not available",
        (unsigned)(uintptr_t)main);
}
