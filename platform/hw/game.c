/* Runs the game: the DS side is prepared, NitroMain (the decomp's `main`, renamed by
 * -Dmain=NitroMain) starts on its own thread, and the Vita's main thread becomes the display:
 * every Vita VBlank it samples the controls into the DS registers, raises the DS VBlank
 * interrupt and presents the two screens.
 *
 * What the boot ROM and the ARM7 leave in memory before the ARM9 starts is set up here: the
 * cartridge header in the shared area and the ARM7's PXI handlers. */
#include "audio/audio_out.h"
#include "audio/snd7.h"
#include "hw/gpu2d.h"
#include "hw/gpu3d.h"
#include "hw/gx3d.h"
#include "hw/textures.h"
#include "hw/io.h"
#include "hw/memmap.h"
#include "hw/overlays.h"
#include "hw/shared_area.h"
#include "config.h"
#include "console.h"
#include "workers.h"
#include "input.h"
#include "log.h"
#include "nitro/arm7.h"
#include "nitro/backup.h"
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
#include <psp2/display.h>

static int s_gpu3d;          /* the GPU 3D renderer is up */
static uint64_t s_render_total, s_window_start; /* 2D time and wall clock since the last report */
static uint32_t s_render_max;

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
static int s_console; /* the on-screen log: off at start, L+R+Start toggles it */

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
    kh_gx3d_init();
    audio_out_init();
    s_gpu3d = kh_gpu3d_init(kh_config.render_scale);
    kh_gpu2d_init();
    workers_init();
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
static volatile uint32_t s_vblanks; /* VBlanks raised so far (vblank_thread) */
static uint32_t s_2d_skipped;      /* displayed frames that reused the last 2D image */
static volatile uint32_t s_debug_shown = 0x80000000u; /* VBlank of the last debug mode change */

/* Where the display loop is, for the stall monitor: it runs on its own, so a display loop
 * stuck in a call (GPU, worker, a lock) still gets reported. */
static volatile const char *s_stage = "start";
static volatile uint32_t s_beats;

static int stall_monitor(SceSize args, void *argp)
{
    uint32_t last = 0, still = 0;
    (void)args;
    (void)argp;
    for (;;) {
        sceKernelDelayThread(1000000);
        if (s_beats != last) {
            last = s_beats;
            still = 0;
        } else if (++still == 3) {
            LOG("monitor: the display loop has been in '%s' for 3 s", (const char *)s_stage);
            log_flush();
        }
    }
    return 0;
}

/* The 2D frame as chunks of lines, both engines, shared by the display thread and the
 * helper core (platform/core/workers.c). */
#define BANDS 8
#define BAND_LINES (192 / BANDS)

typedef struct {
    uint32_t *fb[2]; /* engine A's screen, engine B's */
    int a3d[BANDS];
    int eng[2], neng; /* the engines drawn this frame */
} Frame2d;

/* per-stage display times since the last report, for the 10 s line */
static uint64_t s_t3d_total, s_join_total, s_present_total;

static void render_chunk(int chunk, void *arg)
{
    Frame2d *f = arg;
    const int engine = f->eng[chunk % f->neng], band = chunk / f->neng;
    int r = kh_gpu2d_render_lines(engine, f->fb[engine], band * BAND_LINES, (band + 1) * BAND_LINES);
    if (engine == KH_ENGINE_A)
        f->a3d[band] = r;
}

