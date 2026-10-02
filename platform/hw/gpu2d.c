/* The DS 2D display engines A and B, rendered a frame at a time from the state the port keeps
 * for them: the engine's I/O page (DISPCNT, BGxCNT, scroll, affine, windows, blending, master
 * brightness), palette RAM, OAM, the VRAM views the game writes through (platform/compat/
 * kh_hw_map.h) and the extended-palette banks, which stay at their home (hw/vram.c).
 *
 * Each scanline is built as separate layers (BG0-BG3, OBJ), then composed per pixel by
 * priority, cut by the windows, and put through the colour effect and master brightness, as
 * GBATEK describes the hardware. Registers are read once per frame: mid-frame (HBlank)
 * changes and mosaic are not reproduced yet.
 *
 * Engine A's BG0 as the 3D layer is drawn on the GPU (hw/gpu3d.c); here it is a layer opaque
 * everywhere. A pixel whose frontmost layer is the 3D one comes out as the colour under the 3D
 * layer, with alpha carrying a code instead of 255 (gpu2d.h), for the GPU composition. */
#include "hw/gpu2d.h"

#include "hw/io.h"
#include "hw/memmap.h"
#include "hw/vram.h"

#include <string.h>

#define W KH_GPU2D_W
#define H KH_GPU2D_H

#define OPAQUE 0x8000u /* in a layer pixel: bits 0-14 are a BGR555 colour */

enum { L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_BD };

typedef struct {
    int is_a;
    const uint8_t *io; /* the engine's register page */
    const uint16_t *bg_pal, *obj_pal;
    const uint8_t *bg_vram, *obj_vram;
    uint32_t bg_mask, obj_mask;
    const uint16_t *oam;
    const uint16_t *bg_ext[4]; /* extended palette slots (8 KiB each), NULL when unmapped */
    const uint16_t *obj_ext;
    uint32_t dispcnt;
    int has3d;
} Engine;

typedef struct {
    uint16_t col[W];
    uint8_t prio[W];
    uint8_t alpha[W]; /* 0: opaque; 1-16: bitmap OBJ alpha + 1; 0x80: semi-transparent */
    uint8_t win[W];   /* OBJ window mask */
} ObjLine;

static inline uint16_t io16(const Engine *e, uint32_t off)
{
    uint16_t v;
    memcpy(&v, e->io + off, 2);
    return v;
}

static inline uint32_t io32(const Engine *e, uint32_t off)
{
    uint32_t v;
    memcpy(&v, e->io + off, 4);
    return v;
}

static inline uint16_t rd16(const uint8_t *base, uint32_t off, uint32_t mask)
{
    uint16_t v;
    memcpy(&v, base + (off & mask & ~1u), 2);
    return v;
}

/* ---- setup ---------------------------------------------------------------------------- */

static int bank_is(int bank, int mst)
{
    uint8_t cnt = kh_vram_bank_cnt(bank);
    return (cnt & 0x80) && (cnt & 7) == mst;
}

static void engine_setup(Engine *e, int engine)
{
    int i;
    memset(e, 0, sizeof(*e));
    e->is_a = engine == KH_ENGINE_A;
    e->io = kh_ds_io + (e->is_a ? 0x0000 : 0x1000);
    e->bg_pal = (const uint16_t *)(kh_ds_palette + (e->is_a ? 0x000 : 0x400));
    e->obj_pal = (const uint16_t *)(kh_ds_palette + (e->is_a ? 0x200 : 0x600));
    e->oam = (const uint16_t *)(kh_ds_oam + (e->is_a ? 0x000 : 0x400));
    if (e->is_a) {
        e->bg_vram = kh_vram_bg_a, e->bg_mask = sizeof(kh_vram_bg_a) - 1;
        e->obj_vram = kh_vram_obj_a, e->obj_mask = sizeof(kh_vram_obj_a) - 1;
        /* E: BG slots 0-3; F, G: two slots each (OFS bit 0 picks 0-1 or 2-3), or OBJ */
        if (bank_is(4, 4))
            for (i = 0; i < 4; i++)
                e->bg_ext[i] = (const uint16_t *)(kh_vram_bank_home(4) + 0x2000 * i);
        for (i = 5; i <= 6; i++) {
            if (bank_is(i, 4)) {
                int s = ((kh_vram_bank_cnt(i) >> 3) & 1) * 2;
                e->bg_ext[s] = (const uint16_t *)kh_vram_bank_home(i);
                e->bg_ext[s + 1] = (const uint16_t *)(kh_vram_bank_home(i) + 0x2000);
            } else if (bank_is(i, 5)) {
                e->obj_ext = (const uint16_t *)kh_vram_bank_home(i);
            }
        }
    } else {
        e->bg_vram = kh_vram_bg_b, e->bg_mask = sizeof(kh_vram_bg_b) - 1;
        e->obj_vram = kh_vram_obj_b, e->obj_mask = sizeof(kh_vram_obj_b) - 1;
        if (bank_is(7, 2)) /* H: BG slots 0-3 */
            for (i = 0; i < 4; i++)
                e->bg_ext[i] = (const uint16_t *)(kh_vram_bank_home(7) + 0x2000 * i);
        if (bank_is(8, 3)) /* I: OBJ */
            e->obj_ext = (const uint16_t *)kh_vram_bank_home(8);
    }
    e->dispcnt = io32(e, 0x00);
    e->has3d = e->is_a && (e->dispcnt & 0x108) == 0x108 && (e->dispcnt & 7) != 7;
}

