/* Names for ROM offsets from the cartridge's file system (FNT/FAT), for the log. */
#ifndef KH_NITRO_ROMFS_H
#define KH_NITRO_ROMFS_H

#include <stdint.h>

/* Read the FNT and FAT of the dump (after rom_open). */
void kh_romfs_init(void);

/* "path+offset" of the file holding ROM offset `off`, "file N+offset" for unnamed files
 * (overlays), "rom XXXXXXXX" outside every file. Returns buf. */
const char *kh_romfs_describe(uint32_t off, char *buf, int size);

/* The ROM offset and size of the file at `path` ("/mv/802.mods"); 0 when there is none. */
int kh_romfs_find(const char *path, uint32_t *offset, uint32_t *size);

#endif
