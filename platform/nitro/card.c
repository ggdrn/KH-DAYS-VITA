/* The cartridge: CARDi_ReadRom, the single path every ROM read of the NitroSDK goes through
 * (FS's ROM archive calls it asynchronously, with a completion callback).
 *
 * The data comes from the user's dump (rom_read). An asynchronous read completes later, so
 * the FS state machine sees "started" before "done" as it expects. On the DS the CARD task
 * thread calls the callback when it finishes the transfer, whether interrupts are on or not:
 * main() loads ov001 before anything sets IME. So the completion is a task deferral
 * (kh_cpu_defer_task), not an interrupt one. */
#include "log.h"
#include "nitro/cpu.h"
#include "rom.h"
#include "nitro/card.h"
#include "nitro/romfs.h"

#include <stddef.h>
#include <stdint.h>

typedef void (*CARDCallback)(void *arg);

extern uint32_t data_02046b00; /* CARD's ROM base offset (common->src bias) */

volatile uint32_t kh_card_reads;

static void complete(void *cb, void *arg, void *unused)
{
    (void)unused;
    ((CARDCallback)cb)(arg);
}

void CARDi_ReadRom(uint32_t dma, const void *src, void *dst, uint32_t len, CARDCallback cb,
                   void *arg, int async)
{
    KH_PROBE("CARDi_ReadRom");
    uint32_t off = (uint32_t)(uintptr_t)src + data_02046b00;
    int n;
    (void)dma;

    n = rom_read(off, dst, len);
    /* every read of the boot by file name, then every 100th */
    if (kh_card_reads < 500 || kh_card_reads % 100 == 0) {
        char name[160];
        LOG("card: read %s, %x bytes%s (#%u)", kh_romfs_describe(off, name, sizeof(name)), len,
            async ? " async" : "", (unsigned)kh_card_reads);
    }
    kh_card_reads++;
    if ((uint32_t)n != len)
        LOG("card: short read at %08x: %d of %u", off, n, len);
    if (!cb)
        return;
    if (async)
        kh_cpu_defer_task(complete, (void *)cb, arg, NULL);
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
