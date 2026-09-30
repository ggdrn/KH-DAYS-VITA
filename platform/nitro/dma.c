/* The ARM9's four DMA channels, as the NitroSDK's MI functions use them.
 *
 * On the DS these program DMAxSAD/DAD/CNT and then wait for the enable bit to drop, often
 * through a pointer kept from before the start, which no register-read hook could serve. The
 * port replaces the MI DMA functions and performs each transfer when it is started:
 *
 *   immediate timing       copied at once (the DS also finished before the code went on)
 *   GX FIFO timing         the words go to the geometry FIFO
 *   VBlank/HBlank/display  noted in the log; not used by the boot path (TODO(dma))
 *   card timing            the card path is native (card.c), never reached
 *
 * Asynchronous transfers complete like the DMA-end interrupt: the callback runs at the next
 * interrupt point, on the running NitroSDK thread, in IRQ mode. */
#include "hw/io.h"
#include "log.h"
#include "nitro/cpu.h"

#include <stdint.h>
#include <string.h>

typedef void (*MIDmaCallback)(void *arg);

#define CNT_ENABLE 0x80000000u
#define CNT_32BIT (1u << 26)
#define CNT_REPEAT (1u << 25)
#define CNT_TIMING(c) (((c) >> 27) & 7)
#define CNT_SRC(c) (((c) >> 23) & 3)
#define CNT_DST(c) (((c) >> 21) & 3)

enum { TIMING_IMM = 0, TIMING_VBLANK = 1, TIMING_HBLANK = 2, TIMING_GXFIFO = 7 };

static void callback_trampoline(void *cb, void *arg, void *unused)
{
    (void)unused;
    ((MIDmaCallback)cb)(arg);
}

static void complete(MIDmaCallback cb, void *arg)
{
    if (cb)
        kh_cpu_defer(callback_trampoline, (void *)cb, arg, NULL);
}

static void clear_enable(uint32_t ch)
{
    if (ch < 4)
        KH_IO32(0x040000b8 + ch * 12) &= ~CNT_ENABLE;
}

/* A transfer as the channel registers describe it. */
static void run(uint32_t ch, uint32_t src, uint32_t dst, uint32_t ctrl)
{
    uint32_t count = ctrl & 0x1fffff, unit = (ctrl & CNT_32BIT) ? 4 : 2, i;
    int sstep = (CNT_SRC(ctrl) == 0) ? (int)unit : (CNT_SRC(ctrl) == 1) ? -(int)unit : 0;
    int dstep = (CNT_DST(ctrl) == 1) ? -(int)unit : (CNT_DST(ctrl) == 2) ? 0 : (int)unit;

    if (!(ctrl & CNT_ENABLE))
        return;
    if (count == 0)
        count = 0x200000;
    if (dst == 0 || src == 0) {
        /* the SDK's dummy transfer on channel 0 (0 -> 0, one halfword) */
        clear_enable(ch);
        return;
    }
    switch (CNT_TIMING(ctrl)) {
    case TIMING_IMM:
        for (i = 0; i < count; i++, src += sstep, dst += dstep) {
            if (unit == 4)
                *(volatile uint32_t *)(uintptr_t)dst = *(volatile uint32_t *)(uintptr_t)src;
            else
                *(volatile uint16_t *)(uintptr_t)dst = *(volatile uint16_t *)(uintptr_t)src;
        }
        break;
    case TIMING_GXFIFO:
        for (i = 0; i < count; i++, src += sstep)
            kh_io_fifo_write32((volatile void *)(uintptr_t)dst, *(volatile uint32_t *)(uintptr_t)src);
        break;
    default: {
        static int warned[8];
        if (!warned[CNT_TIMING(ctrl)]++)
            LOG("dma: timing %u on channel %u not emulated (ctrl %08x)", CNT_TIMING(ctrl),
                (unsigned)ch, (unsigned)ctrl);
        break;
    }
    }
    if (!(ctrl & CNT_REPEAT) || CNT_TIMING(ctrl) == TIMING_IMM)
        clear_enable(ch);
    if (ctrl & (1u << 30))
        kh_irq_raise(1u << (8 + ch));
}

