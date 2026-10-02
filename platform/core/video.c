#include "video.h"

#include "config.h"
#include "log.h"
#include "paths.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
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
static GLint u_c2d, u_c3d, u_bright, u_hofs, u_blend, u_backdrop, u_prev, u_cap, u_flip;

/* The display capture on the GPU (platform/hw/capture.c): two targets used in turn, the newest
 * one holding the last capture (GL orientation, row 0 the bottom); the graphics screen to
 * capture, source B when it is not the last capture, and a clear texture for "no 3D". */
static GLuint s_cap_tex[2], s_cap_fbo[2], s_cap_gfx, s_cap_srcb, s_clear_tex;
static int s_cap_cur = -1, s_cap_w, s_cap_h;
static struct {
    int pending, src3d;
    const uint32_t *gfx, *srcb;
    unsigned tex3d;
    float ka, kb;
} s_cap;
static int s_show_cap = -1;     /* screen showing the last capture in the next present */
static uint16_t s_show_cap_bright;
static uint16_t s_3d_bldalpha, s_3d_backdrop;
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
    "            uniform float2 uBright, uniform float uHofs, uniform float2 uBlend,\n"
    "            uniform float3 uBackdrop, uniform sampler2D uPrev, uniform float4 uCap,\n"
    "            uniform float uFlip) : COLOR\n"
    "{\n"
    "    float2 uv2 = vUv;\n"
    "    if (uFlip > 0.5)\n"
    "        uv2.y = 1.0 - vUv.y;\n"
    "    float4 b = tex2D(u2d, uv2);\n"
    "    float3 c = b.rgb;\n"
    "    float u = vUv.x + uHofs;\n"
    "    float4 t = tex2D(u3d, float2(u, 1.0 - vUv.y));\n"
    "    if (u < 0.0 || u > 1.0)\n"
    "        t = float4(0.0, 0.0, 0.0, 0.0);\n"
    /* uCap.z: a capture of the 3D layer alone */
    "    if (uCap.z > 0.5) {\n"
    "        c = t.rgb;\n"
    "    } else if (b.a < 0.99) {\n"
    "        float code = floor(b.a * 255.0 + 0.5);\n"
    /* a 2D layer blended over the 3D one (gpu2d.h OVER_3D / BLEND_3D): b is the 2D colour,
     * the 3D layer over the backdrop the second target */
    "        if (code >= 192.0) {\n"
    "            float2 ev = uBlend;\n"
    "            if (code < 224.0)\n"
    "                ev = float2((code - 192.0) / 16.0, 1.0 - (code - 192.0) / 16.0);\n"
    "            float3 under = t.rgb + uBackdrop * (1.0 - t.a);\n"
    "            c = min(b.rgb * ev.x + under * ev.y, 1.0);\n"
    "        } else {\n"
    /* the 3D layer's brightness effect touches its own pixels only (t is premultiplied);
     * where it is clear, the layer behind shows as gpu2d left it */
    "            if (code >= 128.0)\n"
    "                t.rgb = t.rgb - t.rgb * ((code - 128.0) / 16.0);\n"
    "            else if (code >= 64.0)\n"
    "                t.rgb = t.rgb + (t.aaa - t.rgb) * ((code - 64.0) / 16.0);\n"
    "            c = t.rgb + b.rgb * (1.0 - t.a);\n"
    "        }\n"
    "    }\n"
    /* a display capture: this picture times EVA plus the source B picture times EVB */
    "    if (uCap.w > 0.5)\n"
    "        c = min(c * uCap.x + tex2D(uPrev, float2(vUv.x, 1.0 - vUv.y)).rgb * uCap.y, 1.0);\n"
    "    if (uBright.x > 0.5 && uBright.x < 1.5)\n"
    "        c = c + (1.0 - c) * uBright.y;\n"
    "    else if (uBright.x > 1.5)\n"
    "        c = c - c * uBright.y;\n"
    "    return float4(c, 1.0);\n"
    "}\n";

/* Compiled shaders are kept in ux0:data/khdays/shaders, named by a hash of their source: the
 * CG compiler (libshacccg) took seconds of every boot. A cached binary that does not load or
 * link is dropped and the source compiled again. */
#define SHADER_DIR KH_DATA_DIR "/shaders"

static uint32_t source_hash(const char *src, GLenum type)
{
    uint32_t h = 2166136261u ^ (uint32_t)type;
    while (*src)
        h = (h ^ (uint8_t)*src++) * 16777619u;
    return h;
}

static void shader_path(char *out, size_t n, uint32_t h)
{
    snprintf(out, n, SHADER_DIR "/%08x.gxp", (unsigned)h);
}

/* the shader from the cache; 0 when it is not there */
static GLuint shader_cached(GLenum type, uint32_t h)
{
    char path[96];
    SceUID fd;
    SceIoStat st;
    void *bin;
    GLuint sh;
    shader_path(path, sizeof(path), h);
    if (sceIoGetstat(path, &st) < 0 || st.st_size <= 0 || st.st_size > 256 * 1024)
        return 0;
    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return 0;
    bin = malloc((size_t)st.st_size);
    if (!bin || sceIoRead(fd, bin, (SceSize)st.st_size) != (int)st.st_size) {
        sceIoClose(fd);
        free(bin);
        return 0;
    }
    sceIoClose(fd);
    sh = glCreateShader(type);
    glShaderBinary(1, &sh, 0, bin, (GLsizei)st.st_size);
    free(bin);
    return sh;
}

