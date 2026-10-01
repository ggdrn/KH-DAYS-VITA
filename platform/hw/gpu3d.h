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
} KhGpu3dStats;
void kh_gpu3d_take_stats(KhGpu3dStats *out);

#endif
