/* Host test for platform/hw/gx3d.c: command streams in (packed FIFO, command ports, STMIA-style
 * matrix copies), then the published frame and the read-back registers are checked.
 *
 *     tools/gx3d_test/run.sh */
#include "hw/gx3d.h"
#include "hw/io.h"
#include "hw/textures.h"

#include <math.h>
#include <stdio.h>

uint8_t kh_ds_io[KH_IO_SIZE];

/* banks A-G as the texture code sees them: A texture slot 1, B slot 0, E palette */
static uint8_t s_bank[7][0x20000];
static uint8_t s_cnt[9] = { 0x80 | 3 | 1 << 3, 0x80 | 3, 0, 0, 0x80 | 3, 0, 0 };
uint8_t *kh_vram_bank_home(int bank) { return s_bank[bank]; }
uint8_t kh_vram_bank_cnt(int bank) { return s_cnt[bank]; }

static int s_fail;
#define CHECK(c, ...) do { if (!(c)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define FIFO ((volatile void *)(kh_ds_io + 0x400))
#define PORT(cmd) ((volatile void *)(kh_ds_io + 0x400 + (cmd) * 4))
#define FX(f) ((uint32_t)(int32_t)((f) * 4096.0))
#define XY(x, y) ((FX(x) & 0xffff) | (FX(y) << 16))

static void fifo(uint32_t w) { kh_gx_cmd(FIFO, w); }
static void port(int cmd, uint32_t p) { kh_gx_cmd(PORT(cmd), p); }

static void triangle(uint32_t attr, int flip)
{
    port(0x10, 0);    /* projection */
    port(0x15, 0);    /* identity */
    port(0x10, 2);    /* position & vector */
    port(0x15, 0);
    port(0x29, attr);
    port(0x40, 0);    /* triangles */
    port(0x20, 0x7fff);
    port(0x23, XY(-0.5, -0.5)); port(0x23, FX(0.0) & 0xffff);
    if (flip) {
        port(0x23, XY(0.0, 0.5)); port(0x23, 0);
        port(0x23, XY(0.5, -0.5)); port(0x23, 0);
    } else {
        port(0x23, XY(0.5, -0.5)); port(0x23, 0);
        port(0x23, XY(0.0, 0.5)); port(0x23, 0);
    }
    port(0x41, 0);
}

