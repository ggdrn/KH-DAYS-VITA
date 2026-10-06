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
    kh_gpu2d_profiling = kh_log_verbose; /* where the 2D time goes, in the statistics */
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
    /* the firmware's user settings: the language (bits 0-2 at +0x64), which the game reads at
     * boot (Game_ReadLocalProfile) to pick its text, screens and fonts: 1 English, 2 French,
     * 3 German, 4 Italian, 5 Spanish (config language). Left at 0, the game took English. */
    *(volatile uint16_t *)KH_SHARED(0x027ffc80 + 0x64) = (uint16_t)(kh_config.language & 7);
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

/* this display frame's stages, for the slow-frame lines of the detailed log */
static struct {
    uint32_t prep, t3d, join, present;
} s_cur;

/* the 2D's CPU time per engine (both cores together), for the statistics */
static volatile uint32_t s_2d_engine_us[2];

static void render_chunk(int chunk, void *arg)
{
    Frame2d *f = arg;
    const int engine = f->eng[chunk % f->neng], band = chunk / f->neng;
    const uint64_t t = sceKernelGetProcessTimeWide();
    int r = kh_gpu2d_render_lines(engine, f->fb[engine], band * BAND_LINES, (band + 1) * BAND_LINES);
    if (engine == KH_ENGINE_A)
        f->a3d[band] = r;
    __atomic_add_fetch(&s_2d_engine_us[engine], (uint32_t)(sceKernelGetProcessTimeWide() - t),
                       __ATOMIC_RELAXED);
}

/* Engine B showing nothing but a bitmap BG3 from bank C or bitmap sprites from bank D, sub
 * BG / sub OBJ: the bank a capture went to (the dual-3D scenes), -1 otherwise. */
static int engine_b_bank(uint32_t db, uint8_t cnt_c, uint8_t cnt_d)
{
    if (((db >> 16) & 1) && ((db >> 8) & 0x1f) == 0x08 && (cnt_c & 0x87) == 0x84)
        return 2;
    if (((db >> 16) & 1) && ((db >> 8) & 0x1f) == 0x10 && (cnt_d & 0x87) == 0x84)
        return 3;
    return -1;
}

static volatile int s_dump_2d; /* L+R+Triangle: the 2D side of the frame too */
static volatile int s_fast_forward; /* L+R+Square */
static void dump_2d(int a_on_top);

/* the last screen toggle of a dual-3D scene (kh_dual3d_toggled), with the display registers
 * as the game set them for that frame: read later, they were sometimes the next frame's */
static volatile uint32_t s_toggle_seq, s_toggle_vb = 0x80000000u, s_toggle_serial;
/* the last toggle the display finished showing, and the last whose 2D it has drawn */
static volatile uint32_t s_toggle_done, s_toggle_drawn;
static volatile int s_toggle_top;
static struct {
    uint32_t dispcnt_a, dispcnt_b, vramcnt, dispcapcnt;
    uint16_t bright_a, bright_b;
} s_toggle_regs;

void kh_dual3d_toggled(void)
{
    /* The game toggles every VBlank; the display, drawing both screens' 2D, the 3D and the
     * screen copy, is slower than that, and showed every second toggle or so: the same screen
     * for several frames, then the other (0.0.89). While the scene runs the game waits for the
     * display to finish the last toggle (33 ms at most), so that each one is shown. */
    if (s_vblanks - s_toggle_vb < 8) {
        const uint64_t until = sceKernelGetProcessTimeWide() + 33000;
        while (s_toggle_done != s_toggle_seq && sceKernelGetProcessTimeWide() < until)
            sceKernelDelayThread(100);
    }
    s_toggle_top = (KH_IO16(0x04000304) >> 15) & 1;
    s_toggle_serial = kh_gx3d_serial();
    s_toggle_regs.dispcnt_a = KH_IO32(0x04000000);
    s_toggle_regs.dispcnt_b = KH_IO32(0x04001000);
    s_toggle_regs.vramcnt = KH_IO32(0x04000240);
    s_toggle_regs.dispcapcnt = KH_IO32(0x04000064);
    s_toggle_regs.bright_a = KH_IO16(0x0400006c);
    s_toggle_regs.bright_b = KH_IO16(0x0400106c);
    /* the frame these screens are for, kept: the game may swap the next one before the
     * display gets to it (0.0.84 drew the next frame half the time) */
    kh_gx3d_pin();
    s_toggle_vb = s_vblanks;
    __atomic_add_fetch(&s_toggle_seq, 1, __ATOMIC_RELEASE);
}

