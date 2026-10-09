/* The Vita's face buttons in the game's text: the fonts' A, B, X and Y icons (a filled circle
 * with the letter cut out) drawn again as Circle, Cross, Triangle and Square, the buttons in
 * the same places on the Vita (config.c maps A to Circle, B to Cross, X to Triangle, Y to
 * Square). The icons are there twice: glyphs 0x3349, 0x3314, 0x3322 and 0x334d of the
 * European fonts, and 0xe000-0xe003 of the 10-pixel "all" fonts (European and Japanese), the
 * ones the camp menu's help and buttons use (0.4.16-0.4.22 drew only the first: a frame dump
 * of the camp menu showed the DS's X and Y). The fonts are changed as the game reads them from
 * the dump (rom_patch); config button_icons = 0 keeps the DS's letters. */
#include "nitro/button_glyphs.h"

#include "config.h"
#include "log.h"
#include "nitro/romfs.h"
#include "rom.h"

#include <stdlib.h>
#include <string.h>

/* in the fonts' style: a 9x9 filled circle, the symbol cut out ('#' set, 1 bit a pixel) */
static const struct {
    uint16_t code, code2; /* the European fonts' code, and the private-use one */
    const char *rows[10];
} s_icons[] = {
    /* A (right) -> Circle */
    { 0x3349, 0xe000, { "  #####   ", " ##   ##  ", "## ### ## ", "# ##### # ", "# ##### # ",
                "# ##### # ", "## ### ## ", " ##   ##  ", "  #####   ", "          " } },
    /* B (bottom) -> Cross */
    { 0x3314, 0xe001, { "  #####   ", " #######  ", "## ### ## ", "### # ### ", "#### #### ",
                "### # ### ", "## ### ## ", " #######  ", "  #####   ", "          " } },
    /* X (top) -> Triangle */
    { 0x3322, 0xe002, { "  #####   ", " ### ###  ", "### # ### ", "## ### ## ", "# ##### # ",
                "#       # ", "######### ", " #######  ", "  #####   ", "          " } },
    /* Y (left) -> Square */
    { 0x334d, 0xe003, { "  #####   ", " #######  ", "##     ## ", "## ### ## ", "## ### ## ",
                "## ### ## ", "##     ## ", " #######  ", "  #####   ", "          " } },
};
#define NICONS ((int)(sizeof(s_icons) / sizeof(s_icons[0])))

/* the fonts, and whether their 0x33xx codes are the icons (in the Japanese font they are
 * other characters: only its 0xe00x) */
static const struct {
    const char *path;
    int letters;
} s_fonts[] = {
    { "/text/font_eu_10.nftr", 1 }, { "/text/font_eu_10s.nftr", 1 },
    { "/text/font_eu_10all.nftr", 1 }, { "/text/font_jp_10all.nftr", 0 },
};
#define NFONTS ((int)(sizeof(s_fonts) / sizeof(s_fonts[0])))

/* the patches of the A and B glyphs (rom_patch ids) and their sizes: config confirm_cross
 * (kh_button_glyphs_menu) draws them the other way round in memory */
#define AB_MAX 16
static int s_patch_ab[AB_MAX], s_ab_is_a[AB_MAX], s_nab;
static uint16_t s_ab_size[AB_MAX];
static uint8_t s_bits_circle[32], s_bits_cross[32];

