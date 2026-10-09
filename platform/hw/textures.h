/* DS 3D textures: the texture-image and texture-palette slots as VRAMCNT maps them, and the
 * seven texture formats decoded to RGBA8888 (memory order R, G, B, A) for the GPU. Plain C,
 * no GL: hw/gpu3d.c caches the results, tools/gx3d_test checks them on the host. */
#ifndef KH_HW_TEXTURES_H
#define KH_HW_TEXTURES_H

#include <stdint.h>

/* Rebuild the slot map from the banks' VRAMCNT registers (once per rendered frame). Returns a
 * count that changes whenever one of VRAMCNT A-G did. */
uint32_t kh_tex_map_slots(void);

/* Texture size from TEXIMAGE_PARAM. */
static inline int kh_tex_width(uint32_t teximage) { return 8 << ((teximage >> 20) & 7); }
static inline int kh_tex_height(uint32_t teximage) { return 8 << ((teximage >> 23) & 7); }
static inline int kh_tex_format(uint32_t teximage) { return (teximage >> 26) & 7; }

/* A hash of everything the texture's appearance depends on: its parameters and the bytes of
 * texture and palette VRAM it reads. */
uint32_t kh_tex_hash(uint32_t teximage, uint32_t pltt);

/* Diagnostics: bank-frames where VRAMCNT differed from what hw/vram.c applied, since the
 * last call. */
uint32_t kh_tex_stale_vramcnt(void);
/* Diagnostics: bits 0-3 the texture slots mapped, 8-13 the palette slots. */
unsigned kh_tex_slots_mapped(void);
/* Diagnostics: the texture's texel bytes are all zero (an unmapped slot, or not loaded yet). */
int kh_tex_source_empty(uint32_t teximage);

/* Decode into out (width x height texels). */
void kh_tex_decode(uint32_t teximage, uint32_t pltt, uint32_t *out);

/* Diagnostics (the frame dump): the texture's texel bytes and then its palette's colours (16
 * bits each) into out, at most max bytes; returns the bytes written, *texels the texel part.
 * 4x4-compressed textures give their texels only. */
uint32_t kh_tex_raw(uint32_t teximage, uint32_t pltt, uint8_t *out, uint32_t max, uint32_t *texels);

#endif
