#include "menubg.h"
#include "log.h"
#include "paths.h"

#include <stdlib.h>

/* stb_image (public domain, platform/compat/stb_image.h): PNG and JPEG only */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "stb_image.h"
#pragma GCC diagnostic pop

/* The picture behind the title's menus in the single screen (config single_screen): the
 * player's own (ux0:data/khdays/menu_bg.png or .jpg; none comes with the port), scaled to cover
 * w x h and cropped in the middle. NULL when there is none. */
uint32_t *menubg_load(int w, int h)
{
    static const char *const names[] = { KH_DATA_DIR "/menu_bg.png", KH_DATA_DIR "/menu_bg.jpg",
                                         KH_DATA_DIR "/menu_bg.jpeg" };
    unsigned char *img = NULL;
    int iw = 0, ih = 0, n, i, x, y;
    uint32_t *out;
    float s, ox, oy;
    for (i = 0; i < 3 && !img; i++) {
        img = stbi_load(names[i], &iw, &ih, &n, 4);
        if (img)
            LOG("video: menu background %s, %dx%d", names[i], iw, ih);
    }
    if (!img) {
        LOG("video: no menu background (%s or .jpg): the title's menu over black", names[0]);
        return NULL;
    }
    out = malloc((size_t)w * h * 4);
    if (!out) {
        stbi_image_free(img);
        return NULL;
    }
    /* cover: the larger scale, the picture's middle */
    s = (float)iw / w < (float)ih / h ? (float)iw / w : (float)ih / h;
    ox = (iw - w * s) / 2.0f, oy = (ih - h * s) / 2.0f;
    for (y = 0; y < h; y++) {
        const int sy0 = (int)(oy + y * s), sy1 = (int)(oy + (y + 1) * s);
        for (x = 0; x < w; x++) {
            /* the box of source pixels under this one, averaged */
            const int sx0 = (int)(ox + x * s), sx1 = (int)(ox + (x + 1) * s);
            uint32_t r = 0, g = 0, b = 0, c = 0;
            int yy, xx;
            for (yy = sy0; yy <= sy1 && yy < ih; yy++)
                for (xx = sx0; xx <= sx1 && xx < iw; xx++) {
                    const unsigned char *p = img + ((size_t)yy * iw + xx) * 4;
                    r += p[0], g += p[1], b += p[2], c++;
                }
            if (!c)
                c = 1;
            out[y * w + x] = (r / c) | (g / c) << 8 | (b / c) << 16 | 0xffu << 24;
        }
    }
    stbi_image_free(img);
    return out;
}