static void icon_bits(int i, uint8_t *bits)
{
    int x, y;
    memset(bits, 0, 32);
    for (y = 0; y < 10; y++)
        for (x = 0; x < 10; x++)
            if (s_icons[i].rows[y][x] == '#')
                bits[(y * 10 + x) >> 3] |= (uint8_t)(0x80 >> ((y * 10 + x) & 7));
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* the glyph index of code in the NFTR's character maps, -1 when it has none */
static int glyph_of(const uint8_t *d, uint32_t size, uint32_t cmap, uint16_t code)
{
    int guard = 0;
    while (cmap >= 8 && cmap + 0x14 <= size && guard++ < 256) {
        const uint8_t *m = d + cmap - 8;
        const uint16_t first = rd16(m + 8), last = rd16(m + 10), type = rd16(m + 12);
        const uint32_t next = rd32(m + 16);
        if (type == 0 && code >= first && code <= last) {
            return rd16(m + 20) + (code - first);
        } else if (type == 1 && code >= first && code <= last) {
            const uint32_t at = cmap - 8 + 20 + (uint32_t)(code - first) * 2;
            if (at + 2 <= size) {
                const uint16_t g = rd16(d + at);
                return g == 0xffff ? -1 : g;
            }
        } else if (type == 2) {
            const uint16_t n = rd16(m + 20);
            uint32_t i;
            for (i = 0; i < n && cmap - 8 + 22 + i * 4 + 4 <= size; i++)
                if (rd16(m + 22 + i * 4) == code)
                    return rd16(m + 24 + i * 4);
        }
        cmap = next;
    }
    return -1;
}

static int patch_font(const char *path, int letters)
{
    uint32_t off, size, finf, cglp, cmap;
    uint8_t *d;
    int i, done = 0;
    if (!kh_romfs_find(path, &off, &size) || size < 0x40 || size > 1024 * 1024)
        return 0;
    d = malloc(size);
    if (!d)
        return 0;
    if (rom_read(off, d, size) != (int)size || memcmp(d, "RTFN", 4)) {
        free(d);
        return 0;
    }
    finf = rd16(d + 0xc);
    if (finf + 0x1c > size || memcmp(d + finf, "FNIF", 4)) {
        free(d);
        return 0;
    }
    cglp = rd32(d + finf + 0x10) - 8; /* the block's header */
    cmap = rd32(d + finf + 0x18);
    if (cglp + 0x10 <= size && !memcmp(d + cglp, "PLGC", 4)) {
        const int w = d[cglp + 8], h = d[cglp + 9], bpp = d[cglp + 0xe];
        const uint16_t gsize = rd16(d + cglp + 10);
        for (i = 0; i < NICONS * 2 && w == 10 && h == 10 && bpp == 1; i++) {
            uint8_t bits[32];
            const int k = i / 2;
            const uint16_t code = (i & 1) ? s_icons[k].code2 : s_icons[k].code;
            int g, id;
            if (!(i & 1) && !letters)
                continue;
            g = glyph_of(d, size, cmap, code);
            if (g < 0 || gsize > sizeof(bits) || cglp + 0x10 + (uint32_t)(g + 1) * gsize > size)
                continue;
            /* 1 bit a pixel, rows one after the other, the first pixel in the top bit */
            icon_bits(k, bits);
            id = rom_patch(off + cglp + 0x10 + (uint32_t)g * gsize, bits, gsize);
            if (k < 2 && id >= 0 && s_nab < AB_MAX) {
                s_patch_ab[s_nab] = id;
                s_ab_is_a[s_nab] = k == 0;
                s_ab_size[s_nab] = gsize;
                s_nab++;
            }
            done++;
        }
    }
    free(d);
    return done;
}

void kh_button_glyphs_init(void)
{
    int i, n = 0;
    if (!kh_config.button_icons)
        return;
    icon_bits(0, s_bits_circle);
    icon_bits(1, s_bits_cross);
    for (i = 0; i < NFONTS; i++)
        n += patch_font(s_fonts[i].path, s_fonts[i].letters);
    LOG("text: %d button icons drawn as the Vita's (%d fonts)", n, NFONTS);
}

/* The fonts' A and B in memory: Circle and Cross as the buttons are placed (swap 0), or the
 * other way round while the menus confirm with Cross (swap 1; input.c). Only where the font a
 * patch was read into still holds one of the two (it may have been freed and used since). */
void kh_button_glyphs_menu(int swap)
{
    int i;
    for (i = 0; i < s_nab; i++) {
        uint8_t *p = rom_patch_last_dst(s_patch_ab[i]);
        const uint16_t n = s_ab_size[i];
        const uint8_t *want = s_ab_is_a[i] == !swap ? s_bits_circle : s_bits_cross;
        if (p && (!memcmp(p, s_bits_circle, n) || !memcmp(p, s_bits_cross, n)) && memcmp(p, want, n))
            memcpy(p, want, n);
    }
}
