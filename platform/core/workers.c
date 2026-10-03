#include "workers.h"

#include "log.h"

#include <psp2/kernel/threadmgr.h>

static SceUID s_go = -1, s_done = -1;
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
    (void)args;
    (void)argp;
    for (;;) {
        sceKernelWaitSema(s_go, 1, NULL);
        pull();
    }
    return 0;
}

void workers_init(void)
{
    SceUID th;
    s_go = sceKernelCreateSema("kh_work_go", 0, 0, 1, NULL);
    s_done = sceKernelCreateSema("kh_work_done", 0, 0, 1, NULL);
    th = sceKernelCreateThread("kh_worker", worker, 0x10000100, 0x10000, 0,
                               SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (s_go < 0 || s_done < 0 || th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        LOG("workers: no helper thread (%08x %08x %08x): single-threaded", s_go, s_done, th);
        s_go = -1;
        return;
    }
    LOG("workers: helper thread on core 2");
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