/* The single screen's autohide panels (config panel autohide: the INFORMATION bar, the mission
 * gauge): each one slides in for 7 s when what it shows changes to something new -- a picture
 * seen in the last few seconds (a marker blinking) does not count -- and stays while the game
 * is paused (its own pause state, however the pause menu was opened or closed) or Select has
 * pinned it. */
#define PANEL_SHOW_US 7000000u
#define PANEL_SLIDE_US 300000.0f
#define PANEL_RECENT 8

static struct {
    float vis;
    uint64_t until;
    uint32_t seen[PANEL_RECENT];
    uint64_t seen_at[PANEL_RECENT];
} s_panels[KH_PANELS];
static int s_pin_select;
extern int PauseMenu_GetMode(void); /* the game's pause state: 0 running */

/* MASTER_BRIGHT brightening or darkening by a non-zero factor */
static int fading(uint16_t mb)
{
    const int mode = (mb >> 14) & 3;
    return (mode == 1 || mode == 2) && (mb & 31);
}

/* whether the panel's top 8 rows hold a few red pixels: the TARGET tab's letters */
static int panel_has_red(const uint32_t *fb, const int *p)
{
    int x, y, n = 0;
    for (y = p[KH_PANEL_SY]; y < p[KH_PANEL_SY] + 8 && y < p[KH_PANEL_SY] + p[KH_PANEL_SH]; y++)
        for (x = p[KH_PANEL_SX]; x < p[KH_PANEL_SX] + p[KH_PANEL_SW]; x++) {
            const uint32_t c = fb[y * 256 + x];
            const int r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
            n += r > 150 && g < 90 && b < 90;
        }
    return n >= 6;
}

static uint32_t panel_hash(const uint32_t *fb, const int *p)
{
    uint32_t h = 2166136261u;
    int x, y;
    for (y = p[KH_PANEL_SY]; y < p[KH_PANEL_SY] + p[KH_PANEL_SH]; y++)
        for (x = p[KH_PANEL_SX]; x < p[KH_PANEL_SX] + p[KH_PANEL_SW]; x++)
            h = (h ^ (fb[y * 256 + x] & 0xffffffu)) * 16777619u;
    return h;
}

