/* The top 4 KiB of the DS main RAM (0x027ff000-0x02800000): the "system shared area" where the
 * boot ROM and the ARM7 leave the cartridge header, locks, the touch/button words and the
 * ARM9<->ARM7 hand-off data. The game names parts of it by address (data_027ffff0, the init lock
 * word); the linker points those names into this block (configure_vita.py). */
#include "hw/shared_area.h"

unsigned char kh_ds_shared_area[KH_SHARED_AREA_SIZE] __attribute__((aligned(32)));
