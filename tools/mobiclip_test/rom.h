#include <stdint.h>
int rom_read(uint32_t offset, void *dst, uint32_t size);
const uint8_t *rom_header(void);