/* ---- backgrounds ---------------------------------------------------------------------- */

static uint32_t char_base(const Engine *e, uint16_t cnt)
{
    return ((cnt >> 2) & 15) * 0x4000u + (e->is_a ? ((e->dispcnt >> 24) & 7) * 0x10000u : 0);
}

static uint32_t screen_base(const Engine *e, uint16_t cnt)
{
    return ((cnt >> 8) & 31) * 0x800u + (e->is_a ? ((e->dispcnt >> 27) & 7) * 0x10000u : 0);
}

/* the extended palette an 8bpp BG uses, or NULL for the standard one */
static const uint16_t *bg_ext_pal(const Engine *e, int bg, uint16_t cnt)
{
    int slot = bg;
    if (!(e->dispcnt & (1u << 30)))
        return NULL;
    if (bg < 2 && (cnt & 0x2000))
        slot += 2;
    return e->bg_ext[slot];
}

static void bg_text(const Engine *e, int bg, int line, uint16_t *out)
{
    const uint16_t cnt = io16(e, 0x08 + 2 * bg);
    const uint32_t cb = char_base(e, cnt), sb = screen_base(e, cnt);
    const int w = (cnt & 0x4000) ? 512 : 256, h = (cnt & 0x8000) ? 512 : 256;
    const int hofs = io16(e, 0x10 + 4 * bg) & 0x1ff, vofs = io16(e, 0x12 + 4 * bg) & 0x1ff;
    const int y = (line + vofs) & (h - 1);
    const int bpp8 = cnt & 0x80;
    const uint16_t *ext = bpp8 ? bg_ext_pal(e, bg, cnt) : NULL;
    const uint32_t row = sb + (uint32_t)((y >> 8) * (w >> 8)) * 0x800u + ((y >> 3) & 31) * 64u;
    const uint8_t *v = e->bg_vram;
    const uint32_t m = e->bg_mask;
    int x = 0;

    while (x < W) {
        const int sx = (x + hofs) & (w - 1);
        const uint16_t se = rd16(v, row + (uint32_t)(sx >> 8) * 0x800u + ((sx >> 3) & 31) * 2u, m);
        const int tile = se & 0x3ff, pal = se >> 12, hflip = se & 0x400;
        const int ty = (se & 0x800) ? 7 - (y & 7) : (y & 7);
        int px = sx & 7;
        /* the tile's row of 8 texels at once; an empty row (common) is skipped whole */
        if (bpp8) {
            uint32_t lo, hi;
            const uint32_t a = (cb + tile * 64u + ty * 8u) & m;
            const uint16_t *pp = ext ? ext + pal * 256 : e->bg_pal;
            memcpy(&lo, v + a, 4);
            memcpy(&hi, v + ((a + 4) & m), 4);
            if (!(lo | hi)) {
                x += 8 - px;
                continue;
            }
            for (; px < 8 && x < W; px++, x++) {
                const int tx = hflip ? 7 - px : px;
                const int idx = ((tx < 4 ? lo >> (tx * 8) : hi >> ((tx - 4) * 8))) & 0xff;
                if (idx)
                    out[x] = pp[idx] | OPAQUE;
            }
        } else {
            uint32_t bits;
            const uint16_t *pp = e->bg_pal + pal * 16;
            memcpy(&bits, v + ((cb + tile * 32u + ty * 4u) & m), 4);
            if (!bits) {
                x += 8 - px;
                continue;
            }
            for (; px < 8 && x < W; px++, x++) {
                const int tx = hflip ? 7 - px : px;
                const int idx = (bits >> (tx * 4)) & 15;
                if (idx)
                    out[x] = pp[idx] | OPAQUE;
            }
        }
    }
}

