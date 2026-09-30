/* The cartridge's save chip, served by the ARM7 over PXI (backup.c). */
#ifndef KH_NITRO_BACKUP_H
#define KH_NITRO_BACKUP_H

#include <stdint.h>

/* A word the ARM9 sent on PXI tag FS (11). */
void kh_backup_pxi(uint32_t data);

/* Once per frame: writes the save file a second after the last change to the chip. */
void kh_backup_tick(void);

/* Writes pending changes now (suspend, exit). */
void kh_backup_flush(void);

#endif
