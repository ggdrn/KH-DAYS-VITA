/* Native versions of the library functions the decomp keeps as CodeWarrior assembly
 * (asm_stubs/ C files, excluded from the Vita build by tools/decomp_sources.py).
 *
 * Each one reproduces what the original routine leaves in memory, not just its intent: copy
 * direction, matrix layouts, the quicksort's pivot and swap order, the streaming LZ state. Code
 * that only makes sense on the ARM946E (cache and protection-unit maintenance, CPU mode, BIOS
 * busy loops) becomes a no-op. Thread contexts belong to the NitroSDK scheduler, which the port
 * replaces with its own (platform/nitro/os_*.c); until then those entries only log. */
#include "hw/io.h"
#include "hw/shared_area.h"
#include "log.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include <string.h>

#define UNIMPLEMENTED()                                            \
    do {                                                           \
        static int s_warned;                                       \
        if (!s_warned++)                                           \
            LOG("nitro: %s not implemented on the port", __func__); \
    } while (0)

/* ---- copies and fills (MI) ------------------------------------------------------------------
 * The SDK copies forward. Where source and destination overlap forward within one word the
 * result depends on the word size, so those cases go element by element in the original's
 * unit; everything else is a plain memcpy. */

static int overlaps_forward(const void *src, const void *dst, uint32_t size)
{
    return (uintptr_t)dst > (uintptr_t)src && (uintptr_t)dst < (uintptr_t)src + size;
}

void MI_CpuCopy8(const void *src, void *dst, uint32_t size)
{
    if (overlaps_forward(src, dst, size)) {
        const uint8_t *s = src;
        uint8_t *d = dst;
        while (size--)
            *d++ = *s++;
    } else {
        memmove(dst, src, size);
    }
}

void MI_CpuFill8(void *dst, uint32_t value, uint32_t size)
{
    memset(dst, (uint8_t)value, size);
}

void MIi_CpuCopy16(const void *src, void *dst, uint32_t size)
{
    if (overlaps_forward(src, dst, size)) {
        const uint16_t *s = src;
        uint16_t *d = dst;
        for (size &= ~1u; size; size -= 2)
            *d++ = *s++;
    } else {
        memmove(dst, src, size & ~1u);
    }
}

void MIi_CpuClear16(uint16_t value, void *dst, uint32_t size)
{
    uint16_t *d = dst;
    for (size &= ~1u; size; size -= 2)
        *d++ = value;
}

static void copy32(const void *src, void *dst, uint32_t size)
{
    if (overlaps_forward(src, dst, size)) {
        const uint32_t *s = src;
        uint32_t *d = dst;
        for (size &= ~3u; size; size -= 4)
            *d++ = *s++;
    } else {
        memmove(dst, src, size & ~3u);
    }
}

static void clear32(uint32_t value, void *dst, uint32_t size)
{
    uint32_t *d = dst;
    for (size &= ~3u; size; size -= 4)
        *d++ = value;
}

void MIi_CpuCopy32(const void *src, void *dst, uint32_t size) { copy32(src, dst, size); }
void MIi_CpuCopyFast(const void *src, void *dst, uint32_t size) { copy32(src, dst, size); }
void MIi_CpuClearFast(uint32_t value, void *dst, uint32_t size) { clear32(value, dst, size); }
void INITi_CpuClear32_0x01ff86fc(uint32_t value, void *dst, uint32_t size) { clear32(value, dst, size); }

/* Every word to the same address: a FIFO port. */
void MIi_CpuSend32(const void *src, volatile void *port, uint32_t size)
{
    const uint32_t *s = src;
    for (size &= ~3u; size; size -= 4)
        kh_io_fifo_write32(port, *s++);
}

void MI_Copy36B(const void *src, void *dst) { memcpy(dst, src, 36); }
void MI_Copy48B(const void *src, void *dst) { memcpy(dst, src, 48); }
void MI_Copy64B(const void *src, void *dst) { memcpy(dst, src, 64); }
void MI_Zero36B(void *dst) { memset(dst, 0, 36); }

uint32_t MI_SwapWord(uint32_t value, volatile uint32_t *p)
{
    return __atomic_exchange_n(p, value, __ATOMIC_SEQ_CST);
}

