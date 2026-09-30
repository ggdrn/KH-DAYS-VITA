/* Port entry point: bring up the Vita side, check the user's ROM, then hand over to the game.
 *
 * Until the decomp is linked in (KH_WITH_GAME), the shell boots, verifies the dump and shows
 * a diagnostic screen: the ROM header and the controls, so the install can be tested alone. */
#include "fault.h"
#include "input.h"
#include "log.h"
#include "msgdialog.h"
#include "paths.h"
#include "rom.h"
#include "video.h"

#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <stdio.h>
#include <string.h>

#ifndef KH_VERSION
#define KH_VERSION "dev"
#endif

/* Heap for newlib (malloc): the game's arenas and the port's buffers come out of it. Kept well
 * inside the app's memory budget: the executable, vitaGL's pools and the Vita's own share must
 * fit beside it. */
int _newlib_heap_size_user = 96 * 1024 * 1024;

/* Boot markers in ux0:data/khdays/boot.txt, opened, written and closed at every stage: they
 * survive a hang or a crash at any point, even before the log exists. */
static void mark(const char *stage)
{
    SceUID fd = sceIoOpen(KH_DATA_DIR "/boot.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, stage, strlen(stage));
        sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
    }
}

static uint32_t s_top[DS_SCREEN_W * DS_SCREEN_H] __attribute__((unused));
static uint32_t s_bottom[DS_SCREEN_W * DS_SCREEN_H] __attribute__((unused));

static void fill(uint32_t *fb, uint32_t rgba)
{
    int i;
    for (i = 0; i < DS_SCREEN_W * DS_SCREEN_H; i++)
        fb[i] = rgba;
}

static void bar(uint32_t *fb, int y0, int y1, int x1, uint32_t rgba)
{
    int x, y;
    for (y = y0; y < y1; y++)
        for (x = 16; x < x1; x++)
            fb[y * DS_SCREEN_W + x] = rgba;
}

/* SHA-1 progress: a bar on the top screen, redrawn every few MiB. */
static void hash_progress(uint32_t done, uint32_t total)
{
    if ((done & 0x7fffff) != 0 && done != total)
        return;
    fill(s_top, 0xff201010);
    bar(s_top, 88, 104, 16 + (int)((uint64_t)(DS_SCREEN_W - 32) * done / total), 0xffe0c080);
    video_present(s_top, NULL);
}

static void fatal(const char *text)
{
    LOG("fatal: %s", text);
    log_flush();
    msgdialog_show(text);
    sceKernelExitProcess(0);
}

static void check_rom(void)
{
    switch (rom_open(KH_ROM_PATH, KH_ROM_STAMP, hash_progress)) {
    case ROM_OK:
        return;
    case ROM_MISSING:
        fatal("Kingdom Hearts 358/2 Days ROM not found.\n\n"
              "Copy your own dump of the European cartridge to\n" KH_ROM_PATH);
        break;
    case ROM_WRONG_SIZE:
    case ROM_WRONG_GAME:
        fatal("The file at " KH_ROM_PATH " is not the European\n"
              "Kingdom Hearts 358/2 Days (YKGP) cartridge.");
        break;
    case ROM_WRONG_HASH:
        fatal("The ROM at " KH_ROM_PATH " does not match the expected dump\n"
              "(SHA-1 " ROM_EXPECTED_SHA1 ").\n\nRe-dump the European cartridge.");
        break;
    }
}

static void init_system(void)
{
    SceAppUtilInitParam init = { 0 };
    SceAppUtilBootParam boot = { 0 };

    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    sceAppUtilInit(&init, &boot);
    sceIoMkdir(KH_DATA_DIR, 0777);
}

#ifdef KH_WITH_GAME
extern void kh_game_run(void); /* platform/hw: starts the game thread and never returns */
#endif

static void diagnostic_loop(void)
{
    InputState in;

    input_init();
    for (;;) {
        int i;
        input_poll(&in);
        if (in.swap_layout)
            video_set_layout(video_layout() + 1);

        fill(s_top, 0xff402010);
        fill(s_bottom, 0xff102040);
        /* one lit bar per held DS button */
        for (i = 0; i < 12; i++)
            bar(s_top, 20 + i * 13, 30 + i * 13, (in.held >> i) & 1 ? 240 : 24, 0xff80ff80);
        if (in.touching)
            bar(s_bottom, in.touch_y > 4 ? in.touch_y - 4 : 0, in.touch_y + 4,
                in.touch_x + 4 < DS_SCREEN_W ? in.touch_x + 4 : DS_SCREEN_W, 0xffffffff);
        video_present(s_top, s_bottom);
    }
}

int main(void)
{
    SceCtrlData pad;
    SceKernelFreeMemorySizeInfo mem = { .size = sizeof(mem) };

    sceIoMkdir(KH_DATA_DIR, 0777);
    sceIoRemove(KH_DATA_DIR "/boot.txt");
    mark("main " KH_VERSION);
    init_system();
    mark("system");
    log_init(KH_DATA_DIR);
    mark("log");
    LOG("khdays-vita %s", KH_VERSION);
    sceKernelGetFreeMemorySize(&mem);
    LOG("memory free: user %u KiB, cdram %u KiB, phycont %u KiB", (unsigned)(mem.size_user >> 10),
        (unsigned)(mem.size_cdram >> 10), (unsigned)(mem.size_phycont >> 10));
    fault_init();
    mark("fault");

    video_init();
    mark("video");
    check_rom();
    mark("rom");

    /* holding L at start-up skips the game: the controls test, to check the install */
    sceCtrlPeekBufferPositive(0, &pad, 1);
#ifdef KH_WITH_GAME
    if (!(pad.buttons & SCE_CTRL_LTRIGGER)) {
        mark("game");
        kh_game_run();
    }
#endif
    mark("diagnostic");
    LOG("diagnostic mode");
    diagnostic_loop();
    return 0;
}