static void present(void)
{
    /* POWCNT1 bit 15: engine A on the top screen */
    int a_on_top = (KH_IO16(0x04000304) >> 15) & 1;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    unsigned tex3d = 0;
    int a3d = 0, i, draw2d;
    static Frame2d f2d;
    static uint32_t seen_serial, serial_vb, drawn_vb, pending;
    static int last_a3d;

    /* The 2D image only changes when the game has finished a frame (its SWAP_BUFFERS) and the
     * VBlank after it has applied the frame's OAM and register updates: at the game's 30 fps
     * every other display frame would draw the same picture again. At least every 4 VBlanks
     * regardless, for effects that VBlank handlers run on their own. */
    {
        const uint32_t serial = kh_gx3d_serial(), vb = s_vblanks;
        if (serial != seen_serial) {
            seen_serial = serial;
            serial_vb = vb;
            pending = 1;
        }
        draw2d = (pending && vb != serial_vb) || vb - drawn_vb >= 4;
        if (draw2d) {
            pending = 0;
            drawn_vb = vb;
        } else {
            s_2d_skipped++;
        }
    }

    /* the helper core starts on the 2D chunks while this thread sends the 3D frame to the
     * GPU (engine A's 3D layer is on: DISPCNT bits 3 and 8), then both share what is left */
    f2d.fb[KH_ENGINE_A] = a_on_top ? s_top : s_bottom;
    f2d.fb[KH_ENGINE_B] = a_on_top ? s_bottom : s_top;
    {
        /* the small screen (layout inset) is redrawn every other frame: 30 Hz is plenty at
         * that size, and it is a third of the 2D work saved */
        static uint32_t parity;
        const int inset = video_inset_screen(); /* 0 top, 1 bottom, -1 none */
        const int inset_engine = inset < 0 ? -1 : ((inset == 0) == a_on_top ? KH_ENGINE_A : KH_ENGINE_B);
        f2d.neng = 0;
        if (draw2d) {
            for (i = 0; i < 2; i++)
                if (i != inset_engine || (parity & 1))
                    f2d.eng[f2d.neng++] = i;
            parity++;
        }
    }
    s_stage = "2d";
    workers_begin(render_chunk, BANDS * f2d.neng, &f2d);
    s_stage = "3d";
    {
        uint64_t t = sceKernelGetProcessTimeWide();
        if ((KH_IO32(0x04000000) & 0x108) == 0x108 && s_gpu3d)
            tex3d = kh_gpu3d_render(kh_gx3d_acquire());
        s_t3d_total += sceKernelGetProcessTimeWide() - t;
    }
    s_stage = "2d join";
    {
        uint64_t t = sceKernelGetProcessTimeWide();
        workers_join();
        s_join_total += sceKernelGetProcessTimeWide() - t;
    }
    for (i = 0; i < BANDS; i++)
        a3d |= f2d.a3d[i];
    (void)last_a3d;
    s_render_us = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    s_render_total += s_render_us;
    if (s_render_us > s_render_max)
        s_render_max = s_render_us;
    if (!a3d)
        tex3d = 0;
    video_set_3d(tex3d ? (a_on_top ? 0 : 1) : -1, tex3d, KH_IO16(0x0400006c),
                 /* BG0HOFS scrolls the 3D layer: 9 bits, signed */
                 (int)((int16_t)(KH_IO16(0x04000010) << 7) >> 7));
    if (a3d && !tex3d) {
        /* no 3D to lay in: the 3D pixels show what is under them */
        uint32_t *fb = a_on_top ? s_top : s_bottom;
        int i;
        for (i = 0; i < 256 * 192; i++)
            fb[i] |= 0xff000000u;
    }
    s_stage = "present";
    {
        uint64_t t = sceKernelGetProcessTimeWide();
        /* unchanged screens are not uploaded again */
        video_present(draw2d ? s_top : NULL, draw2d ? s_bottom : NULL);
        s_present_total += sceKernelGetProcessTimeWide() - t;
    }
    s_stage = "loop";
    s_beats++;
}

static void sample_input(void);

/* VBlank at the Vita's own 60 Hz, whatever the rendering costs: the game counts time in
 * VBlanks, so tying them to the display loop slowed the whole game down with every frame
 * that took longer than 16.7 ms to draw. */

static int vblank_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (;;) {
        sceDisplayWaitVblankStart();
        sample_input();
        kh_hw_vblank_start_us = sceKernelGetProcessTimeWide();
        (*(volatile uint32_t *)KH_SHARED(HW_VBLANK_COUNT_BUF))++;
        if (KH_IO16(0x04000004) & 0x08) /* DISPSTAT: VBlank IRQ enabled */
            kh_irq_raise(KH_IRQ_VBLANK);
        s_vblanks++;
    }
    return 0;
}

