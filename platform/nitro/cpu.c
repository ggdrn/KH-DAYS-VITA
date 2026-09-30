/* The ARM9 "CPU" the NitroSDK expects: one thread running at a time, interrupts delivered
 * between instructions, context switches by the SDK's own scheduler.
 *
 * Threads. Every NitroSDK thread (OSThread) is backed by a Vita thread, but only one of them
 * holds the baton: the others wait on their own semaphore. The SDK's scheduler
 * (OSi_RescheduleThread, compiled from the decomp) still decides who runs; the port replaces
 * only the two routines that switch: OS_SaveContext binds the calling Vita thread to its
 * OSThread and returns 0, OS_LoadContext hands the baton to the chosen thread and parks the
 * caller until it is chosen again. When the parked thread resumes, OS_LoadContext returns into
 * OSi_RescheduleThread, which returns to whoever asked to reschedule -- exactly where
 * OS_SaveContext would have returned TRUE on the DS.
 *
 * Interrupts. Hardware events (VBlank from the display loop, timer overflows, card and ARM7
 * completions) set bits in kh_irq_pending from any Vita thread. They are delivered on the
 * baton holder at the points where the DS could take them and the port can see: interrupts
 * being re-enabled, the idle thread halting, and busy-waits on DISPSTAT/VCOUNT. Delivery
 * follows OS_IrqHandler: lowest pending bit first, IF acknowledged, the handler from the DTCM
 * vector table run in IRQ mode, then OSi_IrqThreadQueue woken and a pending reschedule done.
 */
#include "nitro/cpu.h"

#include "hw/io.h"
#include "hw/memmap.h"
#include "hw/timers.h"
#include "log.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>
#include <string.h>

/* ---- decomp symbols -------------------------------------------------------------------- */

typedef void (*IrqFn)(void);
extern IrqFn data_027e0000[22];                  /* OS_IRQTable, at the start of DTCM */
extern struct { void *head, *tail; } data_027e0058; /* OSi_IrqThreadQueue */
extern struct {
    uint16_t isNeedRescheduling;
    uint16_t irqDepth;
} data_02044330;                                 /* OSi_ThreadInfo */

extern void OSi_RescheduleThread(void);
extern void OS_WakeupThread(void *queue);

/* ---- CPU state ------------------------------------------------------------------------- */

volatile uint32_t kh_cpsr_if;          /* 0x80: IRQ masked, 0x40: FIQ masked */
static volatile int s_in_irq;
static volatile SceUID s_owner = -1;   /* the Vita thread holding the baton */
static volatile uint32_t s_pending;
static SceUID s_event = -1;            /* raised with every new pending bit */

#define REG_IME 0x04000208
#define REG_IE 0x04000210
#define REG_IF 0x04000214

void kh_cpu_init(void)
{
    s_event = sceKernelCreateEventFlag("kh_irq", SCE_EVENT_WAITMULTIPLE, 0, NULL);
}

void kh_irq_raise(uint32_t bits)
{
    __atomic_fetch_or(&s_pending, bits, __ATOMIC_SEQ_CST);
    KH_IO32(REG_IF) = s_pending;
    if (s_event >= 0)
        sceKernelSetEventFlag(s_event, 1);
}

void kh_irq_ack(uint32_t bits)
{
    __atomic_fetch_and(&s_pending, ~bits, __ATOMIC_SEQ_CST);
    KH_IO32(REG_IF) = s_pending;
}

/* OS_ResetRequestIrqMask: IF is write-1-to-clear, which a memory store cannot do. */
uint32_t OS_ResetRequestIrqMask(uint32_t mask)
{
    uint32_t old = s_pending;
    kh_irq_ack(mask);
    return old;
}

/* ---- deferred completions (card reads, ARM7 replies) ----------------------------------------
 * Run like an interrupt handler: on the baton holder, in IRQ mode. */

typedef struct Deferred {
    void (*fn)(void *a, void *b, void *c);
    void *a, *b, *c;
    struct Deferred *next;
} Deferred;

static Deferred *volatile s_deferred;
static SceUID s_deferred_lock = -1;

