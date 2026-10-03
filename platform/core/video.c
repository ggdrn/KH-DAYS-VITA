#include "video.h"

#include "config.h"
#include "log.h"
#include "paths.h"

#include <math.h>
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
static GLint u_c2d, u_c3d, u_bright, u_hofs, u_blend, u_backdrop, u_prev, u_cap, u_flip, u_fx,
    u_filt;
static float s_hud_scale = 1.0f; /* < 1: the 2D kept 4:3 over a widescreen 3D (config hud) */

/* The display capture on the GPU (platform/hw/capture.c): two targets used in turn, the newest
 * one holding the last capture (GL orientation, row 0 the bottom); the graphics screen to
 * capture, source B when it is not the last capture, and a clear texture for "no 3D". */
/* one target per VRAM bank A-D that can receive a capture, plus a spare drawn into and then
 * traded with the bank's (a capture can blend the bank's last one in) */
/* banks 4 and 5 are not VRAM: the last picture engine A gave the top and the bottom screen
 * (VIDEO_SCREEN_MEMORY), for the dual-3D scenes */
static GLuint s_cap_tex[7], s_cap_fbo[7], s_cap_gfx, s_cap_srcb, s_clear_tex;
static int s_bank_slot[6] = { 0, 1, 2, 3, 4, 5 }, s_spare_slot = 6, s_bank_valid[6];
static int s_cap_w, s_cap_h;
static struct {
    int pending, src3d, dest, srcb_bank;
    const uint32_t *gfx, *srcb;
    unsigned tex3d;
    float ka, kb;
} s_cap;
static int s_show_bank[2] = { -1, -1 }; /* per screen: the bank whose capture it shows */
static uint16_t s_show_bright[2];
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
    /* The 2D scaled up (config filter_2d, uFilt.x): 1 sharp, the DS's square pixels with
     * their edges smoothed over uFilt.y (Vita pixels per DS pixel) to one Vita pixel; 2 smooth,
     * bilinear. Only 2D texels take part: the 3D-coded ones hold the colour under the 3D. */
    "float3 smooth2d(sampler2D s, float2 uv, float2 filt)\n"
    "{\n"
    "    float2 p = uv * float2(256.0, 192.0) - 0.5;\n"
    "    float2 i = floor(p);\n"
    "    float2 f = p - i;\n"
    "    if (filt.x < 1.5)\n"
    "        f = saturate((f - 0.5) * filt.y + 0.5);\n"
    "    float2 t0 = (i + 0.5) / float2(256.0, 192.0);\n"
    "    float2 t1 = t0 + float2(1.0 / 256.0, 1.0 / 192.0);\n"
    "    float4 a = tex2D(s, t0), b = tex2D(s, float2(t1.x, t0.y));\n"
    "    float4 c = tex2D(s, float2(t0.x, t1.y)), d = tex2D(s, t1);\n"
    "    float wa = (1.0 - f.x) * (1.0 - f.y) * step(0.99, a.a);\n"
    "    float wb = f.x * (1.0 - f.y) * step(0.99, b.a);\n"
    "    float wc = (1.0 - f.x) * f.y * step(0.99, c.a);\n"
    "    float wd = f.x * f.y * step(0.99, d.a);\n"
    "    return (a.rgb * wa + b.rgb * wb + c.rgb * wc + d.rgb * wd) / max(wa + wb + wc + wd, 0.0001);\n"
    "}\n"
    "\n"
    "float4 main(float2 vUv : TEXCOORD0, uniform sampler2D u2d, uniform sampler2D u3d,\n"
    "            uniform float2 uBright, uniform float uHofs, uniform float2 uBlend,\n"
    "            uniform float3 uBackdrop, uniform sampler2D uPrev, uniform float4 uCap,\n"
    "            uniform float uFlip, uniform float4 uFx, uniform float2 uFilt) : COLOR\n"
    "{\n"
    "    float2 uv2 = vUv;\n"
    "    if (uFlip > 0.5)\n"
    "        uv2.y = 1.0 - vUv.y;\n"
    /* uFx.y < 1: the 2D (HUD) kept 4:3 in the middle of a widescreen 3D; beside it, the 3D */
    "    uv2.x = (uv2.x - 0.5) / uFx.y + 0.5;\n"
    "    float4 b = float4(0.0, 0.0, 0.0, 0.0);\n"
    "    if (uv2.x >= 0.0 && uv2.x <= 1.0) {\n"
    "        b = tex2D(u2d, uv2);\n"
    "        if (uFilt.x > 0.5 && b.a > 0.99)\n"
    "            b.rgb = smooth2d(u2d, uv2, uFilt);\n"
    "    }\n"
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
    /* uFx.x: the screen effect, on the DS's own pixel grid: 1 scanlines, 2 LCD grid */
    "    if (uFx.x > 0.5) {\n"
    "        float2 p = frac(vUv * float2(256.0, 192.0));\n"
    "        if (uFx.x < 1.5) {\n"
    "            if (p.y > 0.5)\n"
    "                c = c * 0.72;\n"
    "        } else if (p.x < 0.16 || p.y < 0.16) {\n"
    "            c = c * 0.7;\n"
    "        }\n"
    "    }\n"
    "    return float4(c, uFx.z);\n"
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
    u_fx = glGetUniformLocation(s_compose, "uFx");
    u_filt = glGetUniformLocation(s_compose, "uFilt");
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
 * cap: 0 to the screen; 1 a capture pass (no master brightness, source B blended in).
 * filter: how tex2d is read: FILTER_TEXEL texel by texel, FILTER_LINEAR bilinear (a capture,
 * already at the 3D's resolution), FILTER_2D a DS screen, through config filter_2d. */
enum { FILTER_TEXEL, FILTER_LINEAR, FILTER_2D };

static void compose_pass(const float *v, GLuint tex2d, int flip2d, GLuint tex3d, uint16_t bright,
                         int cap, float hud, float alpha, int filter)
{
    const int linear = filter == FILTER_LINEAR;
    const int mode = (bright >> 14) & 3;
    int f = bright & 31;

    if (f > 16)
        f = 16;
    glUseProgram(s_compose);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D,
                  !cap ? s_clear_tex
                  : s_cap.srcb ? s_cap_srcb
                  : s_cap.srcb_bank >= 0 && s_bank_valid[s_cap.srcb_bank]
                      ? s_cap_tex[s_bank_slot[s_cap.srcb_bank]] : s_clear_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex3d ? tex3d : s_clear_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex2d);
    /* a picture with 3D codes in its alpha is read texel by texel; a plain one smoothly */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glUniform1i(u_c2d, 0);
    glUniform1i(u_c3d, 1);
    glUniform1i(u_prev, 2);
    glUniform1f(u_hofs, s_3d_hofs);
    glUniform1f(u_flip, flip2d ? 1.0f : 0.0f);
    glUniform4f(u_fx, cap ? 0.0f : (float)kh_config.screen_effect, hud, alpha, 0.0f);
    /* Vita pixels per DS pixel, from the quad's height */
    glUniform2f(u_filt, filter == FILTER_2D ? (float)kh_config.filter_2d : 0.0f,
                fabsf(v[1] - v[9]) * (DISPLAY_H / 2.0f) / 192.0f);
    if (alpha < 0.999f) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
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
    glDisable(GL_BLEND);
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

static float screen_alpha(int screen)
{
    return screen == s_inset ? (float)kh_config.inset_opacity / 100.0f : 1.0f;
}

static void draw_composed(int screen)
{
    float v[16];
    screen_quad(screen, v);
    compose_pass(v, s_tex[screen], 0, s_3d_tex, s_3d_bright, 0,
                 screen == s_inset ? 1.0f : s_hud_scale, screen_alpha(screen), FILTER_2D);
}

void video_set_hud_scale(float scale)
{
    s_hud_scale = scale;
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
    for (i = 0; i < 7; i++) {
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
                   const uint32_t *srcb, int srcb_bank, int dest)
{
    s_cap.dest = dest >= 0 && dest < 6 ? dest : (dest & 3);
    s_cap.srcb_bank = srcb_bank;
    s_cap.gfx = gfx;
    s_cap.src3d = src3d;
    s_cap.tex3d = tex3d;
    s_cap.ka = ka;
    s_cap.kb = kb;
    s_cap.srcb = srcb;
    s_cap.pending = 1;
}

int video_shown_bank(int screen)
{
    return s_show_bank[screen & 1];
}

static uint16_t s_memory_bright[2];

void video_forget_screen_memory(void)
{
    s_bank_valid[VIDEO_SCREEN_MEMORY] = s_bank_valid[VIDEO_SCREEN_MEMORY + 1] = 0;
}

int video_screen_memory_valid(int screen)
{
    return s_bank_valid[VIDEO_SCREEN_MEMORY + (screen & 1)];
}

void video_screen_memory_bright(int screen, uint16_t master_bright)
{
    s_memory_bright[screen & 1] = master_bright;
}

void video_show_capture(int screen, int bank, uint16_t master_bright)
{
    if (bank >= VIDEO_SCREEN_MEMORY && bank < VIDEO_SCREEN_MEMORY + 2)
        master_bright = s_memory_bright[bank - VIDEO_SCREEN_MEMORY];
    s_show_bank[screen & 1] = bank >= 0 && bank < 6 && s_bank_valid[bank] ? bank : -1;
    s_show_bright[screen & 1] = master_bright;
}

static void run_capture(void)
{
    static const float full[] = { -1, 1, 0, 0, 1, 1, 1, 0, -1, -1, 0, 1, 1, -1, 1, 1 };
    const int next = s_spare_slot;
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
    compose_pass(full, s_cap.gfx ? s_cap_gfx : s_clear_tex, 0, s_cap.tex3d, 0, 1, 1.0f, 1.0f,
                 FILTER_TEXEL);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* the spare now holds the bank's capture: trade the two */
    s_spare_slot = s_bank_slot[s_cap.dest];
    s_bank_slot[s_cap.dest] = next;
    s_bank_valid[s_cap.dest] = 1;
}

/* The small screen: 4:3, inset_width wide, in the corner config inset_corner names. */
static ScreenRect inset_rect(void)
{
    const int w = kh_config.inset_width, h = w * 3 / 4, margin = 4;
    const int right = !(kh_config.inset_corner & 1), bottom = kh_config.inset_corner >= 2;
    return (ScreenRect){ right ? DISPLAY_W - w - margin : margin,
                         bottom ? DISPLAY_H - h - margin : margin, w, h };
}

static void compute_layout(void)
{
    switch (s_layout) {
    case LAYOUT_TOP_MAIN:
        /* the top screen over the whole display (16:9), or in its own 4:3 (config aspect),
         * the touch screen small */
        s_rect[0] = kh_config.aspect == KH_ASPECT_4_3
                        ? (ScreenRect){ (DISPLAY_W - 725) / 2, 0, 725, 544 }
                        : (ScreenRect){ 0, 0, DISPLAY_W, DISPLAY_H };
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

void video_relayout(void)
{
    video_set_layout(video_layout());
}

float video_screen_aspect(int screen)
{
    const ScreenRect *r = &s_rect[screen & 1];
    return r->h ? (float)r->w / (float)r->h : 4.0f / 3.0f;
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

static volatile int s_overlay_dirty;

void video_overlay_changed(void)
{
    s_overlay_dirty = 1;
}

void video_set_overlay(const uint32_t *pixels)
{
    if (pixels != s_overlay)
        s_overlay_dirty = 1;
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
        if (s_show_bank[i] >= 0 && s_compose) {
            /* the screen shows a VRAM bank a capture went to (engine A's VRAM display, or
             * engine B's bitmap BG or sprites in the dual-3D scenes) */
            float v[16];
            screen_quad(i, v);
            compose_pass(v, s_cap_tex[s_bank_slot[s_show_bank[i]]], 1, 0, s_show_bright[i], 0,
                         1.0f, screen_alpha(i), FILTER_LINEAR);
        } else if (i == s_3d_screen) {
            draw_composed(i);
        } else if (s_compose) {
            /* a plain 2D screen, through the same pass for the effect and the opacity */
            float v[16];
            screen_quad(i, v);
            compose_pass(v, s_tex[i], 0, 0, 0, 0, 1.0f, screen_alpha(i), FILTER_2D);
        } else {
            draw_quad(&s_rect[i]);
        }
    }
    if (s_overlay) {
        static const ScreenRect full = { 0, 0, DISPLAY_W, DISPLAY_H };
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindTexture(GL_TEXTURE_2D, s_overlay_tex);
        if (s_overlay_dirty) {
            s_overlay_dirty = 0;
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VIDEO_OVERLAY_W, VIDEO_OVERLAY_H, GL_RGBA,
                            GL_UNSIGNED_BYTE, s_overlay);
        }
        draw_quad(&full);
        glDisable(GL_BLEND);
    }
    vglSwapBuffers(GL_FALSE);
}