static void sample_input(void)
{
    InputState in;
    input_poll(&in);
    if (in.swap_layout)
        video_set_layout(video_layout() + 1);
    if (in.swap_screens)
        video_swap_screens();
    if (in.debug_cycle) {
        static const char *const names[KH_GPU3D_DEBUG_MODES] = {
            "normal", "opaque polygons in frame order", "textures re-hashed every frame",
            "texture-coordinate generation off" };
        kh_gpu3d_debug = (kh_gpu3d_debug + 1) % KH_GPU3D_DEBUG_MODES;
        s_debug_shown = s_vblanks;
        LOG("debug: 3D mode %d (%s)", kh_gpu3d_debug, names[kh_gpu3d_debug]);
    }
    if (in.dump_3d)
        kh_gpu3d_request_dump();
    if (in.toggle_console)
        s_console = !s_console;
    KH_IO16(0x04000130) = in.keyinput;
    *(volatile uint16_t *)KH_SHARED(HW_BUTTON_XY_BUF) = in.extkeys;
    /* raw ADC units, as the calibration written by boot_state defines them */
    kh_arm7_touch(in.touching, in.touch_x * TP_RAW_PER_X, in.touch_y * TP_RAW_PER_Y);
}

static uint32_t s_last_progress, s_stuck_frames;

/* The registers code busy-waits on, for the watchdog: a loop that calls nothing of the port
 * is usually waiting for one of these to change. */