typedef struct {
    int32_t x, y;   /* texture position of screen x = 0 on this line, 20.8 fixed */
    int16_t pa, pc; /* step per screen pixel */
} Affine;

static Affine bg_affine(const Engine *e, int bg, int line)
{
    const uint32_t base = bg == 2 ? 0x20 : 0x30;
    Affine a;
    const int32_t x0 = (int32_t)(io32(e, base + 8) << 4) >> 4;
    const int32_t y0 = (int32_t)(io32(e, base + 12) << 4) >> 4;
    a.pa = (int16_t)io16(e, base + 0);
    a.pc = (int16_t)io16(e, base + 4);
    a.x = x0 + (int16_t)io16(e, base + 2) * line;
    a.y = y0 + (int16_t)io16(e, base + 6) * line;
    return a;
}

/* rotation/scaling BG with an 8-bit map of 8bpp tiles (BG modes 1, 2, 4) */
static void bg_affine_tiles(const Engine *e, int bg, int line, uint16_t *out)
{
    const uint16_t cnt = io16(e, 0x08 + 2 * bg);
    const uint32_t cb = char_base(e, cnt), sb = screen_base(e, cnt);
    const int size = 128 << (cnt >> 14), wrap = cnt & 0x2000;
    const uint8_t *v = e->bg_vram;
    const uint32_t m = e->bg_mask;
    Affine a = bg_affine(e, bg, line);
    int x;

    for (x = 0; x < W; x++, a.x += a.pa, a.y += a.pc) {
        int tx = a.x >> 8, ty = a.y >> 8, idx;
        if (wrap)
            tx &= size - 1, ty &= size - 1;
        else if (tx < 0 || ty < 0 || tx >= size || ty >= size)
            continue;
        idx = v[(cb + v[(sb + (uint32_t)(ty >> 3) * (size >> 3) + (tx >> 3)) & m] * 64u +
                 (ty & 7) * 8u + (tx & 7)) & m];
        if (idx)
            out[x] = e->bg_pal[idx] | OPAQUE;
    }
}

/* extended BG (BG modes 3-5): a 16-bit map of 8bpp tiles, a 256-colour or a direct-colour
 * bitmap, picked by BGxCNT bits 7 and 2 */
static void bg_extended(const Engine *e, int bg, int line, uint16_t *out)
{
    static const int bw[4] = { 128, 256, 512, 512 }, bh[4] = { 128, 256, 256, 512 };
    const uint16_t cnt = io16(e, 0x08 + 2 * bg);
    const int wrap = cnt & 0x2000;
    const uint8_t *v = e->bg_vram;
    const uint32_t m = e->bg_mask;
    Affine a = bg_affine(e, bg, line);
    int x;

    if (!(cnt & 0x80)) {
        const uint32_t cb = char_base(e, cnt), sb = screen_base(e, cnt);
        const int size = 128 << (cnt >> 14);
        const uint16_t *ext = bg_ext_pal(e, bg, cnt);
        for (x = 0; x < W; x++, a.x += a.pa, a.y += a.pc) {
            int tx = a.x >> 8, ty = a.y >> 8, idx;
            uint16_t se;
            if (wrap)
                tx &= size - 1, ty &= size - 1;
            else if (tx < 0 || ty < 0 || tx >= size || ty >= size)
                continue;
            se = rd16(v, sb + ((uint32_t)(ty >> 3) * (size >> 3) + (tx >> 3)) * 2u, m);
            if (se & 0x400)
                tx = 7 - (tx & 7);
            if (se & 0x800)
                ty = 7 - (ty & 7);
            idx = v[(cb + (se & 0x3ff) * 64u + (ty & 7) * 8u + (tx & 7)) & m];
            if (idx)
                out[x] = (ext ? ext[(se >> 12) * 256 + idx] : e->bg_pal[idx]) | OPAQUE;
        }
    } else {
        const int sz = cnt >> 14, w = bw[sz], h = bh[sz], direct = cnt & 0x04;
        const uint32_t base = ((cnt >> 8) & 31) * 0x4000u;
        for (x = 0; x < W; x++, a.x += a.pa, a.y += a.pc) {
            int tx = a.x >> 8, ty = a.y >> 8;
            if (wrap)
                tx &= w - 1, ty &= h - 1;
            else if (tx < 0 || ty < 0 || tx >= w || ty >= h)
                continue;
            if (direct) {
                const uint16_t c = rd16(v, base + ((uint32_t)ty * w + tx) * 2u, m);
                if (c & 0x8000)
                    out[x] = c;
            } else {
                const int idx = v[(base + (uint32_t)ty * w + tx) & m];
                if (idx)
                    out[x] = e->bg_pal[idx] | OPAQUE;
            }
        }
    }
}