/* BIOS SWI 0x0b. control: bits 0-20 unit count, bit 24 fill from *src, bit 26 32-bit units. */
void CpuSet(const void *src, void *dst, uint32_t control)
{
    uint32_t n = control & 0x1fffff;
    int fill = (control >> 24) & 1;
    if (control & (1u << 26)) {
        const uint32_t *s = src;
        uint32_t *d = dst;
        uint32_t v = *s;
        while (n--)
            *d++ = fill ? v : *s++;
    } else {
        const uint16_t *s = src;
        uint16_t *d = dst;
        uint16_t v = *s;
        while (n--)
            *d++ = fill ? v : *s++;
    }
}

/* ---- streaming LZ (MI_ReadUncompLZ8, mi_uncomp_stream.c) ------------------------------------
 * A line-by-line translation of the SDK's ARM routine: the context carries a partially read
 * token across calls. */

/* include/nitro/mi.h */
typedef struct {
    uint8_t *destp;
    int32_t destCount;
    int32_t length;
    uint16_t destTmp;
    uint8_t destTmpCnt;
    uint8_t flags;
    uint8_t flagIndex;
    uint8_t lengthFlg;
    uint8_t exFormat;
    uint8_t padding;
} MIUncompContextLZ;
_Static_assert(sizeof(MIUncompContextLZ) == 0x14, "MIUncompContextLZ layout");

int32_t func_02004484(MIUncompContextLZ *ctx, const uint8_t *src, uint32_t len)
{
    /* labels are the SDK's (@21..@29) */
    uint8_t *dst = ctx->destp;
    int32_t count = ctx->destCount;
    uint32_t flags = ctx->flags;
    uint32_t index = ctx->flagIndex;
    int32_t length = ctx->length;
    uint32_t lenflg = ctx->lengthFlg;
    const uint32_t ex = ctx->exFormat;
    uint32_t off;

l21:
    if (count <= 0)
        goto l29;
    if (index == 0)
        goto l28;
l22:
    if (len == 0)
        goto l29;
    if (!(flags & 0x80)) {
        *dst++ = *src++;
        count--;
        len--;
        goto l26;
    }
l23:
    if (lenflg == 0)
        goto l24;
    if (ex != 1) {
        length = *src++ + 0x30;
        lenflg = 0;
        goto l23_10;
    }
    lenflg--;
    if (lenflg == 0) {
        length += *src++;
        goto l23_10;
    }
    if (lenflg == 1) {
        length += *src++ << 8;
        goto l23_8;
    }
    length = *src++;
    if (length & 0xe0) {
        length += 0x10;
        lenflg = 0;
        goto l23_10;
    }
    if (length & 0x10) {
        length = 0x1110 + ((length & 0xf) << 16);
    } else {
        length = 0x110 + ((length & 0xf) << 8);
        lenflg = 1;
    }
l23_8:
    if (--len == 0)
        goto l29;
    goto l23;
l23_10:
    if (--len == 0)
        goto l29;
l24:
    off = (((uint32_t)length & 0xf) << 8 | *src++) + 1;
    lenflg = 3;
    len--;
    length >>= 4;
    while (length > 0) {
        *dst = *(dst - off);
        dst++;
        count--;
        length--;
    }
l26:
    if (count == 0)
        goto l29;
    flags <<= 1;
    if (--index != 0)
        goto l22;
l28:
    if (len == 0)
        goto l29;
    flags = *src++;
    index = 8;
    len--;
    goto l21;
l29:
    ctx->destp = dst;
    ctx->destCount = count;
    ctx->flags = (uint8_t)flags;
    ctx->flagIndex = (uint8_t)index;
    ctx->length = length;
    ctx->lengthFlg = (uint8_t)lenflg;
    ctx->exFormat = (uint8_t)ex;
    return count;
}

/* The crt0 routine that decompresses the ARM9 binary in place: the port's code is never
 * compressed. */
void MIi_UncompressBackward(void *bottom)
{
    (void)bottom;
}

/* ---- matrices (MTX, fx32 with 1.0 = 0x1000) ------------------------------------------------ */

