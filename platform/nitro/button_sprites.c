/* The battle HUD's A, B, X and Y sprites (the shortcut buttons, the combo prompts) redrawn as
 * the Vita's Circle, Cross, Triangle and Square, the buttons in the same places (config
 * button_icons; the text's icons are nitro/button_glyphs.c). The game decompresses its files
 * through MI_ReadUncompLZ8 (asm_replacements.c), which tells this file where each one went
 * (MI_InitUncompContextLZ) and when it is whole: its tiles are then compared with the ones to
 * redraw, by hash (the table, tools/button_sprites.py, holds no ROM data), and the matching
 * ones get their new pixels. The tiles of an NCGR are 32 bytes at 16-byte steps from the
 * file's start. */
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

static void patch(uint8_t *d, uint32_t size)
{
    uint32_t off;
    int k, done = 0;
    const int variant = kh_config.confirm_cross ? 2 : 1;
    for (off = 0; off + 32 <= size; off += 16) {
        uint32_t w[8], x;
        memcpy(w, d + off, 32);
        x = w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4] ^ w[5] ^ w[6] ^ w[7];
        for (k = 0; k < NEDITS; k++) {
            if (s_edits[k].xor == x && (!s_edits[k].variant || s_edits[k].variant == variant) &&
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
    if (done && s_logged++ < 8)
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