/* BG kind per mode: 0 text, 1 affine, 2 extended, -1 none (the large bitmap of mode 6 is not
 * drawn) */
static const int8_t s_bg_kind[8][4] = {
    { 0, 0, 0, 0 }, { 0, 0, 0, 1 }, { 0, 0, 1, 1 }, { 0, 0, 0, 2 },
    { 0, 0, 1, 2 }, { 0, 0, 2, 2 }, { 0, -1, -1, -1 }, { -1, -1, -1, -1 },
};

/* Draws BG `bg` into out; 0 when the layer is not shown (out is then left as it was). */
static int render_bg(const Engine *e, int bg, int line, uint16_t *out)
{
    const int kind = s_bg_kind[e->dispcnt & 7][bg];
    if (!(e->dispcnt & (0x100u << bg)) || kind < 0)
        return 0;
    if (bg == 0 && e->has3d) {
        int x;
        for (x = 0; x < W; x++)
            out[x] = OPAQUE; /* the 3D layer: opaque here, its pixels are on the GPU */
        return 1;
    }
    memset(out, 0, W * sizeof(*out));
    switch (kind) {
    case 0: bg_text(e, bg, line, out); break;
    case 1: bg_affine_tiles(e, bg, line, out); break;
    default: bg_extended(e, bg, line, out); break;
    }
    return 1;
}

/* ---- sprites -------------------------------------------------------------------------- */

