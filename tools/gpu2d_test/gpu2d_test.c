/* Host test for platform/hw/gpu2d.c: builds synthetic 2D scenes in stand-ins for the port's
 * memory (registers, palettes, OAM, VRAM views, bank homes), renders them, and writes each
 * screen as raw RGBA for run.sh to turn into PNGs. The scenes exercise one feature each, so a
 * wrong image points at the code that drew it.
 *
 *     tools/gpu2d_test/run.sh        -> build/gpu2d_test/<scene>.png */
#include "hw/gpu2d.h"
#include "hw/io.h"
#include "hw/memmap.h"
#include "hw/vram.h"

#include <stdio.h>
#include <string.h>

/* ---- the port's memory, as the renderer sees it ---------------------------------------- */

uint8_t kh_ds_io[KH_IO_SIZE];
unsigned char kh_ds_palette[0x800];
unsigned char kh_ds_oam[0x800];
unsigned char kh_vram_bg_a[0x80000];
unsigned char kh_vram_bg_b[0x20000];
unsigned char kh_vram_obj_a[0x40000];
unsigned char kh_vram_obj_b[0x20000];
unsigned char kh_vram_lcdc[0xa4000];
static uint8_t s_cnt[KH_VRAM_BANKS];
static const uint32_t s_home[KH_VRAM_BANKS] = { 0x00000, 0x20000, 0x40000, 0x60000, 0x80000,
                                                0x90000, 0x94000, 0x98000, 0xa0000 };

uint8_t *kh_vram_bank_home(int bank) { return kh_vram_lcdc + s_home[bank]; }
uint8_t kh_vram_bank_cnt(int bank) { return s_cnt[bank]; }

/* ---- helpers --------------------------------------------------------------------------- */

#define RGB(r, g, b) ((uint16_t)((r) | (g) << 5 | (b) << 10))

static void w16(uint32_t addr, uint16_t v) { memcpy(kh_ds_io + (addr - 0x04000000), &v, 2); }
static void w32(uint32_t addr, uint32_t v) { memcpy(kh_ds_io + (addr - 0x04000000), &v, 4); }
static void pal(uint32_t off, int i, uint16_t c) { memcpy(kh_ds_palette + off + i * 2, &c, 2); }
static void oam(int engine, int i, uint16_t a0, uint16_t a1, uint16_t a2)
{
    uint16_t *o = (uint16_t *)(kh_ds_oam + engine * 0x400) + i * 4;
    o[0] = a0, o[1] = a1, o[2] = a2;
}
static void oam_affine(int engine, int g, int16_t pa, int16_t pb, int16_t pc, int16_t pd)
{
    uint16_t *o = (uint16_t *)(kh_ds_oam + engine * 0x400) + g * 16;
    o[3] = (uint16_t)pa, o[7] = (uint16_t)pb, o[11] = (uint16_t)pc, o[15] = (uint16_t)pd;
}

static void reset(void)
{
    memset(kh_ds_io, 0, sizeof(kh_ds_io));
    memset(kh_ds_palette, 0, sizeof(kh_ds_palette));
    memset(kh_ds_oam, 0, sizeof(kh_ds_oam));
    memset(kh_vram_bg_a, 0, sizeof(kh_vram_bg_a));
    memset(kh_vram_bg_b, 0, sizeof(kh_vram_bg_b));
    memset(kh_vram_obj_a, 0, sizeof(kh_vram_obj_a));
    memset(kh_vram_obj_b, 0, sizeof(kh_vram_obj_b));
    memset(kh_vram_lcdc, 0, sizeof(kh_vram_lcdc));
    memset(s_cnt, 0, sizeof(s_cnt));
    for (int i = 0; i < 128; i++) { /* every sprite hidden */
        oam(0, i, 0x200, 0, 0);
        oam(1, i, 0x200, 0, 0);
    }
}

/* 4bpp tile n at base: a frame of colour 1 around a fill of colour f, a diagonal of 3 */
static void tile4(uint8_t *vram, uint32_t base, int n, int f)
{
    uint8_t *t = vram + base + n * 32;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int c = (x == 0 || y == 0 || x == 7 || y == 7) ? 1 : (x == y ? 3 : f);
            t[y * 4 + x / 2] |= (uint8_t)(c << ((x & 1) * 4));
        }
}

