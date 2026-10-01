/* NitroSDK OS functions whose DS versions return DS addresses or read timer 0.
 *
 * Arenas. The DS carves them out of its memory map: main RAM between the end of the largest
 * overlay and the ARM7's area, the unused ends of ITCM and DTCM, part of the shared area.
 * The port hands out host memory of the same sizes (a heap sized from the arena comes out the
 * same), except DTCM and the shared area, which are the port's copies of those blocks.
 *
 * Tick. OS_GetTick counts the bus clock / 64 (timer 0 with its overflow IRQ on the DS); here it
 * is read straight from the Vita's clock, which neither wraps nor depends on interrupt delivery. */
#include "hw/memmap.h"
#include "hw/timers.h"
#include "log.h"

#include <psp2/kernel/processmgr.h>
#include <stdint.h>
#include <malloc.h>
#include <stdlib.h>

enum {
    ARENA_MAIN = 0,
    ARENA_MAINEX = 2,
    ARENA_ITCM = 3,
    ARENA_DTCM = 4,
    ARENA_SHARED = 5,
    ARENA_WRAM_MAIN = 6,
};

/* DS values from OS_GetInitArenaLo/Hi (EU ROM) */
#define DS_MAIN_ARENA_SIZE (0x023e0000u - 0x020d73e0u)
#define DS_ITCM_ARENA_SIZE (0x02000000u - 0x01fffee0u)
#define DS_DTCM_ARENA_LO 0x0e60u                /* SDK_SECTION_ARENA_DTCM_START - DTCM */
#define DS_SHARED_ARENA_SIZE (0x027ff680u - 0x027ff000u)

static uint8_t *s_main_arena;
static uint8_t s_itcm_arena[DS_ITCM_ARENA_SIZE] __attribute__((aligned(32)));

/* The main arena is placed so that the archive handles the game packs from its pointers
 * (KH_DS_PTR: the low 24 bits of ptr + 0x8000, shifted left by 7, bit 31 set) can never equal
 * an address of the image or of the heap below it (0x81xxxxxx, 0x82xxxxxx), where file names
 * live: ptr + 0x8000 stays within one 16 MiB window and its low 24 bits at or above 0x60000,
 * which puts every handle at 0x83000000 or above. KH_IS_PACKED relies on it. */
#define ARENA_MIN_LOW 0x60000u

static uint8_t *main_arena(void)
{
    if (!s_main_arena) {
        const size_t slack = DS_MAIN_ARENA_SIZE + ARENA_MIN_LOW + 0x8000u;
        uintptr_t raw = (uintptr_t)memalign(32, DS_MAIN_ARENA_SIZE + slack), s = raw;
        uint32_t low = (uint32_t)(s + 0x8000u) & 0x00ffffffu;
        if (low < ARENA_MIN_LOW)
            s += ARENA_MIN_LOW - low;
        else if (low + DS_MAIN_ARENA_SIZE > 0x01000000u)
            s += 0x01000000u - low + ARENA_MIN_LOW; /* past the window's end */
        s = (s + 31) & ~(uintptr_t)31;
        s_main_arena = (uint8_t *)s;
        LOG("os: main arena %p, %u bytes (block %p)", s_main_arena, DS_MAIN_ARENA_SIZE,
            (void *)raw);
    }
    return s_main_arena;
}

void *OS_GetInitArenaLo(int id)
{
    switch (id) {
    case ARENA_MAIN: return main_arena();
    case ARENA_ITCM: return s_itcm_arena;
    /* the launcher's stack takes the rest of DTCM (SDK_SYS_STACKSIZE is 0): an empty arena */
    case ARENA_DTCM: return kh_ds_dtcm + DS_DTCM_ARENA_LO;
    case ARENA_SHARED: return kh_ds_shared_area;
    default: return NULL; /* no expansion RAM, no WRAM left to the ARM9 */
    }
}

