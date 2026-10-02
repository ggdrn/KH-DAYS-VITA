#include "video.h"

#include "config.h"
#include "log.h"

#include <vitaGL.h>

#define DISPLAY_W 960
#define DISPLAY_H 544

static GLuint s_tex[2], s_overlay_tex;
static const uint32_t *s_overlay;
static ScreenLayout s_layout = LAYOUT_TOP_MAIN;
static ScreenRect s_rect[2];
static int s_inset = -1; /* the screen drawn small over the other one, -1 for none */
static volatile int s_pending = -1; /* a layout asked for from another thread (input) */
static GLuint s_compose, s_compose_vbo;
static GLint u_c2d, u_c3d, u_bright, u_hofs;
static float s_3d_hofs;
static int s_3d_screen = -1;
static GLuint s_3d_tex;
static uint16_t s_3d_bright;

/* engine A's 2D pixels (alpha = gpu2d.h code) with the 3D layer laid in, then master
 * brightness; positions in NDC, uv with 0 at the top of the screen */
static const char s_compose_vs[] =
    "void main(float2 aPos, float2 aUv, out float4 vPos : POSITION, out float2 vUv : TEXCOORD0)\n"
    "{\n"
    "    vPos = float4(aPos, 0.0, 1.0);\n"
    "    vUv = aUv;\n"
    "}\n";
static const char s_compose_fs[] =
    "float4 main(float2 vUv : TEXCOORD0, uniform sampler2D u2d, uniform sampler2D u3d,\n"
    "            uniform float2 uBright, uniform float uHofs) : COLOR\n"
    "{\n"
    "    float4 b = tex2D(u2d, vUv);\n"
    "    float3 c = b.rgb;\n"
    "    if (b.a < 0.99) {\n"
    "        float code = floor(b.a * 255.0 + 0.5);\n"
    "        float u = vUv.x + uHofs;\n"
    "        float4 t = tex2D(u3d, float2(u, 1.0 - vUv.y));\n"
    "        if (u < 0.0 || u > 1.0)\n"
    "            t = float4(0.0, 0.0, 0.0, 0.0);\n"
    /* the 3D layer's brightness effect touches its own pixels only (t is premultiplied);
     * where it is clear, the layer behind shows as gpu2d left it */
    "        if (code >= 128.0)\n"
    "            t.rgb = t.rgb - t.rgb * ((code - 128.0) / 16.0);\n"
    "        else if (code >= 64.0)\n"
    "            t.rgb = t.rgb + (t.aaa - t.rgb) * ((code - 64.0) / 16.0);\n"
    "        c = t.rgb + b.rgb * (1.0 - t.a);\n"
    "    }\n"
    "    if (uBright.x > 0.5 && uBright.x < 1.5)\n"
    "        c = c + (1.0 - c) * uBright.y;\n"
    "    else if (uBright.x > 1.5)\n"
    "        c = c - c * uBright.y;\n"
    "    return float4(c, 1.0);\n"
    "}\n";

unsigned video_build_program(const char *vs_src, const char *fs_src, const char *const *attribs,
                             int nattribs)
{
    /* the CG types: vitaGL takes GL_VERTEX_SHADER/GL_FRAGMENT_SHADER source as GLSL and runs
     * it through its translator (0.0.31 crashed in glLinkProgram on that) */
    GLuint vs = glCreateShader(GL_CG_VERTEX_SHADER_EXT), fs = glCreateShader(GL_CG_FRAGMENT_SHADER_EXT), prog;
    GLint ok = 0, len;
    char msg[512] = "";
    int i;

    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(vs, sizeof(msg), &len, msg);
        LOG("video: vertex shader: %s", msg);
        return 0;
    }
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(fs, sizeof(msg), &len, msg);
        LOG("video: fragment shader: %s", msg);
        return 0;
    }
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    for (i = 0; i < nattribs; i++)
        glBindAttribLocation(prog, (GLuint)i, attribs[i]);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        LOG("video: program link failed");
        return 0;
    }
    return prog;
}

static void compose_init(void)
{
    static const char *const attribs[] = { "aPos", "aUv" };
    s_compose = video_build_program(s_compose_vs, s_compose_fs, attribs, 2);
    if (!s_compose) {
        LOG("video: composition shader did not build: the 3D layer will not show");
        return;
    }
    u_c2d = glGetUniformLocation(s_compose, "u2d");
    u_c3d = glGetUniformLocation(s_compose, "u3d");
    u_bright = glGetUniformLocation(s_compose, "uBright");
    u_hofs = glGetUniformLocation(s_compose, "uHofs");
    glGenBuffers(1, &s_compose_vbo);
}

