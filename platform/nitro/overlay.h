#ifndef KH_NITRO_OVERLAY_H
#define KH_NITRO_OVERLAY_H

/* Copy every overlay's initial .data (before the game runs), for FS_LoadOverlayImage. */
void kh_overlays_snapshot(void);

#endif
