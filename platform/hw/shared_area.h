#ifndef KH_HW_SHARED_AREA_H
#define KH_HW_SHARED_AREA_H

#define KH_SHARED_AREA_BASE 0x027ff000u
#define KH_SHARED_AREA_SIZE 0x1000u

extern unsigned char kh_ds_shared_area[KH_SHARED_AREA_SIZE];

/* Host pointer for a DS address inside the shared area. */
#define KH_SHARED(addr) ((void *)(kh_ds_shared_area + ((addr) - KH_SHARED_AREA_BASE)))

#endif
