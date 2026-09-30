/* The DS I/O register pages (0x04000000-0x04001fff: display engine A, DMA, timers, keys, IPC,
 * VRAMCNT, the divider/square root, the 3D engine, display engine B) as host memory.
 *
 * Plain register state lives here. Registers whose writes have side effects (GX FIFO, DMA
 * start, divider) are driven through platform/nitro functions and the hooks below, which keep
 * this copy up to date so that code reading a register back sees what the hardware would show. */
#ifndef KH_HW_IO_H
#define KH_HW_IO_H

#include <stdint.h>

#define KH_IO_BASE 0x04000000u
#define KH_IO_SIZE 0x2000u

extern uint8_t kh_ds_io[KH_IO_SIZE];

#define KH_IO_PTR(addr) ((void *)(kh_ds_io + ((addr) - KH_IO_BASE)))
#define KH_IO16(addr) (*(volatile uint16_t *)KH_IO_PTR(addr))
#define KH_IO32(addr) (*(volatile uint32_t *)KH_IO_PTR(addr))

/* One word pushed to a FIFO-style port (the GX command FIFO or a geometry command register).
 * `port` is the host pointer the game computed for the register. */
void kh_io_fifo_write32(volatile void *port, uint32_t word);

#endif
