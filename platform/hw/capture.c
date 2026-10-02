/* The display capture (DISPCAPCNT, 0x04000064): engine A's picture, or a blend of it with a VRAM
 * bank, written into a VRAM bank in LCDC. The game uses it for its screen blends: a dialogue
 * captures the field into bank D, shows bank D (DISPCNT display mode 2) and blends each new
 * frame over the last capture there.
 *
 * On the DS the capture happens while the frame is scanned out, and the enable bit clears when
 * it is done. Here it runs once per displayed frame from the display loop, on the CPU: engine
 * A's graphics screen and the 3D layer read back from the GPU are combined as the composition
 * shader does (platform/core/video.c), without master brightness, as the DS captures it. */
#include "hw/capture.h"

#include "hw/gpu2d.h"
#include "hw/gpu3d.h"
#include "hw/io.h"
#include "hw/memmap.h"
#include "hw/vram.h"

#include <string.h>

static uint32_t s_gfx[256 * 192], s_3d[256 * 192];
static uint32_t s_captures;

static inline uint16_t bgr555(float r, float g, float b)
{
    const int ri = (int)(r * 31.0f + 0.5f), gi = (int)(g * 31.0f + 0.5f), bi = (int)(b * 31.0f + 0.5f);
    return (uint16_t)((ri > 31 ? 31 : ri) | (gi > 31 ? 31 : gi) << 5 | (bi > 31 ? 31 : bi) << 10);
}

/* one pixel of source A, the graphics screen: what the composition shader would show */
static uint16_t graphics_pixel(uint32_t b2d, const uint32_t *row3d, int x, int hofs, float eva_r,
                               float evb_r, const float bd[3])
{
    const int code = (int)(b2d >> 24);
    float br = (float)(b2d & 0xff) / 255.0f, bg = (float)((b2d >> 8) & 0xff) / 255.0f,
          bb = (float)((b2d >> 16) & 0xff) / 255.0f;
    float tr = 0, tg = 0, tb = 0, ta = 0;
    const int u = x + hofs;

    if (code == 0xff)
        return bgr555(br, bg, bb);
    if (row3d && u >= 0 && u < 256) {
        const uint32_t t = row3d[u];
        tr = (float)(t & 0xff) / 255.0f, tg = (float)((t >> 8) & 0xff) / 255.0f;
        tb = (float)((t >> 16) & 0xff) / 255.0f, ta = (float)(t >> 24) / 255.0f;
    }
    if (code >= 192) {
        float ea = eva_r, eb = evb_r;
        if (code < 224)
            ea = (float)(code - 192) / 16.0f, eb = 1.0f - ea;
        return bgr555(br * ea + (tr + bd[0] * (1 - ta)) * eb, bg * ea + (tg + bd[1] * (1 - ta)) * eb,
                      bb * ea + (tb + bd[2] * (1 - ta)) * eb);
    }
    if (code >= 128) {
        const float f = 1.0f - (float)(code - 128) / 16.0f;
        tr *= f, tg *= f, tb *= f;
    } else if (code >= 64) {
        const float f = (float)(code - 64) / 16.0f;
        tr += (ta - tr) * f, tg += (ta - tg) * f, tb += (ta - tb) * f;
    }
    return bgr555(tr + br * (1 - ta), tg + bg * (1 - ta), tb + bb * (1 - ta));
}

