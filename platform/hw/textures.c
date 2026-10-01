/* DS 3D texture decoding (see textures.h). Formats per GBATEK "DS 3D Texture Formats":
 *   1 A3I5, 2 4-colour, 3 16-colour, 4 256-colour, 5 4x4 compressed, 6 A5I3, 7 direct colour.
 * Texture VRAM is four 128 KiB slots (banks A-D in MST 3), palette VRAM six 16 KiB slots
 * (bank E for slots 0-3, F and G by their offset). The game only writes them through LCDC, so
 * the banks' home storage is current while they are mapped here. */
#include "hw/textures.h"

#include "hw/vram.h"

#include <string.h>

static const uint8_t *s_tex_slot[4];
static const uint8_t *s_pal_slot[6];

void kh_tex_map_slots(void)
{
    int bank, i;
    for (i = 0; i < 4; i++)
        s_tex_slot[i] = NULL;
    for (i = 0; i < 6; i++)
        s_pal_slot[i] = NULL;
    for (bank = 0; bank < 4; bank++) {
        uint8_t cnt = kh_vram_bank_cnt(bank);
        if ((cnt & 0x80) && (cnt & 7) == 3)
            s_tex_slot[(cnt >> 3) & 3] = kh_vram_bank_home(bank);
    }
    {
        uint8_t cnt = kh_vram_bank_cnt(4); /* E: 64 KiB, slots 0-3 */
        if ((cnt & 0x80) && (cnt & 7) == 3)
            for (i = 0; i < 4; i++)
                s_pal_slot[i] = kh_vram_bank_home(4) + i * 0x4000;
    }
    for (bank = 5; bank <= 6; bank++) { /* F, G: 16 KiB each */
        uint8_t cnt = kh_vram_bank_cnt(bank);
        if ((cnt & 0x80) && (cnt & 7) == 3) {
            int ofs = (cnt >> 3) & 3;
            s_pal_slot[(ofs & 1) + ((ofs >> 1) & 1) * 4] = kh_vram_bank_home(bank);
        }
    }
}

static const uint8_t s_zero[0x20000];

/* texture bytes from addr on: the slot's memory (zeros when unmapped) and how many follow */
static const uint8_t *tex_ptr(uint32_t addr, uint32_t *avail)
{
    const uint8_t *slot = s_tex_slot[(addr >> 17) & 3];
    *avail = 0x20000 - (addr & 0x1ffff);
    return (slot ? slot : s_zero) + (addr & 0x1ffff);
}

static inline uint16_t pal_color(uint32_t addr)
{
    const uint8_t *slot;
    addr &= 0x1ffff;
    if (addr >= 0x18000)
        return 0;
    slot = s_pal_slot[addr >> 14];
    if (!slot)
        return 0;
    return (uint16_t)(slot[addr & 0x3fff] | slot[(addr & 0x3fff) + 1] << 8);
}

static inline uint32_t rgba(uint16_t c, uint32_t a8)
{
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = r << 3 | r >> 2;
    g = g << 3 | g >> 2;
    b = b << 3 | b >> 2;
    return r | g << 8 | b << 16 | a8 << 24;
}

static uint32_t tex_bytes(int fmt, int w, int h)
{
    static const uint8_t bpp[8] = { 0, 8, 2, 4, 8, 2, 8, 16 };
    return (uint32_t)w * h * bpp[fmt] / 8;
}

static uint32_t pal_entries(int fmt)
{
    static const uint16_t n[8] = { 0, 32, 4, 16, 256, 0, 8, 0 };
    return n[fmt];
}

static uint32_t pal_base(int fmt, uint32_t pltt)
{
    return fmt == 2 ? (pltt & 0x1fff) * 8 : (pltt & 0x1fff) * 16;
}

/* 4x4 compressed: the 16-bit palette info of each block lives in slot 1, opposite the texel
 * data in slot 0 (first half) or slot 2 (second half) */
static uint32_t c4x4_info_addr(uint32_t addr)
{
    uint32_t a = addr & 0x7ffff;
    return 0x20000 + (a & 0x1ffff) / 2 + (a >= 0x40000 ? 0x10000 : 0);
}

static inline uint32_t mix32(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 0x9e3779b1u;
    return h ^ (h >> 15);
}

static uint32_t hash_bytes(uint32_t h, const uint8_t *p, uint32_t n)
{
    uint32_t i, v;
    for (i = 0; i + 4 <= n; i += 4) {
        memcpy(&v, p + i, 4);
        h = mix32(h, v);
    }
    for (; i < n; i++)
        h = mix32(h, p[i]);
    return h;
}

uint32_t kh_tex_hash(uint32_t teximage, uint32_t pltt)
{
    const int fmt = kh_tex_format(teximage), w = kh_tex_width(teximage), h = kh_tex_height(teximage);
    const uint32_t addr = (teximage & 0xffff) * 8;
    uint32_t hv = mix32(mix32(0x811c9dc5u, teximage & 0x3fffffff), fmt == 7 ? 0 : pltt);
    uint32_t n = tex_bytes(fmt, w, h), avail, i;
    const uint8_t *p;

    if (fmt == 0)
        return hv;
    p = tex_ptr(addr, &avail);
    hv = hash_bytes(hv, p, n < avail ? n : avail);
    if (fmt == 5) {
        /* the block infos, and the palette span they reach */
        uint32_t nblk = (uint32_t)(w / 4) * (h / 4), maxofs = 0;
        const uint8_t *q = tex_ptr(c4x4_info_addr(addr), &avail);
        if (nblk * 2 > avail)
            nblk = avail / 2;
        hv = hash_bytes(hv, q, nblk * 2);
        for (i = 0; i < nblk; i++) {
            uint32_t ofs = (uint32_t)(q[i * 2] | q[i * 2 + 1] << 8) & 0x3fff;
            if (ofs > maxofs)
                maxofs = ofs;
        }
        n = maxofs * 4 + 8;
        for (i = 0; i < n; i += 2)
            hv = mix32(hv, pal_color(pal_base(3, pltt) + i));
    } else if (fmt != 7) {
        const uint32_t base = pal_base(fmt, pltt), ne = pal_entries(fmt);
        for (i = 0; i < ne; i++)
            hv = mix32(hv, pal_color(base + i * 2));
    }
    return hv;
}

