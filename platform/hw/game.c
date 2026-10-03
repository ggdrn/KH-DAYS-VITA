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
#include "hw/capture.h"
#include "hw/textures.h"
#include "hw/vram.h"
#include "hw/io.h"
#include "hw/memmap.h"
#include "hw/overlays.h"
#include "hw/shared_area.h"
#include "config.h"
#include "console.h"
#include "portmenu.h"
#include "workers.h"
#include "fault.h"
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
#include <psp2/power.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>
#include <psp2/kernel/cpu.h>

extern uint32_t kh_reset_parameter(void); /* nitro/asm_replacements.c */
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
    /* OS_ResetSystem's parameter survives the reset (asm_replacements.c): Boot_InitScene picks
     * the first scene from it */
    *(volatile uint32_t *)KH_SHARED(0x027ffc20) = kh_reset_parameter();
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

/* The Vita going to sleep (power button, low battery): the save and the log are written out
 * first, so nothing waits on the second the save flush normally lets pass, in case the system
 * then closes the app. Callbacks run on the thread that sleeps in a ...CB call. */
static int power_callback(int notify_id, int count, int arg, void *common)
{
    (void)notify_id;
    (void)count;
    (void)common;
    if (arg & (SCE_POWER_CB_SYSTEM_SUSPEND | SCE_POWER_CB_APP_SUSPEND |
               SCE_POWER_CB_LOW_BATTERY_SUSPEND | SCE_POWER_CB_THERMAL_SUSPEND)) {
        kh_backup_flush();
        LOG("power: suspending (%08x), save written", (unsigned)arg);
        log_flush();
    }
    return 0;
}

