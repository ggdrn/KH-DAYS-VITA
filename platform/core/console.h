/* On-screen console: the latest log lines and a status line over the game, for bring-up on the
 * console without pulling files off it. */
#ifndef KH_CONSOLE_H
#define KH_CONSOLE_H

#include <stdint.h>

/* Redraw the layer from the log and `status` (one line, may be NULL); returns its pixels. */
const uint32_t *console_render(const char *status);

#endif
