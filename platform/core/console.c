#include "console.h"

#include "font8x8.h"
#include "log.h"
#include "video.h"

#include <string.h>

#define COLS (VIDEO_OVERLAY_W / 8)
#define ROWS (VIDEO_OVERLAY_H / 8)

static uint32_t s_px[VIDEO_OVERLAY_W * VIDEO_OVERLAY_H];

static void text(int col, int row, const char *s, uint32_t fg)
{
    for (; *s && col < COLS; s++, col++) {
        unsigned char c = (unsigned char)*s;
        const uint8_t *g = font8x8_basic[(c >= 0x20 && c < 0x80) ? c - 0x20 : '?' - 0x20];
        int y, x;
        for (y = 0; y < 8; y++) {
            uint32_t *px = &s_px[(row * 8 + y) * VIDEO_OVERLAY_W + col * 8];
            for (x = 0; x < 8; x++)
                if (g[y] & (1 << x))
                    px[x] = fg;
        }
    }
}

const uint32_t *console_render(const char *status)
{
    char line[COLS + 1];
    int row;

    /* translucent black behind the text */
    for (row = 0; row < VIDEO_OVERLAY_W * VIDEO_OVERLAY_H; row++)
        s_px[row] = 0x90000000u;
    if (status)
        text(0, 0, status, 0xff60ffffu);
    for (row = ROWS - 1; row >= 1; row--) {
        if (!log_recent(ROWS - 1 - row, line, sizeof(line)))
            continue;
        /* skip the "[   time] " stamp: the width is precious */
        text(0, row, strlen(line) > 10 && line[0] == '[' ? line + 10 : line, 0xffe0e0e0u);
    }
    return s_px;
}