static int power_thread(SceSize args, void *argp)
{
    SceUID cb = sceKernelCreateCallback("kh_power_cb", 0, power_callback, NULL);
    (void)args;
    (void)argp;
    if (cb < 0 || scePowerRegisterCallback(cb) < 0)
        LOG("power: no suspend callback (%08x)", (unsigned)cb);
    for (;;)
        sceKernelDelayThreadCB(1000000);
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

/* engine A was among the engines drawn: only then do the bands say where its 3D pixels are */
static int drew_a(const Frame2d *f)
{
    int i;
    for (i = 0; i < f->neng; i++)
        if (f->eng[i] == KH_ENGINE_A)
            return 1;
    return 0;
}

/* per-stage display times since the last report, for the 10 s line */
static uint64_t s_t3d_total, s_join_total, s_present_total;
static uint32_t s_2d_async; /* 2D pictures drawn over two Vita frames (60 fps mode) */
static uint32_t s_t3d_max, s_join_max, s_present_max, s_prep_max; /* the worst frame's */

static inline void stage_max(uint32_t *max, uint64_t us)
{
    if (us > *max)
        *max = (uint32_t)us;
}

static void render_chunk(int chunk, void *arg)
{
    Frame2d *f = arg;
    const int engine = f->eng[chunk % f->neng], band = chunk / f->neng;
    int r = kh_gpu2d_render_lines(engine, f->fb[engine], band * BAND_LINES, (band + 1) * BAND_LINES);
    if (engine == KH_ENGINE_A)
        f->a3d[band] = r;
}

static volatile int s_dump_2d; /* L+R+Triangle: the 2D side of the frame too */
static volatile int s_fast_forward; /* L+R+Square */
static void dump_2d(int a_on_top);

static void present(void)
{
    /* POWCNT1 bit 15: engine A on the top screen */
    int a_on_top = (KH_IO16(0x04000304) >> 15) & 1;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    unsigned tex3d = 0, raw3d;
    int a3d = 0, i, draw2d;
    static Frame2d f2d;
    static uint32_t seen_serial, serial_vb, drawn_vb, pending;
    static int last_a3d;
    /* 60 fps mode: the 2D of a new game frame is drawn by the helper over this Vita frame and
     * the next, and shown with the next one, where the new frame's 3D (after its halfway mix)
     * is shown too; it no longer has to fit in one frame with everything else */
    static int async_2d;
    int upload2d = 0;

    if (async_2d) {
        uint64_t t = sceKernelGetProcessTimeWide(), d;
        s_stage = "2d join";
        workers_join();
        async_2d = 0;
        upload2d = 1;
        if (drew_a(&f2d)) {
            last_a3d = 0;
            for (i = 0; i < BANDS; i++)
                last_a3d |= f2d.a3d[i];
        }
        d = sceKernelGetProcessTimeWide() - t;
        s_join_total += d;
        stage_max(&s_join_max, d);
    }

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
    {
        /* widescreen in the field (ov022, the field's action code, is loaded; menus over a
         * 3D model keep the DS's picture) when engine A's screen is drawn wider than 4:3 */
        const float aspect = video_screen_aspect(a_on_top ? 0 : 1);
        kh_gx3d_wide_x = (kh_config.aspect == KH_ASPECT_WIDE && kh_overlay_loaded(22) && aspect > 1.4f)
                             ? (4.0f / 3.0f) / aspect : 1.0f;
        /* config hud: the 2D kept 4:3 in the middle while the 3D is wide */
        video_set_hud_scale(kh_config.hud && kh_gx3d_wide_x < 0.999f ? kh_gx3d_wide_x : 1.0f);
    }
    {
        const KhGxFrame *frame3d = NULL;
        const int on3d = (KH_IO32(0x04000000) & 0x108) == 0x108 && s_gpu3d;
        if (on3d) {
            /* new textures first, decoded on both cores while the helper is free */
            uint64_t t = sceKernelGetProcessTimeWide();
            s_stage = "3d textures";
            frame3d = kh_gx3d_acquire();
            kh_gpu3d_prepare(frame3d);
            stage_max(&s_prep_max, sceKernelGetProcessTimeWide() - t);
        }
        s_stage = "2d";
        if (f2d.neng)
            memset(f2d.a3d, 0, sizeof(f2d.a3d));
        workers_begin(render_chunk, BANDS * f2d.neng, &f2d);
        if (f2d.neng && kh_config.frame_interpolation) {
            async_2d = 1;
            s_2d_async++;
        }
        s_stage = "3d";
        {
            uint64_t t = sceKernelGetProcessTimeWide(), d;
            if (on3d)
                tex3d = kh_gpu3d_render(frame3d);
            d = sceKernelGetProcessTimeWide() - t;
            s_t3d_total += d;
            stage_max(&s_t3d_max, d);
        }
        if (!async_2d) {
            uint64_t t = sceKernelGetProcessTimeWide(), d;
            s_stage = "2d join";
            workers_join();
            d = sceKernelGetProcessTimeWide() - t;
            s_join_total += d;
            stage_max(&s_join_max, d);
            if (f2d.neng) {
                upload2d = 1;
                /* engine A on the small screen is drawn every other time: when only the
                 * other engine was, A's 3D pixels are where they were (0.0.78 flickered) */
                if (drew_a(&f2d)) {
                    last_a3d = 0;
                    for (i = 0; i < BANDS; i++)
                        last_a3d |= f2d.a3d[i];
                }
            }
        }
    }
    /* which pixels are 3D: from the last 2D picture finished */
    a3d = last_a3d;
    s_render_us = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    s_render_total += s_render_us;
    if (s_render_us > s_render_max)
        s_render_max = s_render_us;
    raw3d = tex3d; /* for the capture, which sees the 3D layer whatever is displayed */
    if (!a3d)
        tex3d = 0;
    video_set_3d(tex3d ? (a_on_top ? 0 : 1) : -1, tex3d, KH_IO16(0x0400006c),
                 /* BG0HOFS scrolls the 3D layer: 9 bits, signed */
                 (int)((int16_t)(KH_IO16(0x04000010) << 7) >> 7), KH_IO16(0x04000052),
                 (uint16_t)(kh_ds_palette[0] | kh_ds_palette[1] << 8));
    if (a3d && !tex3d && upload2d) {
        /* no 3D to lay in: the 3D pixels show what is under them */
        uint32_t *fb = a_on_top ? s_top : s_bottom;
        int i;
        for (i = 0; i < 256 * 192; i++)
            fb[i] |= 0xff000000u;
    }
    /* the display capture the game armed for this frame (dialogue screen blends) */
    s_stage = "capture";
    kh_capture_run(raw3d);
    {
        /* engine A showing the VRAM bank the last capture went to: the capture */
        const uint32_t dc = KH_IO32(0x04000000);
        if (((dc >> 16) & 3) == 2 && kh_capture_shown((int)((dc >> 18) & 3)))
            video_show_capture(a_on_top ? 0 : 1, KH_IO16(0x0400006c));
        else
            video_show_capture(-1, 0);
    }
    if (s_dump_2d) {
        s_dump_2d = 0;
        dump_2d(a_on_top);
    }
    s_stage = "present";
    {
        uint64_t t = sceKernelGetProcessTimeWide();
        /* unchanged screens are not uploaded again */
        video_present(upload2d ? s_top : NULL, upload2d ? s_bottom : NULL);
        t = sceKernelGetProcessTimeWide() - t;
        s_present_total += t;
        stage_max(&s_present_max, t);
    }
    /* the 2D drawn over two frames: this thread is idle until the next VBlank, so it takes
     * its share of the chunks rather than leave them all to the helper (which shares its
     * core with the sound) */
    if (async_2d) {
        s_stage = "2d help";
        workers_help();
    }
    s_stage = "loop";
    s_beats++;
}

static void sample_input(void);

/* "60 FPS (jogo 30)": the 3D frames shown per second (the game's own and the mixed ones in
 * between) and the game's, measured over the last second */
static const char *fps_label(void)
{
    static char label[32];
    static uint64_t since;
    static uint32_t serial0, mixes0;
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (!since) {
        since = now;
        serial0 = kh_gx3d_serial();
        mixes0 = kh_gpu3d_mixes;
        snprintf(label, sizeof(label), "-- FPS");
    } else if (now - since >= 1000000) {
        const uint32_t game = kh_gx3d_serial() - serial0, mixes = kh_gpu3d_mixes - mixes0;
        const uint32_t shown = game + mixes > 60 ? 60 : game + mixes;
        snprintf(label, sizeof(label), "%u FPS (jogo %u)", (unsigned)shown, (unsigned)game);
        since = now;
        serial0 = kh_gx3d_serial();
        mixes0 = kh_gpu3d_mixes;
    }
    return label;
}

/* Diagnosis dump of the displayed frame's 2D side: both screens as composed (alpha = the
 * gpu2d.h code per pixel), engine A's layers one by one, and the display registers. */
static void dump_2d(int a_on_top)
{
    static const char dir[] = "ux0:data/khdays/dump";
    static uint32_t layer[5][256 * 192];
    uint32_t *bg[4] = { layer[0], layer[1], layer[2], layer[3] };
    char path[96];
    FILE *f;
    int i, e;

    sceIoMkdir(dir, 0777);
    kh_gpu3d_dump_tga("ux0:data/khdays/dump/screen_a.tga", a_on_top ? s_top : s_bottom, 256, 192);
    kh_gpu3d_dump_tga("ux0:data/khdays/dump/screen_b.tga", a_on_top ? s_bottom : s_top, 256, 192);
    for (e = 0; e < 2; e++) {
        kh_gpu2d_dump_layers(e, bg, layer[4]);
        for (i = 0; i < 5; i++) {
            snprintf(path, sizeof(path), "%s/%c_%s%d.tga", dir, e ? 'b' : 'a', i < 4 ? "bg" : "obj",
                     i < 4 ? i : 0);
            kh_gpu3d_dump_tga(path, layer[i], 256, 192);
        }
    }
    f = fopen("ux0:data/khdays/dump/registers.txt", "w");
    if (f) {
        for (e = 0; e < 2; e++) {
            const uint32_t base = e ? 0x04001000u : 0x04000000u;
            fprintf(f, "engine %c:", e ? 'B' : 'A');
            for (i = 0; i < 0x70; i += 2) {
                if (i % 32 == 0)
                    fprintf(f, "\n  %03x:", i);
                fprintf(f, " %04x", KH_IO16(base + i));
            }
            fprintf(f, "\n");
        }
        fprintf(f, "DISP3DCNT %04x POWCNT1 %04x VRAMCNT %08x %08x\n", KH_IO16(0x04000060),
                KH_IO16(0x04000304), (unsigned)KH_IO32(0x04000240), (unsigned)KH_IO32(0x04000244));
        fprintf(f, "backdrop A %04x B %04x\n", kh_ds_palette[0] | kh_ds_palette[1] << 8,
                kh_ds_palette[0x400] | kh_ds_palette[0x401] << 8);
        {
            /* every bank: what it is mapped as and how much of it holds data */
            static const uint32_t size[9] = { 0x20000, 0x20000, 0x20000, 0x20000, 0x10000,
                                              0x4000, 0x4000, 0x8000, 0x4000 };
            int b;
            for (b = 0; b < 9; b++) {
                const uint32_t *w = (const uint32_t *)kh_vram_bank_home(b);
                uint32_t k, nz = 0;
                for (k = 0; k < size[b] / 4; k++)
                    nz += w[k] != 0;
                fprintf(f, "bank %c: cnt %02x, %u%% non-zero\n", 'A' + b, kh_vram_bank_cnt(b),
                        (unsigned)(nz * 100 / (size[b] / 4)));
            }
        }
        fclose(f);
    }
    LOG("display: dumped the 2D layers to %s", dir);
}

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
        s_vblanks++;
        /* the port menu holds the game: no VBlank reaches it, so nothing advances */
        if (portmenu_is_open())
            continue;
        {
            /* fast-forward (L+R+Square): two or three DS VBlanks per Vita frame, spread over
             * it; the game runs as fast as its core allows, the sound keeps its own pace */
            const int n = s_fast_forward ? kh_config.fast_forward : 1;
            int k;
            for (k = 0; k < n; k++) {
                if (k)
                    sceKernelDelayThread(16000 / n);
                kh_hw_vblank_start_us = sceKernelGetProcessTimeWide();
                (*(volatile uint32_t *)KH_SHARED(HW_VBLANK_COUNT_BUF))++;
                if (KH_IO16(0x04000004) & 0x08) /* DISPSTAT: VBlank IRQ enabled */
                    kh_irq_raise(KH_IRQ_VBLANK);
            }
        }
    }
    return 0;
}

