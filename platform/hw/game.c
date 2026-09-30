/* Starts the decomp's NitroMain (the DS `main`, renamed by -Dmain=NitroMain) on its own thread.
 *
 * The emulated DS state it needs (memory map, IRQs, VBlank) is set up in platform/hw; this file
 * only owns the game thread. */
#include "hw/overlays.h"
#include "log.h"

#include <psp2/kernel/threadmgr.h>

extern void NitroMain(void);

#define GAME_STACK_SIZE (1024 * 1024)

static int game_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    LOG("game: NitroMain");
    NitroMain();
    LOG("game: NitroMain returned");
    return 0;
}

void kh_game_run(void)
{
    int i, entries = 0;
    for (i = 0; i < kh_overlay_count; i++)
        entries += kh_overlays[i].entry != NULL;
    LOG("game: %d overlays, %d with an entry", kh_overlay_count, entries);

    SceUID th = sceKernelCreateThread("kh_game", game_thread, 0x10000100, GAME_STACK_SIZE, 0,
                                      SCE_KERNEL_CPU_MASK_USER_0, NULL);
    if (th < 0) {
        LOG("game: create thread failed %08x", th);
        return;
    }
    sceKernelStartThread(th, 0, NULL);
    sceKernelWaitThreadEnd(th, NULL, NULL);
}