static void decode_4x4(uint32_t addr, uint32_t pltt, int w, int h, uint32_t *out)
{
    uint32_t avail, avail_i;
    const uint8_t *t = tex_ptr(addr, &avail);
    const uint8_t *info = tex_ptr(c4x4_info_addr(addr), &avail_i);
    const uint32_t pbase = pal_base(3, pltt);
    int bx, by, x, y, blk = 0;

    for (by = 0; by < h / 4; by++) {
        for (bx = 0; bx < w / 4; bx++, blk++) {
            uint32_t c[4];
            uint16_t pi;
            uint32_t pa;
            int mode;
            if ((uint32_t)blk * 4 + 4 > avail || (uint32_t)blk * 2 + 2 > avail_i) {
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++)
                        out[(by * 4 + y) * w + bx * 4 + x] = 0;
                continue;
            }
            pi = (uint16_t)(info[blk * 2] | info[blk * 2 + 1] << 8);
            pa = pbase + (pi & 0x3fff) * 4;
            mode = pi >> 14;
            {
                uint16_t c0 = pal_color(pa), c1 = pal_color(pa + 2);
                c[0] = rgba(c0, 255);
                c[1] = rgba(c1, 255);
                switch (mode) {
                case 0:
                    c[2] = rgba(pal_color(pa + 4), 255);
                    c[3] = 0;
                    break;
                case 1: {
                    int k;
                    uint32_t m = 0xff000000u;
                    for (k = 0; k < 24; k += 8)
                        m |= ((((c[0] >> k) & 0xff) + ((c[1] >> k) & 0xff)) / 2) << k;
                    c[2] = m;
                    c[3] = 0;
                    break;
                }
                case 2:
                    c[2] = rgba(pal_color(pa + 4), 255);
                    c[3] = rgba(pal_color(pa + 6), 255);
                    break;
                default: {
                    int k;
                    uint32_t m2 = 0xff000000u, m3 = 0xff000000u;
                    for (k = 0; k < 24; k += 8) {
                        uint32_t a = (c[0] >> k) & 0xff, b = (c[1] >> k) & 0xff;
                        m2 |= ((a * 5 + b * 3) / 8) << k;
                        m3 |= ((a * 3 + b * 5) / 8) << k;
                    }
                    c[2] = m2;
                    c[3] = m3;
                    break;
                }
                }
            }
            for (y = 0; y < 4; y++) {
                uint8_t row = t[blk * 4 + y];
                uint32_t *o = out + (by * 4 + y) * w + bx * 4;
                for (x = 0; x < 4; x++)
                    o[x] = c[(row >> (x * 2)) & 3];
            }
        }
    }
}

void kh_tex_decode(uint32_t teximage, uint32_t pltt, uint32_t *out)
{
    const int fmt = kh_tex_format(teximage), w = kh_tex_width(teximage), h = kh_tex_height(teximage);
    const uint32_t addr = (teximage & 0xffff) * 8, n = (uint32_t)w * h;
    const int zero_clear = (teximage >> 29) & 1;
    uint32_t avail, i, pal[256];
    const uint8_t *p;

    if (fmt == 0) {
        for (i = 0; i < n; i++)
            out[i] = 0xffffffffu;
        return;
    }
    if (fmt == 5) {
        decode_4x4(addr, pltt, w, h, out);
        return;
    }
    p = tex_ptr(addr, &avail);
    if (tex_bytes(fmt, w, h) > avail) {
        memset(out, 0, n * 4);
        return;
    }
    if (fmt != 7) {
        const uint32_t base = pal_base(fmt, pltt), ne = pal_entries(fmt);
        for (i = 0; i < ne; i++)
            pal[i] = rgba(pal_color(base + i * 2), 255);
        if (zero_clear && (fmt == 2 || fmt == 3 || fmt == 4))
            pal[0] = 0;
    }
    switch (fmt) {
    case 1: /* A3I5 */
        for (i = 0; i < n; i++) {
            uint32_t a = p[i] >> 5;
            a = (a * 4 + a / 2) * 255 / 31;
            out[i] = (pal[p[i] & 31] & 0xffffff) | a << 24;
        }
        break;
    case 2:
        for (i = 0; i < n; i++)
            out[i] = pal[(p[i >> 2] >> ((i & 3) * 2)) & 3];
        break;
    case 3:
        for (i = 0; i < n; i++)
            out[i] = pal[(p[i >> 1] >> ((i & 1) * 4)) & 15];
        break;
    case 4:
        for (i = 0; i < n; i++)
            out[i] = pal[p[i]];
        break;
    case 6: /* A5I3 */
        for (i = 0; i < n; i++) {
            uint32_t a = p[i] >> 3;
            out[i] = (pal[p[i] & 7] & 0xffffff) | (a * 255 / 31) << 24;
        }
        break;
    default: /* direct colour, bit 15 opaque */
        for (i = 0; i < n; i++) {
            uint16_t c = (uint16_t)(p[i * 2] | p[i * 2 + 1] << 8);
            out[i] = rgba(c, (c & 0x8000) ? 255 : 0);
        }
        break;
    }
}