static void update_panels(int single, int drawn, const uint32_t *bottom_fb)
{
    static int was_single;
    static uint64_t last;
    static uint16_t keys_prev = 0x3ff;
    static int was_paused;
    static uint64_t quiet_until;
    const uint64_t now = sceKernelGetProcessTimeWide();
    const uint16_t keys = KH_IO16(0x04000130), pressed = (uint16_t)(keys_prev & ~keys);
    const float step = last ? (float)(now - last) / PANEL_SLIDE_US : 0.0f;
    int i, k, paused;
    keys_prev = keys;
    last = now;
    if (!single) {
        was_single = 0;
        s_pin_select = 0;
        return;
    }
    if (!was_single) {
        /* the field (re)loaded -- an area change reloads its overlays: what the panels show
         * then is not news (0.1.6 brought them out at every area change) */
        was_single = 1;
        quiet_until = now + 2000000u;
    }
    /* KEYINPUT, active low: Select is bit 2 */
    if (pressed & 4)
        s_pin_select ^= 1;
    paused = PauseMenu_GetMode() != 0;
    /* a screen fading (an area change, a cutscene): nothing it shows meanwhile counts */
    if (fading(KH_IO16(0x0400006c)) || fading(KH_IO16(0x0400106c)))
        quiet_until = now + 1500000u;
    if (was_paused && !paused) {
        /* the pause menu closed: the panels go at once, and the screen changing back does
         * not bring them out again */
        for (i = 0; i < KH_PANELS; i++)
            s_panels[i].until = 0;
        quiet_until = now + 1000000u;
    }
    was_paused = paused;
    for (i = 0; i < KH_PANELS; i++) {
        const int *p = kh_config.panel[i];
        float target;
        if (!p[KH_PANEL_AUTOHIDE] || !p[KH_PANEL_SW] || !p[KH_PANEL_SH]) {
            video_set_panel_visibility(i, 1.0f);
            continue;
        }
        if (p[KH_PANEL_AUTOHIDE] == KH_SHOW_ON_RED) {
            if (drawn)
                s_panels[i].until = panel_has_red(bottom_fb, p) ? ~0ull : 0;
            target = now < s_panels[i].until ? 1.0f : 0.0f;
            goto slide;
        }
        if (drawn && p[KH_PANEL_AUTOHIDE] == KH_SHOW_ON_CHANGE) {
            const uint32_t h = panel_hash(bottom_fb, p);
            int known = 0, oldest = 0;
            for (k = 0; k < PANEL_RECENT; k++) {
                if (s_panels[i].seen[k] == h && now - s_panels[i].seen_at[k] < 30000000u) {
                    known = 1;
                    s_panels[i].seen_at[k] = now;
                    break;
                }
                if (s_panels[i].seen_at[k] < s_panels[i].seen_at[oldest])
                    oldest = k;
            }
            if (!known) {
                s_panels[i].seen[oldest] = h;
                s_panels[i].seen_at[oldest] = now;
                if (now >= quiet_until)
                    s_panels[i].until = now + PANEL_SHOW_US;
            }
        }
        target = (paused || s_pin_select || now < s_panels[i].until) ? 1.0f : 0.0f;
    slide:
        if (s_panels[i].vis < target)
            s_panels[i].vis = s_panels[i].vis + step > target ? target : s_panels[i].vis + step;
        else if (s_panels[i].vis > target)
            s_panels[i].vis = s_panels[i].vis - step < target ? target : s_panels[i].vis - step;
        video_set_panel_visibility(i, s_panels[i].vis);
    }
}