static void sample_input(void)
{
    InputState in;
    input_poll(&in);
    if (in.port_menu)
        portmenu_toggle();
    if (in.fast_forward && !portmenu_is_open()) {
        s_fast_forward = !s_fast_forward;
        LOG("input: fast-forward %s", s_fast_forward ? "on" : "off");
    }
    if (portmenu_is_open()) {
        /* the game sees no button and no touch while the menu has them */
        portmenu_input(in.vita_buttons, sceKernelGetProcessTimeWide());
        KH_IO16(0x04000130) = 0x03ff;
        *(volatile uint16_t *)KH_SHARED(HW_BUTTON_XY_BUF) = 0x2c00;
        kh_arm7_touch(0, 0, 0);
        return;
    }
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
    if (in.dump_3d) {
        kh_gpu3d_request_dump();
        s_dump_2d = 1;
    }
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
        { "DISPCNT_A_MODE", 0x04000002, 2 },
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
        if (regs[i].name[0] == 'V' || regs[i].addr == 0x04000002 || logged++ < 400)
            LOGV("reg: f%u %s %0*x -> %0*x", (unsigned)frame, regs[i].name, regs[i].size * 2,
                (unsigned)last[i], regs[i].size * 2, (unsigned)v);
        last[i] = v;
    }
}

