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

static uint8_t *main_arena(void)
{
    if (!s_main_arena) {
        s_main_arena = memalign(32, DS_MAIN_ARENA_SIZE);
        LOG("os: main arena %p, %u bytes", s_main_arena, DS_MAIN_ARENA_SIZE);
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
