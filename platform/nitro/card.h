#ifndef KH_NITRO_CARD_H
#define KH_NITRO_CARD_H

/* A 256 MiB Macronix mask ROM, as READ_ID reports it. */
#define KH_CARD_ID 0x0000ffc2u

#include <stdint.h>

/* ROM reads so far (the watchdog's progress counter). */
extern volatile uint32_t kh_card_reads;

#endif
