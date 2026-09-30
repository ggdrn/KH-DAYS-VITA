/* Runs the game: the DS side is prepared, NitroMain (the decomp's `main`, renamed by
 * -Dmain=NitroMain) starts on its own thread, and the Vita's main thread becomes the display:
 * every Vita VBlank it samples the controls into the DS registers, raises the DS VBlank
 * interrupt and presents the two screens.
 *
 * What the boot ROM and the ARM7 leave in memory before the ARM9 starts is set up here: the
 * cartridge header in the shared area and the ARM7's PXI handlers. */
#include "hw/gpu2d.h"
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
#include "nitro/romfs.h"
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
    kh_romfs_init();
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

static uint32_t s_render_us; /* the last frame's 2D rendering time, both engines */

static void present(void)
{
    /* POWCNT1 bit 15: engine A on the top screen */
    int a_on_top = (KH_IO16(0x04000304) >> 15) & 1;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    kh_gpu2d_render(KH_ENGINE_A, a_on_top ? s_top : s_bottom);
    kh_gpu2d_render(KH_ENGINE_B, a_on_top ? s_bottom : s_top);
    s_render_us = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
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

/* The registers that mark the boot's progress, compared once per frame: every change is
 * logged (interrupts switched on, screens configured, VRAM banks mapped). */
static void watch_registers(uint32_t frame)
{
    static const struct {
        const char *name;
        uint32_t addr;
        int size;
    } regs[] = {
        { "IME", 0x04000208, 2 },     { "IE", 0x04000210, 4 },       { "DISPCNT_A", 0x04000000, 4 },
        { "DISPCNT_B", 0x04001000, 4 }, { "POWCNT1", 0x04000304, 2 }, { "VRAMCNT_A-D", 0x04000240, 4 },
        { "VRAMCNT_E-G", 0x04000244, 4 }, { "VRAMCNT_H-I", 0x04000248, 2 },
    };
    static uint32_t last[sizeof(regs) / sizeof(regs[0])];
    static int logged;
    unsigned i;
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint32_t v = regs[i].size == 2 ? KH_IO16(regs[i].addr) : KH_IO32(regs[i].addr);
        if (regs[i].name[0] == 'V' && regs[i].addr == 0x04000244)
            v &= 0x00ffffff; /* 0x04000247 is WRAMCNT */
        if (v == last[i])
            continue;
        if (logged++ < 400)
            LOG("reg: f%u %s %0*x -> %0*x", (unsigned)frame, regs[i].name, regs[i].size * 2,
                (unsigned)last[i], regs[i].size * 2, (unsigned)v);
        last[i] = v;
    }
}

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
        watch_registers(frame);
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
            LOG("game: %s 2d %uus", status, (unsigned)s_render_us);
            log_flush();
        }
    }
}
