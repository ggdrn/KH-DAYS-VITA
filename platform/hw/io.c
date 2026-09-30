#include "hw/io.h"
#include "nitro/cpu.h"

uint8_t kh_ds_io[KH_IO_SIZE] __attribute__((aligned(32)));

void kh_io_fifo_write32(volatile void *port, uint32_t word)
{
    KH_PROBE("the GX FIFO");
    /* TODO(gx): hand GX FIFO / command-port words to the geometry engine. Until then the
     * register only latches the last word, as a plain store would. */
    *(volatile uint32_t *)port = word;
}
