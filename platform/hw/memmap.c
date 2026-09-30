/* Host memory behind KH_HW (platform/compat/kh_hw_map.h), and the registers the hardware
 * computes when they are read. */
#include "hw/memmap.h"

#include "hw/io.h"
#include "hw/timers.h"
#include "nitro/cpu.h"

#include <psp2/kernel/processmgr.h>
#include <string.h>

unsigned char kh_ds_io_hi[0x20] __attribute__((aligned(32)));
unsigned char kh_ds_palette[0x800] __attribute__((aligned(32)));
unsigned char kh_ds_oam[0x800] __attribute__((aligned(32)));
unsigned char kh_vram_bg_a[0x80000] __attribute__((aligned(32)));
unsigned char kh_vram_bg_b[0x20000] __attribute__((aligned(32)));
unsigned char kh_vram_obj_a[0x40000] __attribute__((aligned(32)));
unsigned char kh_vram_obj_b[0x20000] __attribute__((aligned(32)));
unsigned char kh_vram_lcdc[0xa4000] __attribute__((aligned(32)));
unsigned char kh_ds_gba_slot[0x20000] __attribute__((aligned(32)));
unsigned char kh_ds_bios9[0x100] __attribute__((aligned(32)));

volatile uint64_t kh_hw_vblank_start_us;

/* Power-on values the game can observe before writing them. */
void kh_hw_reset(void)
{
    memset(kh_ds_io, 0, KH_IO_SIZE);
    memset(kh_ds_gba_slot, 0xff, sizeof kh_ds_gba_slot); /* no cartridge in slot 2 */
    KH_IO16(0x04000130) = 0x03ff;     /* KEYINPUT: nothing held (active low) */
    KH_IO32(0x04000600) = 0x06000000; /* GXSTAT: FIFO empty and under half full, not busy */
}

/* ---- divider and square root (CP), as the hardware defines the edge cases ---------------- */

enum {
    DIVCNT = 0x04000280,
    DIV_NUMER = 0x04000290,
    DIV_DENOM = 0x04000298,
    DIV_RESULT = 0x040002a0,
    DIVREM_RESULT = 0x040002a8,
    SQRTCNT = 0x040002b0,
    SQRT_RESULT = 0x040002b4,
    SQRT_PARAM = 0x040002b8,
};

static int64_t rd64(uint32_t a) { int64_t v; memcpy(&v, KH_IO_PTR(a), 8); return v; }
static void wr64(uint32_t a, int64_t v) { memcpy(KH_IO_PTR(a), &v, 8); }

uintptr_t kh_hw_sync_div(void)
{
    uint16_t cnt = KH_IO16(DIVCNT);
    int64_t num = rd64(DIV_NUMER), den = rd64(DIV_DENOM), quo, rem;

    switch (cnt & 3) {
    case 0: { /* 32 / 32 */
        int32_t n = (int32_t)num, d = (int32_t)den;
        if (d == 0) {
            /* +-1 in the low word, the high word as if sign-extended from the opposite sign */
            uint32_t lo = n < 0 ? 1u : 0xffffffffu, hi = n < 0 ? 0xffffffffu : 0;
            quo = (int64_t)((uint64_t)hi << 32 | lo);
            rem = n;
        } else if (n == INT32_MIN && d == -1) {
            quo = (int64_t)0x80000000u;
            rem = 0;
        } else {
            quo = n / d;
            rem = n % d;
        }
        break;
    }
    case 1:
    case 3: { /* 64 / 32 */
        int32_t d = (int32_t)den;
        if (d == 0) {
            quo = num < 0 ? 1 : -1;
            rem = num;
        } else if (num == INT64_MIN && d == -1) {
            quo = INT64_MIN;
            rem = 0;
        } else {
            quo = num / d;
            rem = num % d;
        }
        break;
    }
    default: /* 64 / 64 */
        if (den == 0) {
            quo = num < 0 ? 1 : -1;
            rem = num;
        } else if (num == INT64_MIN && den == -1) {
            quo = INT64_MIN;
            rem = 0;
        } else {
            quo = num / den;
            rem = num % den;
        }
        break;
    }
    wr64(DIV_RESULT, quo);
    wr64(DIVREM_RESULT, rem);
    /* bit 14: the whole 64-bit denominator is zero; bit 15 (busy) never set */
    KH_IO16(DIVCNT) = (uint16_t)((cnt & ~0xc000u) | (den == 0 ? 0x4000u : 0));
    return (uintptr_t)KH_IO_PTR(DIV_RESULT);
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

uintptr_t kh_hw_sync_sqrt(void)
{
    uint64_t param = (uint64_t)rd64(SQRT_PARAM);
    if (!(KH_IO16(SQRTCNT) & 1))
        param = (uint32_t)param;
    KH_IO32(SQRT_RESULT) = isqrt64(param);
    KH_IO16(SQRTCNT) &= ~0x8000u;
    return (uintptr_t)KH_IO_PTR(SQRT_RESULT);
}

/* ---- DISPSTAT / VCOUNT from the time since the last emulated VBlank ------------------------
 * 263 lines per frame, VBlank from line 192. */

#define FRAME_US 16715u /* 1 / 59.8261 Hz */
#define LINES 263u

uintptr_t kh_hw_sync_disp(void)
{
    uint64_t now = sceKernelGetProcessTimeWide();
    uint64_t base = kh_hw_vblank_start_us ? kh_hw_vblank_start_us : 0;
    /* the VBlank starts at line 192 */
    uint32_t line = (uint32_t)(((now - base) % FRAME_US) * LINES / FRAME_US + 192) % LINES;
    uint32_t in_line = (uint32_t)(((now - base) % FRAME_US) * LINES % FRAME_US);
    uint16_t stat = KH_IO16(0x04000004) & 0xffb8u;
    uint16_t lyc = (uint16_t)((stat >> 8) | ((stat & 0x80) << 1));

    if (line >= 192 && line < 262)
        stat |= 1;
    if (in_line * 355 / FRAME_US >= 256) /* 355 dots per line, 256 visible */
        stat |= 2;
    if (line == lyc)
        stat |= 4;
    KH_IO16(0x04000004) = stat;
    KH_IO16(0x04000006) = (uint16_t)line;
    /* code that spins on the display status is where the DS would take the VBlank IRQ */
    kh_cpu_poll();
    return (uintptr_t)KH_IO_PTR(0x04000004);
}

uintptr_t kh_hw_sync_timers(void)
{
    kh_timers_update();
    return (uintptr_t)KH_IO_PTR(0x04000100);
}

/* GXSTAT: until the geometry engine exists the FIFO is always empty and idle. The IRQ mode
 * bits (30-31) are the game's; a write that dropped the "under half full" and "empty" bits
 * would otherwise leave MI_SendGXCommandAsync waiting forever. */
uintptr_t kh_hw_sync_gxstat(void)
{
    KH_IO32(0x04000600) = (KH_IO32(0x04000600) & 0xc0000000u) | 0x06000000u;
    return (uintptr_t)KH_IO_PTR(0x04000600);
}
