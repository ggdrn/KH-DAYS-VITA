#include "workers.h"

#include "log.h"

#include <psp2/kernel/threadmgr.h>

static SceUID s_go = -1, s_done = -1;
static void (*volatile s_fn)(void *);
static void *volatile s_arg;
static int s_pending;

static int worker(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (;;) {
        sceKernelWaitSema(s_go, 1, NULL);
        s_fn(s_arg);
        sceKernelSignalSema(s_done, 1);
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

void workers_post(void (*fn)(void *), void *arg)
{
    if (s_go < 0) {
        fn(arg);
        return;
    }
    s_fn = fn;
    s_arg = arg;
    s_pending = 1;
    sceKernelSignalSema(s_go, 1);
}

void workers_wait(void)
{
    if (s_pending) {
        sceKernelWaitSema(s_done, 1, NULL);
        s_pending = 0;
    }
}
