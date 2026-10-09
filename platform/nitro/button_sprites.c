/* The battle HUD's A, B, X and Y sprites (the shortcut buttons, the combo prompts) redrawn as
 * the Vita's Circle, Cross, Triangle and Square, the buttons in the same places (config
 * button_icons; the text's icons are nitro/button_glyphs.c). The game decompresses its files
 * through MI_ReadUncompLZ8 (asm_replacements.c), which tells this file where each one went
 * (MI_InitUncompContextLZ) and when it is whole: its tiles are then compared with the ones to
 * redraw, by hash (the table, tools/button_sprites.py, holds no ROM data), and the matching
 * ones get their new pixels. The tiles of an NCGR are 32 bytes at 16-byte steps from the
 * file's start. */
#include "nitro/button_sprites.h"

#include "config.h"
#include "log.h"

#include <stdint.h>
#include <string.h>

typedef struct {
    uint64_t hash;  /* FNV-1a of the original 32 bytes */
    uint32_t xor;   /* their eight words XORed: the quick test */
    /* a tile alike in two icons: the hash of the tile ctx_off bytes on (the one under it),
     * still the original's as the scan goes forward; 0 when the tile is the only one */
    uint32_t ctx_off;
    uint64_t ctx_hash;
    int variant; /* 0 always, 1 the buttons as placed, 2 with config confirm_cross (A on Cross) */
    int n;
    struct {
        uint8_t at, value;
    } edit[32];
} TileEdit;

static const TileEdit s_edits[] = {
#include "button_sprites.inc"
};
#define NEDITS ((int)(sizeof(s_edits) / sizeof(s_edits[0])))

static struct {
    void *ctx;
    uint8_t *dest;
    uint32_t size;
} s_open[8];
static int s_logged;

void kh_vita_uncomp_begin(void *context, void *dest, unsigned int size)
{
    int i, free_slot = 0;
    for (i = 0; i < 8; i++) {
        if (s_open[i].ctx == context) {
            free_slot = i;
            break;
        }
        if (!s_open[i].ctx)
            free_slot = i;
    }
    s_open[free_slot].ctx = context;
    s_open[free_slot].dest = dest;
    s_open[free_slot].size = size;
}

static uint64_t fnv64(const uint8_t *p)
{
    uint64_t h = 0xcbf29ce484222325ull;
    int i;
    for (i = 0; i < 32; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/* the first entry whose quick test is x: the table is sorted by it (tools/button_sprites.py) */
static int first_with_xor(uint32_t x)
{
    int lo = 0, hi = NEDITS;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (s_edits[mid].xor < x)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void patch(uint8_t *d, uint32_t size)
{
    uint32_t off;
    int k, done = 0;
    const int variant = kh_config.confirm_cross ? 2 : 1;
    for (off = 0; off + 32 <= size; off += 16) {
        uint32_t w[8], x;
        memcpy(w, d + off, 32);
        x = w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4] ^ w[5] ^ w[6] ^ w[7];
        for (k = first_with_xor(x); k < NEDITS && s_edits[k].xor == x; k++) {
            if ((!s_edits[k].variant || s_edits[k].variant == variant) &&
                s_edits[k].hash == fnv64(d + off) &&
                (!s_edits[k].ctx_off || (off + s_edits[k].ctx_off + 32 <= size &&
                                         s_edits[k].ctx_hash == fnv64(d + off + s_edits[k].ctx_off)))) {
                int e;
                for (e = 0; e < s_edits[k].n; e++)
                    d[off + s_edits[k].edit[e].at] = s_edits[k].edit[e].value;
                done++;
                off += 16; /* the tile's second half is its own */
                break;
            }
        }
    }
    if (done && s_logged++ < 16)
        LOG("text: %d sprite tiles drawn with the Vita's buttons (a file of %u bytes)", done,
            (unsigned)size);
}

void kh_vita_uncomp_done(void *context)
{
    int i;
    for (i = 0; i < 8; i++)
        if (s_open[i].ctx == context) {
            s_open[i].ctx = NULL;
            if (kh_config.button_icons && s_open[i].dest && s_open[i].size >= 32 &&
                s_open[i].size <= 4u * 1024 * 1024)
                patch(s_open[i].dest, s_open[i].size);
            return;
        }
}

/* The 3D textures: the combo prompt ("Y-COMBO" over the player, a 64x16 A3I5 texture with
 * the prompt twice: as shown and greyed) has its Y button redrawn as the Vita's Square, in
 * both. Found in a frame dump (0.4.21); named by the hash of its 1024 texel bytes, the bytes
 * drawn over it are the port's (a texel: alpha 7 << 5 | palette index; 0x0f white, 0x00 the
 * button's black, 0x08 its grey in the greyed one). */
static const struct {
    uint64_t hash;
    uint32_t n;
    int nedits;
    struct {
        uint16_t at;
        uint8_t value;
    } edit[64];
} s_tex_edits[] = {
    { 0xaebd117eabeed880ull, 1024, 54,
      { { 386, 0xe0 }, { 388, 0xe0 }, { 390, 0xe0 }, { 451, 0xef }, { 452, 0xef }, { 453, 0xef },
        { 514, 0xef }, { 515, 0xe0 }, { 517, 0xe0 }, { 518, 0xef }, { 578, 0xef }, { 579, 0xe0 },
        { 580, 0xe0 }, { 581, 0xe0 }, { 582, 0xef }, { 642, 0xef }, { 643, 0xe0 }, { 644, 0xe0 },
        { 645, 0xe0 }, { 646, 0xef }, { 706, 0xef }, { 707, 0xef }, { 709, 0xef }, { 710, 0xef },
        { 770, 0xe0 }, { 772, 0xe0 }, { 774, 0xe0 }, { 418, 0xe8 }, { 420, 0xe8 }, { 422, 0xe8 },
        { 483, 0xef }, { 484, 0xef }, { 485, 0xef }, { 546, 0xef }, { 547, 0xe8 }, { 549, 0xe8 },
        { 550, 0xef }, { 610, 0xef }, { 611, 0xe8 }, { 612, 0xe8 }, { 613, 0xe8 }, { 614, 0xef },
        { 674, 0xef }, { 675, 0xe8 }, { 676, 0xe8 }, { 677, 0xe8 }, { 678, 0xef }, { 738, 0xef },
        { 739, 0xef }, { 741, 0xef }, { 742, 0xef }, { 802, 0xe8 }, { 804, 0xe8 }, { 806, 0xe8 } } },
};

static uint64_t fnv64n(const uint8_t *p, uint32_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    uint32_t i;
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

int kh_button_texture(const uint8_t *texels, uint32_t n, uint8_t *out)
{
    uint64_t h;
    int k, e;
    if (!kh_config.button_icons)
        return 0;
    h = fnv64n(texels, n);
    for (k = 0; k < (int)(sizeof(s_tex_edits) / sizeof(s_tex_edits[0])); k++) {
        if (s_tex_edits[k].n != n || s_tex_edits[k].hash != h)
            continue;
        memcpy(out, texels, n);
        for (e = 0; e < s_tex_edits[k].nedits; e++)
            out[s_tex_edits[k].edit[e].at] = s_tex_edits[k].edit[e].value;
        return 1;
    }
    return 0;
}