void *OS_GetInitArenaHi(int id)
{
    switch (id) {
    case ARENA_MAIN: return main_arena() + DS_MAIN_ARENA_SIZE;
    case ARENA_ITCM: return s_itcm_arena + DS_ITCM_ARENA_SIZE;
    case ARENA_DTCM: return kh_ds_dtcm + DS_DTCM_ARENA_LO;
    case ARENA_SHARED: return kh_ds_shared_area + DS_SHARED_ARENA_SIZE;
    default: return NULL;
    }
}

/* ---- packed pointers ----------------------------------------------------------------------------
 * KH_DS_PTR in the decomp (nitro/kh_hw.h). The game packs a pointer's low 24 bits, offset by
 * 0x8000, into archive handles and unpacks them as 0x01ff8000 + those bits: right on the DS,
 * where all of RAM lies in that 16 MiB. Here the top byte is recovered from where such pointers
 * can be: the arenas the game's heaps are carved from (each smaller than 16 MiB, so at most one
 * candidate falls inside). */
unsigned long kh_ds_unpack_ptr(unsigned long ds_addr)
{
    const uint32_t low = ((uint32_t)ds_addr - 0x01ff8000u) & 0x00ffffffu; /* (ptr + 0x8000) */
    const struct { uintptr_t lo, hi; } regions[] = {
        { (uintptr_t)main_arena(), (uintptr_t)main_arena() + DS_MAIN_ARENA_SIZE },
        { (uintptr_t)s_itcm_arena, (uintptr_t)s_itcm_arena + DS_ITCM_ARENA_SIZE },
    };
    static int warned;
    unsigned i;

    for (i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
        uintptr_t c = (((regions[i].lo + 0x8000u) & ~(uintptr_t)0x00ffffffu) | low) - 0x8000u;
        if (c < regions[i].lo)
            c += 0x01000000u;
        if (c < regions[i].hi)
            return c;
    }
    if (warned++ < 8)
        LOG("os: packed pointer %08lx is in no arena", ds_addr);
    return ds_addr;
}

/* KH_IS_PACKED in the decomp. A packed handle unpacks to a pack header that
 * Msg_OpenContainerAndReadHeader filled: in an arena, with the ROM archive at +8. A file name's
 * address does not (it is in the image, and what its bits unpack to holds no such header). */
extern uint8_t data_02046334[]; /* fsi_arc_rom, the ROM archive */

static int unpack_quiet(unsigned long v, uintptr_t *out)
{
    const uint32_t low = ((uint32_t)v >> 7) & 0x00fffffcu; /* (ptr + 0x8000), as packed */
    const uintptr_t lo = (uintptr_t)main_arena(), hi = lo + DS_MAIN_ARENA_SIZE;
    uintptr_t c = (((lo + 0x8000u) & ~(uintptr_t)0x00ffffffu) | low) - 0x8000u;
    if (c < lo)
        c += 0x01000000u;
    if (c + 12 > hi)
        return 0; /* pack headers are allocated from the game's heaps, in the main arena */
    *out = c;
    return 1;
}

int kh_is_packed_handle(unsigned long v)
{
    uintptr_t hdr;
    if (!(v & 0x80000000u) || !unpack_quiet(v, &hdr))
        return 0;
    return *(const uint32_t *)(hdr + 8) == (uint32_t)(uintptr_t)data_02046334;
}

/* ---- tick ------------------------------------------------------------------------------------ */

extern uint16_t data_02044664; /* the tick system's "initialized" flag (OS_IsTickAvailable) */

static uint64_t s_tick_origin_us;

void OS_InitTick(void)
{
    if (data_02044664)
        return;
    data_02044664 = 1;
    s_tick_origin_us = sceKernelGetProcessTimeWide();
}

uint64_t OS_GetTick(void)
{
    uint64_t us = sceKernelGetProcessTimeWide() - s_tick_origin_us;
    return us * (KH_DS_BUS_HZ / 64) / 1000000u;
}

uint16_t OS_GetTickLo(void)
{
    return (uint16_t)OS_GetTick();
}
