#include "video.h"

#include "log.h"

#include <vitaGL.h>

#define DISPLAY_W 960
#define DISPLAY_H 544

static GLuint s_tex[2], s_overlay_tex;
static const uint32_t *s_overlay;
static ScreenLayout s_layout = LAYOUT_SIDE_BY_SIDE;
static ScreenRect s_rect[2];

static void compute_layout(void)
{
    switch (s_layout) {
    case LAYOUT_TOP_FOCUS:
        s_rect[0] = (ScreenRect){ 0, 0, 725, 544 };
        s_rect[1] = (ScreenRect){ 725, 368, 235, 176 };
        break;
    case LAYOUT_BOTTOM_FOCUS:
        s_rect[0] = (ScreenRect){ 725, 0, 235, 176 };
        s_rect[1] = (ScreenRect){ 0, 0, 725, 544 };
        break;
    default:
        s_rect[0] = (ScreenRect){ 0, 92, 480, 360 };
        s_rect[1] = (ScreenRect){ 480, 92, 480, 360 };
        break;
    }
}

void video_init(void)
{
    int i;

    /* A GPU pool > 0 is required; vitaGL's defaults for the rest. */
    vglInitExtended(0, DISPLAY_W, DISPLAY_H, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);

    glGenTextures(2, s_tex);
    for (i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, s_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, DS_SCREEN_W, DS_SCREEN_H, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    glGenTextures(1, &s_overlay_tex);
    glBindTexture(GL_TEXTURE_2D, s_overlay_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VIDEO_OVERLAY_W, VIDEO_OVERLAY_H, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, DISPLAY_W, DISPLAY_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);

    compute_layout();
    LOG("video: vitaGL up, layout %d", s_layout);
}

void video_set_layout(ScreenLayout layout)
{
    s_layout = layout % LAYOUT_COUNT;
    compute_layout();
}

ScreenLayout video_layout(void)
{
    return s_layout;
}

void video_set_overlay(const uint32_t *pixels)
{
    s_overlay = pixels;
}

ScreenRect video_bottom_rect(void)
{
    return s_rect[1];
}

static void draw_quad(const ScreenRect *r)
{
    const float x0 = r->x, y0 = r->y, x1 = r->x + r->w, y1 = r->y + r->h;
    const float pos[] = { x0, y0, x1, y0, x0, y1, x1, y1 };
    const float uv[] = { 0, 0, 1, 0, 0, 1, 1, 1 };

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void video_present(const uint32_t *top, const uint32_t *bottom)
{
    const uint32_t *src[2] = { top, bottom };
    int i;

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    for (i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, s_tex[i]);
        if (src[i])
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DS_SCREEN_W, DS_SCREEN_H, GL_RGBA,
                            GL_UNSIGNED_BYTE, src[i]);
        draw_quad(&s_rect[i]);
    }
    if (s_overlay) {
        static const ScreenRect full = { 0, 0, DISPLAY_W, DISPLAY_H };
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindTexture(GL_TEXTURE_2D, s_overlay_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VIDEO_OVERLAY_W, VIDEO_OVERLAY_H, GL_RGBA,
                        GL_UNSIGNED_BYTE, s_overlay);
        draw_quad(&full);
        glDisable(GL_BLEND);
    }
    vglSwapBuffers(GL_FALSE);
}