void kh_cpu_defer(void (*fn)(void *, void *, void *), void *a, void *b, void *c)
{
    Deferred *d = malloc(sizeof(*d)), **tail;
    d->fn = fn, d->a = a, d->b = b, d->c = c, d->next = NULL;
    if (s_deferred_lock < 0)
        s_deferred_lock = sceKernelCreateMutex("kh_defer", 0, 0, NULL);
    sceKernelLockMutex(s_deferred_lock, 1, NULL);
    for (tail = (Deferred **)&s_deferred; *tail; tail = &(*tail)->next)
        ;
    *tail = d;
    sceKernelUnlockMutex(s_deferred_lock, 1);
    if (s_event >= 0)
        sceKernelSetEventFlag(s_event, 1);
}

static Deferred *take_deferred(void)
{
    Deferred *list;
    if (!s_deferred)
        return NULL;
    sceKernelLockMutex(s_deferred_lock, 1, NULL);
    list = s_deferred;
    s_deferred = NULL;
    sceKernelUnlockMutex(s_deferred_lock, 1);
    return list;
}

/* ---- delivery -------------------------------------------------------------------------- */

static int deliverable(void)
{
    return (KH_IO16(REG_IME) & 1) && !(kh_cpsr_if & 0x80) &&
           ((KH_IO32(REG_IE) & s_pending) || s_deferred);
}

static void after_irq(void)
{
    if (data_027e0058.head)
        OS_WakeupThread(&data_027e0058);
    if (data_02044330.isNeedRescheduling) {
        data_02044330.isNeedRescheduling = 0;
        s_in_irq = 0;
        OSi_RescheduleThread();
    }
}

void kh_cpu_set_owner(void)
{
    s_owner = sceKernelGetThreadId();
}

void kh_cpu_poll(void)
{
    uint32_t saved;
    if (s_in_irq || sceKernelGetThreadId() != s_owner)
        return;
    kh_timers_update();
    while (deliverable()) {
        uint32_t live = KH_IO32(REG_IE) & s_pending;
        saved = kh_cpsr_if;
        kh_cpsr_if |= 0x80; /* the CPU masks IRQs on entry */
        s_in_irq = 1;
        if (live) {
            int bit = __builtin_ctz(live);
            kh_irq_ack(1u << bit);
            if (bit < 22 && data_027e0000[bit])
                data_027e0000[bit]();
        } else {
            Deferred *d = take_deferred();
            while (d) {
                Deferred *next = d->next;
                d->fn(d->a, d->b, d->c);
                free(d);
                d = next;
            }
        }
        kh_cpsr_if = saved;
        after_irq();
        s_in_irq = 0;
    }
}

int kh_cpu_in_irq(void)
{
    return s_in_irq;
}

/* The idle thread's OS_Halt: sleep until something can be delivered. */
void OS_Halt(void)
{
    for (;;) {
        uint32_t timeout = kh_timers_next_event_us();
        if (deliverable()) {
            kh_cpu_poll();
            return;
        }
        sceKernelClearEventFlag(s_event, 0);
        if (deliverable())
            continue;
        if (timeout > 2000)
            timeout = 2000; /* timers and busy registers are re-read at least this often */
        sceKernelWaitEventFlag(s_event, 1, SCE_EVENT_WAITOR, NULL, &timeout);
        kh_timers_update();
    }
}

/* ---- interrupt mask (CPSR) --------------------------------------------------------------- */

uint32_t OS_DisableInterrupts(void)
{
    uint32_t old = kh_cpsr_if & 0x80;
    kh_cpsr_if |= 0x80;
    return old;
}

uint32_t OS_EnableInterrupts(void)
{
    uint32_t old = kh_cpsr_if & 0x80;
    kh_cpsr_if &= ~0x80u;
    kh_cpu_poll();
    return old;
}

uint32_t OS_RestoreInterrupts(uint32_t state)
{
    uint32_t old = kh_cpsr_if & 0x80;
    kh_cpsr_if = (kh_cpsr_if & ~0x80u) | (state & 0x80);
    if (!(state & 0x80))
        kh_cpu_poll();
    return old;
}

uint32_t OS_DisableInterrupts_IrqAndFiq(void)
{
    uint32_t old = kh_cpsr_if & 0xc0;
    kh_cpsr_if |= 0xc0;
    return old;
}

uint32_t OS_RestoreInterrupts_IrqAndFiq(uint32_t state)
{
    uint32_t old = kh_cpsr_if & 0xc0;
    kh_cpsr_if = (kh_cpsr_if & ~0xc0u) | (state & 0xc0);
    if (!(state & 0x80))
        kh_cpu_poll();
    return old;
}