/* 4bpp 16x16 arrow pointing right, as four 1D tiles starting at tile n (32-byte units) */
static void arrow16(uint8_t *vram, int n, int col)
{
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int dy = y < 8 ? 7 - y : y - 8;
            int on = x < 10 ? (dy < 2) : (dy <= 15 - x);
            if (!on)
                continue;
            int tile = (y / 8) * 2 + x / 8;
            uint8_t *t = vram + (n + tile) * 32;
            t[(y & 7) * 4 + (x & 7) / 2] |= (uint8_t)(col << ((x & 1) * 4));
        }
}

static void save(const char *name, int engine)
{
    static uint32_t fb[256 * 192];
    char path[256];
    FILE *f;
    kh_gpu2d_render(engine, fb);
    snprintf(path, sizeof(path), "%s.rgba", name);
    f = fopen(path, "wb");
    fwrite(fb, sizeof(fb), 1, f);
    fclose(f);
}

/* ---- scenes ---------------------------------------------------------------------------- */

/* Engine A, mode 0: BG0 4bpp tiles scrolled, BG1 8bpp with priority above BG0, backdrop */
static void scene_text(void)
{
    reset();
    w32(0x04000000, 0x00010000 | 0x0300);              /* display on, BG0 + BG1 */
    w16(0x04000008, 0x0001 | (0 << 2) | (30 << 8));    /* BG0: prio 1, char 0, screen 30 */
    w16(0x0400000a, 0x0000 | (1 << 2) | 0x80 | (31 << 8)); /* BG1: prio 0, char 1, 8bpp, screen 31 */
    w16(0x04000010, 4), w16(0x04000012, 4);            /* BG0 scroll 4,4 */
    pal(0, 0, RGB(4, 4, 8));                           /* backdrop */
    for (int p = 0; p < 4; p++) {
        pal(0, p * 16 + 1, RGB(31, 31, 31));
        pal(0, p * 16 + 2, RGB(p == 0 ? 20 : 0, p == 1 ? 20 : 0, p == 2 ? 20 : 8));
        pal(0, p * 16 + 3, RGB(31, 0, 0));
    }
    tile4(kh_vram_bg_a, 0, 1, 2);
    for (int i = 0; i < 32 * 32; i++) { /* checkerboard of palettes 0-3 */
        uint16_t se = 1 | (uint16_t)(((i % 32 + i / 32) % 4) << 12);
        if (i % 3 == 0) se |= 0x400; /* h-flip */
        memcpy(kh_vram_bg_a + 30 * 0x800 + i * 2, &se, 2);
    }
    /* BG1: 8bpp gradient tiles in a band, colours 128-135 */
    for (int c = 0; c < 8; c++)
        pal(0, 128 + c, RGB(c * 4, 31 - c * 4, 16));
    for (int t = 0; t < 8; t++)
        for (int p = 0; p < 64; p++)
            kh_vram_bg_a[0x4000 + (t + 1) * 64 + p] = (uint8_t)(128 + ((p % 8 + t) % 8));
    for (int x = 0; x < 32; x++) {
        uint16_t se = (uint16_t)(1 + x % 8);
        memcpy(kh_vram_bg_a + 31 * 0x800 + (10 * 32 + x) * 2, &se, 2);
        memcpy(kh_vram_bg_a + 31 * 0x800 + (11 * 32 + x) * 2, &se, 2);
    }
    save("text", 0);
}

/* sprites: normal, flipped, affine rotated, double-size, semi-transparent over BG, 1D map */
static void scene_obj(void)
{
    reset();
    w32(0x04000000, 0x00010000 | 0x1100 | 0x10);      /* BG0 + OBJ, 1D tile mapping (32 B) */
    w16(0x04000008, 0x0003 | (8 << 8));                /* BG0 prio 3 */
    pal(0, 0, RGB(0, 0, 6));
    pal(0, 1, RGB(10, 10, 10)), pal(0, 2, RGB(6, 6, 6)), pal(0, 3, RGB(12, 6, 6));
    tile4(kh_vram_bg_a, 0, 1, 2);
    for (int i = 0; i < 32 * 32; i++) {
        uint16_t se = 1;
        memcpy(kh_vram_bg_a + 8 * 0x800 + i * 2, &se, 2);
    }
    pal(0x200, 1, RGB(31, 31, 0));
    pal(0x200, 16 + 1, RGB(0, 31, 31));
    arrow16(kh_vram_obj_a, 0, 1);
    /* 16x16 = shape 0 size 1 */
    oam(0, 0, 20, 20 | (1 << 14), 0);                          /* plain */
    oam(0, 1, 20, 60 | 0x1000 | (1 << 14), 1 << 12);           /* h-flip, palette 1 */
    oam(0, 2, 60 | 0x100, 20 | (1 << 14), 0);                   /* affine, group 0 */
    oam_affine(0, 0, 0, -256, 256, 0);                          /* rotated 90 degrees */
    oam(0, 3, 60 | 0x300, 60 | (1 << 14) | (1 << 9), 1 << 12); /* double size, group 1 */
    oam_affine(0, 1, 181, -181, 181, 181);                      /* 45 degrees */
    oam(0, 4, 120 | 0x400, 120 | (1 << 14), 0);                 /* semi-transparent */
    w16(0x04000050, 0x0100 | (1 << 6));                         /* BG0 is the 2nd target */
    w16(0x04000052, 8 | (8 << 8));                              /* 50/50 */
    oam(0, 5, (uint16_t)(186), 200 | (1 << 14), 0);             /* wraps from y=186 */
    save("obj", 0);
}

