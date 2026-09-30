/* The top 4 KiB of the DS main RAM (0x027ff000-0x02800000): the "system shared area" where the
 * boot ROM and the ARM7 leave the cartridge header, locks, the touch/button words and the
 * ARM9<->ARM7 hand-off data. The game names parts of it by address (data_027ffff0, the init lock
 * word): those names are labels inside the block. A label, not --defsym: ld makes name=sym+off
 * an absolute symbol, which vita-elf-create does not relocate. */
#include "hw/shared_area.h"

__asm__(".section .bss.kh_ds_shared_area,\"aw\",%nobits\n"
        ".balign 32\n"
        ".global kh_ds_shared_area\n"
        ".type kh_ds_shared_area, %object\n"
        "kh_ds_shared_area:\n"
        ".space 0xff0\n"
        ".global data_027ffff0\n" /* OS_InitLock's lock word */
        ".type data_027ffff0, %object\n"
        "data_027ffff0:\n"
        ".space 0x10\n"
        ".size kh_ds_shared_area, 0x1000\n"
        ".previous\n");