#define FX_ONE 0x1000

void MTX_Identity22_(int32_t *m)
{
    m[0] = FX_ONE, m[1] = 0, m[2] = 0, m[3] = FX_ONE;
}

void MTX_Identity33_(int32_t *m)
{
    memset(m, 0, 9 * 4);
    m[0] = m[4] = m[8] = FX_ONE;
}

void MTX_Identity43_(int32_t *m)
{
    memset(m, 0, 12 * 4);
    m[0] = m[4] = m[8] = FX_ONE;
}

void MTX_Identity44_(int32_t *m)
{
    memset(m, 0, 16 * 4);
    m[0] = m[5] = m[10] = m[15] = FX_ONE;
}

/* rows: (cos 0 -sin) (0 1 0) (sin 0 cos) */
void MTX_RotY33_(int32_t *m, int32_t s, int32_t c)
{
    m[0] = c, m[1] = 0, m[2] = -s;
    m[3] = 0, m[4] = FX_ONE, m[5] = 0;
    m[6] = s, m[7] = 0, m[8] = c;
}

/* rows: (cos sin 0) (-sin cos 0) (0 0 1) */
void MTX_RotZ33_(int32_t *m, int32_t s, int32_t c)
{
    m[0] = c, m[1] = s, m[2] = 0;
    m[3] = -s, m[4] = c, m[5] = 0;
    m[6] = 0, m[7] = 0, m[8] = FX_ONE;
}

/* rows: (1 0 0) (0 cos sin) (0 -sin cos) (0 0 0) */
void MTX_RotX43_(int32_t *m, int32_t s, int32_t c)
{
    m[0] = FX_ONE, m[1] = 0, m[2] = 0;
    m[3] = 0, m[4] = c, m[5] = s;
    m[6] = 0, m[7] = -s, m[8] = c;
    m[9] = 0, m[10] = 0, m[11] = 0;
}

/* rows: (cos 0 -sin) (0 1 0) (sin 0 cos) (0 0 0) */
void MTX_RotY43_(int32_t *m, int32_t s, int32_t c)
{
    m[0] = c, m[1] = 0, m[2] = -s;
    m[3] = 0, m[4] = FX_ONE, m[5] = 0;
    m[6] = s, m[7] = 0, m[8] = c;
    m[9] = 0, m[10] = 0, m[11] = 0;
}

void MTX_Copy43To44_(const int32_t *src, int32_t *dst)
{
    int r;
    for (r = 0; r < 4; r++) {
        dst[r * 4 + 0] = src[r * 3 + 0];
        dst[r * 4 + 1] = src[r * 3 + 1];
        dst[r * 4 + 2] = src[r * 3 + 2];
        dst[r * 4 + 3] = r == 3 ? FX_ONE : 0;
    }
}

void MTX_Copy44To43_(const int32_t *src, int32_t *dst)
{
    int r;
    for (r = 0; r < 4; r++) {
        dst[r * 3 + 0] = src[r * 4 + 0];
        dst[r * 3 + 1] = src[r * 4 + 1];
        dst[r * 3 + 2] = src[r * 4 + 2];
    }
}

/* ---- MATH_QSort (qsort.c), translated from the SDK's ARM ------------------------------------
 * Same pivot (the middle element swapped to the front), same partition and the same order of
 * pushes, so equal keys end up where the DS put them. */

typedef int32_t (*MATHCompareFunc)(void *a, void *b);

static void swap_elems(uint8_t *a, uint8_t *b, uint32_t width)
{
    if ((width & 3) == 0) {
        for (; width; width -= 4, a += 4, b += 4) {
            uint32_t t = *(uint32_t *)a;
            *(uint32_t *)a = *(uint32_t *)b;
            *(uint32_t *)b = t;
        }
    } else {
        for (; width; width--, a++, b++) {
            uint8_t t = *a;
            *a = *b;
            *b = t;
        }
    }
}