static GLuint shader_compiled(GLenum type, const char *src, uint32_t h, const char *what)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0, len;
    char msg[512] = "";
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(sh, sizeof(msg), &len, msg);
        LOG("video: %s shader: %s", what, msg);
        glDeleteShader(sh);
        return 0;
    }
    {
        static uint8_t bin[256 * 1024];
        GLsizei n = 0;
        vglGetShaderBinary(sh, sizeof(bin), &n, bin);
        if (n > 0) {
            char path[96];
            SceUID fd;
            sceIoMkdir(SHADER_DIR, 0777);
            shader_path(path, sizeof(path), h);
            fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
            if (fd >= 0) {
                sceIoWrite(fd, bin, (SceSize)n);
                sceIoClose(fd);
            }
        }
    }
    return sh;
}

static GLuint link_program(GLuint vs, GLuint fs, const char *const *attribs, int nattribs)
{
    GLuint prog = glCreateProgram();
    GLint ok = 0;
    int i;
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    for (i = 0; i < nattribs; i++)
        glBindAttribLocation(prog, (GLuint)i, attribs[i]);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

unsigned video_build_program(const char *vs_src, const char *fs_src, const char *const *attribs,
                             int nattribs)
{
    /* the CG types: vitaGL takes GL_VERTEX_SHADER/GL_FRAGMENT_SHADER source as GLSL and runs
     * it through its translator (0.0.31 crashed in glLinkProgram on that) */
    const uint32_t hv = source_hash(vs_src, GL_CG_VERTEX_SHADER_EXT);
    const uint32_t hf = source_hash(fs_src, GL_CG_FRAGMENT_SHADER_EXT);
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    GLuint vs = shader_cached(GL_CG_VERTEX_SHADER_EXT, hv);
    GLuint fs = shader_cached(GL_CG_FRAGMENT_SHADER_EXT, hf);
    GLuint prog = 0;
    const int cached = vs && fs;

    if (cached)
        prog = link_program(vs, fs, attribs, nattribs);
    if (!prog) {
        char path[96];
        if (cached) {
            LOG("video: cached shaders %08x/%08x did not link, compiling", (unsigned)hv, (unsigned)hf);
            shader_path(path, sizeof(path), hv);
            sceIoRemove(path);
            shader_path(path, sizeof(path), hf);
            sceIoRemove(path);
        }
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        vs = shader_compiled(GL_CG_VERTEX_SHADER_EXT, vs_src, hv, "vertex");
        fs = vs ? shader_compiled(GL_CG_FRAGMENT_SHADER_EXT, fs_src, hf, "fragment") : 0;
        if (!vs || !fs)
            return 0;
        prog = link_program(vs, fs, attribs, nattribs);
        if (!prog) {
            LOG("video: program link failed");
            return 0;
        }
    }
    LOG("video: shaders %08x/%08x %s in %u ms", (unsigned)hv, (unsigned)hf,
        prog && cached ? "from the cache" : "compiled",
        (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
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
    u_blend = glGetUniformLocation(s_compose, "uBlend");
    u_backdrop = glGetUniformLocation(s_compose, "uBackdrop");
    u_prev = glGetUniformLocation(s_compose, "uPrev");
    u_cap = glGetUniformLocation(s_compose, "uCap");
    u_flip = glGetUniformLocation(s_compose, "uFlip");
    glGenBuffers(1, &s_compose_vbo);
}

void video_set_3d(int screen, unsigned tex, uint16_t master_bright, int hofs, uint16_t bldalpha,
                  uint16_t backdrop)
{
    s_3d_bldalpha = bldalpha;
    s_3d_backdrop = backdrop;
    s_3d_hofs = (float)hofs / 256.0f;
    s_3d_screen = (s_compose && tex) ? screen : -1;
    s_3d_tex = tex;
    s_3d_bright = master_bright;
}

/* One pass of the composition shader over the NDC rectangle v (x, y, u, v per corner).
 * cap: 0 to the screen; 1 a capture pass (no master brightness, source B blended in). */
static void compose_pass(const float *v, GLuint tex2d, int flip2d, GLuint tex3d, uint16_t bright,
                         int cap)
{
    const int mode = (bright >> 14) & 3;
    int f = bright & 31;

    if (f > 16)
        f = 16;
    glUseProgram(s_compose);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, cap && !s_cap.srcb && s_cap_cur >= 0 ? s_cap_tex[s_cap_cur]
                                 : cap && s_cap.srcb ? s_cap_srcb : s_clear_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex3d ? tex3d : s_clear_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex2d);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform1i(u_c2d, 0);
    glUniform1i(u_c3d, 1);
    glUniform1i(u_prev, 2);
    glUniform1f(u_hofs, s_3d_hofs);
    glUniform1f(u_flip, flip2d ? 1.0f : 0.0f);
    if (cap)
        glUniform4f(u_cap, s_cap.ka, s_cap.kb, s_cap.src3d ? 1.0f : 0.0f, 1.0f);
    else
        glUniform4f(u_cap, 1.0f, 0.0f, 0.0f, 0.0f);
    {
        float eva = (float)(s_3d_bldalpha & 31), evb = (float)((s_3d_bldalpha >> 8) & 31);
        const uint16_t bd = s_3d_backdrop;
        glUniform2f(u_blend, (eva > 16 ? 16 : eva) / 16.0f, (evb > 16 ? 16 : evb) / 16.0f);
        glUniform3f(u_backdrop, (float)(bd & 31) / 31.0f, (float)((bd >> 5) & 31) / 31.0f,
                    (float)((bd >> 10) & 31) / 31.0f);
    }
    glUniform2f(u_bright, (!cap && (mode == 1 || mode == 2)) ? (float)mode : 0.0f, (float)f / 16.0f);
    glBindBuffer(GL_ARRAY_BUFFER, s_compose_vbo);
    glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(float), v, GL_DYNAMIC_DRAW);
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

static void screen_quad(int screen, float *v)
{
    const ScreenRect *r = &s_rect[screen];
    const float x0 = r->x / (DISPLAY_W / 2.0f) - 1.0f, x1 = (r->x + r->w) / (DISPLAY_W / 2.0f) - 1.0f;
    const float y0 = 1.0f - r->y / (DISPLAY_H / 2.0f), y1 = 1.0f - (r->y + r->h) / (DISPLAY_H / 2.0f);
    const float q[] = { x0, y0, 0, 0, x1, y0, 1, 0, x0, y1, 0, 1, x1, y1, 1, 1 };
    memcpy(v, q, sizeof(q));
}

static void draw_composed(int screen)
{
    float v[16];
    screen_quad(screen, v);
    compose_pass(v, s_tex[screen], 0, s_3d_tex, s_3d_bright, 0);
}

static GLuint new_texture(int w, int h, GLint filter)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void capture_init(void)
{
    static const uint32_t clear = 0;
    int i;
    s_cap_w = DS_SCREEN_W * kh_config.render_scale;
    s_cap_h = DS_SCREEN_H * kh_config.render_scale;
    for (i = 0; i < 2; i++) {
        s_cap_tex[i] = new_texture(s_cap_w, s_cap_h, GL_LINEAR);
        glGenFramebuffers(1, &s_cap_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_cap_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_cap_tex[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_cap_gfx = new_texture(DS_SCREEN_W, DS_SCREEN_H, GL_NEAREST);
    s_cap_srcb = new_texture(DS_SCREEN_W, DS_SCREEN_H, GL_NEAREST);
    s_clear_tex = new_texture(1, 1, GL_NEAREST);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &clear);
}

void video_capture(const uint32_t *gfx, int src3d, unsigned tex3d, float ka, float kb,
                   const uint32_t *srcb)
{
    s_cap.gfx = gfx;
    s_cap.src3d = src3d;
    s_cap.tex3d = tex3d;
    s_cap.ka = ka;
    s_cap.kb = kb;
    s_cap.srcb = srcb;
    s_cap.pending = 1;
}

void video_show_capture(int screen, uint16_t master_bright)
{
    s_show_cap = s_cap_cur >= 0 ? screen : -1;
    s_show_cap_bright = master_bright;
}

static void run_capture(void)
{
    static const float full[] = { -1, 1, 0, 0, 1, 1, 1, 0, -1, -1, 0, 1, 1, -1, 1, 1 };
    const int next = s_cap_cur < 0 ? 0 : 1 - s_cap_cur;
    s_cap.pending = 0;
    if (!s_compose)
        return;
    if (s_cap.gfx) {
        glBindTexture(GL_TEXTURE_2D, s_cap_gfx);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DS_SCREEN_W, DS_SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE,
                        s_cap.gfx);
    }
    if (s_cap.srcb) {
        glBindTexture(GL_TEXTURE_2D, s_cap_srcb);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DS_SCREEN_W, DS_SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE,
                        s_cap.srcb);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_cap_fbo[next]);
    glViewport(0, 0, s_cap_w, s_cap_h);
    compose_pass(full, s_cap.gfx ? s_cap_gfx : s_clear_tex, 0, s_cap.tex3d, 0, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_cap_cur = next;
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
    {
        const uint64_t t0 = sceKernelGetProcessTimeWide();
        vglInitExtended(0, DISPLAY_W, DISPLAY_H, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
        LOG("video: vitaGL initialised in %u ms",
            (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
    }

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
    capture_init();
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
    if (s_cap.pending)
        run_capture();
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
        if (i == s_show_cap && s_compose) {
            /* the screen shows the VRAM bank the last capture went to */
            float v[16];
            screen_quad(i, v);
            compose_pass(v, s_cap_tex[s_cap_cur], 1, 0, s_show_cap_bright, 0);
        } else if (i == s_3d_screen)
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
