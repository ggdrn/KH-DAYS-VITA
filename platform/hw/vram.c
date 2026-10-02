/* VRAM banks A-I and their mapping (VRAMCNT, 0x04000240-0x04000249).
 *
 * Every bank has its own storage, its home. The CPU-visible views the game writes through (the
 * LCDC window kh_vram_lcdc, engine A/B BG, engine A/B OBJ) are separate buffers (platform/
 * compat/kh_hw_map.h). When a bank is mapped into a view, its contents are copied there; when
 * it leaves the view, they are copied back home. Banks mapped to texture, texture-palette,
 * extended-palette or ARM7 slots are not CPU-visible on the DS either: they stay home and the
 * renderer reads them there (kh_vram_bank_home). Remaps are rare (scene changes, texture
 * uploads), and this keeps the game's pointer arithmetic within a view valid.
 *
 * The LCDC window is a view like the others: on the DS, writes to the LCDC address of a bank
 * that is not in LCDC go nowhere. The game clears the whole window (0xa4000 bytes) with only
 * bank D in LCDC at the end of an event (Ov023_Teardown): with the banks living in the window,
 * that wiped the field's textures and dialogue scenes showed a black field (0.0.37-0.0.65).
 *
 * kh_vram_sync() applies what VRAMCNT says. The NitroSDK functions that write VRAMCNT call it
 * on exit (KH_VRAM_SYNC_ON_EXIT in the decomp patch). */
#include "hw/vram.h"

#include "hw/io.h"
#include "hw/memmap.h"
#include "log.h"

#include <string.h>

enum { VIEW_NONE, VIEW_LCDC, VIEW_BG_A, VIEW_BG_B, VIEW_OBJ_A, VIEW_OBJ_B };

typedef struct {
    uint32_t lcdc; /* offset of the home slot in kh_vram_lcdc */
    uint32_t size;
    uint16_t cnt_addr_off; /* VRAMCNT register offset from 0x04000240 */
} Bank;

static const Bank s_banks[KH_VRAM_BANKS] = {
    { 0x00000, 0x20000, 0 }, /* A */
    { 0x20000, 0x20000, 1 }, /* B */
    { 0x40000, 0x20000, 2 }, /* C */
    { 0x60000, 0x20000, 3 }, /* D */
    { 0x80000, 0x10000, 4 }, /* E */
    { 0x90000, 0x04000, 5 }, /* F */
    { 0x94000, 0x04000, 6 }, /* G */
    { 0x98000, 0x08000, 8 }, /* H (0x247 is WRAMCNT) */
    { 0xa0000, 0x04000, 9 }, /* I */
};

static uint8_t s_applied[KH_VRAM_BANKS]; /* VRAMCNT values in effect; 0 = disabled, home */
static uint8_t s_store[0xa4000] __attribute__((aligned(32))); /* the homes, at their LCDC offsets */

static uint8_t *store_ptr(int b)
{
    return s_store + s_banks[b].lcdc;
}

typedef struct {
    int view;
    uint32_t off;
} Placement;

/* Where a VRAMCNT value puts bank b, for the CPU-visible views; VIEW_NONE otherwise. */
static Placement placement(int b, uint8_t cnt)
{
    Placement p = { VIEW_NONE, 0 };
    int mst = cnt & 7, ofs = (cnt >> 3) & 3;

    if (!(cnt & 0x80))
        return p;
    if (mst == 0) {
        p.view = VIEW_LCDC;
        p.off = s_banks[b].lcdc;
        return p;
    }
    switch (b) {
    case 0: case 1: /* A, B */
        if (mst == 1) p.view = VIEW_BG_A, p.off = 0x20000u * ofs;
        else if (mst == 2) p.view = VIEW_OBJ_A, p.off = 0x20000u * (ofs & 1);
        break;
    case 2: /* C */
        if (mst == 1) p.view = VIEW_BG_A, p.off = 0x20000u * ofs;
        else if (mst == 4) p.view = VIEW_BG_B, p.off = 0;
        break;
    case 3: /* D */
        if (mst == 1) p.view = VIEW_BG_A, p.off = 0x20000u * ofs;
        else if (mst == 4) p.view = VIEW_OBJ_B, p.off = 0;
        break;
    case 4: /* E */
        if (mst == 1) p.view = VIEW_BG_A, p.off = 0;
        else if (mst == 2) p.view = VIEW_OBJ_A, p.off = 0;
        break;
    case 5: case 6: /* F, G */
        if (mst == 1 || mst == 2) {
            p.view = mst == 1 ? VIEW_BG_A : VIEW_OBJ_A;
            p.off = 0x4000u * (ofs & 1) + 0x10000u * (ofs >> 1);
        }
        break;
    case 7: /* H */
        if (mst == 1) p.view = VIEW_BG_B, p.off = 0;
        break;
    case 8: /* I */
        if (mst == 1) p.view = VIEW_BG_B, p.off = 0x8000;
        else if (mst == 2) p.view = VIEW_OBJ_B, p.off = 0;
        break;
    }
    return p;
}