/* windows and colour effects: win0 shows BG1 only in a box, the rest is darkened */
static void scene_window(void)
{
    scene_text();
    w32(0x04000000, 0x00010000 | 0x0300 | 0x2000);    /* + window 0 */
    w16(0x04000040, (64 << 8) | 192);                  /* x 64-192 */
    w16(0x04000044, (48 << 8) | 144);                  /* y 48-144 */
    w16(0x04000048, 0x0002);                           /* inside: BG1, no effects */
    w16(0x0400004a, 0x0021);                           /* outside: BG0 + effects */
    w16(0x04000050, 0x0001 | (3 << 6));                /* darken BG0 */
    w16(0x04000054, 10);
    save("window", 0);
}

/* engine B, mode 5: BG3 a direct-colour bitmap, BG2 a rotated 256-colour bitmap */
static void scene_bitmap(void)
{
    reset();
    w32(0x04001000, 0x00010000 | 5 | 0x0c00);
    /* BG3: direct colour 128x128 at 0, wrapping */
    w16(0x0400100e, 0x0003 | 0x0080 | 0x0004 | 0x2000 | (0 << 8) | (0 << 14));
    w16(0x04001030, 0x100), w16(0x04001036, 0x100);                 /* identity */
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) {
            uint16_t c = (uint16_t)(0x8000 | RGB(x / 4, y / 4, 16));
            memcpy(kh_vram_bg_b + (y * 128 + x) * 2, &c, 2);
        }
    /* BG2: 256-colour 128x128 bitmap at 0x10000 (screen base 4 * 16K), rotated, prio 0 */
    w16(0x0400100c, 0x0000 | 0x0080 | (4 << 8) | (0 << 14));
    for (int i = 1; i < 256; i++)
        pal(0x400, i, RGB(31, i / 8, 0));
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++)
            kh_vram_bg_b[0x10000 + y * 128 + x] = ((x / 16 + y / 16) & 1) ? (uint8_t)(x * 2) : 0;
    /* 30 degrees, centred */
    w16(0x04001020, 222), w16(0x04001022, (uint16_t)-128);
    w16(0x04001024, 128), w16(0x04001026, 222);
    w32(0x04001028, (uint32_t)(-20 * 256)), w32(0x0400102c, (uint32_t)(-30 * 256));
    save("bitmap", 1);
}

/* master brightness half way to white, and display off */
static void scene_brightness(void)
{
    scene_text();
    w16(0x0400006c, (1 << 14) | 8);
    save("bright_up", 0);
    w16(0x0400006c, (2 << 14) | 12);
    save("bright_down", 0);
    w32(0x04000000, 0);
    save("display_off", 0);
}

/* extended palettes: BG1 8bpp through slot 1 in bank E, OBJ 8bpp through bank F */
static void scene_extpal(void)
{
    scene_text();
    w32(0x04000000, 0x00010000 | 0x0300 | (1u << 30));
    s_cnt[4] = 0x84; /* E: BG extended palette */
    for (int c = 0; c < 8; c++) {
        uint16_t v = RGB(31 - c * 4, 0, c * 4);
        memcpy(kh_vram_bank_home(4) + 0x2000 * 1 + (128 + c) * 2, &v, 2);
    }
    save("extpal", 0);
}

int main(void)
{
    scene_text();
    scene_obj();
    scene_window();
    scene_bitmap();
    scene_brightness();
    scene_extpal();
    return 0;
}