static void present(void)
{
    /* POWCNT1 bit 15: engine A on the top screen */
    int a_on_top = (KH_IO16(0x04000304) >> 15) & 1;
    /* Dual 3D (3D on both screens, one frame each in turn): the game swaps the screens to
     * match each 3D frame a little after the VBlank that shows it (Gfx_ToggleCaptureMode,
     * which calls kh_dual3d_toggled). Read at the VBlank, POWCNT was sometimes the previous
     * frame's and sometimes the next one's: the picture jumped between the screens (0.0.80 to
     * 0.0.83). While the scene runs, the display waits for that toggle (8 ms at most) and
     * takes the screens it recorded. */
    const int dual = s_vblanks - s_toggle_vb < 8;
    int trace_before = a_on_top;
    uint32_t trace_wait = 0;
    int trace_polys = -1;
    uint32_t toggle_seq = 0;
    if (dual) {
        const uint64_t t = sceKernelGetProcessTimeWide(), until = t + 8000;
        while (s_toggle_seq == s_toggle_done && sceKernelGetProcessTimeWide() < until)
            sceKernelDelayThread(200);
        trace_wait = (uint32_t)(sceKernelGetProcessTimeWide() - t);
        if (s_toggle_seq == s_toggle_done) {
            /* no new toggle: the screens stay as shown. Drawn again, they mixed this toggle's
             * registers with the next frame's VRAM, which the VBlank had already brought */
            return;
        }
        toggle_seq = s_toggle_seq;
        a_on_top = s_toggle_top;
    }
    kh_gpu3d_direct = dual;
    {
        /* a new dual-3D scene: the screen memories are the last scene's */
        static int was_dual;
        if (dual && !was_dual)
            video_forget_screen_memory();
        was_dual = dual;
    }
    /* the 2D drawn with the frame's own layers: the game changes engine A's visible layers
     * (and B's) with every screen it draws for */
    kh_gpu2d_dispcnt_override[KH_ENGINE_A] = dual ? s_toggle_regs.dispcnt_a : 0;
    kh_gpu2d_dispcnt_override[KH_ENGINE_B] = dual ? s_toggle_regs.dispcnt_b : 0;
    /* experimental single screen (config single_screen): in the field (ov022, its action code,
     * loaded; the same test as the widescreen 3D) with engine A on the top screen, the top
     * screen alone and the bottom one's map, target and mission gauge as panels over it */
    const int single = kh_config.single_screen && !dual && a_on_top && kh_overlay_loaded(22);
    video_set_single_screen(single);
    /* a tutorial page (Ov002_OpenTutorialPage leaves the sub engine with BG2 and BG3 alone,
     * which nothing else in the field does) is shown whole; while the game is paused the bottom
     * screen is drawn without the fade it gets behind the pause menu, the panels being part of
     * the menu then */
    const int tutorial = single && ((KH_IO32(0x04001000) >> 8) & 0x1f) == 0x0c;
    video_set_tutorial(tutorial);
    /* the field's HUD at config hud_size (engine A, with the 3D, on top in the field) */
    /* not while paused: the pause menu's buttons reach into the HP gauge's corner */
    video_set_hud_shrink(!dual && a_on_top && kh_overlay_loaded(22) && PauseMenu_GetMode() == 0);
    kh_gpu2d_plain[KH_ENGINE_B] = single && PauseMenu_GetMode() != 0;
    /* with the detailed log, once a second: this frame's GPU time measured alone */
    video_gpu_probe_begin();
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
        s_cur.join += (uint32_t)d;
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
        /* dual 3D: the engines change screens with every toggle, so both are drawn each
         * time (a skipped one left the other engine's picture on its screen, 0.0.88) */
        draw2d = dual || (pending && vb != serial_vb) || vb - drawn_vb >= 4;
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
        /* dual 3D: engine B's screen shows the last picture engine A gave it, its own 2D (a
         * bitmap the size of the screen) is not seen; it was half the 2D time there */
        const int skip_b = dual && engine_b_bank(s_toggle_regs.dispcnt_b,
                                                 (uint8_t)(s_toggle_regs.vramcnt >> 16),
                                                 (uint8_t)(s_toggle_regs.vramcnt >> 24)) >= 0 &&
                           ((s_toggle_regs.dispcnt_a >> 16) & 3) != 2 &&
                           video_screen_memory_valid(a_on_top ? 1 : 0);
        /* single screen: the bottom screen only shows through small panels; drawn one 2D
         * frame in three (every other one for a tutorial page) it was a third of the 2D time
         * at full rate (0.1.6) */
        static uint32_t single_parity;
        const int skip_single_b = single && (tutorial ? (single_parity & 1) : (single_parity % 3));
        f2d.neng = 0;
        if (draw2d) {
            if (single)
                single_parity++;
            for (i = 0; i < 2; i++)
                if (i == KH_ENGINE_B && (skip_b || skip_single_b))
                    continue;
                else if (dual || i != inset_engine || (parity & 1))
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
            frame3d = dual ? kh_gx3d_pinned() : kh_gx3d_acquire();
            trace_polys = frame3d ? frame3d->npoly : -1;
            kh_gpu3d_prepare(frame3d);
            s_cur.prep = (uint32_t)(sceKernelGetProcessTimeWide() - t);
            stage_max(&s_prep_max, s_cur.prep);
        }
        s_stage = "2d";
        if (f2d.neng)
            memset(f2d.a3d, 0, sizeof(f2d.a3d));
        workers_begin(render_chunk, BANDS * f2d.neng, &f2d);
        if (f2d.neng && kh_config.frame_interpolation && !dual) {
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
            s_cur.t3d = (uint32_t)d;
            stage_max(&s_t3d_max, d);
        }
        if (!async_2d) {
            uint64_t t = sceKernelGetProcessTimeWide(), d;
            s_stage = "2d join";
            workers_join();
            d = sceKernelGetProcessTimeWide() - t;
            s_join_total += d;
            s_cur.join += (uint32_t)d;
            stage_max(&s_join_max, d);
            if (dual)
                s_toggle_drawn = toggle_seq;
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
    {
        /* dual 3D: the frame's own capture, once per toggle (its enable bit cleared too) */
        static uint32_t captured_seq;
        /* engine A showing a VRAM bank itself (the cross-fades of these scenes: display
         * mode 2, each capture blended with the last): the game's own captures are needed */
        const int a_shows_bank = ((s_toggle_regs.dispcnt_a >> 16) & 3) == 2;
        if (!dual) {
            kh_capture_run(raw3d);
        } else if (captured_seq != s_toggle_seq) {
            /* not the game's own capture into bank C or D: what engine A drew is kept for the
             * screen it was on, and that screen shows it while engine B has it. The banks'
             * roles did not follow a fixed two-frame turn (the Sora video on the bottom screen
             * takes three toggles in four VBlanks), and a screen showed the other one's
             * picture now and then (0.0.80 to 0.0.87). */
            captured_seq = s_toggle_seq;
            if (a_shows_bank)
                kh_capture_run_regs(raw3d, s_toggle_regs.dispcapcnt, s_toggle_regs.dispcnt_a);
            else
                kh_capture_screen(a_on_top ? s_top : s_bottom, tex3d, a_on_top ? 0 : 1,
                              tex3d ? s_toggle_regs.bright_a : 0);
            KH_IO32(0x04000064) &= ~0x80000000u;
        }
    }
    {
        /* a screen showing a VRAM bank a capture went to shows the capture: engine A in VRAM
         * display mode (dialogue blends), or engine B with nothing but a bitmap BG3 from bank C
         * or bitmap sprites from bank D (the dual-3D scenes: 3D on both screens, one frame each,
         * the other screen showing the last capture) */
        const uint32_t dc = dual ? s_toggle_regs.dispcnt_a : KH_IO32(0x04000000);
        const uint32_t db = dual ? s_toggle_regs.dispcnt_b : KH_IO32(0x04001000);
        const uint8_t cnt_c = dual ? (uint8_t)(s_toggle_regs.vramcnt >> 16) : kh_ds_io[0x242];
        const uint8_t cnt_d = dual ? (uint8_t)(s_toggle_regs.vramcnt >> 24) : kh_ds_io[0x243];
        const int bank_a = ((dc >> 16) & 3) == 2 ? (int)((dc >> 18) & 3) : -1;
        int bank_b = engine_b_bank(db, cnt_c, cnt_d);
        /* dual 3D: engine B's screen shows the last picture engine A gave it, or with engine
         * A showing a bank, the bank's capture as on the DS (0.0.88 to 0.0.90 kept the
         * cross-fades' banks stale) */
        video_show_capture(a_on_top ? 0 : 1, kh_capture_shown(bank_a, !dual) ? bank_a : -1,
                           dual ? s_toggle_regs.bright_a : KH_IO16(0x0400006c));
        if (dual && bank_b >= 0 && bank_a < 0)
            bank_b = VIDEO_SCREEN_MEMORY + (a_on_top ? 1 : 0); /* its own brightness */
        else if (!kh_capture_shown(bank_b, !dual))
            bank_b = -1;
        video_show_capture(a_on_top ? 1 : 0, bank_b,
                           dual ? s_toggle_regs.bright_b : KH_IO16(0x0400106c));
    }
    if (kh_log_verbose && dual) {
        /* dual 3D, frame by frame: what each screen gets (the first 240 frames of the run) */
        static int traced;
        if (traced++ < 600) {
            LOG("dual: vb %u serial %u (toggled at %u) polys %d top %d->%d waited %uus cap %08x "
                "showA %d showB %d 2d %d/%d a3d %d tex %u dispcnt %08x %08x", (unsigned)s_vblanks, (unsigned)kh_gx3d_serial(),
                (unsigned)s_toggle_serial, trace_polys, trace_before,
                a_on_top, (unsigned)trace_wait, (unsigned)KH_IO32(0x04000064),
                video_shown_bank(a_on_top ? 0 : 1), video_shown_bank(a_on_top ? 1 : 0),
                draw2d, f2d.neng, a3d, tex3d, (unsigned)s_toggle_regs.dispcnt_a,
                (unsigned)s_toggle_regs.dispcnt_b);
        }
    }
    if (s_dump_2d) {
        s_dump_2d = 0;
        dump_2d(a_on_top);
    }
    update_panels(video_single_screen(), upload2d, s_bottom);
    s_stage = "present";
    {
        uint64_t t = sceKernelGetProcessTimeWide();
        /* unchanged screens are not uploaded again */
        video_present(upload2d ? s_top : NULL, upload2d ? s_bottom : NULL);
        if (dual)
            s_toggle_done = toggle_seq;
        t = sceKernelGetProcessTimeWide() - t;
        s_present_total += t;
        stage_max(&s_present_max, t);
        s_cur.present = (uint32_t)t;
    }
    {
        /* a display frame over 20 ms (a VBlank missed): its stages, to find the hitches */
        const uint32_t total = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
        static uint32_t logged;
        if (kh_log_verbose && total > 20000 && logged < 400) {
            logged++;
            uint32_t up, sw;
            video_present_times(&up, &sw);
            LOG("slow frame: %uus at vb %u: textures %uus, 3d submit %uus, 2d wait %uus, "
                "present %uus (uploads %uus, swap %uus)%s", (unsigned)total, (unsigned)s_vblanks,
                (unsigned)s_cur.prep, (unsigned)s_cur.t3d, (unsigned)s_cur.join,
                (unsigned)s_cur.present, (unsigned)up, (unsigned)sw, dual ? " (dual 3D)" : "");
        }
        memset(&s_cur, 0, sizeof(s_cur));
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
        /* dual 3D: the VBlank brings the next frame's VRAM and sprites (the game's transfers
         * run in its handler), so it waits for the display to have drawn the 2D of the last
         * toggle (25 ms at most); before, a screen was now and then drawn with half of the
         * next frame (0.0.91) */
        if (s_vblanks - s_toggle_vb < 8) {
            const uint64_t until = sceKernelGetProcessTimeWide() + 25000;
            while (s_toggle_drawn != s_toggle_seq && sceKernelGetProcessTimeWide() < until)
                sceKernelDelayThread(100);
        }
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

/* At most one 3D frame per VBlank, as on the DS, whose geometry engine stalls after a swap
 * until the next VBlank. Some scenes rely on it: the dual-3D cutscenes' loop does not wait for
 * the VBlank itself and swapped two or three times a frame here, half of them empty frames,
 * which flickered (0.0.81: 1252 swaps in 10 s). */
static void swap_wait(void)
{
    static uint32_t last;
    volatile uint32_t *count = (volatile uint32_t *)KH_SHARED(HW_VBLANK_COUNT_BUF);
    const uint64_t give_up = sceKernelGetProcessTimeWide() + 50000;
    while (*count == last && sceKernelGetProcessTimeWide() < give_up)
        sceKernelDelayThread(200);
    last = *count;
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
    kh_gx3d_swap_wait = swap_wait;
    portmenu_on_scale = kh_gpu3d_set_scale;
    portmenu_on_texture_filter = kh_gpu3d_reload_textures;
    portmenu_on_volume = set_volume;
    set_volume(kh_config.volume);

    /* the display keeps core 0 whatever the game does; the game runs on core 1 */
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, KH_DISPLAY_PRIORITY);
    th = sceKernelCreateThread("kh_game", game_thread, KH_COMPUTE_PRIORITY, GAME_STACK_SIZE, 0,
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
                    {
                        uint32_t ga, gm;
                        video_take_gpu_probe(&ga, &gm);
                        LOG("display: gpu time of one frame drawn alone (1 a second): avg %uus, "
                            "worst %uus", (unsigned)ga, (unsigned)gm);
                    }
                    LOG("display: 2d cpu per frame: engine A %uus, B %uus",
                        (unsigned)(s_2d_engine_us[0] / 600), (unsigned)(s_2d_engine_us[1] / 600));
                    s_2d_engine_us[0] = s_2d_engine_us[1] = 0;
                    if (kh_gpu2d_profiling) {
                        uint32_t us[2][3], spr[2][3];
                        int k;
                        kh_gpu2d_take_profile(us, spr);
                        for (k = 0; k < 2; k++)
                            LOG("display: 2d %c per frame: BGs %uus, sprites %uus, the rest %uus; "
                                "sprite lines %u tiles, %u bitmap, %u affine", "AB"[k],
                                (unsigned)(us[k][0] / 600), (unsigned)(us[k][1] / 600),
                                (unsigned)((us[k][2] - us[k][0] - us[k][1]) / 600),
                                (unsigned)(spr[k][0] / 600), (unsigned)(spr[k][1] / 600),
                                (unsigned)(spr[k][2] / 600));
                    }
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