/* ---- the channel-register primitives ------------------------------------------------------ */

static void set_regs(uint32_t ch, uint32_t src, uint32_t dst, uint32_t ctrl)
{
    if (ch < 4) {
        KH_IO32(0x040000b0 + ch * 12) = src;
        KH_IO32(0x040000b4 + ch * 12) = dst;
        KH_IO32(0x040000b8 + ch * 12) = ctrl;
    }
    run(ch, src, dst, ctrl);
}

void MIi_DmaSetParams(uint32_t ch, uint32_t src, uint32_t dst, uint32_t ctrl) { set_regs(ch, src, dst, ctrl); }
void func_01ff85d0(uint32_t ch, uint32_t src, uint32_t dst, uint32_t ctrl) { set_regs(ch, src, dst, ctrl); }
void MIi_DmaSetChannelRegs(int ch, uint32_t src, uint32_t dst, uint32_t ctrl) { set_regs((uint32_t)ch, src, dst, ctrl); }
void func_01ff8664(uint32_t ch, uint32_t src, uint32_t dst, uint32_t ctrl) { set_regs(ch, src, dst, ctrl); }

void MI_StopDma(uint32_t ch) { clear_enable(ch); }
void MI_WaitDma(uint32_t ch) { clear_enable(ch); }

/* ---- the MI API ----------------------------------------------------------------------------- */

#define COPY32(n) (CNT_ENABLE | CNT_32BIT | ((n) / 4))
#define COPY16(n) (CNT_ENABLE | ((n) / 2))
#define FILL32(n) (CNT_ENABLE | CNT_32BIT | (2u << 23) | ((n) / 4))

void MI_DmaCopy32(uint32_t ch, const void *src, void *dst, uint32_t size)
{
    if (size)
        set_regs(ch, (uint32_t)(uintptr_t)src, (uint32_t)(uintptr_t)dst, COPY32(size));
}

void MI_DmaCopy16(uint32_t ch, const void *src, void *dst, uint32_t size)
{
    if (size)
        set_regs(ch, (uint32_t)(uintptr_t)src, (uint32_t)(uintptr_t)dst, COPY16(size));
}

void MIi_CardDmaCopy32(uint32_t ch, const void *src, void *dst, uint32_t size)
{
    MI_DmaCopy32(ch, src, dst, size);
}

/* A fill takes its value from the source register, which the SDK points at a word it keeps. */
static uint32_t s_fill_value[4];

void MI_DmaFill32(uint32_t ch, void *dst, uint32_t data, uint32_t size)
{
    if (!size)
        return;
    s_fill_value[ch & 3] = data;
    set_regs(ch, (uint32_t)(uintptr_t)&s_fill_value[ch & 3], (uint32_t)(uintptr_t)dst, FILL32(size));
}

void MI_DmaCopy32Async(uint32_t ch, const void *src, void *dst, uint32_t size, MIDmaCallback cb,
                       void *arg)
{
    MI_DmaCopy32(ch, src, dst, size);
    complete(cb, arg);
}

void MI_DmaFill32Async(uint32_t ch, void *dst, uint32_t data, uint32_t size, MIDmaCallback cb,
                       void *arg)
{
    MI_DmaFill32(ch, dst, data, size);
    complete(cb, arg);
}

/* Command lists to the geometry FIFO (0x04000400). */
static void send_gx(const void *src, uint32_t size)
{
    const uint32_t *w = src;
    volatile void *fifo = KH_IO_PTR(0x04000400);
    uint32_t i;
    for (i = 0; i < size / 4; i++)
        kh_io_fifo_write32(fifo, w[i]);
}

void MI_SendGXCommandAsync(uint32_t ch, const void *src, uint32_t size, MIDmaCallback cb, void *arg)
{
    (void)ch;
    send_gx(src, size);
    if (cb) {
        if (size)
            complete(cb, arg);
        else
            cb(arg);
    }
}

void MI_SendGXCommandAsyncFast(uint32_t ch, const void *src, uint32_t size, MIDmaCallback cb,
                               void *arg)
{
    MI_SendGXCommandAsync(ch, src, size, cb, arg);
}
