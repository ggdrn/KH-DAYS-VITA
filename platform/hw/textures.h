/* DS 3D textures: the texture-image and texture-palette slots as VRAMCNT maps them, and the
 * seven texture formats decoded to RGBA8888 (memory order R, G, B, A) for the GPU. Plain C,
 * no GL: hw/gpu3d.c caches the results, tools/gx3d_test checks them on the host. */
#ifndef KH_HW_TEXTURES_H
#define KH_HW_TEXTURES_H

#include <stdint.h>

/* Rebuild the slot map from the banks' VRAMCNT (once per rendered frame). */
void kh_tex_map_slots(void);

/* Texture size from TEXIMAGE_PARAM. */
static inline int kh_tex_width(uint32_t teximage) { return 8 << ((teximage >> 20) & 7); }
static inline int kh_tex_height(uint32_t teximage) { return 8 << ((teximage >> 23) & 7); }
static inline int kh_tex_format(uint32_t teximage) { return (teximage >> 26) & 7; }

/* A hash of everything the texture's appearance depends on: its parameters and the bytes of
 * texture and palette VRAM it reads. */
uint32_t kh_tex_hash(uint32_t teximage, uint32_t pltt);

/* Decode into out (width x height texels). */
void kh_tex_decode(uint32_t teximage, uint32_t pltt, uint32_t *out);

#endif
