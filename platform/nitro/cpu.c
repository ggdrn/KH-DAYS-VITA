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
#include "workers.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>
#include <string.h>

/* ---- decomp symbols -------------------------------------------------------------------- */

typedef void (*IrqFn)(void);
extern IrqFn data_027e0000[22];                  /* OS_IRQTable, at the start of DTCM */
/* OSi_IrqThreadQueue: the queue OS_WaitIrq sleeps on (OS_InitIrqTable clears it) */
extern struct { void *head, *tail; } data_027e006c;
extern struct {
    uint16_t isNeedRescheduling;
    uint16_t irqDepth;
    uint8_t *current; /* OSThread * */
    uint8_t *list;    /* by priority, through OSThread.next */
} data_02044330;                                 /* OSi_ThreadInfo */

extern void OSi_RescheduleThread(void);
extern void OS_WakeupThread(void *queue);

/* ---- CPU state ------------------------------------------------------------------------- */

volatile uint32_t kh_cpsr_if;          /* 0x80: IRQ masked, 0x40: FIQ masked */
static volatile SceUID s_owner = -1;   /* the Vita thread holding the baton */
static volatile SceUID s_irq_thread = -1; /* the Vita thread running a handler, or -1 */
static volatile int s_irq_busy;        /* a handler is running (either delivery path) */
static volatile uint32_t s_pending;
static SceUID s_event = -1;            /* raised with every new pending bit */

volatile uint32_t kh_cpu_irqs_delivered, kh_cpu_preempted, kh_cpu_switches;

#define REG_IME 0x04000208
#define REG_IE 0x04000210
#define REG_IF 0x04000214

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
 * Run like an interrupt handler, in IRQ mode. */

typedef struct Deferred {
    void (*fn)(void *a, void *b, void *c);
    void *a, *b, *c;
    struct Deferred *next;
} Deferred;

static volatile uint32_t s_irq_cpsr = 0x80; /* IRQ mode's own I/F bits (see OS_DisableInterrupts) */
static Deferred *volatile s_deferred;      /* interrupt completions: need IME and IE */
static Deferred *volatile s_task_deferred; /* thread completions: need only the I bit clear */
static SceUID s_deferred_lock = -1;

static void defer_on(Deferred *volatile *list, void (*fn)(void *, void *, void *), void *a,
                     void *b, void *c)
{
    Deferred *d = malloc(sizeof(*d)), **tail;
    d->fn = fn, d->a = a, d->b = b, d->c = c, d->next = NULL;
    sceKernelLockMutex(s_deferred_lock, 1, NULL);
    for (tail = (Deferred **)list; *tail; tail = &(*tail)->next)
        ;
    *tail = d;
    sceKernelUnlockMutex(s_deferred_lock, 1);
    if (s_event >= 0)
        sceKernelSetEventFlag(s_event, 1);
}

void kh_cpu_defer(void (*fn)(void *, void *, void *), void *a, void *b, void *c)
{
    defer_on(&s_deferred, fn, a, b, c);
}

void kh_cpu_defer_task(void (*fn)(void *, void *, void *), void *a, void *b, void *c)
{
    defer_on(&s_task_deferred, fn, a, b, c);
}

static Deferred *take_deferred(Deferred *volatile *list)
{
    Deferred *taken;
    if (!*list)
        return NULL;
    sceKernelLockMutex(s_deferred_lock, 1, NULL);
    taken = *list;
    *list = NULL;
    sceKernelUnlockMutex(s_deferred_lock, 1);
    return taken;
}

static void run_deferred(Deferred *d)
{
    while (d) {
        Deferred *next = d->next;
        d->fn(d->a, d->b, d->c);
        free(d);
        d = next;
    }
}

/* ---- delivery -------------------------------------------------------------------------------
 * Two paths share it. The baton holder delivers at the points the port can see (interrupts
 * re-enabled, the idle thread halting, DISPSTAT spins). If it does not get there -- a loop that
 * waits for something only an interrupt handler sets, as the DS allowed -- the IRQ thread
 * delivers instead, alongside it, like a real interrupt arriving between two instructions.
 *
 * Mutual exclusion with OS_DisableInterrupts is a store-then-check handshake on two words
 * (s_irq_busy, the I bit), sequentially consistent: a handler starts only if it sees the I bit
 * clear after claiming s_irq_busy, and OS_DisableInterrupts returns only after it sees
 * s_irq_busy clear after setting the I bit. So no handler runs inside a critical section. */

