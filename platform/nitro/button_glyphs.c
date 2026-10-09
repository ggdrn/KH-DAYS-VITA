/* The Vita's face buttons in the game's text: the fonts' A, B, X and Y icons (a filled circle
 * with the letter cut out, glyphs 0x3349, 0x3314, 0x3322 and 0x334d of the European fonts)
 * drawn again as Circle, Cross, Triangle and Square, the buttons in the same places on the
 * Vita (config.c maps A to Circle, B to Cross, X to Triangle, Y to Square). The fonts are
 * changed as the game reads them from the dump (rom_patch); config button_icons = 0 keeps the
 * DS's letters. */
#include "nitro/button_glyphs.h"

#include "config.h"
#include "log.h"
#include "nitro/romfs.h"
#include "rom.h"

#include <stdlib.h>
#include <string.h>

/* in the fonts' style: a 9x9 filled circle, the symbol cut out ('#' set, 1 bit a pixel) */
static const struct {
    uint16_t code;
    const char *rows[10];
} s_icons[] = {
    /* A (right) -> Circle */
    { 0x3349, { "  #####   ", " ##   ##  ", "## ### ## ", "# ##### # ", "# ##### # ",
                "# ##### # ", "## ### ## ", " ##   ##  ", "  #####   ", "          " } },
    /* B (bottom) -> Cross */
    { 0x3314, { "  #####   ", " #######  ", "## ### ## ", "### # ### ", "#### #### ",
                "### # ### ", "## ### ## ", " #######  ", "  #####   ", "          " } },
    /* X (top) -> Triangle */
    { 0x3322, { "  #####   ", " ### ###  ", "### # ### ", "## ### ## ", "# ##### # ",
                "#       # ", "######### ", " #######  ", "  #####   ", "          " } },
    /* Y (left) -> Square */
    { 0x334d, { "  #####   ", " #######  ", "##     ## ", "## ### ## ", "## ### ## ",
                "## ### ## ", "##     ## ", " #######  ", "  #####   ", "          " } },
};
#define NICONS ((int)(sizeof(s_icons) / sizeof(s_icons[0])))

static const char *const s_fonts[] = {
    "/text/font_eu_10.nftr", "/text/font_eu_10s.nftr", "/text/font_eu_10all.nftr",
};

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

static int patch_font(const char *path)
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
        for (i = 0; i < NICONS && w == 10 && h == 10 && bpp == 1; i++) {
            uint8_t bits[32];
            const int g = glyph_of(d, size, cmap, s_icons[i].code);
            int x, y;
            if (g < 0 || gsize > sizeof(bits) || cglp + 0x10 + (uint32_t)(g + 1) * gsize > size)
                continue;
            /* 1 bit a pixel, rows one after the other, the first pixel in the top bit */
            memset(bits, 0, sizeof(bits));
            for (y = 0; y < 10; y++)
                for (x = 0; x < 10; x++)
                    if (s_icons[i].rows[y][x] == '#')
                        bits[(y * 10 + x) >> 3] |= (uint8_t)(0x80 >> ((y * 10 + x) & 7));
            rom_patch(off + cglp + 0x10 + (uint32_t)g * gsize, bits, gsize);
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
    for (i = 0; i < (int)(sizeof(s_fonts) / sizeof(s_fonts[0])); i++)
        n += patch_font(s_fonts[i]);
    LOG("text: %d button icons drawn as the Vita's (3 fonts, 4 each)", n);
}
