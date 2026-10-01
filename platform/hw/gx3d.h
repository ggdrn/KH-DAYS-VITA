/* The DS geometry engine: GX commands (the packed FIFO at 0x04000400 and the command ports
 * 0x04000440-0x040005cb) in, a per-frame list of transformed polygons out.
 *
 * Commands run synchronously, as they arrive, so the read-back registers (GXSTAT stack levels
 * and test results, RAM counts, the clip and vector matrices) are current whenever the game
 * reads them. SWAP_BUFFERS closes the frame being built and publishes it, with a snapshot of the
 * rendering-engine registers, for the rasterizer (kh_gx3d_acquire). */
#ifndef KH_HW_GX3D_H
#define KH_HW_GX3D_H

#include <stdint.h>

#define KH_GX_MAX_VERTICES 6144
#define KH_GX_MAX_POLYGONS 2048

typedef struct {
    /* clip space with the polygon's viewport folded in: x/w, y/w are GL NDC over the whole
     * 256x192 screen (y up). z and w are the DS's own (4.12 units), for depth. */
    float x, y, z, w;
    float s, t;       /* texture coordinates in texels */
    uint8_t r, g, b;  /* 0-63, the rasterizer's 6-bit vertex colour */
    uint8_t a;        /* the polygon's alpha (POLYGON_ATTR 16-20), 0-31 */
} KhGxVertex;

typedef struct {
    uint16_t v[4];     /* indices into the frame's vertices */
    uint8_t count;     /* 3 or 4 */
    uint8_t translucent;
    int16_t ytop, ybottom; /* screen rows, for the translucent sort */
    uint32_t attr;     /* POLYGON_ATTR latched at BEGIN_VTXS */
    uint32_t teximage; /* TEXIMAGE_PARAM */
    uint32_t pltt;     /* PLTT_BASE */
} KhGxPolygon;

typedef struct {
    KhGxVertex vtx[KH_GX_MAX_VERTICES];
    KhGxPolygon poly[KH_GX_MAX_POLYGONS];
    uint16_t order[KH_GX_MAX_POLYGONS]; /* drawing order: opaque, then sorted translucent */
    int nvtx, npoly;
    uint32_t swap;        /* SWAP_BUFFERS parameter: bit 0 manual sort, bit 1 w-buffering */
    uint32_t disp3dcnt;   /* 0x04000060 */
    uint8_t regs[0x90];   /* 0x04000330-0x040003bf: edge, alpha test, clear, fog, toon */
    uint32_t serial;      /* counts published frames */
} KhGxFrame;

void kh_gx3d_init(void);

/* A word written to a GX register as the decomp's KH_GX_CMD (or a FIFO copy) does it. */
void kh_gx_cmd(volatile void *reg, unsigned long value);

/* True when a host pointer lies on the geometry FIFO or a command port. */
int kh_gx3d_is_port(const volatile void *p);

/* GXSTAT as the hardware would show it (0x04000600), after the game's write-1-to-clear. */
void kh_gx3d_sync_gxstat(void);

/* The latest published frame, or NULL before the first SWAP_BUFFERS. Valid until the next
 * kh_gx3d_acquire. */
const KhGxFrame *kh_gx3d_acquire(void);

/* SWAP_BUFFERS so far: the game's main loop sends one at the end of every frame it completes,
 * 2D-only screens included (main.c), so it also counts the game's frames. */
uint32_t kh_gx3d_serial(void);

typedef struct {
    uint32_t frames, polygons, vertices, commands, unknown, overflows, culled;
} KhGx3dStats;
/* Totals since the last call. */
void kh_gx3d_take_stats(KhGx3dStats *out);

#endif
