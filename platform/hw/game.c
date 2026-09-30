/* Runs the game: the DS side is prepared, NitroMain (the decomp's `main`, renamed by
 * -Dmain=NitroMain) starts on its own thread, and the Vita's main thread becomes the display:
 * every Vita VBlank it samples the controls into the DS registers, raises the DS VBlank
 * interrupt and presents the two screens.
 *
 * What the boot ROM and the ARM7 leave in memory before the ARM9 starts is set up here: the
 * cartridge header in the shared area and the ARM7's PXI handlers. */
#include "hw/io.h"
#include "hw/memmap.h"
#include "hw/overlays.h"
#include "hw/shared_area.h"
#include "console.h"
#include "input.h"
#include "log.h"
#include "nitro/arm7.h"
#include "nitro/card.h"
#include "nitro/cpu.h"
#include "nitro/overlay.h"
#include "rom.h"
#include "video.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>
#include <psp2/kernel/cpu.h>

extern void NitroMain(void);

#define GAME_STACK_SIZE (1024 * 1024)

#define HW_ROM_HEADER_BUF 0x027ffe00u
#define HW_CARD_ROM_HEADER 0x027ffa80u
#define HW_ROM_HEADER_SIZE 0x160u
#define HW_BUTTON_XY_BUF 0x027fffa8u
#define HW_VBLANK_COUNT_BUF 0x027ffc3cu

/* The firmware's touch calibration (NVRAMConfig.ncd.tp, user settings + 0x58): two screen
 * points and the raw ADC values they read. The port reports touches as raw = pixel * these
 * factors, and the SDK's TP_GetUserInfo turns them back into pixels. */
#define HW_TP_CALIBRATION 0x027ffcd8u
#define TP_RAW_PER_X 16
#define TP_RAW_PER_Y 21

static uint32_t s_top[256 * 192], s_bottom[256 * 192];
static int s_console = 1; /* bring-up builds start with the console shown */

static int game_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    kh_cpu_set_owner();
    LOG("game: NitroMain");
    log_flush();
    NitroMain();
    LOG("game: NitroMain returned");
    return 0;
}

static void boot_state(void)
{
    kh_hw_reset();
    memcpy(KH_SHARED(HW_ROM_HEADER_BUF), rom_header(), HW_ROM_HEADER_SIZE);
    memcpy(KH_SHARED(HW_CARD_ROM_HEADER), rom_header(), HW_ROM_HEADER_SIZE);
    *(volatile uint16_t *)KH_SHARED(HW_BUTTON_XY_BUF) = 0x2c00; /* nothing held, lid open */
    {
        volatile uint8_t *tp = KH_SHARED(HW_TP_CALIBRATION);
        const uint16_t x1 = 16, y1 = 16, x2 = 240, y2 = 176;
        *(volatile uint16_t *)(tp + 0) = x1 * TP_RAW_PER_X;
        *(volatile uint16_t *)(tp + 2) = y1 * TP_RAW_PER_Y;
        tp[4] = (uint8_t)x1, tp[5] = (uint8_t)y1;
        *(volatile uint16_t *)(tp + 6) = x2 * TP_RAW_PER_X;
        *(volatile uint16_t *)(tp + 8) = y2 * TP_RAW_PER_Y;
        tp[10] = (uint8_t)x2, tp[11] = (uint8_t)y2;
    }
    /* the card ID the boot read, where CARDi_CheckPulledOutCore compares it */
    *(volatile uint32_t *)KH_SHARED(0x027ff800) = KH_CARD_ID;
    *(volatile uint32_t *)KH_SHARED(0x027ffc00) = KH_CARD_ID;
    kh_cpu_init();
    kh_arm7_init();
    kh_overlays_snapshot();
}

/* BGR555 -> RGBA8888 */
static uint32_t rgba(uint16_t c)
{
    uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    return 0xff000000u | b << 16 | g << 8 | r;
}

/* Until the 2D engines are emulated: each screen shows its engine's backdrop colour (BG
 * palette entry 0), white while the engine's display is off, dimmed or brightened as
 * MASTER_BRIGHT says. Enough to see the game's fades and screen changes. */
