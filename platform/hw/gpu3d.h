/* The DS 3D rendering engine on the Vita's GPU (vitaGL): a frame from the geometry engine
 * (hw/gx3d.h) is drawn into an off-screen 256x192 x scale target, which the presentation
 * composites with engine A's 2D layers (platform/core/video.c). */
#ifndef KH_HW_GPU3D_H
#define KH_HW_GPU3D_H

#include "hw/gx3d.h"

/* scale: the internal resolution, 1-3 times the DS's. 0 when the shaders failed to build. */
int kh_gpu3d_init(int scale);

/* Draw the frame; returns the GL texture holding the 3D layer (premultiplied RGBA, GL
 * orientation: row 0 is the bottom of the screen), 0 when there is nothing to show. */
unsigned kh_gpu3d_render(const KhGxFrame *frame);

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
} KhGpu3dStats;
void kh_gpu3d_take_stats(KhGpu3dStats *out);

#endif
