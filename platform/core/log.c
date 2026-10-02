#include "log.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static SceUID s_fd = -1;
static SceUID s_lock = -1;

/* the last lines, for the on-screen console */
#define RING_LINES 64
#define RING_WIDTH 96
static char s_ring[RING_LINES][RING_WIDTH];
static volatile unsigned s_ring_count;

/* Lines are gathered here and written out when it fills, on log_flush (the 10 s reports, the
 * watchdog, traces) and before a fault report: one sceIoWrite costs milliseconds on the memory
 * card, and a line each slowed the loads that log every cartridge read down. */
#define BUF_SIZE (32 * 1024)
static char s_buf[BUF_SIZE];
static int s_len;

static void buf_out(void)
{
    if (s_len && s_fd >= 0)
        sceIoWrite(s_fd, s_buf, s_len);
    s_len = 0;
}

void log_init(const char *dir)
{
    char path[256], prev[256];
    snprintf(path, sizeof(path), "%s/log.txt", dir);
    snprintf(prev, sizeof(prev), "%s/log_prev.txt", dir);
    sceIoRemove(prev);
    sceIoRename(path, prev);
    s_fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    s_lock = sceKernelCreateMutex("kh_log", 0, 0, NULL);
}

static void log_vprintf(const char *prefix, const char *fmt, va_list ap)
{
    char buf[1024];
    int n;
    unsigned int ms = (unsigned int)(sceKernelGetProcessTimeWide() / 1000);

    n = snprintf(buf, sizeof(buf), "[%7u.%03u] %s", ms / 1000, ms % 1000, prefix);
    n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    if (n >= (int)sizeof(buf) - 1)
        n = sizeof(buf) - 2;
    if (buf[n - 1] != '\n')
        buf[n++] = '\n';

    if (s_lock >= 0)
        sceKernelLockMutex(s_lock, 1, NULL);
    if (s_len + n > BUF_SIZE)
        buf_out();
    memcpy(s_buf + s_len, buf, n);
    s_len += n;
    {
        char *line = s_ring[s_ring_count % RING_LINES];
        int len = n - 1 < RING_WIDTH - 1 ? n - 1 : RING_WIDTH - 1;
        memcpy(line, buf, len);
        line[len] = 0;
        s_ring_count++;
    }
    if (s_lock >= 0)
        sceKernelUnlockMutex(s_lock, 1);
}

void log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vprintf("", fmt, ap);
    va_end(ap);
}

/* KH_TRACE in the decomp (nitro/kh_hw.h): boot milestones, synced at once so that the last
 * one reached survives a freeze or a crash. */
int kh_log_verbose;

void kh_trace(const char *fmt, ...)
{
    va_list ap;
    if (!kh_log_verbose)
        return;
    va_start(ap, fmt);
    log_vprintf("trace: ", fmt, ap);
    va_end(ap);
}

void kh_trace_always(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vprintf("trace: ", fmt, ap);
    va_end(ap);
}

void log_flush(void)
{
    if (s_lock >= 0)
        sceKernelLockMutex(s_lock, 1, NULL);
    buf_out();
    if (s_lock >= 0)
        sceKernelUnlockMutex(s_lock, 1);
    if (s_fd >= 0)
        sceIoSyncByFd(s_fd, 0);
}

/* The fault handler's path: no lock (the faulting thread may hold it), what is buffered first. */
void log_write_raw(const char *buf, int len)
{
    if (s_fd >= 0) {
        buf_out();
        sceIoWrite(s_fd, buf, len);
        sceIoSyncByFd(s_fd, 0);
    }
}

int log_recent(int back, char *out, int size)
{
    unsigned count = s_ring_count;
    if (back < 0 || (unsigned)back >= count || back >= RING_LINES)
        return 0;
    if (s_lock >= 0)
        sceKernelLockMutex(s_lock, 1, NULL);
    snprintf(out, size, "%s", s_ring[(count - 1 - back) % RING_LINES]);
    if (s_lock >= 0)
        sceKernelUnlockMutex(s_lock, 1);
    return 1;
}
