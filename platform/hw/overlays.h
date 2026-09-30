/* The game's 303 overlays. On the port all of them are linked in at once; "loading" one resets
 * its .bss (and later its .data) and runs its static initializer, as FS_LoadOverlay did, and its
 * FSOverlayInfo reports `entry` as the load address. */
#ifndef KH_HW_OVERLAYS_H
#define KH_HW_OVERLAYS_H

#include <stdint.h>

typedef struct {
    char *start, *end;
} KhRange;

enum { KH_OV_TEXT, KH_OV_RODATA, KH_OV_DATA, KH_OV_BSS, KH_OV_KINDS };

typedef struct {
    uint32_t id;
    uint32_t ds_ram;    /* where the DS loaded it */
    uint32_t ds_size;   /* .text + .rodata + .data */
    uint32_t ds_bss;
    char *bss_start;    /* its block in build/gen/ds_bss.S, or NULL */
    char *bss_end;
    void (*entry)(void); /* the function at ds_ram: the game calls an overlay's load address */
    void (*sinit)(void); /* NitroSDK static initializer, or NULL */
    KhRange host[KH_OV_KINDS]; /* where the port's link put its sections (build/gen/overlays.ld) */
} KhOverlay;

extern const KhOverlay kh_overlays[];
extern const int kh_overlay_count;

#endif