uint32_t OS_GetCpsrIrq(void)
{
    return kh_cpsr_if & 0x80;
}

uint32_t OS_GetProcMode(void)
{
    return s_in_irq ? 0x12 : 0x1f; /* IRQ mode while a handler runs, system mode otherwise */
}

/* ---- threads ------------------------------------------------------------------------------ */

/* OSContext offsets (include/nitro/os.h) */
#define CTX_R0 0x04
#define CTX_LR 0x3c
#define CTX_PC4 0x40
#define CTX_SP_SVC 0x44

#define HOST_STACK_SIZE (256 * 1024)

typedef struct KhThread {
    void *ctx;
    SceUID host;
    SceUID sema;
    volatile int exit_requested;
    struct KhThread *next;
} KhThread;

static KhThread *s_threads;

static KhThread *by_ctx(void *ctx)
{
    KhThread *t;
    for (t = s_threads; t; t = t->next)
        if (t->ctx == ctx)
            return t;
    t = calloc(1, sizeof(*t));
    t->ctx = ctx;
    t->sema = sceKernelCreateSema("kh_th", 0, 0, 1, NULL);
    t->next = s_threads;
    s_threads = t;
    return t;
}

static KhThread *by_host(SceUID host)
{
    KhThread *t;
    for (t = s_threads; t; t = t->next)
        if (t->host == host)
            return t;
    return NULL;
}

static uint32_t ctx_word(void *ctx, int off)
{
    return *(uint32_t *)((uint8_t *)ctx + off);
}

static void park(KhThread *me)
{
    sceKernelWaitSema(me->sema, 1, NULL);
    s_owner = me->host;
    if (me->exit_requested) {
        me->exit_requested = 0;
        sceKernelExitDeleteThread(0);
    }
}

static int trampoline(SceSize args, void *argp)
{
    KhThread *me = *(KhThread **)argp;
    void (*entry)(void *);
    void (*on_return)(void);

    park(me);
    entry = (void (*)(void *))(uintptr_t)(ctx_word(me->ctx, CTX_PC4) - 4);
    on_return = (void (*)(void))(uintptr_t)ctx_word(me->ctx, CTX_LR);
    entry((void *)(uintptr_t)ctx_word(me->ctx, CTX_R0));
    if (on_return)
        on_return(); /* OS_ExitThread: reschedules away and never comes back */
    for (;;)
        park(me);
    return 0;
}

/* The DS version prepares the registers the thread starts with. The port keeps the same
 * fields (the SDK stores the argument and the exit hook into them afterwards) and forgets any
 * Vita thread a previous life of this OSThread had. */
void OS_InitContext(void *ctx, uint32_t newpc, uint32_t newsp)
{
    uint32_t *w = ctx;
    KhThread *t = by_ctx(ctx);
    memset(w, 0, 0x48);
    w[CTX_PC4 / 4] = newpc + 4;
    w[CTX_SP_SVC / 4] = newsp;
    w[0x38 / 4] = (newsp - 0x40) & ~7u;
    w[0] = 0x1f | ((newpc & 1) ? 0x20 : 0);
    if (t->host > 0) {
        t->exit_requested = 1;
        sceKernelSignalSema(t->sema, 1);
    }
    t->host = 0;
}

int OS_SaveContext(void *ctx)
{
    SceUID self = sceKernelGetThreadId();
    KhThread *t = by_ctx(ctx);
    if (t->host <= 0)
        t->host = self; /* the launcher thread: the Vita thread NitroMain started on */
    return 0;
}

void OS_LoadContext(void *ctx)
{
    SceUID self = sceKernelGetThreadId();
    KhThread *me = by_host(self);
    KhThread *next = by_ctx(ctx);

    if (next == me)
        return;
    if (next->host <= 0) {
        KhThread *arg = next;
        next->host = sceKernelCreateThread("kh_nitro", trampoline, 0x10000100, HOST_STACK_SIZE, 0,
                                           SCE_KERNEL_CPU_MASK_USER_0, NULL);
        if (next->host < 0) {
            LOG("cpu: thread create failed %08x", next->host);
            return;
        }
        sceKernelStartThread(next->host, sizeof(arg), &arg);
    }
    s_owner = next->host;
    sceKernelSignalSema(next->sema, 1);
    if (me)
        park(me);
}
