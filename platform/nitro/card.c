/* The cartridge: CARDi_ReadRom, the single path every ROM read of the NitroSDK goes through
 * (FS's ROM archive calls it asynchronously, with a completion callback).
 *
 * The data comes from the user's dump (rom_read). An asynchronous read completes the way a
 * card DMA did: its callback runs later, on the running NitroSDK thread in IRQ mode, so the
 * FS state machine sees "started" before "done" as it expects. */
#include "log.h"
#include "nitro/cpu.h"
#include "rom.h"
#include "nitro/card.h"

#include <stddef.h>
#include <stdint.h>

typedef void (*CARDCallback)(void *arg);

extern uint32_t data_02046b00; /* CARD's ROM base offset (common->src bias) */

static void complete(void *cb, void *arg, void *unused)
{
    (void)unused;
    ((CARDCallback)cb)(arg);
}

void CARDi_ReadRom(uint32_t dma, const void *src, void *dst, uint32_t len, CARDCallback cb,
                   void *arg, int async)
{
    uint32_t off = (uint32_t)(uintptr_t)src + data_02046b00;
    int n;
    (void)dma;

    n = rom_read(off, dst, len);
    if ((uint32_t)n != len)
        LOG("card: short read at %08x: %d of %u", off, n, len);
    if (!cb)
        return;
    if (async)
        kh_cpu_defer(complete, (void *)cb, arg, NULL);
    else
        cb(arg);
}

/* The chip ID the card answers READ_ID with. The SDK compares it with the one the boot left in
 * the shared area (CARDi_CheckPulledOutCore) to notice a pulled-out card; game.c stores the
 * same value there. */
uint32_t CARDi_ReadRomIDCore(void)
{
    return KH_CARD_ID;
}