void video_set_3d(int screen, unsigned tex, uint16_t master_bright, int hofs)
{
    s_3d_hofs = (float)hofs / 256.0f;
    s_3d_screen = (s_compose && tex) ? screen : -1;
    s_3d_tex = tex;
    s_3d_bright = master_bright;
}

static void draw_composed(int screen)
{
    const ScreenRect *r = &s_rect[screen];
    const float x0 = r->x / (DISPLAY_W / 2.0f) - 1.0f, x1 = (r->x + r->w) / (DISPLAY_W / 2.0f) - 1.0f;
    const float y0 = 1.0f - r->y / (DISPLAY_H / 2.0f), y1 = 1.0f - (r->y + r->h) / (DISPLAY_H / 2.0f);
    const float v[] = { x0, y0, 0, 0, x1, y0, 1, 0, x0, y1, 0, 1, x1, y1, 1, 1 };
    const int mode = (s_3d_bright >> 14) & 3;
    int f = s_3d_bright & 31;

    if (f > 16)
        f = 16;
    glUseProgram(s_compose);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_3d_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex[screen]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform1i(u_c2d, 0);
    glUniform1i(u_c3d, 1);
    glUniform1f(u_hofs, s_3d_hofs);
    glUniform2f(u_bright, (mode == 1 || mode == 2) ? (float)mode : 0.0f, (float)f / 16.0f);
    glBindBuffer(GL_ARRAY_BUFFER, s_compose_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

/* The small screen: 4:3, inset_width wide (config.ini), in the top-right corner. */
static ScreenRect inset_rect(void)
{
    const int w = kh_config.inset_width, h = w * 3 / 4, margin = 4;
    return (ScreenRect){ DISPLAY_W - w - margin, margin, w, h };
}

static void compute_layout(void)
{
    switch (s_layout) {
    case LAYOUT_TOP_MAIN:
        /* the top screen over the whole display (16:9), the touch screen small */
        s_rect[0] = (ScreenRect){ 0, 0, DISPLAY_W, DISPLAY_H };
        s_rect[1] = inset_rect();
        s_inset = 1;
        break;
    case LAYOUT_BOTTOM_MAIN:
        /* the touch screen large in its own 4:3, the top screen small */
        s_rect[1] = (ScreenRect){ (DISPLAY_W - 725) / 2, 0, 725, 544 };
        s_rect[0] = inset_rect();
        s_inset = 0;
        break;
    default:
        s_rect[0] = (ScreenRect){ 0, 92, 480, 360 };
        s_rect[1] = (ScreenRect){ 480, 92, 480, 360 };
        s_inset = -1;
        break;
    }
}

int video_inset_screen(void)
{
    return s_inset;
}

void video_swap_screens(void)
{
    video_set_layout(video_layout() == LAYOUT_TOP_MAIN ? LAYOUT_BOTTOM_MAIN : LAYOUT_TOP_MAIN);
}

int video_on_inset(int px, int py)
{
    const ScreenRect *r;
    if (s_inset < 0 || s_pending >= 0)
        return 0;
    r = &s_rect[s_inset];
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

int video_map_touch(int px, int py, int *x, int *y)
{
    const ScreenRect *r = &s_rect[1];
    if (s_inset >= 0 && video_on_inset(px, py))
        return 0; /* the small screen is a button (it swaps the screens), not the DS's */
    if (px < r->x || px >= r->x + r->w || py < r->y || py >= r->y + r->h)
        return 0;
    *x = (px - r->x) * DS_SCREEN_W / r->w;
    *y = (py - r->y) * DS_SCREEN_H / r->h;
    return 1;
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

    s_layout = (ScreenLayout)(kh_config.layout % LAYOUT_COUNT);
    compute_layout();
    compose_init();
    LOG("video: vitaGL up, layout %d", s_layout);
}

/* applied by video_present, on the thread that draws */
void video_set_layout(ScreenLayout layout)
{
    s_pending = (int)(layout % LAYOUT_COUNT);
}

ScreenLayout video_layout(void)
{
    int p = s_pending;
    return p >= 0 ? (ScreenLayout)p : s_layout;
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
    int k;

    if (s_pending >= 0) {
        s_layout = (ScreenLayout)s_pending;
        s_pending = -1;
        compute_layout();
    }
    glViewport(0, 0, DISPLAY_W, DISPLAY_H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    for (k = 0; k < 2; k++) {
        /* the large screen first, the inset over it */
        const int i = s_inset == 0 ? 1 - k : k;
        glBindTexture(GL_TEXTURE_2D, s_tex[i]);
        if (src[i])
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DS_SCREEN_W, DS_SCREEN_H, GL_RGBA,
                            GL_UNSIGNED_BYTE, src[i]);
        if (i == s_3d_screen)
            draw_composed(i);
        else
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
