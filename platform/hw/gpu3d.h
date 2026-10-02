/* The DS 3D rendering engine on the Vita's GPU (vitaGL): a frame from the geometry engine
 * (hw/gx3d.h) is drawn into an off-screen 256x192 x scale target, which the presentation
 * composites with engine A's 2D layers (platform/core/video.c). */
#ifndef KH_HW_GPU3D_H
#define KH_HW_GPU3D_H

#include "hw/gx3d.h"

/* scale: the internal resolution, 1-4 times the DS's. 0 when the shaders failed to build. */
int kh_gpu3d_init(int scale);

/* Draw the frame; returns the GL texture holding the 3D layer (premultiplied RGBA, GL
 * orientation: row 0 is the bottom of the screen), 0 when there is nothing to show. */
unsigned kh_gpu3d_render(const KhGxFrame *frame);

/* Before kh_gpu3d_render, with the helper core free: decodes the frame's new and changed
 * textures on both cores (platform/core/workers.h). Optional; render does the rest. */
void kh_gpu3d_prepare(const KhGxFrame *frame);

/* Diagnosis switches, cycled with L+R+Circle: 0 normal, 1 opaque polygons in the frame's
 * order (no grouping by state), 2 every texture re-hashed every frame, 3 texture-coordinate
 * generation ignored (raw TEXCOORD values). */
extern volatile int kh_gpu3d_debug;
#define KH_GPU3D_DEBUG_MODES 4

/* Write the next rendered frame's textures (TGA) and polygons (text) to
 * ux0:data/khdays/dump/, for diagnosis. Any thread. */
void kh_gpu3d_request_dump(void);

/* a 32-bit TGA, px as RGBA in memory order, top row first */
void kh_gpu3d_dump_tga(const char *path, const uint32_t *px, int w, int h);

typedef struct {
    uint32_t render_us, batches, textures_decoded, textures_live, skipped;
    uint32_t fmt[8];      /* textures decoded per format */
    uint32_t empty_src;   /* of them, read from all-zero texture VRAM */
    uint32_t slots;       /* kh_tex_slots_mapped at the last frame */
    uint32_t modes[4];    /* polygons drawn per mode: modulation, decal, toon/highlight, shadow */
    uint32_t disp3dcnt;   /* of the last frame */
    uint32_t texgen[4];   /* textured polygons per texture-coordinate generation mode */
    uint32_t flat_st;     /* textured polygons whose vertices all have the same s,t */
    uint32_t depth_equal; /* polygons drawn with the depth-equal test */
    uint32_t tex_wanted, tex_none; /* polygons with a texture format; of them drawn without */
    uint32_t prepare_us, burst_max; /* parallel texture decoding: time, most in one frame */
    uint32_t interpolated; /* frames shown after a halfway mix (60 fps interpolation) */
} KhGpu3dStats;
void kh_gpu3d_take_stats(KhGpu3dStats *out);

#endif