static void set_volume(int percent)
{
    snd7_port_volume = (float)percent / 100.0f;
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
    portmenu_on_scale = kh_gpu3d_set_scale;
    portmenu_on_texture_filter = kh_gpu3d_reload_textures;
    portmenu_on_volume = set_volume;
    set_volume(kh_config.volume);

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
    th = sceKernelCreateThread("kh_power", power_thread, 0x10000100 - 20, 0x4000, 0,
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
        if (portmenu_is_open()) {
            video_set_overlay(portmenu_render());
        } else if (s_vblanks - s_debug_shown < 180) {
            /* a debug mode change: the console for 3 s, its top line naming the mode */
            snprintf(status, sizeof(status), "3D DEBUG MODE %d", kh_gpu3d_debug);
            video_set_overlay(console_render(status));
        } else {
            static char label[48];
            label[0] = 0;
            if (kh_config.show_fps)
                snprintf(label, sizeof(label), "%s", fps_label());
            if (s_fast_forward)
                snprintf(label + strlen(label), sizeof(label) - strlen(label), "%s>> %dx",
                         label[0] ? "  " : "", kh_config.fast_forward);
            video_set_overlay(s_console ? console_render(status)
                              : label[0] ? portmenu_render_fps(label) : NULL);
        }
        present(); /* waits for the Vita's VBlank */

        /* watchdog: the game side has done nothing observable for 5 s. Interrupts do not
         * count: VBlanks keep arriving while every NitroSDK thread is stuck (0.0.18: the main
         * thread asleep in OS_WaitIrq); a running frame loop switches threads every frame. */
        progress = kh_cpu_switches + kh_card_reads;
        if (progress != s_last_progress || portmenu_is_open()) {
            s_last_progress = progress;
            s_stuck_frames = 0;
        } else if (++s_stuck_frames == 300) {
            LOG("watchdog: no progress for 5 s (%s)", status);
            kh_cpu_log_state();
            log_wait_registers();
            kh_cpu_log_owner_stack();
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
                {
                    /* the game core's load (time not idle in OS_Halt), and the colour effects
                     * of both engines: a screen that turns dark or bright shows here */
                    static uint64_t halt_last;
                    const uint64_t halt = kh_cpu_halt_us;
                    const unsigned busy = 1000 - (unsigned)((halt - halt_last) * 1000 / (now - s_window_start + 1));
                    halt_last = halt;
                    LOG("display: per frame 3d submit %uus, 2d after it %uus, present %uus; 2d "
                        "reused in %u of 600, %u drawn over two frames; game core %u.%u%% busy",
                        (unsigned)(s_t3d_total / 600), (unsigned)(s_join_total / 600),
                        (unsigned)(s_present_total / 600), (unsigned)s_2d_skipped,
                        (unsigned)s_2d_async, busy / 10, busy % 10);
                    s_2d_async = 0;
                    LOG("display: worst frame: textures %uus, 3d submit %uus, 2d after it %uus, "
                        "present %uus", (unsigned)s_prep_max, (unsigned)s_t3d_max,
                        (unsigned)s_join_max, (unsigned)s_present_max);
                    s_prep_max = s_t3d_max = s_join_max = s_present_max = 0;
                    LOG("display: effects A %04x/%02x mb %04x, B %04x/%02x mb %04x, 3D %04x; "
                        "%u captures (DISPCAPCNT %08x)",
                        KH_IO16(0x04000050), KH_IO16(0x04000054) & 31, KH_IO16(0x0400006c),
                        KH_IO16(0x04001050), KH_IO16(0x04001054) & 31, KH_IO16(0x0400106c),
                        KH_IO16(0x04000060), (unsigned)kh_capture_take_count(),
                        (unsigned)KH_IO32(0x04000064));
                }
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
                if (rs.interpolated)
                    LOG("gpu3d: 10 s: %u frames shown after a halfway mix (60 fps)",
                        (unsigned)rs.interpolated);
                if (rs.textures_decoded)
                    LOG("gpu3d: 10 s: %u textures decoded (%u live), %u ms decoding in parallel, "
                        "at most %u in one frame", (unsigned)rs.textures_decoded,
                        (unsigned)rs.textures_live, (unsigned)(rs.prepare_us / 1000),
                        (unsigned)rs.burst_max);
                if (kh_log_verbose && (rs.batches || rs.textures_decoded)) {
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
                RomStats rs2;
                rom_take_stats(&rs2);
                if (rs2.reads)
                    LOG("rom: 10 s: %u reads, %u from the cache, %u file reads in %u ms",
                        (unsigned)rs2.reads, (unsigned)rs2.hits, (unsigned)rs2.io_calls,
                        (unsigned)(rs2.io_us / 1000));
            }
            {
                Snd7Stats ss;
                snd7_take_stats(&ss);
                if (ss.lists)
                    LOGV("snd: 10 s: %u command lists (%u commands), %u sequences started, %u notes, "
                        "%u alarms, %u unknown commands, %u unknown sequence ops", (unsigned)ss.lists,
                        (unsigned)ss.commands, (unsigned)ss.seq_starts, (unsigned)ss.notes,
                        (unsigned)ss.alarms, (unsigned)ss.unknown_cmd, (unsigned)ss.unknown_seq);
            }
            if (gs.commands)
                LOGV("gx3d: 10 s: %u swaps, %u cmds, %u polys (%u culled), %u verts, %u unknown, "
                    "%u over RAM", (unsigned)gs.frames, (unsigned)gs.commands, (unsigned)gs.polygons,
                    (unsigned)gs.culled, (unsigned)gs.vertices, (unsigned)gs.unknown,
                    (unsigned)gs.overflows);
            log_flush();
        }
    }
}
