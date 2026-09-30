/* KH_HW(addr) on the Vita: a fixed DS address -> the host memory that stands in for it.
 *
 * The decomp's include/nitro/kh_hw.h pulls this in under PLATFORM_VITA (on the DS KH_HW is the
 * identity). tools/rewrite_decomp.py wraps the fixed addresses of the game code in KH_HW.
 *
 * The result is an integer (uintptr_t), as the literal was, so casts and arithmetic around it
 * keep working. With a constant address the chain folds to `symbol + offset`: no run-time cost,
 * and it stays a constant expression for static initializers. Only the registers whose value
 * the hardware computes on read (divider and square-root results, VCOUNT/DISPSTAT) go through
 * a function, which refreshes them first; that branch is never evaluated for other addresses,
 * so it does not break constant initializers either.
 *
 * Map (sizes are the DS's; mirrors fold onto the first copy):
 *   0x04000000 +0x2000   I/O: engine A, DMA, timers, keys, IPC, VRAMCNT, CP, 3D, engine B
 *   0x04100000 +0x20     I/O: IPC FIFO receive, card data
 *   0x05000000           palettes, 2 KiB (mirrored)
 *   0x06000000 +512 KiB  VRAM view: engine A BG        0x06200000 +128 KiB  engine B BG
 *   0x06400000 +256 KiB  VRAM view: engine A OBJ       0x06600000 +128 KiB  engine B OBJ
 *   0x06800000 +656 KiB  VRAM view: LCDC (banks A-I in order)
 *   0x07000000           OAM, 2 KiB (mirrored)
 *   0x027ff000 +4 KiB    main RAM shared area
 *   0x027e0000 +16 KiB   DTCM (the IRQ vector table, the IRQ check word, the SDK's stacks)
 * Anything else is left as the DS address, which faults on the Vita and shows in the log. */
#ifndef KH_HW_MAP_H
#define KH_HW_MAP_H

#include <stdint.h>

extern unsigned char kh_ds_io[0x2000];
extern unsigned char kh_ds_io_hi[0x20];
extern unsigned char kh_ds_palette[0x800];
extern unsigned char kh_ds_oam[0x800];
extern unsigned char kh_vram_bg_a[0x80000];
extern unsigned char kh_vram_bg_b[0x20000];
extern unsigned char kh_vram_obj_a[0x40000];
extern unsigned char kh_vram_obj_b[0x20000];
extern unsigned char kh_vram_lcdc[0xa4000];
extern unsigned char kh_ds_shared_area[0x1000];
extern unsigned char kh_ds_dtcm[0x4000];

/* refresh a computed register block and return its host address */
extern uintptr_t kh_hw_sync_div(void);   /* 0x040002a0: DIV_RESULT, DIVREM_RESULT */
extern uintptr_t kh_hw_sync_sqrt(void);  /* 0x040002b4: SQRT_RESULT */
extern uintptr_t kh_hw_sync_disp(void);  /* 0x04000004: DISPSTAT, VCOUNT */
extern uintptr_t kh_hw_sync_timers(void); /* 0x04000100: TM0CNT_L .. TM3CNT_H */
extern uintptr_t kh_hw_sync_gxstat(void); /* 0x04000600: GXSTAT */

/* VRAMCNT writers run this at the top of their body: the banks are moved when the function
 * returns, whichever return it takes (platform/hw/vram.c). */
extern void kh_vram_sync_cleanup(int *unused);
#define KH_VRAM_SYNC_ON_EXIT() \
    int kh__vram_sync __attribute__((cleanup(kh_vram_sync_cleanup), unused)) = 0

#define KH__A(a) ((uint32_t)(a))
#define KH__IN(a, base, size) (KH__A(a) - (uint32_t)(base) < (uint32_t)(size))
#define KH__AT(arr, off) ((uintptr_t)(arr) + (uint32_t)(off))

#define KH_HW(a)                                                                              \
    (KH__IN(a, 0x040002a0, 0x10)  ? kh_hw_sync_div() + (KH__A(a) - 0x040002a0u) :             \
     KH__IN(a, 0x040002b4, 4)     ? kh_hw_sync_sqrt() + (KH__A(a) - 0x040002b4u) :            \
     KH__IN(a, 0x04000004, 4)     ? kh_hw_sync_disp() + (KH__A(a) - 0x04000004u) :            \
     KH__IN(a, 0x04000100, 0x10)  ? kh_hw_sync_timers() + (KH__A(a) - 0x04000100u) :          \
     KH__IN(a, 0x04000600, 4)     ? kh_hw_sync_gxstat() + (KH__A(a) - 0x04000600u) :          \
     KH__IN(a, 0x04000000, 0x2000) ? KH__AT(kh_ds_io, KH__A(a) - 0x04000000u) :               \
     KH__IN(a, 0x04100000, 0x20)  ? KH__AT(kh_ds_io_hi, KH__A(a) - 0x04100000u) :             \
     KH__IN(a, 0x05000000, 0x01000000) ? KH__AT(kh_ds_palette, KH__A(a) & 0x7ffu) :           \
     KH__IN(a, 0x06000000, 0x00200000) ? KH__AT(kh_vram_bg_a, KH__A(a) & 0x7ffffu) :          \
     KH__IN(a, 0x06200000, 0x00200000) ? KH__AT(kh_vram_bg_b, KH__A(a) & 0x1ffffu) :          \
     KH__IN(a, 0x06400000, 0x00200000) ? KH__AT(kh_vram_obj_a, KH__A(a) & 0x3ffffu) :         \
     KH__IN(a, 0x06600000, 0x00200000) ? KH__AT(kh_vram_obj_b, KH__A(a) & 0x1ffffu) :         \
     KH__IN(a, 0x06800000, 0xa4000) ? KH__AT(kh_vram_lcdc, KH__A(a) - 0x06800000u) :          \
     KH__IN(a, 0x07000000, 0x01000000) ? KH__AT(kh_ds_oam, KH__A(a) & 0x7ffu) :               \
     KH__IN(a, 0x027ff000, 0x1000) ? KH__AT(kh_ds_shared_area, KH__A(a) - 0x027ff000u) :      \
     KH__IN(a, 0x027e0000, 0x4000) ? KH__AT(kh_ds_dtcm, KH__A(a) - 0x027e0000u) :             \
     (uintptr_t)KH__A(a))

#endif