static void log_wait_registers(void)
{
    LOG("regs: DMA0 %08x DMA1 %08x DMA2 %08x DMA3 %08x", (unsigned)KH_IO32(0x040000b8),
        (unsigned)KH_IO32(0x040000c4), (unsigned)KH_IO32(0x040000d0), (unsigned)KH_IO32(0x040000dc));
    LOG("regs: DISPSTAT %04x VCOUNT %04x GXSTAT %08x IF %08x IPCSYNC %04x IPCFIFOCNT %04x",
        KH_IO16(0x04000004), KH_IO16(0x04000006), (unsigned)KH_IO32(0x04000600),
        (unsigned)KH_IO32(0x04000214), KH_IO16(0x04000180), KH_IO16(0x04000184));
    LOG("regs: ROMCTRL %08x AUXSPICNT %04x EXMEMCNT %04x DIVCNT %04x SQRTCNT %04x",
        (unsigned)KH_IO32(0x040001a4), KH_IO16(0x040001a0), KH_IO16(0x04000204),
        KH_IO16(0x04000280), KH_IO16(0x040002b0));
}

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
        { "MASTER_BRIGHT_A", 0x0400006c, 2 }, { "MASTER_BRIGHT_B", 0x0400106c, 2 },
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
    th = sceKernelCreateThread("kh_vblank", vblank_thread, 0x10000100 - 30, 0x4000, 0,
                               SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);
    th = sceKernelCreateThread("kh_monitor", stall_monitor, 0x10000100 - 20, 0x4000, 0,
                               SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);

    for (;;) {
        char status[96];
        uint32_t progress;

        s_stage = "input";
        watch_registers(frame);
        s_stage = "save";
        kh_backup_tick();

        snprintf(status, sizeof(status),
                 "f%u irq%u pre%u sw%u IE%08x IME%d DISP%08x",
                 (unsigned)frame, (unsigned)kh_cpu_irqs_delivered, (unsigned)kh_cpu_preempted,
                 (unsigned)kh_cpu_switches, (unsigned)KH_IO32(0x04000210),
                 KH_IO16(0x04000208) & 1, (unsigned)KH_IO32(0x04000000));
        if (s_vblanks - s_debug_shown < 180) {
            /* a debug mode change: the console for 3 s, its top line naming the mode */
            snprintf(status, sizeof(status), "3D DEBUG MODE %d", kh_gpu3d_debug);
            video_set_overlay(console_render(status));
        } else {
            video_set_overlay(s_console ? console_render(status) : NULL);
        }
        present(); /* waits for the Vita's VBlank */

        /* watchdog: the game side has done nothing observable for 5 s. Interrupts do not
         * count: VBlanks keep arriving while every NitroSDK thread is stuck (0.0.18: the main
         * thread asleep in OS_WaitIrq); a running frame loop switches threads every frame. */
        progress = kh_cpu_switches + kh_card_reads;
        if (progress != s_last_progress) {
            s_last_progress = progress;
            s_stuck_frames = 0;
        } else if (++s_stuck_frames == 300) {
            LOG("watchdog: no progress for 5 s (%s)", status);
            kh_cpu_log_state();
            log_wait_registers();
            kh_probe_arm();
            log_flush();
        }
        if (s_stuck_frames > 300) {
            if (kh_probe_report()) {
                log_flush();
            } else if (s_stuck_frames == 420) {
                LOG("probe: no port call from the running thread in 2 s: it spins on memory "
                    "(see the registers above)");
                log_flush();
            }
        }
        if ((++frame % 600) == 0) {
            KhGx3dStats gs;
            {
                uint64_t now = sceKernelGetProcessTimeWide();
                static uint32_t vb_last;
                LOG("game: %s 2d avg %uus max %uus, %u vblanks, %u.%u fps", status,
                    (unsigned)(s_render_total / 600), (unsigned)s_render_max,
                    (unsigned)(s_vblanks - vb_last),
                    (unsigned)(6000000000ull / (now - s_window_start)) / 10,
                    (unsigned)(6000000000ull / (now - s_window_start)) % 10);
                LOG("display: per frame 3d submit %uus, 2d after it %uus, present %uus; 2d reused "
                    "in %u of 600", (unsigned)(s_t3d_total / 600), (unsigned)(s_join_total / 600),
                    (unsigned)(s_present_total / 600), (unsigned)s_2d_skipped);
                s_2d_skipped = 0;
                s_t3d_total = s_join_total = s_present_total = 0;
                s_window_start = now;
                vb_last = s_vblanks;
                s_render_total = 0;
                s_render_max = 0;
            }
            kh_gx3d_take_stats(&gs);
            {
                KhGpu3dStats rs;
                kh_gpu3d_take_stats(&rs);
                if (rs.batches || rs.textures_decoded) {
                    LOG("gpu3d: 10 s: %u us drawing, %u batches, %u textures decoded (%u live), "
                        "%u polys skipped", (unsigned)rs.render_us, (unsigned)rs.batches,
                        (unsigned)rs.textures_decoded, (unsigned)rs.textures_live,
                        (unsigned)rs.skipped);
                    LOG("gpu3d: textures by format %u/%u/%u/%u/%u/%u/%u (A3I5 4c 16c 256c 4x4 "
                        "A5I3 direct), %u from empty VRAM; slots tex %x pltt %02x; VRAMCNT "
                        "ahead of the applied banks %u times",
                        (unsigned)rs.fmt[1], (unsigned)rs.fmt[2], (unsigned)rs.fmt[3],
                        (unsigned)rs.fmt[4], (unsigned)rs.fmt[5], (unsigned)rs.fmt[6],
                        (unsigned)rs.fmt[7], (unsigned)rs.empty_src, rs.slots & 15,
                        (rs.slots >> 8) & 63, (unsigned)kh_tex_stale_vramcnt());
                    LOG("gpu3d: polygons by mode %u/%u/%u/%u (modulate decal toon shadow), "
                        "DISP3DCNT %04x", (unsigned)rs.modes[0], (unsigned)rs.modes[1],
                        (unsigned)rs.modes[2], (unsigned)rs.modes[3], (unsigned)rs.disp3dcnt);
                    LOG("gpu3d: textured polygons by texgen %u/%u/%u/%u (none texcoord normal "
                        "vertex), %u with one s,t at every vertex; %u of %u wanting a texture drawn "
                        "without; %u depth-equal", (unsigned)rs.texgen[0],
                        (unsigned)rs.texgen[1], (unsigned)rs.texgen[2], (unsigned)rs.texgen[3],
                        (unsigned)rs.flat_st, (unsigned)rs.tex_none, (unsigned)rs.tex_wanted,
                        (unsigned)rs.depth_equal);
                }
            }
            {
                Snd7Stats ss;
                snd7_take_stats(&ss);
                if (ss.lists)
                    LOG("snd: 10 s: %u command lists (%u commands), %u sequences started, %u notes, "
                        "%u alarms, %u unknown commands, %u unknown sequence ops", (unsigned)ss.lists,
                        (unsigned)ss.commands, (unsigned)ss.seq_starts, (unsigned)ss.notes,
                        (unsigned)ss.alarms, (unsigned)ss.unknown_cmd, (unsigned)ss.unknown_seq);
            }
            if (gs.commands)
                LOG("gx3d: 10 s: %u swaps, %u cmds, %u polys (%u culled), %u verts, %u unknown, "
                    "%u over RAM", (unsigned)gs.frames, (unsigned)gs.commands, (unsigned)gs.polygons,
                    (unsigned)gs.culled, (unsigned)gs.vertices, (unsigned)gs.unknown,
                    (unsigned)gs.overflows);
            log_flush();
        }
    }
}