void Util_QuickSortWithWork(void *head, uint32_t num, uint32_t width, MATHCompareFunc comp,
                            void *stackBuf)
{
    uint8_t *stack[2 * 64]; /* the SDK sizes it 8 bytes per bit of num: 32 pairs at most */
    int sp = 0;
    uint32_t shift = 32 - __builtin_clz(width);

    (void)stackBuf;
    if ((int32_t)num <= 1)
        return;
    stack[sp++] = head;
    stack[sp++] = (uint8_t *)head + (num - 1) * width;

    while (sp) {
        uint8_t *right = stack[--sp];
        uint8_t *left = stack[--sp];
        uint8_t *l, *r, *mid;

        if ((uint32_t)(right - left) == width) {
            if (comp(left, right) > 0)
                swap_elems(left, right, width);
            continue;
        }
        mid = left + (((uint32_t)(right - left)) >> shift) * width;
        swap_elems(mid, left, width);

        l = left + width;
        r = right;
        for (;;) {
            while (l < right && comp(l, left) < 0)
                l += width;
            while (comp(r, left) > 0)
                r -= width;
            if (l >= r)
                break;
            swap_elems(l, r, width);
            l += width;
            r -= width;
            if (l > r)
                break;
        }
        swap_elems(left, r, width);

        if ((int32_t)(r - left) > (int32_t)(right - r)) {
            if (left < r - width) {
                stack[sp++] = left;
                stack[sp++] = r - width;
            }
            if (r + width < right) {
                stack[sp++] = r + width;
                stack[sp++] = right;
            }
        } else {
            if (r + width < right) {
                stack[sp++] = r + width;
                stack[sp++] = right;
            }
            if (left < r - width) {
                stack[sp++] = left;
                stack[sp++] = r - width;
            }
        }
    }
}

/* ---- SHA-1 block transform (DGT's libdgt.a) ------------------------------------------------ */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

