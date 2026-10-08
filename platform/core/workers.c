#include "workers.h"

#include "log.h"
#include "threadstat.h"

#include <psp2/kernel/threadmgr.h>

static SceUID s_go = -1, s_done = -1;
/* the second helper, on the game's core below the game's threads: it only works while the
 * game waits for its next frame (the game core is about half idle in the field), and only on
 * jobs with a frame to spare (workers_begin_spare): taken over by the game in the middle of a
 * chunk, it can hold that chunk for a few milliseconds */
static SceUID s_go_spare = -1;
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

/* The second helper: it starts a chunk only when it gets the game's core (below the game's
 * threads), but runs it above them, so that no chunk it took waits for the game. Taken over in
 * the middle of one, it held that chunk as long as the game kept its core busy: in the videos
 * (the game decoding at 100%) the display waited up to 1.9 s for the 2D (0.4.0's log: the
 * cutscenes froze, a frame repeated). The priority goes up before the chunk is taken. */
static int spare_worker(SceSize args, void *argp)
{
    (void)args, (void)argp;
    for (;;) {
        sceKernelWaitSema(s_go_spare, 1, NULL);
        for (;;) {
            int c;
            sceKernelChangeThreadPriority(0, KH_SPARE_RUN_PRIORITY);
            c = __atomic_fetch_add(&s_next, 1, __ATOMIC_ACQ_REL);
            if (c < s_n) {
                s_fn(c, s_arg);
                if (__atomic_add_fetch(&s_finished, 1, __ATOMIC_ACQ_REL) == s_n)
                    sceKernelSignalSema(s_done, 1);
            }
            sceKernelChangeThreadPriority(0, KH_SPARE_PRIORITY);
            if (c >= s_n)
                break;
        }
    }
    return 0;
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
    threadstat_add("2d helper", th);
    s_go_spare = sceKernelCreateSema("kh_work_spare", 0, 0, 1, NULL);
    th = s_go_spare < 0 ? -1
                        : sceKernelCreateThread("kh_worker_spare", spare_worker, KH_SPARE_PRIORITY,
                                                0x10000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0)
        s_go_spare = -1;
    else
        threadstat_add("2d spare", th);
    LOG("workers: helper thread on core 2%s",
        s_go_spare >= 0 ? ", and one on core 1 below the game" : "");
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
