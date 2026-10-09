/* The user's ROM dump at ux0:data/khdays/days.nds.
 *
 * The port never ships game data: code is compiled from the decomp, and everything the game
 * loads from the cartridge (header, file system, overlay tables, sound, video) is read from
 * this file on demand, exactly where the DS card controller would have read it. */
#ifndef KH_ROM_H
#define KH_ROM_H

#include <stdint.h>

/* The EU dump the decomp is built against (gamecode YKGP). */
#define ROM_EXPECTED_SHA1 "6fae8f5bbe80114b4e2535260eab5f4d0fc8a844"
#define ROM_EXPECTED_SIZE 0x10000000u
#define ROM_GAMECODE "YKGP"

typedef enum {
    ROM_OK = 0,
    ROM_MISSING,
    ROM_WRONG_SIZE,
    ROM_WRONG_GAME,
    ROM_WRONG_HASH,
} RomStatus;

/* progress(done, total) is called while hashing; it may be NULL. */
RomStatus rom_open(const char *path, const char *stamp_path,
                   void (*progress)(uint32_t done, uint32_t total));
int rom_read(uint32_t offset, void *dst, uint32_t size);
/* Bytes the reads see in place of the dump's at offset (the port's own changes to game data,
 * such as the button icons of nitro/button_glyphs.c); the file itself is never written. Data
 * is copied; patches are added before the game reads that range. */
int rom_patch(uint32_t offset, const void *data, uint32_t size);
/* Where patch id (rom_patch's result) was last read to, whole; NULL before. The game may have
 * freed that memory since: check what is there before writing to it. */
void *rom_patch_last_dst(int id);
const uint8_t *rom_header(void); /* the 0x200-byte cartridge header */

typedef struct {
    uint32_t reads, hits, io_calls, io_us;
} RomStats;
/* Since the last call: reads served, cache hits, file reads made and the time they took. */
void rom_take_stats(RomStats *out);
void rom_close(void);

#endif