void DGTi_Hash2ProcessBlock(uint32_t *h, const uint8_t *p, unsigned long len)
{
    for (; len >= 64; len -= 64, p += 64) {
        uint32_t w[80], a, b, c, d, e, t;
        int i;
        for (i = 0; i < 16; i++)
            w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
                   (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (; i < 80; i++)
            w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      f = (b & c) | (~b & d),          k = 0x5a827999;
            else if (i < 40) f = b ^ c ^ d,                   k = 0x6ed9eba1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
            else             f = b ^ c ^ d,                   k = 0xca62c1d6;
            t = ROL(a, 5) + f + e + k + w[i];
            e = d, d = c, c = ROL(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
}

/* ---- divider / square root context (CP) ------------------------------------------------------
 * {DIV_NUMER, DIV_DENOM (16 bytes at 0x04000290), SQRT_PARAM (8 at 0x040002b8),
 *  DIVCNT & 3, SQRTCNT & 1}. */

void CP_SaveContext(void *ctx)
{
    uint8_t *c = ctx;
    uint16_t divcnt = KH_IO16(0x04000280) & 3, sqrtcnt = KH_IO16(0x040002b0) & 1;
    memcpy(c, KH_IO_PTR(0x04000290), 16);
    memcpy(c + 16, KH_IO_PTR(0x040002b8), 8);
    memcpy(c + 24, &divcnt, 2);
    memcpy(c + 26, &sqrtcnt, 2);
}

void CPi_RestoreContext(const void *ctx)
{
    const uint8_t *c = ctx;
    uint16_t divcnt, sqrtcnt;
    memcpy(&divcnt, c + 24, 2);
    memcpy(&sqrtcnt, c + 26, 2);
    /* TODO(cp): recompute the results once the divider is emulated (platform/nitro/cp.c) */
    KH_IO16(0x04000280) = divcnt;
    KH_IO16(0x040002b0) = sqrtcnt;
    memcpy(KH_IO_PTR(0x04000290), c, 16);
    memcpy(KH_IO_PTR(0x040002b8), c + 16, 8);
}

/* ---- GX FIFO bursts ------------------------------------------------------------------------ */

void GX_SendFifo48B(const void *src, volatile void *port)
{
    const uint32_t *s = src;
    int i;
    for (i = 0; i < 12; i++)
        kh_io_fifo_write32(port, s[i]);
}

void GXi_NopClearFifo128_(volatile void *port)
{
    int i;
    for (i = 0; i < 128; i++)
        kh_io_fifo_write32(port, 0);
}

/* ---- CPU state -------------------------------------------------------------------------------
 * The port runs NitroSDK threads one at a time under a single lock, so "interrupts disabled"
 * is a flag the IRQ dispatcher honours (platform/nitro/os_irq.c) rather than a CPU state. The
 * return values follow CPSR: 0x80 = IRQ masked, 0x40 = FIQ masked. */

volatile uint32_t kh_cpsr_if;

uint32_t OS_DisableInterrupts(void)
{
    uint32_t old = kh_cpsr_if & 0x80;
    kh_cpsr_if |= 0x80;
    return old;
}

uint32_t OS_EnableInterrupts(void)
{
    uint32_t old = kh_cpsr_if & 0x80;
    kh_cpsr_if &= ~0x80u;
    return old;
}

uint32_t OS_RestoreInterrupts(uint32_t state)
{
    uint32_t old = kh_cpsr_if & 0x80;
    kh_cpsr_if = (kh_cpsr_if & ~0x80u) | (state & 0x80);
    return old;
}

uint32_t OS_DisableInterrupts_IrqAndFiq(void)
{
    uint32_t old = kh_cpsr_if & 0xc0;
    kh_cpsr_if |= 0xc0;
    return old;
}

uint32_t OS_RestoreInterrupts_IrqAndFiq(uint32_t state)
{
    uint32_t old = kh_cpsr_if & 0xc0;
    kh_cpsr_if = (kh_cpsr_if & ~0xc0u) | (state & 0xc0);
    return old;
}

uint32_t OS_GetCpsrIrq(void)
{
    return kh_cpsr_if & 0x80;
}

uint32_t OS_GetProcMode(void)
{
    return 0x1f; /* system mode: game code never runs in IRQ mode on the port */
}

uint32_t OsCountZeroBits(uint32_t x)
{
    return x ? (uint32_t)__builtin_clz(x) : 32;
}

/* Lock ids 0x40..0x7f from the two flag words at 0x027fffb0 (set bit = free). */
uint32_t OS_GetLockID(void)
{
    volatile uint32_t *words = KH_SHARED(0x027fffb0);
    uint32_t base = 0x40;
    int w;
    for (w = 0; w < 2; w++, base += 0x20) {
        uint32_t v = words[w];
        if (v) {
            uint32_t bit = __builtin_clz(v);
            words[w] = v & ~(0x80000000u >> bit);
            return base + bit;
        }
    }
    return 0xfffffffd; /* OS_LOCK_ID_ERROR */
}

extern void OS_UnlockCartridge_0x02001704(int processor);

void OS_UnLockCartridge(int processor)
{
    OS_UnlockCartridge_0x02001704(processor);
}

/* ---- waits -------------------------------------------------------------------------------- */

#define ARM9_HZ 67027964u

/* OS_SpinWait counts ARM9 cycles, 4 per loop. */
void OS_SpinWait(uint32_t cycles)
{
    uint32_t us = (uint32_t)((uint64_t)cycles * 1000000u / ARM9_HZ);
    if (us)
        sceKernelDelayThread(us);
}

/* BIOS SWI 0x03 (WaitByLoop): r0 loop count, 4 cycles each. The veneer takes no declared
 * argument, so the count cannot be read portably; the waits it is used for are sub-µs. */
void WaitByLoop(void)
{
}

void RtcWaitBusy(void)
{
}

void OS_Halt(void)
{
    /* TODO(os): wait for the next emulated interrupt (platform/nitro/os_irq.c) */
    sceKernelDelayThread(100);
}

void OS_ResetSystem(uint32_t param)
{
    (void)param;
    LOG("nitro: OS_ResetSystem, exiting");
    log_flush();
    sceKernelExitProcess(0);
}

/* ---- ARM946E only: caches, protection unit, TCM ------------------------------------------- */

void DC_InvalidateRange(void *p, uint32_t n) { (void)p; (void)n; }
void DC_StoreRange(const void *p, uint32_t n) { (void)p; (void)n; }
void DC_FlushAll(void) {}
void DC_StoreAll(void) {}
void DC_WaitWriteBufferEmpty(void) {}
void IC_InvalidateRange(void *p, uint32_t n) { (void)p; (void)n; }
void IC_InvalidateAll(void) {}
void OS_SetDPermissionsForProtectionRegion(uint32_t mask, uint32_t flags) { (void)mask; (void)flags; }
void OS_SetProtectionRegion1(uint32_t param) { (void)param; }
void OS_SetProtectionRegion2(uint32_t param) { (void)param; }
void OSi_CancelDma0(void) {}

/* The DTCM arena (16 KiB on the DS). Game variables placed in DTCM are ordinary globals on the
 * port; only the arena the SDK carves out of it needs memory. */
static uint8_t s_dtcm[0x4000] __attribute__((aligned(32)));

uint32_t OS_GetDTCMAddress(void)
{
    return (uint32_t)(uintptr_t)s_dtcm;
}

/* ---- the NitroSDK scheduler's context switching: replaced wholesale by platform/nitro/os ---- */

void OS_InitContext(void *context, uint32_t newpc, uint32_t newsp)
{
    (void)context, (void)newpc, (void)newsp;
    UNIMPLEMENTED();
}

int OS_SaveContext(void *context)
{
    (void)context;
    UNIMPLEMENTED();
    return 0;
}

void OS_LoadContext(void *context)
{
    (void)context;
    UNIMPLEMENTED();
}

void OSi_ExceptionHandler(void)
{
    UNIMPLEMENTED();
}

extern void OSi_ArrangeTimer(void);

void OSi_AlarmHandler(void)
{
    OSi_ArrangeTimer();
}

/* ---- MobiClip: YCoCg frame to 15-bit pixels (libs/mobiclip/video/asm_stubs/.../BlitRows) ----
 * Follows the original's pointer walk: luma rows of 256 bytes, chroma rows of 256 with Co at +0
 * and Cg at +0x80, one chroma sample per 2x2 quad, the anti-diagonal pixels dithered 4 levels
 * down, and the saturating ramp indexed through three windows (G = Y+Cg, R = Y+Co-Cg,
 * B = R-2Co). The stride is in bytes, the width in pixels, 16 pixels per inner step. */

typedef struct {
    const uint8_t *luma;
    const uint8_t *chroma;
    uint8_t *dest;
    int32_t strideBytes;
    int32_t widthPixels;
    int32_t heightRows;
    const uint8_t *table;
} MobiClipBlitView;

void Ov024_MobiClip_BlitRows(MobiClipBlitView *v)
{
    const uint8_t *luma = v->luma, *chroma = v->chroma, *ramp = v->table + 0x100;
    uint8_t *row0 = v->dest, *row1 = v->dest + v->strideBytes;
    const int32_t lumaGap = 0x200 - v->widthPixels;
    const int32_t chromaGap = 0x100 - (v->widthPixels >> 1);
    const int32_t destGap = (v->strideBytes - v->widthPixels) * 2;
    int32_t y = v->heightRows;

    do {
        int32_t x = v->widthPixels;
        do {
            int q;
            for (q = 0; q < 8; q++) {
                int co = chroma[0] - 0x80, cg = chroma[0x80] - 0x80;
                const uint8_t *g = ramp + cg, *r = ramp + co - cg, *b = r - co * 2;
                int y00 = luma[0], y01 = luma[1] - 4, y10 = luma[0x100] - 4, y11 = luma[0x101];
                uint32_t w0 = 0x80008000u | r[y00] | g[y00] << 5 | b[y00] << 10 |
                              (uint32_t)(r[y01] | g[y01] << 5 | b[y01] << 10) << 16;
                uint32_t w1 = 0x80008000u | r[y10] | g[y10] << 5 | b[y10] << 10 |
                              (uint32_t)(r[y11] | g[y11] << 5 | b[y11] << 10) << 16;
                memcpy(row0, &w0, 4);
                memcpy(row1, &w1, 4);
                row0 += 4, row1 += 4, luma += 2, chroma += 1;
            }
            x -= 16;
        } while (x > 0);
        luma += lumaGap;
        chroma += chromaGap;
        row0 += destGap;
        row1 += destGap;
        y -= 2;
    } while (y > 0);
}
