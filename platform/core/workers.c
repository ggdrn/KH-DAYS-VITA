#include "workers.h"

#include "log.h"

#include <psp2/kernel/threadmgr.h>

static SceUID s_go = -1, s_done = -1;
/* the second helper, on the game's core below the game's threads: it only works while the
 * game waits for its next frame (the game core is about half idle in the field), and only on
 * jobs with a frame to spare (workers_begin_spare): taken over by the game in the middle of a
 * chunk, it can hold that chunk for a few milliseconds */
static SceUID s_go_spare = -1;
/* the side thread: one job at a time, beside the chunk jobs */
static SceUID s_side_go = -1, s_side_done = -1;
static void (*volatile s_side_fn)(void *);
static void *volatile s_side_arg;
static WorkFn s_fn;
static void *s_arg;
static int s_n;
static volatile int s_next, s_finished;
static int s_active;

/* chunks until none is left; the one finishing the last chunk signals the joiner */
static void pull(void)
{
    for (;;) {
        int c = __atomic_fetch_add(&s_next, 1, __ATOMIC_ACQ_REL);
        if (c >= s_n)
            return;
        s_fn(c, s_arg);
        if (__atomic_add_fetch(&s_finished, 1, __ATOMIC_ACQ_REL) == s_n)
            sceKernelSignalSema(s_done, 1);
    }
}

static int worker(SceSize args, void *argp)
{
    const SceUID go = *(const SceUID *)argp;
    (void)args;
    for (;;) {
        sceKernelWaitSema(go, 1, NULL);
        pull();
    }
    return 0;
}

static int side_worker(SceSize args, void *argp)
{
    (void)args, (void)argp;
    for (;;) {
        sceKernelWaitSema(s_side_go, 1, NULL);
        s_side_fn(s_side_arg);
        sceKernelSignalSema(s_side_done, 1);
    }
    return 0;
}

int workers_side_begin(void (*fn)(void *), void *arg)
{
    if (s_side_go < 0)
        return 0;
    s_side_fn = fn;
    s_side_arg = arg;
    sceKernelSignalSema(s_side_go, 1);
    return 1;
}

void workers_side_join(void)
{
    sceKernelWaitSema(s_side_done, 1, NULL);
}

void workers_init(void)
{
    SceUID th;
    s_go = sceKernelCreateSema("kh_work_go", 0, 0, 1, NULL);
    s_done = sceKernelCreateSema("kh_work_done", 0, 0, 1, NULL);
    th = sceKernelCreateThread("kh_worker", worker, KH_COMPUTE_PRIORITY, 0x10000, 0,
                               SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (s_go < 0 || s_done < 0 || th < 0 || sceKernelStartThread(th, sizeof(s_go), &s_go) < 0) {
        LOG("workers: no helper thread (%08x %08x %08x): single-threaded", s_go, s_done, th);
        s_go = -1;
        return;
    }
    s_go_spare = sceKernelCreateSema("kh_work_spare", 0, 0, 1, NULL);
    th = s_go_spare < 0 ? -1
                        : sceKernelCreateThread("kh_worker_spare", worker, KH_SPARE_PRIORITY,
                                                0x10000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th < 0 || sceKernelStartThread(th, sizeof(s_go_spare), &s_go_spare) < 0)
        s_go_spare = -1;
    s_side_go = sceKernelCreateSema("kh_side_go", 0, 0, 1, NULL);
    s_side_done = sceKernelCreateSema("kh_side_done", 0, 0, 1, NULL);
    th = s_side_go < 0 || s_side_done < 0
             ? -1
             : sceKernelCreateThread("kh_worker_side", side_worker, KH_SIDE_PRIORITY, 0x10000, 0,
                                     SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0)
        s_side_go = -1;
    LOG("workers: helper thread on core 2%s%s",
        s_go_spare >= 0 ? ", and one on core 1 below the game" : "",
        s_side_go >= 0 ? ", a side one on cores 1-2" : "");
}

void workers_begin(WorkFn fn, int n, void *arg)
{
    s_fn = fn;
    s_arg = arg;
    s_n = n;
    s_finished = 0;
    __atomic_store_n(&s_next, 0, __ATOMIC_RELEASE);
    s_active = n > 0;
    if (s_go >= 0 && n > 0)
        sceKernelSignalSema(s_go, 1);
}

void workers_begin_spare(WorkFn fn, int n, void *arg)
{
    workers_begin(fn, n, arg);
    if (s_go_spare >= 0 && n > 1)
        sceKernelSignalSema(s_go_spare, 1);
}

void workers_help(void)
{
    if (s_active && s_go >= 0)
        pull();
}

void workers_join(void)
{
    if (!s_active)
        return;
    s_active = 0;
    if (s_go < 0) {
        pull();
        return;
    }
    pull();
    sceKernelWaitSema(s_done, 1, NULL);
    /* the helper may still be between its last failed pull and its wait: harmless, the next
     * begin resets the counters before signalling it */
}