int kh_capture_run(void)
{
    static const uint8_t sizes[4][2] = { { 128, 128 }, { 0, 64 }, { 0, 128 }, { 0, 192 } }; /* 0: 256 */
    const uint32_t cnt = KH_IO32(0x04000064);
    const uint32_t dispcnt = KH_IO32(0x04000000);
    const int w = sizes[(cnt >> 20) & 3][0] ? sizes[(cnt >> 20) & 3][0] : 256;
    const int h = sizes[(cnt >> 20) & 3][1];
    const int mode = (cnt >> 29) & 3, src_a_3d = (cnt >> 24) & 1, src_b_fifo = (cnt >> 25) & 1;
    int eva = cnt & 31, evb = (cnt >> 8) & 31;
    uint8_t *const dst_bank = kh_vram_bank_home((int)((cnt >> 16) & 3));
    const uint8_t *const src_bank = kh_vram_bank_home((int)((dispcnt >> 18) & 3));
    const uint32_t dst_off = ((cnt >> 18) & 3) * 0x8000u;
    const uint32_t src_off = src_b_fifo ? 0 : ((cnt >> 26) & 3) * 0x8000u;
    int have3d = 0, x, y;

    if (!(cnt & 0x80000000u))
        return 0;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;

    if (mode != 1) {
        /* source A: the 3D layer alone, or the whole graphics screen */
        have3d = kh_gpu3d_read_layer(s_3d);
        if (!src_a_3d)
            kh_gpu2d_render_graphics(KH_ENGINE_A, s_gfx);
    }
    {
        const uint16_t bldalpha = KH_IO16(0x04000052);
        const int hofs = (int)((int16_t)(KH_IO16(0x04000010) << 7) >> 7);
        const uint16_t bd16 = (uint16_t)(kh_ds_palette[0] | kh_ds_palette[1] << 8);
        const float bd[3] = { (float)(bd16 & 31) / 31.0f, (float)((bd16 >> 5) & 31) / 31.0f,
                              (float)((bd16 >> 10) & 31) / 31.0f };
        float ea_r = (float)(bldalpha & 31), eb_r = (float)((bldalpha >> 8) & 31);
        ea_r = (ea_r > 16 ? 16 : ea_r) / 16.0f;
        eb_r = (eb_r > 16 ? 16 : eb_r) / 16.0f;

        for (y = 0; y < h; y++) {
            const uint32_t *row3d = have3d ? s_3d + y * 256 : NULL;
            for (x = 0; x < w; x++) {
                uint16_t a = 0, b = 0, out;
                if (mode != 1) {
                    if (src_a_3d) {
                        const uint32_t t = row3d ? row3d[x] : 0;
                        const float ta = (float)(t >> 24) / 255.0f;
                        a = ta > 0 ? (uint16_t)(bgr555((float)(t & 0xff) / 255.0f / ta,
                                                       (float)((t >> 8) & 0xff) / 255.0f / ta,
                                                       (float)((t >> 16) & 0xff) / 255.0f / ta) | 0x8000)
                                   : 0;
                    } else {
                        a = graphics_pixel(s_gfx[y * 256 + x], row3d, x, hofs, ea_r, eb_r, bd) | 0x8000;
                    }
                }
                if (mode != 0 && !src_b_fifo) {
                    const uint32_t o = (src_off + (uint32_t)(y * w + x) * 2) & 0x1ffff;
                    b = (uint16_t)(src_bank[o] | src_bank[o + 1] << 8);
                }
                if (mode == 0) {
                    out = a;
                } else if (mode == 1) {
                    out = b;
                } else {
                    /* (A * alpha A * EVA + B * alpha B * EVB) / 16, per channel */
                    const int aa = (a >> 15) ? eva : 0, bb = (b >> 15) ? evb : 0;
                    int r = ((a & 31) * aa + (b & 31) * bb) >> 4;
                    int g = (((a >> 5) & 31) * aa + ((b >> 5) & 31) * bb) >> 4;
                    int bl = (((a >> 10) & 31) * aa + ((b >> 10) & 31) * bb) >> 4;
                    out = (uint16_t)((r > 31 ? 31 : r) | (g > 31 ? 31 : g) << 5 | (bl > 31 ? 31 : bl) << 10);
                    if ((aa && (a >> 15)) || (bb && (b >> 15)))
                        out |= 0x8000;
                }
                {
                    const uint32_t o = (dst_off + (uint32_t)(y * w + x) * 2) & 0x1ffff;
                    dst_bank[o] = (uint8_t)out;
                    dst_bank[o + 1] = (uint8_t)(out >> 8);
                }
            }
        }
    }
    kh_vram_touch();
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