static int irq_work(void)
{
    return (KH_IO16(REG_IME) & 1) && ((KH_IO32(REG_IE) & s_pending) || s_deferred);
}

static int deliverable(void)
{
    return !(__atomic_load_n(&kh_cpsr_if, __ATOMIC_SEQ_CST) & 0x80) &&
           (irq_work() || s_task_deferred);
}

static int claim(void)
{
    int zero = 0;
    if (!__atomic_compare_exchange_n(&s_irq_busy, &zero, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
    if (__atomic_load_n(&kh_cpsr_if, __ATOMIC_SEQ_CST) & 0x80) {
        __atomic_store_n(&s_irq_busy, 0, __ATOMIC_SEQ_CST);
        return 0;
    }
    return 1;
}

static void release(void)
{
    __atomic_store_n(&s_irq_busy, 0, __ATOMIC_SEQ_CST);
}

/* One round: every deliverable source, as OS_IrqHandler would take them one after another.
 * Called with the claim held. */
static void run_handlers(void)
{
    s_irq_thread = sceKernelGetThreadId();
    s_irq_cpsr = 0x80; /* IRQ mode is entered with IRQs masked */
    while (irq_work() || s_task_deferred) {
        uint32_t live = (KH_IO16(REG_IME) & 1) ? KH_IO32(REG_IE) & s_pending : 0;
        if (live) {
            int bit = __builtin_ctz(live);
            kh_irq_ack(1u << bit);
            if (bit < 22 && data_027e0000[bit])
                data_027e0000[bit]();
        } else if (s_task_deferred) {
            run_deferred(take_deferred(&s_task_deferred));
        } else {
            run_deferred(take_deferred(&s_deferred));
        }
        kh_cpu_irqs_delivered++;
        /* OS_IrqHandler_ThreadSwitch: threads waiting in OS_WaitIrq become ready */
        if (data_027e006c.head)
            OS_WakeupThread(&data_027e006c);
    }
    s_irq_thread = -1;
}

/* The reschedule the DS did on return from the interrupt, on the thread that was interrupted. */
static void reschedule_if_needed(void)
{
    if (data_02044330.isNeedRescheduling && !(kh_cpsr_if & 0x80)) {
        data_02044330.isNeedRescheduling = 0;
        kh_cpu_switches++;
        OSi_RescheduleThread();
    }
}

void kh_cpu_set_owner(void)
{
    s_owner = sceKernelGetThreadId();
}

void kh_cpu_poll(void)
{
    SceUID self = sceKernelGetThreadId();
    if (self != s_owner || self == s_irq_thread)
        return;
    kh_timers_update();
    if (deliverable() && claim()) {
        run_handlers();
        release();
    }
    reschedule_if_needed();
}

int kh_cpu_in_irq(void)
{
    return sceKernelGetThreadId() == s_irq_thread;
}

/* The IRQ thread: whatever the baton holder leaves pending for more than half a millisecond is
 * delivered here. The reschedule it may call for happens on the baton holder's next poll. */
static int irq_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (;;) {
        sceKernelDelayThread(500);
        kh_timers_update();
        if (s_owner < 0 || !deliverable() || !claim())
            continue;
        kh_cpu_preempted++;
        run_handlers();
        release();
    }
    return 0;
}

void kh_cpu_init(void)
{
    SceUID th;
    s_event = sceKernelCreateEventFlag("kh_irq", SCE_EVENT_WAITMULTIPLE, 0, NULL);
    s_deferred_lock = sceKernelCreateMutex("kh_defer", 0, 0, NULL);
    th = sceKernelCreateThread("kh_irq", irq_thread, 0x10000100 - 10, 0x10000, 0,
                               SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);
}

/* The idle thread's OS_Halt: sleep until something can be delivered. */
volatile uint64_t kh_cpu_halt_us; /* time the game core spent idle in OS_Halt */

void OS_Halt(void)
{
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    for (;;) {
        uint32_t timeout = kh_timers_next_event_us();
        if (deliverable() || data_02044330.isNeedRescheduling) {
            kh_cpu_halt_us += sceKernelGetProcessTimeWide() - t0;
            kh_cpu_poll();
            return;
        }
        sceKernelClearEventFlag(s_event, 0);
        if (deliverable())
            continue;
        if (timeout > 2000)
            timeout = 2000; /* timers are re-read at least this often */
        sceKernelWaitEventFlag(s_event, 1, SCE_EVENT_WAITOR, NULL, &timeout);
        kh_timers_update();
    }
}

/* ---- interrupt mask (CPSR) --------------------------------------------------------------- */

static void wait_for_handler(void)
{
    /* a handler started by the IRQ thread before we masked: let it finish */
    while (__atomic_load_n(&s_irq_busy, __ATOMIC_SEQ_CST) &&
           sceKernelGetThreadId() != s_irq_thread)
        ;
}

/* IRQ mode has its own CPSR on the DS: a handler masks and unmasks without touching the I bit of
 * the code it interrupted, which the hardware restores on return. Here handlers run on another
 * Vita thread (or nested on the baton holder), so they get their own copy, s_irq_cpsr, set
 * masked on entry (run_handlers). Sharing kh_cpsr_if let a handler's OS_DisableInterrupts land
 * in the interrupted thread's saved state: it then restored "disabled" and stayed so (0.0.26
 * waited in TP_WaitBusy with the I bit set for good). */
uint32_t OS_DisableInterrupts(void)
{
    KH_PROBE("OS_DisableInterrupts");
    if (kh_cpu_in_irq()) {
        uint32_t old = s_irq_cpsr & 0x80;
        s_irq_cpsr |= 0x80;
        return old;
    } else {
        uint32_t old = __atomic_fetch_or(&kh_cpsr_if, 0x80, __ATOMIC_SEQ_CST) & 0x80;
        wait_for_handler();
        return old;
    }
}

uint32_t OS_EnableInterrupts(void)
{
    KH_PROBE("OS_EnableInterrupts");
    if (kh_cpu_in_irq()) {
        uint32_t old = s_irq_cpsr & 0x80;
        s_irq_cpsr &= ~0x80u; /* nested interrupts are not taken: the round goes on */
        return old;
    } else {
        uint32_t old = __atomic_fetch_and(&kh_cpsr_if, ~0x80u, __ATOMIC_SEQ_CST) & 0x80;
        kh_cpu_poll();
        return old;
    }
}

uint32_t OS_RestoreInterrupts(uint32_t state)
{
    KH_PROBE("OS_RestoreInterrupts");
    uint32_t old;
    if (kh_cpu_in_irq()) {
        old = s_irq_cpsr & 0x80;
        s_irq_cpsr = (s_irq_cpsr & ~0x80u) | (state & 0x80);
    } else if (state & 0x80) {
        old = __atomic_fetch_or(&kh_cpsr_if, 0x80, __ATOMIC_SEQ_CST) & 0x80;
        wait_for_handler();
    } else {
        old = __atomic_fetch_and(&kh_cpsr_if, ~0x80u, __ATOMIC_SEQ_CST) & 0x80;
        kh_cpu_poll();
    }
    return old;
}

uint32_t OS_DisableInterrupts_IrqAndFiq(void)
{
    KH_PROBE("OS_DisableInterrupts_IrqAndFiq");
    if (kh_cpu_in_irq()) {
        uint32_t old = s_irq_cpsr & 0xc0;
        s_irq_cpsr |= 0xc0;
        return old;
    } else {
        uint32_t old = __atomic_fetch_or(&kh_cpsr_if, 0xc0, __ATOMIC_SEQ_CST) & 0xc0;
        wait_for_handler();
        return old;
    }
}

uint32_t OS_RestoreInterrupts_IrqAndFiq(uint32_t state)
{
    KH_PROBE("OS_RestoreInterrupts_IrqAndFiq");
    uint32_t old;
    if (kh_cpu_in_irq()) {
        old = s_irq_cpsr & 0xc0;
        s_irq_cpsr = (s_irq_cpsr & ~0xc0u) | (state & 0xc0);
        return old;
    }
    old = kh_cpsr_if & 0xc0;
    kh_cpsr_if = (kh_cpsr_if & ~0xc0u) | (state & 0xc0);
    if (!(state & 0x80))
        kh_cpu_poll();
    return old;
}

uint32_t OS_GetCpsrIrq(void)
{
    KH_PROBE("OS_GetCpsrIrq");
    return (kh_cpu_in_irq() ? s_irq_cpsr : kh_cpsr_if) & 0x80;
}

uint32_t OS_GetProcMode(void)
{
    /* IRQ mode on the thread running a handler, system mode everywhere else */
    return kh_cpu_in_irq() ? 0x12 : 0x1f;
}

/* ---- threads ------------------------------------------------------------------------------ */

/* OSContext offsets (include/nitro/os.h) */
#define CTX_R0 0x04
#define CTX_LR 0x3c
#define CTX_PC4 0x40
#define CTX_SP_SVC 0x44

#define HOST_STACK_SIZE (256 * 1024)

#define BT_MAX 16

typedef struct KhThread {
    void *ctx;
    SceUID host;
    SceUID sema;
    volatile int exit_requested;
    uint32_t cpsr_if; /* its I/F bits while switched out (the DS saves CPSR in the context) */
    uint32_t bt[BT_MAX]; /* where it last gave up the CPU (return addresses, innermost first) */
    int bt_n;
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

static KhThread *find_ctx(const void *ctx)
{
    KhThread *t;
    for (t = s_threads; t; t = t->next)
        if (t->ctx == ctx)
            return t;
    return NULL;
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

/* ARM EHABI unwinder in libgcc (the SDK ships no unwind.h). Everything is built with
 * -funwind-tables, so a parked thread's stack can be walked: the watchdog shows where each
 * NitroSDK thread is waiting. */
struct _Unwind_Context;
extern int _Unwind_Backtrace(int (*fn)(struct _Unwind_Context *, void *), void *arg);
extern int _Unwind_VRS_Get(struct _Unwind_Context *c, int regclass, uint32_t reg, int repr,
                           void *value);
#define URC_OK 0
#define URC_END_OF_STACK 5

typedef struct {
    uint32_t *pc;
    int *n;
} BtOut;

static int bt_frame(struct _Unwind_Context *c, void *arg)
{
    BtOut *o = arg;
    uint32_t pc;
    if (*o->n >= BT_MAX)
        return URC_END_OF_STACK;
    _Unwind_VRS_Get(c, 0 /* core */, 15, 0 /* uint32 */, &pc);
    o->pc[(*o->n)++] = pc & ~1u;
    return URC_OK;
}

static void backtrace_into(uint32_t *pc, int *n)
{
    BtOut o = { pc, n };
    *n = 0;
    _Unwind_Backtrace(bt_frame, &o);
}

/* ---- the probe: where is the running thread? ------------------------------------------------
 * A parked thread's stack is known (park records it); the baton holder's is not. When the
 * watchdog finds no progress it arms the probe, and the next call the baton holder makes into
 * the port (interrupt masking, a computed register, the GX FIFO, DMA, PXI, the card) records
 * its stack. If none comes, the thread is spinning on plain memory. */
volatile int kh_probe_armed;
static volatile int s_probe_done;
static const char *s_probe_where;
static uint32_t s_probe_bt[BT_MAX];
static int s_probe_n;

void kh_probe_hit(const char *where)
{
    if (sceKernelGetThreadId() != s_owner || s_probe_done)
        return;
    backtrace_into(s_probe_bt, &s_probe_n);
    s_probe_where = where;
    kh_probe_armed = 0;
    __atomic_store_n(&s_probe_done, 1, __ATOMIC_SEQ_CST);
}

void kh_probe_arm(void)
{
    s_probe_done = 0;
    kh_probe_armed = 1;
}

int kh_probe_report(void)
{
    int i;
    if (!__atomic_load_n(&s_probe_done, __ATOMIC_SEQ_CST))
        return 0;
    LOG("probe: the running thread called %s", s_probe_where);
    /* frame 0 is kh_probe_hit, frame 1 the port function */
    for (i = 1; i < s_probe_n; i++)
        LOG("probe:   at %08x", (unsigned)s_probe_bt[i]);
    s_probe_done = 0;
    return 1;
}

static void park(KhThread *me)
{
    backtrace_into(me->bt, &me->bt_n);
    sceKernelWaitSema(me->sema, 1, NULL);
    s_owner = me->host;
    /* its own interrupt state again, as OS_LoadContext restores the CPSR saved with the
     * context: a thread that went to sleep inside OS_DisableInterrupts must not leave the
     * I bit set for the one that runs next */
    __atomic_store_n(&kh_cpsr_if, me->cpsr_if, __ATOMIC_SEQ_CST);
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
    /* a new thread starts from the CPSR OS_InitContext gave it: IRQs enabled */
    __atomic_and_fetch(&kh_cpsr_if, ~0xc0u, __ATOMIC_SEQ_CST);
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
    t->cpsr_if = 0; /* w[0]: system mode, IRQ and FIQ enabled */
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
        next->host = sceKernelCreateThread("kh_nitro", trampoline, KH_COMPUTE_PRIORITY, HOST_STACK_SIZE, 0,
                                           KH_GAME_CPU_MASK, NULL);
        if (next->host < 0) {
            LOG("cpu: thread create failed %08x", next->host);
            return;
        }
        sceKernelStartThread(next->host, sizeof(arg), &arg);
        LOGV("cpu: thread %p started (entry %08x)", next->ctx, (unsigned)(ctx_word(next->ctx, CTX_PC4) - 4));
    }
    /* saved before the baton moves: the next thread writes its own state as soon as it runs */
    if (me)
        me->cpsr_if = __atomic_load_n(&kh_cpsr_if, __ATOMIC_SEQ_CST);
    s_owner = next->host;
    kh_cpu_switches++;
    sceKernelSignalSema(next->sema, 1);
    if (me)
        park(me);
}

void kh_cpu_log_state(void)
{
    const uint8_t *t;
    int n = 0;
    LOG("cpu: pending %08x IE %08x IME %d cpsr %02x busy %d owner %08x irq_thread %08x "
        "resched %d deferred %d task %d",
        (unsigned)s_pending, (unsigned)KH_IO32(REG_IE), KH_IO16(REG_IME) & 1,
        (unsigned)kh_cpsr_if, s_irq_busy, (unsigned)s_owner, (unsigned)s_irq_thread,
        data_02044330.isNeedRescheduling, s_deferred != NULL, s_task_deferred != NULL);
    /* the SDK's threads, highest priority first (OSThread: state +0x64, next +0x68, id +0x6c,
     * priority +0x70, queue +0x78; the context's pc+4 at +0x40: the entry, as the port does not
     * save registers on a switch) */
    for (t = data_02044330.list; t && n < 16; t = *(uint8_t *const *)(t + 0x68), n++) {
        const KhThread *k = find_ctx(t);
        LOG("cpu:   thread %p id %u prio %u state %u queue %p%s entry %08x host %08x%s",
            (void *)t, (unsigned)*(const uint32_t *)(t + 0x6c),
            (unsigned)*(const uint32_t *)(t + 0x70), (unsigned)*(const uint32_t *)(t + 0x64),
            *(void *const *)(t + 0x78), t == data_02044330.current ? " (current)" : "",
            (unsigned)(*(const uint32_t *)(t + 0x40) - 4), k ? (unsigned)k->host : 0u,
            k && k->host == s_owner ? " (baton)" : "");
        {
            int i;
            /* frame 0 is park itself: from 1 on, OS_LoadContext and its callers */
            for (i = 1; k && i < k->bt_n; i++)
                LOG("cpu:     at %08x", (unsigned)k->bt[i]);
        }
    }
}

/* The baton holder's live stack, for a game that spins without calling the port: every word
 * that is a return address (the instruction before it a BL or BLX in the eboot's code), from
 * the oldest frame down. Words below the thread's current sp are stale and may follow; the
 * chain that ends in the spinning function comes first. */
extern int kh_is_code(unsigned long a);

static int is_call_return(uint32_t w)
{
    uint32_t ins;
    if (!kh_is_code(w) || !kh_is_code(w - 4))
        return 0;
    ins = *(const uint32_t *)(uintptr_t)(w - 4);
    return (ins & 0x0f000000u) == 0x0b000000u ||   /* BL */
           (ins & 0xfe000000u) == 0xfa000000u ||   /* BLX imm */
           (ins & 0x0ffffff0u) == 0x012fff30u;     /* BLX reg */
}

void kh_cpu_log_owner_stack(void)
{
    SceKernelThreadInfo info = { .size = sizeof(info) };
    const uint32_t *lo, *hi, *p;
    int n = 0;
    if (s_owner < 0 || sceKernelGetThreadInfo(s_owner, &info) < 0) {
        LOG("cpu: no stack for the baton holder");
        return;
    }
    lo = (const uint32_t *)info.stack;
    hi = (const uint32_t *)((const uint8_t *)info.stack + info.stackSize);
    LOG("cpu: return addresses on %s's stack (%p-%p), oldest first:", info.name, (const void *)lo,
        (const void *)hi);
    for (p = hi - 1; p >= lo && n < 64; p--) {
        if (is_call_return(*p)) {
            LOG("cpu:   +%05x %08x", (unsigned)((const uint8_t *)hi - (const uint8_t *)p),
                (unsigned)*p);
            n++;
        }
    }
}
