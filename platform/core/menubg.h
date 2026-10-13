#ifndef KH_MENUBG_H
#define KH_MENUBG_H

#include <stdint.h>

/* The player's picture for the single screen's title menus, w x h RGBA (free() it), NULL none. */
uint32_t *menubg_load(int w, int h);

#endif
