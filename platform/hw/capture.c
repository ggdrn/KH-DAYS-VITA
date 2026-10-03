/* The display capture (DISPCAPCNT, 0x04000064): engine A's picture, or a blend of it with a VRAM
 * bank, written into a VRAM bank in LCDC. The game uses it for its screen blends: a dialogue
 * captures the field into bank D, shows bank D (DISPCNT display mode 2) and blends each new
 * frame over the last capture there.
 *
 * The capture is done on the GPU (platform/core/video.c), where the 3D layer is: reading that
 * back to the CPU stalled vitaGL for seconds (0.0.63). The result stays a GPU texture, shown
 * while engine A displays the bank it went to and the CPU has not written that bank since; the
 * bank's own bytes are not updated. Source B is the last capture when it reads that bank, else
 * the bank's bytes. */
#include "hw/capture.h"

#include "hw/gpu2d.h"
#include "hw/gpu3d.h"
#include "hw/io.h"
#include "hw/memmap.h"
#include "hw/vram.h"
#include "log.h"
#include "video.h"

#include <string.h>

static uint32_t s_gfx[256 * 192], s_srcb[256 * 192];
static uint32_t s_captures;
/* per VRAM bank A-D: a capture is held on the GPU for it, and the bank's bytes then */
static int s_valid[4];
static uint32_t s_hash[4];

/* the bank's first 256x192 pixels, to see whether the CPU wrote it after the capture */
static uint32_t bank_hash(int bank)
{
    const uint32_t *p = (const uint32_t *)kh_vram_bank_home(bank);
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < 256 * 192 / 2; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

int kh_capture_shown(int bank)
{
    return bank >= 0 && bank < 4 && s_valid[bank] && bank_hash(bank) == s_hash[bank];
}

int kh_capture_run(unsigned tex3d)
{
    const uint32_t cnt = KH_IO32(0x04000064);
    const uint32_t dispcnt = KH_IO32(0x04000000);
    const int mode = (cnt >> 29) & 3, src_a_3d = (cnt >> 24) & 1, src_b_fifo = (cnt >> 25) & 1;
    const int dest = (int)((cnt >> 16) & 3), bank_b = (int)((dispcnt >> 18) & 3);
    int eva = cnt & 31, evb = (cnt >> 8) & 31;
    float ka, kb;
    const uint32_t *srcb = NULL;

    if (!(cnt & 0x80000000u))
        return 0;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    ka = mode == 0 ? 1.0f : mode == 1 ? 0.0f : (float)eva / 16.0f;
    kb = mode == 0 ? 0.0f : mode == 1 ? 1.0f : (float)evb / 16.0f;
    if (mode != 1 && !src_a_3d)
        kh_gpu2d_render_graphics(KH_ENGINE_A, s_gfx);
    if (kb > 0 && !src_b_fifo && !kh_capture_shown(bank_b)) {
        /* source B from the bank's bytes, bottom row first like the GPU targets */
        const uint8_t *src = kh_vram_bank_home(bank_b);
        const uint32_t off = ((cnt >> 26) & 3) * 0x8000u;
        int x, y;
        for (y = 0; y < 192; y++)
            for (x = 0; x < 256; x++) {
                const uint32_t o = (off + (uint32_t)(y * 256 + x) * 2) & 0x1ffff;
                const uint16_t c = (uint16_t)(src[o] | src[o + 1] << 8);
                uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
                s_srcb[(191 - y) * 256 + x] = (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 |
                                              (b << 3 | b >> 2) << 16 | 0xff000000u;
            }
        srcb = s_srcb;
    } else if (kb > 0 && src_b_fifo) {
        memset(s_srcb, 0, sizeof(s_srcb));
        srcb = s_srcb;
    }
    video_capture(mode != 1 && !src_a_3d ? s_gfx : NULL, src_a_3d && mode != 1, tex3d, ka, kb, srcb,
                  kb > 0 && !srcb ? bank_b : -1, dest);
    if (kh_log_verbose) {
        static uint32_t logged;
        if (logged++ < 60)
            LOG("capture: DISPCAPCNT %08x DISPCNT %08x VRAMCNT %08x, bank B %s", (unsigned)cnt,
                (unsigned)dispcnt, (unsigned)KH_IO32(0x04000240),
                kb > 0 ? (srcb ? "from its bytes" : "the last capture") : "unused");
    }
    s_valid[dest] = 1;
    s_hash[dest] = bank_hash(dest);
    s_captures++;
    /* done: the enable bit clears, as the hardware's does at the end of the frame */
    __atomic_and_fetch((volatile uint32_t *)&KH_IO32(0x04000064), ~0x80000000u, __ATOMIC_SEQ_CST);
    return 1;
}

uint32_t kh_capture_take_count(void)
{
    const uint32_t n = s_captures;
    s_captures = 0;
    return n;
}
