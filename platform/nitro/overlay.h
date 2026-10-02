#ifndef KH_NITRO_OVERLAY_H
#define KH_NITRO_OVERLAY_H

/* Copy every overlay's initial .data (before the game runs), for FS_LoadOverlayImage. */
void kh_overlays_snapshot(void);
/* Whether overlay id is loaded now (its DS range not taken over by another since). */
int kh_overlay_loaded(int id);

#endif
