#ifndef KH_HW_VRAM_H
#define KH_HW_VRAM_H

#include <stdint.h>

#define KH_VRAM_BANKS 9 /* A-I */

/* Apply VRAMCNT: move banks between their home and the CPU-visible views. */
void kh_vram_sync(void);
/* For __attribute__((cleanup)): KH_VRAM_SYNC_ON_EXIT in the decomp's kh_hw_map.h users. */
void kh_vram_sync_cleanup(int *unused);

/* A bank's permanent storage (its LCDC slot). Current while the bank is not in a view. */
uint8_t *kh_vram_bank_home(int bank);
/* Changes whenever a bank A-G is remapped (texture data can only change in between). */
uint32_t kh_vram_tex_generation(void);
/* The VRAMCNT value in effect for a bank. */
uint8_t kh_vram_bank_cnt(int bank);

#endif