static uint8_t *view_ptr(Placement p, uint32_t size)
{
    uint8_t *base;
    uint32_t len;
    switch (p.view) {
    case VIEW_BG_A: base = kh_vram_bg_a, len = sizeof(kh_vram_bg_a); break;
    case VIEW_BG_B: base = kh_vram_bg_b, len = sizeof(kh_vram_bg_b); break;
    case VIEW_OBJ_A: base = kh_vram_obj_a, len = sizeof(kh_vram_obj_a); break;
    case VIEW_OBJ_B: base = kh_vram_obj_b, len = sizeof(kh_vram_obj_b); break;
    case VIEW_LCDC: base = kh_vram_lcdc, len = sizeof(kh_vram_lcdc); break;
    default: return NULL; /* not CPU-visible */
    }
    if (p.off + size > len)
        return NULL;
    return base + p.off;
}

uint8_t *kh_vram_bank_home(int bank)
{
    /* a bank in LCDC is current in the window, where the CPU writes it */
    const uint8_t cnt = s_applied[bank];
    if ((cnt & 0x80) && (cnt & 7) == 0)
        return kh_vram_lcdc + s_banks[bank].lcdc;
    return store_ptr(bank);
}

uint8_t kh_vram_bank_cnt(int bank)
{
    return s_applied[bank];
}

/* bumped whenever a bank that can hold textures or texture palettes (A-G) is remapped: the
 * game only writes them while they are in LCDC, so between two bumps they do not change */
static volatile uint32_t s_tex_gen;

void kh_vram_sync(void)
{
    uint8_t now[KH_VRAM_BANKS];
    int b;

    for (b = 0; b < KH_VRAM_BANKS; b++)
        now[b] = kh_ds_io[0x240 + s_banks[b].cnt_addr_off];

    /* first bring every bank that moves back home, then place them: two banks can trade
     * places in one call */
    for (b = 0; b < KH_VRAM_BANKS; b++) {
        uint8_t *view;
        if (now[b] == s_applied[b])
            continue;
        view = view_ptr(placement(b, s_applied[b]), s_banks[b].size);
        if (view)
            memcpy(store_ptr(b), view, s_banks[b].size);
    }
    for (b = 0; b < KH_VRAM_BANKS; b++) {
        uint8_t *view;
        if (now[b] == s_applied[b])
            continue;
        view = view_ptr(placement(b, now[b]), s_banks[b].size);
        if (view)
            memcpy(view, store_ptr(b), s_banks[b].size);
        if (kh_log_verbose) {
            static uint32_t logged;
            if (logged++ < 3000) {
                const uint32_t *w = (const uint32_t *)store_ptr(b);
                uint32_t i, nz = 0;
                for (i = 0; i < s_banks[b].size / 4; i++)
                    nz += w[i] != 0;
                LOG("vram: bank %c %02x -> %02x, %u%% of it non-zero", 'A' + b, s_applied[b], now[b],
                    (unsigned)(nz * 100 / (s_banks[b].size / 4)));
            }
        }
        s_applied[b] = now[b];
        if (b <= 6)
            s_tex_gen++;
    }
}

void kh_vram_touch(void)
{
    s_tex_gen++;
}

uint32_t kh_vram_tex_generation(void)
{
    return s_tex_gen;
}

void kh_vram_sync_cleanup(int *unused)
{
    (void)unused;
    kh_vram_sync();
}
