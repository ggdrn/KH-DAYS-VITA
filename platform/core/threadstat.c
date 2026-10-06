#include "threadstat.h"

#include "log.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

#define MAX_THREADS 64

static struct {
    const char *group;
    SceUID thid;
    uint64_t last; /* run time at the last log, us */
} s_th[MAX_THREADS];
static int s_n;
static uint64_t s_last_wall;
static SceUID s_lock = -1;

void threadstat_add(const char *group, SceUID thid)
{
    if (s_lock < 0)
        s_lock = sceKernelCreateMutex("kh_threadstat", 0, 0, NULL);
    sceKernelLockMutex(s_lock, 1, NULL);
    if (s_n < MAX_THREADS) {
        s_th[s_n].group = group;
        s_th[s_n].thid = thid;
        s_th[s_n].last = 0;
        s_n++;
    }
    sceKernelUnlockMutex(s_lock, 1);
}

static uint64_t run_us(SceUID thid)
{
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(thid, &info) < 0)
        return 0;
    return info.runClocks; /* microseconds */
}

uint64_t threadstat_self_us(void)
{
    return run_us(sceKernelGetThreadId());
}

void threadstat_log(void)
{
    const char *names[16];
    uint64_t sums[16];
    int ng = 0, i, g;
    char line[256];
    size_t at;
    const uint64_t now = sceKernelGetProcessTimeWide();
    const uint64_t wall = now - s_last_wall;

    if (s_lock < 0)
        return;
    sceKernelLockMutex(s_lock, 1, NULL);
    for (i = 0; i < s_n; i++) {
        const uint64_t r = run_us(s_th[i].thid);
        const uint64_t d = r >= s_th[i].last ? r - s_th[i].last : 0;
        s_th[i].last = r;
        for (g = 0; g < ng && strcmp(names[g], s_th[i].group); g++)
            ;
        if (g == ng) {
            if (ng == 16)
                continue;
            names[ng] = s_th[i].group;
            sums[ng++] = 0;
        }
        sums[g] += d;
    }
    sceKernelUnlockMutex(s_lock, 1);
    if (s_last_wall && wall) {
        at = (size_t)snprintf(line, sizeof(line), "cpu: of one core, over %u s:",
                              (unsigned)(wall / 1000000));
        for (g = 0; g < ng && at < sizeof(line); g++)
            at += (size_t)snprintf(line + at, sizeof(line) - at, " %s %u%%", names[g],
                                   (unsigned)(sums[g] * 100 / wall));
        LOG("%s", line);
    }
    s_last_wall = now;
}