static const uint8_t s_obj_w[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const uint8_t s_obj_h[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

/* colour of texel (tx, ty) of a sprite; 0 when transparent */
static uint16_t obj_texel(const Engine *e, uint16_t a0, uint16_t a2, int w, int tx, int ty)
{
    const uint8_t *v = e->obj_vram;
    const uint32_t m = e->obj_mask;
    const int tile = a2 & 0x3ff;

    if (((a0 >> 10) & 3) == 3) { /* bitmap OBJ: direct colour */
        uint32_t addr;
        uint16_t c;
        if (e->dispcnt & 0x40)
            addr = tile * ((e->dispcnt & (1u << 22)) ? 256u : 128u) + ((uint32_t)ty * w + tx) * 2u;
        else if (e->dispcnt & 0x20)
            addr = (tile & 0x1f) * 0x10u + (tile & ~0x1f) * 0x80u + ((uint32_t)ty * 256 + tx) * 2u;
        else
            addr = (tile & 0x0f) * 0x10u + (tile & ~0x0f) * 0x80u + ((uint32_t)ty * 128 + tx) * 2u;
        c = rd16(v, addr, m);
        return (c & 0x8000) ? c : 0;
    } else {
        const int bpp8 = a0 & 0x2000;
        uint32_t addr;
        int idx;
        if (e->dispcnt & 0x10) /* 1D mapping */
            addr = tile * (32u << ((e->dispcnt >> 20) & 3)) +
                   ((uint32_t)(ty >> 3) * (w >> 3) + (tx >> 3)) * (bpp8 ? 64u : 32u);
        else /* 2D: a 32x32 grid of 32-byte tiles */
            addr = tile * 32u + ((uint32_t)(ty >> 3) * 32u + (tx >> 3) * (bpp8 ? 2u : 1u)) * 32u;
        if (bpp8) {
            idx = v[(addr + (ty & 7) * 8u + (tx & 7)) & m];
            if (!idx)
                return 0;
            if ((e->dispcnt & (1u << 31)) && e->obj_ext)
                return e->obj_ext[(a2 >> 12) * 256 + idx] | OPAQUE;
            return e->obj_pal[idx] | OPAQUE;
        }
        idx = v[(addr + (ty & 7) * 4u + ((tx & 7) >> 1)) & m];
        idx = (tx & 1) ? idx >> 4 : idx & 15;
        return idx ? e->obj_pal[(a2 >> 12) * 16 + idx] | OPAQUE : 0;
    }
}

/* Draws the sprites of `line`; returns 1 if any sprite pixel was drawn, plus 2 if one of them
 * blends by itself (semi-transparent or bitmap), plus 4 if any OBJ window pixel was set. */
/* One sprite pixel into the line: window, priority against what is there, alpha. */
static inline int obj_put(ObjLine *o, int x, uint16_t c, int mode, int prio, uint16_t a2)
{
    if (mode == 2) {
        o->win[x] = 1;
        return 4;
    }
    if ((o->col[x] & OPAQUE) && prio >= o->prio[x])
        return 0; /* a sprite earlier in OAM, or with a lower priority value, wins */
    if (mode == 3) {
        const int alpha = a2 >> 12;
        if (!alpha)
            return 0;
        o->alpha[x] = (uint8_t)(alpha + 1);
    } else {
        o->alpha[x] = mode == 1 ? 0x80 : 0;
    }
    o->col[x] = c | OPAQUE;
    o->prio[x] = (uint8_t)prio;
    return o->alpha[x] ? 3 : 1;
}

/* A plain (not affine, not bitmap) sprite's line, a tile row of 8 texels at a time; empty
 * rows are skipped whole. */
static int obj_line_tiles(const Engine *e, ObjLine *o, uint16_t a0, uint16_t a1, uint16_t a2,
                          int w, int h, int dy, int x0)
{
    const uint8_t *v = e->obj_vram;
    const uint32_t m = e->obj_mask;
    const int tile = a2 & 0x3ff, bpp8 = a0 & 0x2000, mode = (a0 >> 10) & 3;
    const int prio = (a2 >> 10) & 3, hflip = a1 & 0x1000;
    const int ty = (a1 & 0x2000) ? h - 1 - dy : dy;
    const uint16_t *pal;
    int col, got = 0;

    if (bpp8)
        pal = ((e->dispcnt & (1u << 31)) && e->obj_ext) ? e->obj_ext + (a2 >> 12) * 256 : e->obj_pal;
    else
        pal = e->obj_pal + (a2 >> 12) * 16;
    for (col = 0; col < w / 8; col++) {
        /* screen x of this tile column's leftmost pixel */
        const int xs = x0 + (hflip ? w - 8 - col * 8 : col * 8);
        uint32_t addr, lo, hi = 0;
        int px;
        if (xs >= W || xs + 8 <= 0)
            continue;
        if (e->dispcnt & 0x10) /* 1D mapping */
            addr = tile * (32u << ((e->dispcnt >> 20) & 3)) +
                   ((uint32_t)(ty >> 3) * (w >> 3) + col) * (bpp8 ? 64u : 32u);
        else /* 2D: a 32x32 grid of 32-byte tiles */
            addr = tile * 32u + ((uint32_t)(ty >> 3) * 32u + col * (bpp8 ? 2u : 1u)) * 32u;
        if (bpp8) {
            addr += (ty & 7) * 8u;
            memcpy(&lo, v + (addr & m), 4);
            memcpy(&hi, v + ((addr + 4) & m), 4);
        } else {
            memcpy(&lo, v + ((addr + (ty & 7) * 4u) & m), 4);
        }
        if (!(lo | hi))
            continue;
        for (px = 0; px < 8; px++) {
            const int t = hflip ? 7 - px : px, x = xs + px;
            int idx;
            if (x < 0 || x >= W)
                continue;
            if (bpp8)
                idx = (t < 4 ? lo >> (t * 8) : hi >> ((t - 4) * 8)) & 0xff;
            else
                idx = (lo >> (t * 4)) & 15;
            if (idx)
                got |= obj_put(o, x, pal[idx], mode, prio, a2);
        }
    }
    return got;
}

static int render_obj(const Engine *e, int line, ObjLine *o)
{
    int i, got = 0;
    memset(o->col, 0, sizeof(o->col));
    memset(o->win, 0, sizeof(o->win));
    if (!(e->dispcnt & 0x1000))
        return 0;
    for (i = 0; i < 128; i++) {
        const uint16_t a0 = e->oam[i * 4], a1 = e->oam[i * 4 + 1], a2 = e->oam[i * 4 + 2];
        const int affine = a0 & 0x100, mode = (a0 >> 10) & 3, shape = a0 >> 14;
        const int prio = (a2 >> 10) & 3;
        int w, h, bw, bh, dy, x0, lx;
        int16_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;

        if ((!affine && (a0 & 0x200)) || shape == 3)
            continue; /* hidden, or the prohibited shape */
        w = s_obj_w[shape][a1 >> 14], h = s_obj_h[shape][a1 >> 14];
        bw = w, bh = h;
        if (affine && (a0 & 0x200))
            bw *= 2, bh *= 2; /* double size */
        dy = (line - (a0 & 0xff)) & 0xff;
        if (dy >= bh)
            continue;
        x0 = a1 & 0x1ff;
        if (x0 >= 256)
            x0 -= 512;
        if (!affine && mode != 3) {
            got |= obj_line_tiles(e, o, a0, a1, a2, w, h, dy, x0);
            continue;
        }
        if (affine) {
            const int g = ((a1 >> 9) & 31) * 16;
            pa = (int16_t)e->oam[g + 3], pb = (int16_t)e->oam[g + 7];
            pc = (int16_t)e->oam[g + 11], pd = (int16_t)e->oam[g + 15];
        }
        for (lx = 0; lx < bw; lx++) {
            const int x = x0 + lx;
            int tx, ty;
            uint16_t c;
            if (x < 0)
                continue;
            if (x >= W)
                break;
            if (affine) {
                const int ix = lx - bw / 2, iy = dy - bh / 2;
                tx = ((pa * ix + pb * iy) >> 8) + w / 2;
                ty = ((pc * ix + pd * iy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h)
                    continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - lx : lx;
                ty = (a1 & 0x2000) ? h - 1 - dy : dy;
            }
            c = obj_texel(e, a0, a2, w, tx, ty);
            if (c)
                got |= obj_put(o, x, c & 0x7fff, mode, prio, a2);
        }
    }
    return got;
}

/* ---- composition ---------------------------------------------------------------------- */

static inline uint16_t blend(uint16_t a, uint16_t b, int eva, int evb)
{
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return (uint16_t)(r | g << 5 | bl << 10);
}

static inline uint16_t brighten(uint16_t c, int f)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r += ((31 - r) * f) >> 4, g += ((31 - g) * f) >> 4, b += ((31 - b) * f) >> 4;
    return (uint16_t)(r | g << 5 | b << 10);
}

static inline uint16_t darken(uint16_t c, int f)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r -= (r * f) >> 4, g -= (g * f) >> 4, b -= (b * f) >> 4;
    return (uint16_t)(r | g << 5 | b << 10);
}

static uint32_t s_rgba[0x8000]; /* BGR555 -> RGBA8888 */

static void init_rgba(void)
{
    uint32_t c;
    if (s_rgba[0x7fff])
        return;
    for (c = 0; c < 0x8000; c++) {
        uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        r = r << 3 | r >> 2, g = g << 3 | g >> 2, b = b << 3 | b >> 2;
        s_rgba[c] = 0xff000000u | b << 16 | g << 8 | r;
    }
}

/* arithmetic rather than s_rgba: the 128 KiB table does not stay in the A9's 32 KiB L1, and
 * this vectorises (NEON) in the row loops */
static inline uint32_t to_rgba(uint16_t c)
{
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = r << 3 | r >> 2;
    g = g << 3 | g >> 2;
    b = b << 3 | b >> 2;
    return 0xff000000u | b << 16 | g << 8 | r;
}

static int in_span(int v, int lo, int hi)
{
    return lo <= hi ? (v >= lo && v < hi) : (v >= lo || v < hi);
}

/* the window control (WININ/WINOUT bits 0-5) of each pixel of the line */
static int window_line(const Engine *e, int line, const ObjLine *o, uint8_t *ctl)
{
    const uint32_t dc = e->dispcnt;
    const uint16_t winin = io16(e, 0x48), winout = io16(e, 0x4a);
    int x, win_on[2];

    if (!(dc & 0xe000))
        return 0;
    for (x = 0; x < 2; x++) {
        const uint16_t v = io16(e, 0x44 + 2 * x);
        win_on[x] = (dc & (0x2000u << x)) && in_span(line, v >> 8, v & 0xff);
    }
    for (x = 0; x < W; x++) {
        uint8_t c = winout & 0x3f;
        if (win_on[0] && in_span(x, io16(e, 0x40) >> 8, io16(e, 0x40) & 0xff))
            c = winin & 0x3f;
        else if (win_on[1] && in_span(x, io16(e, 0x42) >> 8, io16(e, 0x42) & 0xff))
            c = (winin >> 8) & 0x3f;
        else if ((dc & 0x8000) && o->win[x])
            c = (winout >> 8) & 0x3f;
        ctl[x] = c;
    }
    return 1;
}

/* One line into out as BGR555. The layers shown are put in drawing order once per line
 * (priority, then BG number); per pixel only those are looked at, and the window and colour
 * effect work is done only when they are on. */
typedef struct {
    uint16_t bgl[4][W];
    ObjLine obj;
} Scratch; /* one per thread rendering lines */

static void compose_line(const Engine *e, int line, uint16_t *out, uint8_t *code, Scratch *sc)
{
    uint16_t (*bgl)[W] = sc->bgl;
    ObjLine *const objp = &sc->obj;
#define obj (*objp)
    uint8_t ctl[W];
    const uint16_t bldcnt = io16(e, 0x50), bldalpha = io16(e, 0x52);
    const int effect = (bldcnt >> 6) & 3;
    int eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = io16(e, 0x54) & 31;
    const uint16_t backdrop = e->bg_pal[0] & 0x7fff;
    int order[4], prio[4], nb = 0, objs, windows, bg, x, i;

    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    for (bg = 0; bg < 4; bg++) {
        if (!render_bg(e, bg, line, bgl[bg]))
            continue;
        /* insertion by (priority, BG number) */
        {
            const int p = io16(e, 0x08 + 2 * bg) & 3;
            for (i = nb; i > 0 && prio[i - 1] > p; i--)
                order[i] = order[i - 1], prio[i] = prio[i - 1];
            order[i] = bg, prio[i] = p, nb++;
        }
    }
    objs = render_obj(e, line, &obj);
    windows = window_line(e, line, &obj, ctl);

    if (!windows && !effect && !(objs & 2)) {
        /* the common case: the frontmost opaque pixel, nothing else */
        for (x = 0; x < W; x++) {
            uint16_t c = backdrop, below = backdrop;
            int p = 4, pb = 4, is3d = 0;
            for (i = 0; i < nb; i++) {
                const uint16_t v = bgl[order[i]][x];
                if (!(v & OPAQUE))
                    continue;
                if (!is3d && order[i] == 0 && e->has3d) {
                    is3d = 1, p = prio[i];
                    continue;
                }
                if (is3d)
                    below = v & 0x7fff, pb = prio[i];
                else
                    c = v & 0x7fff, p = prio[i];
                break;
            }
            if ((objs & 1) && (obj.col[x] & OPAQUE)) {
                if (obj.prio[x] <= p)
                    c = obj.col[x] & 0x7fff, is3d = 0;
                else if (is3d && obj.prio[x] <= pb)
                    below = obj.col[x] & 0x7fff;
            }
            out[x] = is3d ? below : c;
            code[x] = is3d ? KH_GPU2D_3D : KH_GPU2D_2D;
        }
        return;
    }

    if (!windows)
        memset(ctl, 0x3f, W);
    for (x = 0; x < W; x++) {
        /* the two frontmost visible layers: OBJ goes above BGs of the same priority */
        uint16_t c[2] = { backdrop, backdrop };
        int id[2] = { L_BD, L_BD }, n = 0;
        int obj_left = (ctl[x] & 0x10) && (obj.col[x] & OPAQUE);
        uint16_t out_c;
        for (i = 0; i < nb && n < 2; i++) {
            const int b = order[i];
            if (obj_left && obj.prio[x] <= prio[i]) {
                c[n] = obj.col[x] & 0x7fff, id[n] = L_OBJ, n++, obj_left = 0;
                if (n == 2)
                    break;
            }
            if ((ctl[x] & (1 << b)) && (bgl[b][x] & OPAQUE))
                c[n] = bgl[b][x] & 0x7fff, id[n] = b, n++;
        }
        if (obj_left && n < 2)
            c[n] = obj.col[x] & 0x7fff, id[n] = L_OBJ, n++;
        code[x] = KH_GPU2D_2D;
        if (id[0] == 0 && e->has3d) {
            /* the 3D layer in front: the GPU blends it over c[1], which is what shows
             * where the 3D is clear, and then the frontmost layer for the colour effect */
            out[x] = c[1];
            if ((ctl[x] & 0x20) && (bldcnt & (1 << id[1]))) {
                if (effect == 2)
                    out[x] = brighten(c[1], evy);
                else if (effect == 3)
                    out[x] = darken(c[1], evy);
            }
            code[x] = KH_GPU2D_3D;
            if ((ctl[x] & 0x20) && (bldcnt & 1) && (effect == 2 || effect == 3))
                code[x] = (uint8_t)((effect == 2 ? KH_GPU2D_3D_BRIGHTEN : KH_GPU2D_3D_DARKEN) | evy);
            continue;
        }
        if (id[1] == 0 && e->has3d) {
            /* a 2D layer over the 3D one: an alpha blend with it happens on the GPU, which
             * has the 3D colour (dialogue scenes dim the field with a translucent BG) */
            if (id[0] == L_OBJ && obj.alpha[x] && (bldcnt & 0x100)) {
                out[x] = c[0];
                code[x] = obj.alpha[x] == 0x80 ? KH_GPU2D_BLEND_3D : (uint8_t)(KH_GPU2D_OVER_3D | obj.alpha[x]);
                continue;
            }
            if (effect == 1 && (ctl[x] & 0x20) && (bldcnt & (1 << id[0])) && (bldcnt & 0x100)) {
                out[x] = c[0];
                code[x] = KH_GPU2D_BLEND_3D;
                continue;
            }
            id[1] = L_BD; /* otherwise as over the backdrop: brighten/darken need no 3D */
        }
        out_c = c[0];
        if (id[0] == L_OBJ && obj.alpha[x] && (bldcnt & (0x100 << id[1]))) {
            /* semi-transparent and bitmap sprites blend with what is under them */
            if (obj.alpha[x] == 0x80)
                out_c = blend(c[0], c[1], eva, evb);
            else
                out_c = blend(c[0], c[1], obj.alpha[x], 16 - obj.alpha[x]);
        } else if ((ctl[x] & 0x20) && (bldcnt & (1 << id[0]))) {
            if (effect == 1 && (bldcnt & (0x100 << id[1])))
                out_c = blend(c[0], c[1], eva, evb);
            else if (effect == 2)
                out_c = brighten(c[0], evy);
            else if (effect == 3)
                out_c = darken(c[0], evy);
        }
        out[x] = out_c;
    }
}

#undef obj

/* MASTER_BRIGHT as (mode, factor): mode 1 brighter, 2 darker, 0 off */
static int master_mode(const Engine *e, int *f)
{
    const uint16_t mb = io16(e, 0x6c);
    const int mode = (mb >> 14) & 3;
    *f = mb & 31;
    if (*f > 16)
        *f = 16;
    return (mode == 1 || mode == 2) && *f ? mode : 0;
}

/* one BGR555 row to RGBA, through master brightness */
static void row_out(const uint16_t *row, uint32_t *dst, int mmode, int mf)
{
    int x;
    if (mmode == 1)
        for (x = 0; x < W; x++)
            dst[x] = to_rgba(brighten(row[x], mf));
    else if (mmode == 2)
        for (x = 0; x < W; x++)
            dst[x] = to_rgba(darken(row[x], mf));
    else
        for (x = 0; x < W; x++)
            dst[x] = to_rgba(row[x]);
}

void kh_gpu2d_init(void)
{
    init_rgba();
}

int kh_gpu2d_render_lines(int engine, uint32_t *fb, int y0, int y1)
{
    Engine e;
    Scratch sc;
    int mode, line, x, mmode, mf;

    engine_setup(&e, engine);
    mode = (e.dispcnt >> 16) & (e.is_a ? 3 : 1);
    fb += y0 * W;
    if (mode == 0 || (e.dispcnt & 0x80)) { /* display off, or forced blank: white */
        memset(fb, 0xff, (size_t)(y1 - y0) * W * sizeof(*fb));
        return 0;
    }
    mmode = master_mode(&e, &mf);
    if (mode == 2) { /* engine A shows a VRAM bank (A-D) as a 256x192 direct-colour bitmap */
        const uint8_t *bank = kh_vram_bank_home((e.dispcnt >> 18) & 3);
        for (line = y0; line < y1; line++, fb += W) {
            uint16_t row[W];
            memcpy(row, bank + line * W * 2, sizeof(row));
            for (x = 0; x < W; x++)
                row[x] &= 0x7fff;
            row_out(row, fb, mmode, mf);
        }
        return 0;
    }
    if (mode == 3) { /* main memory display FIFO: not emulated */
        memset(fb, 0, (size_t)(y1 - y0) * W * sizeof(*fb));
        return 0;
    }
    for (line = y0; line < y1; line++, fb += W) {
        uint16_t row[W];
        uint8_t code[W];
        compose_line(&e, line, row, code, &sc);
        if (e.has3d) {
            /* master brightness then comes after the composition, on the GPU */
            for (x = 0; x < W; x++)
                fb[x] = (to_rgba(row[x]) & 0xffffffu) | (uint32_t)code[x] << 24;
        } else {
            row_out(row, fb, mmode, mf);
        }
    }
    return e.has3d;
}

int kh_gpu2d_render(int engine, uint32_t *fb)
{
    init_rgba();
    return kh_gpu2d_render_lines(engine, fb, 0, H);
}