int main(void)
{
    const KhGxFrame *f;
    int i;
    kh_gx3d_init();
    KH_IO32(0x04000600) = 0x06000000;
    CHECK(kh_gx3d_acquire() == NULL, "a frame before any swap");

    /* 1: command ports, front-facing triangle, full viewport */
    port(0x60, 0xbfff0000);
    triangle(0x001f00c0, 0);
    port(0x50, 0);
    f = kh_gx3d_acquire();
    CHECK(f && f->npoly == 1 && f->nvtx == 3, "1: %d polys %d verts", f ? f->npoly : -1, f ? f->nvtx : -1);
    if (f && f->nvtx == 3) {
        CHECK(fabsf(f->vtx[0].x / f->vtx[0].w + 0.5f) < 0.01f && fabsf(f->vtx[2].y / f->vtx[2].w - 0.5f) < 0.01f,
              "1: positions %f %f", f->vtx[0].x / f->vtx[0].w, f->vtx[2].y / f->vtx[2].w);
        CHECK(f->vtx[0].r == 63 && f->vtx[0].g == 63, "1: colour %d", f->vtx[0].r);
        CHECK(!f->poly[0].translucent, "1: opaque");
        CHECK(f->poly[0].ytop == 48 && f->poly[0].ybottom == 144, "1: rows %d-%d", f->poly[0].ytop, f->poly[0].ybottom);
    }

    /* 2: same triangle wound the other way, front only -> culled; both sides -> drawn */
    triangle(0x001f0080, 1);
    triangle(0x001f00c0, 1);
    port(0x50, 0);
    f = kh_gx3d_acquire();
    CHECK(f && f->npoly == 1 && f->nvtx == 6, "2: %d polys %d verts", f ? f->npoly : -1, f ? f->nvtx : -1);

    /* 3: packed FIFO: MTX_MODE(2) MTX_IDENTITY BEGIN(quad strip) COLOR, then 6 VTX_10 */
    fifo(0x10 | 0x15 << 8 | 0x29 << 16 | 0x40 << 24);
    fifo(2);                 /* MTX_MODE param (IDENTITY takes none) */
    fifo(0x001f00c0);        /* POLYGON_ATTR */
    fifo(3);                 /* BEGIN quad strip */
    for (i = 0; i < 6; i++) {
        fifo(0x24);          /* one VTX_10 per packed word */
        fifo(((i & 1) ? 0x40u : 0x3c0u) | ((uint32_t)(i / 2 * 0x10) << 10));
    }
    fifo(0x00000050); fifo(0); /* SWAP */
    f = kh_gx3d_acquire();
    CHECK(f && f->npoly == 2 && f->nvtx == 6, "3: %d polys %d verts", f ? f->npoly : -1, f ? f->nvtx : -1);
    fifo(0); /* an all-zero word: one NOP, nothing pending after it */
    fifo(0x11 | 0x11 << 8 | 0x11 << 16);  /* three PUSHes, no parameters */
    kh_gx3d_sync_gxstat();
    CHECK(((KH_IO32(0x04000600) >> 8) & 31) == 3, "3: stack level %u", (unsigned)(KH_IO32(0x04000600) >> 8) & 31);
    port(0x12, 3);           /* POP 3 */
    kh_gx3d_sync_gxstat();
    CHECK(((KH_IO32(0x04000600) >> 8) & 31) == 0, "3: stack level after pop %u", (unsigned)(KH_IO32(0x04000600) >> 8) & 31);

    /* 4: MTX_LOAD_4x4 as MI_Copy64B writes it (incrementing addresses), clip read-back */
    {
        uint32_t m[16] = { FX(2), 0, 0, 0, 0, FX(3), 0, 0, 0, 0, FX(1), 0, FX(1), FX(2), FX(3), FX(1) };
        port(0x10, 1);
        for (i = 0; i < 16; i++)
            kh_gx_cmd((volatile uint32_t *)PORT(0x16) + i, m[i]);
        port(0x10, 0);
        port(0x15, 0);
        port(0x71, XY(1.0, 1.0)); port(0x71, FX(1.0) & 0xffff); /* POS_TEST (1,1,1) */
        CHECK(KH_IO32(0x04000640) == FX(2) && KH_IO32(0x04000654) == FX(3) && KH_IO32(0x04000670) == FX(1),
              "4: clip %08x %08x %08x", (unsigned)KH_IO32(0x04000640), (unsigned)KH_IO32(0x04000654),
              (unsigned)KH_IO32(0x04000670));
        CHECK(KH_IO32(0x04000620) == FX(3) && KH_IO32(0x04000624) == FX(5) && KH_IO32(0x04000628) == FX(4),
              "4: pos test %08x %08x %08x", (unsigned)KH_IO32(0x04000620), (unsigned)KH_IO32(0x04000624),
              (unsigned)KH_IO32(0x04000628));
    }

    /* 5: MTX_SCALE then MTX_TRANS on the identity, box test in and out of view */
    port(0x10, 1); port(0x15, 0);
    port(0x1b, FX(0.5)); port(0x1b, FX(0.5)); port(0x1b, FX(0.5));
    port(0x1c, FX(1)); port(0x1c, 0); port(0x1c, 0);
    CHECK(KH_IO32(0x04000640) == FX(0.5) && KH_IO32(0x04000670) == FX(0.5), "5: scale/trans %08x %08x",
          (unsigned)KH_IO32(0x04000640), (unsigned)KH_IO32(0x04000670));
    port(0x15, 0);
    port(0x70, XY(-0.25, -0.25)); port(0x70, (FX(-0.25) & 0xffff) | FX(0.5) << 16); port(0x70, (FX(0.5) & 0xffff) | FX(0.5) << 16);
    kh_gx3d_sync_gxstat();
    CHECK(KH_IO32(0x04000600) & 2, "5: box in view");
    port(0x70, XY(3, 3)); port(0x70, (FX(3) & 0xffff) | FX(0.5) << 16); port(0x70, (FX(0.5) & 0xffff) | FX(0.5) << 16);
    kh_gx3d_sync_gxstat();
    CHECK(!(KH_IO32(0x04000600) & 2), "5: box out of view");

    /* 6: translucent sort: two alpha-16 triangles, the lower one submitted first */
    port(0x10, 2); port(0x15, 0);
    port(0x29, 0x001000c0); port(0x40, 0);
    port(0x23, XY(-0.5, -0.9)); port(0x23, 0); port(0x23, XY(0.5, -0.9)); port(0x23, 0); port(0x23, XY(0, -0.5)); port(0x23, 0);
    port(0x23, XY(-0.5, 0.5)); port(0x23, 0); port(0x23, XY(0.5, 0.5)); port(0x23, 0); port(0x23, XY(0, 0.9)); port(0x23, 0);
    port(0x50, 0);
    f = kh_gx3d_acquire();
    CHECK(f && f->npoly == 2 && f->poly[f->order[0]].ytop < f->poly[f->order[1]].ytop, "6: sort");
    port(0x50, 1); /* manual sort, empty frame */
    f = kh_gx3d_acquire();
    CHECK(f && f->npoly == 0 && f->swap == 1, "6: empty manual-sort frame");

    /* 7: textures. 16-colour 8x8 at 0x100 (slot 0 = bank B), colour 0 transparent */
    {
        static uint32_t out[64 * 64];
        uint16_t *pal = (uint16_t *)s_bank[4];
        uint32_t ti, h1;
        pal[8 + 1] = 0x001f;           /* palette base 1 (16 bytes): entry 1 red */
        pal[8 + 2] = 0x7c00;           /* entry 2 blue */
        s_bank[1][0x100] = 0x21;       /* texels 0,1 = 1,2 */
        kh_tex_map_slots();
        ti = (0x100 / 8) | 3u << 26 | 1u << 29; /* 8x8, 16-colour, colour 0 clear */
        kh_tex_decode(ti, 1, out);
        CHECK(out[0] == 0xff0000ffu && out[1] == 0xffff0000u && out[2] == 0, "7: 16-colour %08x %08x %08x",
              (unsigned)out[0], (unsigned)out[1], (unsigned)out[2]);
        h1 = kh_tex_hash(ti, 1);
        pal[8 + 2] = 0x03e0;
        CHECK(kh_tex_hash(ti, 1) != h1, "7: hash follows the palette");
        /* 4x4: 8x8 texels at slot 0 offset 0 (4 blocks), infos in slot 1 (bank A) at 0;
         * block 0 mode 1 with palette offset 0 -> colours red, green, their mean, clear */
        pal[0] = 0x001f; pal[1] = 0x03e0;
        s_bank[1][0] = 0xe4;           /* row 0: texels 0,1,2,3 */
        s_bank[0][0] = 0x00; s_bank[0][1] = 0x40; /* info: offset 0, mode 1 */
        kh_tex_decode(5u << 26, 0, out);
        CHECK(out[0] == 0xff0000ffu && out[1] == 0xff00ff00u && out[2] == 0xff007f7fu && out[3] == 0,
              "7: 4x4 %08x %08x %08x %08x", (unsigned)out[0], (unsigned)out[1], (unsigned)out[2], (unsigned)out[3]);
    }

    printf(s_fail ? "gx3d_test: %d failures\n" : "gx3d_test: ok\n", s_fail);
    return s_fail != 0;
}