static void fill_backdrop(uint32_t *fb, uint32_t dispcnt_addr, uint32_t pal_off,
                          uint32_t bright_addr)
{
    uint32_t dispcnt = KH_IO32(dispcnt_addr), c;
    uint16_t bright = KH_IO16(bright_addr);
    int i;

    if (((dispcnt >> 16) & 3) == 0) {
        c = 0xffffffffu; /* display mode 0: the engine outputs white */
    } else {
        uint16_t col;
        memcpy(&col, kh_ds_palette + pal_off, 2);
        c = rgba(col);
        if ((bright >> 14) & 3) {
            int f = bright & 31, up = ((bright >> 14) & 3) == 1, ch;
            uint32_t out = 0xff000000u;
            if (f > 16)
                f = 16;
            for (ch = 0; ch < 24; ch += 8) {
                int v = (c >> ch) & 0xff;
                v = up ? v + (255 - v) * f / 16 : v - v * f / 16;
                out |= (uint32_t)v << ch;
            }
            c = out;
        }
    }
    for (i = 0; i < 256 * 192; i++)
        fb[i] = c;
}

static void present(void)
{
    /* POWCNT1 bit 15: engine A on the top screen */
    int a_on_top = (KH_IO16(0x04000304) >> 15) & 1;
    fill_backdrop(a_on_top ? s_top : s_bottom, 0x04000000, 0x000, 0x0400006c);
    fill_backdrop(a_on_top ? s_bottom : s_top, 0x04001000, 0x400, 0x0400106c);
    video_present(s_top, s_bottom);
}

static void sample_input(void)
{
    InputState in;
    input_poll(&in);
    if (in.swap_layout)
        video_set_layout(video_layout() + 1);
    if (in.toggle_console)
        s_console = !s_console;
    KH_IO16(0x04000130) = in.keyinput;
    *(volatile uint16_t *)KH_SHARED(HW_BUTTON_XY_BUF) = in.extkeys;
    /* raw ADC units, as the calibration written by boot_state defines them */
    kh_arm7_touch(in.touching, in.touch_x * TP_RAW_PER_X, in.touch_y * TP_RAW_PER_Y);
}

static uint32_t s_last_progress, s_stuck_frames;

void kh_game_run(void)
{
    int i, entries = 0;
    uint32_t frame = 0;
    SceUID th;

    for (i = 0; i < kh_overlay_count; i++)
        entries += kh_overlays[i].entry != NULL;
    LOG("game: %d overlays, %d with an entry", kh_overlay_count, entries);

    boot_state();
    input_init();

    /* the display keeps core 0 whatever the game does; the game runs on core 1 */
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    th = sceKernelCreateThread("kh_game", game_thread, 0x10000100, GAME_STACK_SIZE, 0,
                               KH_GAME_CPU_MASK, NULL);
    if (th < 0) {
        LOG("game: create thread failed %08x", th);
        return;
    }
    sceKernelStartThread(th, 0, NULL);

    for (;;) {
        char status[96];
        uint32_t progress;

        sample_input();
        kh_hw_vblank_start_us = sceKernelGetProcessTimeWide();
        (*(volatile uint32_t *)KH_SHARED(HW_VBLANK_COUNT_BUF))++;
        if (KH_IO16(0x04000004) & 0x08) /* DISPSTAT: VBlank IRQ enabled */
            kh_irq_raise(KH_IRQ_VBLANK);

        snprintf(status, sizeof(status),
                 "f%u irq%u pre%u sw%u IE%08x IME%d DISP%08x",
                 (unsigned)frame, (unsigned)kh_cpu_irqs_delivered, (unsigned)kh_cpu_preempted,
                 (unsigned)kh_cpu_switches, (unsigned)KH_IO32(0x04000210),
                 KH_IO16(0x04000208) & 1, (unsigned)KH_IO32(0x04000000));
        video_set_overlay(s_console ? console_render(status) : NULL);
        present(); /* waits for the Vita's VBlank */

        /* watchdog: the game side has done nothing observable for 5 s */
        progress = kh_cpu_irqs_delivered + kh_cpu_switches + kh_card_reads;
        if (progress != s_last_progress) {
            s_last_progress = progress;
            s_stuck_frames = 0;
        } else if (++s_stuck_frames == 300) {
            LOG("watchdog: no progress for 5 s (%s)", status);
            kh_cpu_log_state();
            log_flush();
        }
        if ((++frame % 600) == 0) {
            LOG("game: %s", status);
            log_flush();
        }
    }
}
