#include "video.h"

#include "config.h"
#include "log.h"
#include "paths.h"
#include "threadstat.h"

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

/* The two DS screens' 2D, several textures each used in turn: a texture the GPU may still be
 * reading for a frame not yet shown is not written. vitaGL copies a texture whole (read back
 * from video memory, slow for the CPU) before a glTexSubImage2D when it was drawn in the last
 * 4 frames (FRAME_PURGE_FREQ): with 3 a screen, every upload paid that copy. With 6, the
 * texture written was last drawn at least 6 frames before. */
#define TEX_RING 6
static GLuint s_tex_ring[2][TEX_RING], s_overlay_tex;
static int s_tex_at[2];
#define s_tex_cur(i) (s_tex_ring[i][s_tex_at[i]])
/* the last video_present's time in its uploads and in vglSwapBuffers (us), for the log */
static uint32_t s_upload_us, s_swap_us;
static uint64_t s_swap_cpu_total, s_swap_wall_total; /* for video_take_swap_cpu */
static uint64_t s_upload_total;
/* With the detailed log, one frame a second is drawn alone: the GPU is drained before it
 * (video_gpu_probe_begin) and waited for before its swap, which gives that frame's own GPU time.
 * 0.1.0 only waited at the swap, which also counted the frames queued for display. */
static uint32_t s_gpu_total, s_gpu_max, s_gpu_n;
static int s_gpu_probing;

int video_gpu_probe_begin(void)
{
    static uint32_t n;
    /* config debug = 2 only: draining the GPU costs frames, the logs of debug = 1 runs showed
     * lower frame rates than the game had */
    s_gpu_probing = kh_config.debug >= 2 && ++n % 60 == 0;
    if (s_gpu_probing)
        glFinish();
    return s_gpu_probing;
}
static const uint32_t *s_overlay;
static ScreenLayout s_layout = LAYOUT_TOP_MAIN;
static ScreenRect s_rect[2];
static int s_inset = -1; /* the screen drawn small over the other one, -1 for none */
static volatile int s_pending = -1; /* a layout asked for from another thread (input) */
static GLuint s_compose, s_compose_vbo;
/* The composition shader in four builds, each with only what its passes use: the screens
 * (covering the whole display every frame), the screens with the HUD blocks masked, the panels
 * and HUD blocks, the display captures. One build with everything cost the GPU frames (0.1.8). */
enum { PROG_SCREEN, PROG_PANEL, PROG_CAP, PROGS };
typedef struct {
    GLuint id;
    GLint c2d, c3d, bright, hofs, blend, backdrop, prev, cap, flip, fx, filt, panel, panel_rect;
} ComposeProg;
static ComposeProg s_progs[PROGS];
/* config hud_size: the HUD's corners on the DS top screen (x0, y0, x1, y1), as gpu2d's
 * hud_codes marks them, each drawn smaller in its own corner (top-left, top-right,
 * bottom-left, bottom-right): hearts and chain, none (the target's name and HP keep their
 * size), the command deck, the HP gauge with the faces */
static const float s_hud_blocks[4][4] = {
    { 0, 0, 120, 60 }, { 0, 0, 0, 0 }, { 0, 60, 140, 192 }, { 140, 90, 256, 192 } };

static int s_hud_shrink; /* the field's HUD corners are drawn smaller (gpu2d marks them) */
/* the panel being drawn (draw_panels): style, radius, width, height; its texture rectangle */
static float s_panel[4], s_panel_rect[4];
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
    "    float wa = (1.0 - f.x) * (1.0 - f.y) * step(0.999, a.a);\n"
    "    float wb = f.x * (1.0 - f.y) * step(0.999, b.a);\n"
    "    float wc = (1.0 - f.x) * f.y * step(0.999, c.a);\n"
    "    float wd = f.x * f.y * step(0.999, d.a);\n"
    "    return (a.rgb * wa + b.rgb * wb + c.rgb * wc + d.rgb * wd) / max(wa + wb + wc + wd, 0.0001);\n"
    "}\n"
    "\n"
    "float4 main(float2 vUv : TEXCOORD0, uniform sampler2D u2d, uniform sampler2D u3d,\n"
    "            uniform float2 uBright, uniform float uHofs, uniform float2 uBlend,\n"
    "            uniform float3 uBackdrop, uniform sampler2D uPrev, uniform float4 uCap,\n"
    "            uniform float uFlip, uniform float4 uFx, uniform float2 uFilt,\n"
    "            uniform float4 uPanel, uniform float4 uPanelRect) : COLOR\n"
    "{\n"
    "    float2 uv2 = vUv;\n"
    "    if (uFlip > 0.5)\n"
    "        uv2.y = 1.0 - vUv.y;\n"
    /* uFx.y < 1: the 2D (HUD) kept 4:3 in the middle of a widescreen 3D; beside it, the 3D */
    "    uv2.x = (uv2.x - 0.5) / uFx.y + 0.5;\n"
    "    float4 b = float4(0.0, 0.0, 0.0, 0.0);\n"
    "    if (uv2.x >= 0.0 && uv2.x <= 1.0) {\n"
    "        b = tex2D(u2d, uv2);\n"
    "        if (uFilt.x > 0.5 && b.a > 0.999)\n"
    "            b.rgb = smooth2d(u2d, uv2, uFilt);\n"
    "    }\n"
    /* the HUD size's codes (gpu2d hud_codes): left out here, drawn again smaller in the
     * corners (a PANEL pass); the 3D over the backdrop in their place */
    "#ifndef PANEL\n"
    "    {\n"
    "        float hc0 = floor(b.a * 255.0 + 0.5);\n"
    "        if (hc0 == 254.0 || hc0 == 225.0 || (hc0 > 160.5 && hc0 < 176.5))\n"
    "            b = float4(uBackdrop, 0.0);\n"
    "    }\n"
    "#endif\n"
    /* config hud_size: the four HUD blocks are drawn again smaller in their corners (a later
     * pass); here their 2D pixels give way to the 3D over the backdrop */
    "    float3 c = b.rgb;\n"
    "    float u = vUv.x + uHofs;\n"
    "    float4 t = tex2D(u3d, float2(u, 1.0 - vUv.y));\n"
    "    if (u < 0.0 || u > 1.0)\n"
    "        t = float4(0.0, 0.0, 0.0, 0.0);\n"
    /* uCap.z: a capture of the 3D layer alone */
    "#ifdef CAP\n"
    "    if (uCap.z > 0.5) {\n"
    "        c = t.rgb;\n"
    "    } else\n"
    "#endif\n"
    "    if (b.a < 0.99) {\n"
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
    "#ifdef CAP\n"
    "    if (uCap.w > 0.5)\n"
    "        c = min(c * uCap.x + tex2D(uPrev, float2(vUv.x, 1.0 - vUv.y)).rgb * uCap.y, 1.0);\n"
    "#endif\n"
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
    /* a single-screen panel (uPanel: style, corner radius, size in Vita pixels; uPanelRect its
     * texture rectangle): its colours, then its rounded corners */
    "    float a = uFx.z;\n"
    "#ifdef PANEL\n"
    "    if (uPanel.x > 2.5) {\n"
    /* a HUD block: its 2D pixels, and its pixels blended over the 3D blended over what is
     * already there with their weight (the 3D under the block's new place) */
    "        float hc = floor(b.a * 255.0 + 0.5);\n"
    "        c = b.rgb;\n"
    "        if (hc == 225.0)\n"
    "            a = a * uBlend.x;\n"
    "        else if (hc > 160.5 && hc < 176.5)\n"
    "            a = a * (hc - 160.0) / 16.0;\n"
    "        else if (hc != 254.0)\n"
    "            a = 0.0;\n"
    "    } else if (uPanel.x > 1.5) {\n"
    "        if (max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)) < 0.1)\n"
    "            c = 1.0 - c;\n"
    "    } else if (uPanel.x > 0.5) {\n"
    "        if (min(c.r, min(c.g, c.b)) > 0.85)\n"
    "            c = float3(0.0, 0.0, 0.0);\n"
    "    }\n"
    "    if (uPanel.y > 0.5) {\n"
    "        float2 lp = (vUv - uPanelRect.xy) / (uPanelRect.zw - uPanelRect.xy) * uPanel.zw;\n"
    "        float2 q = abs(lp - uPanel.zw * 0.5) - (uPanel.zw * 0.5 - uPanel.y);\n"
    "        float d = length(max(q, float2(0.0, 0.0))) - uPanel.y;\n"
    "        a = a * saturate(0.5 - d);\n"
    "    }\n"
    "#endif\n"
    "    return float4(c, a);\n"
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
    static const char *const defines[PROGS] = { "", "#define PANEL\n", "#define CAP\n" };
    int k;
    for (k = 0; k < PROGS; k++) {
        ComposeProg *g = &s_progs[k];
        const size_t n = strlen(defines[k]) + sizeof(s_compose_fs);
        char *src = malloc(n);
        if (!src)
            return;
        snprintf(src, n, "%s%s", defines[k], s_compose_fs);
        g->id = video_build_program(s_compose_vs, src, attribs, 2);
        free(src);
        if (!g->id) {
            LOG("video: composition shader %d did not build: the 3D layer will not show", k);
            return;
        }
        g->c2d = glGetUniformLocation(g->id, "u2d");
        g->c3d = glGetUniformLocation(g->id, "u3d");
        g->bright = glGetUniformLocation(g->id, "uBright");
        g->hofs = glGetUniformLocation(g->id, "uHofs");
        g->blend = glGetUniformLocation(g->id, "uBlend");
        g->backdrop = glGetUniformLocation(g->id, "uBackdrop");
        g->prev = glGetUniformLocation(g->id, "uPrev");
        g->cap = glGetUniformLocation(g->id, "uCap");
        g->flip = glGetUniformLocation(g->id, "uFlip");
        g->fx = glGetUniformLocation(g->id, "uFx");
        g->filt = glGetUniformLocation(g->id, "uFilt");
        g->panel = glGetUniformLocation(g->id, "uPanel");
        g->panel_rect = glGetUniformLocation(g->id, "uPanelRect");

    }
    s_compose = s_progs[PROG_SCREEN].id;
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
    const ComposeProg *g = &s_progs[cap ? PROG_CAP
                                    : (s_panel[0] > 0.5f || s_panel[1] > 0.5f) ? PROG_PANEL
                                    : PROG_SCREEN];
/* a uniform the build leaves out (not used there) has no location */
#define SET(loc, call) do { if ((loc) >= 0) call; } while (0)
    glUseProgram(g->id);
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
    SET(g->c2d, glUniform1i(g->c2d, 0));
    SET(g->c3d, glUniform1i(g->c3d, 1));
    SET(g->prev, glUniform1i(g->prev, 2));
    SET(g->hofs, glUniform1f(g->hofs, s_3d_hofs));
    SET(g->flip, glUniform1f(g->flip, flip2d ? 1.0f : 0.0f));
    SET(g->fx, glUniform4f(g->fx, cap ? 0.0f : (float)kh_config.screen_effect, hud, alpha, 0.0f));
    /* Vita pixels per DS pixel, from the quad's height */
    SET(g->filt, glUniform2f(g->filt, filter == FILTER_2D ? (float)kh_config.filter_2d : 0.0f,
                             fabsf(v[1] - v[9]) * (DISPLAY_H / 2.0f) / 192.0f));
    SET(g->panel, glUniform4f(g->panel, s_panel[0], s_panel[1], s_panel[2], s_panel[3]));
    SET(g->panel_rect, glUniform4f(g->panel_rect, s_panel_rect[0], s_panel_rect[1],
                                   s_panel_rect[2], s_panel_rect[3]));
    if (alpha < 0.999f || s_panel[1] > 0.5f || s_panel[0] > 2.5f) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    if (cap)
        SET(g->cap, glUniform4f(g->cap, s_cap.ka, s_cap.kb, s_cap.src3d ? 1.0f : 0.0f, 1.0f));
    else
        SET(g->cap, glUniform4f(g->cap, 1.0f, 0.0f, 0.0f, 0.0f));
    {
        float eva = (float)(s_3d_bldalpha & 31), evb = (float)((s_3d_bldalpha >> 8) & 31);
        const uint16_t bd = s_3d_backdrop;
        SET(g->blend, glUniform2f(g->blend, (eva > 16 ? 16 : eva) / 16.0f,
                                  (evb > 16 ? 16 : evb) / 16.0f));
        SET(g->backdrop, glUniform3f(g->backdrop, (float)(bd & 31) / 31.0f,
                                     (float)((bd >> 5) & 31) / 31.0f,
                                     (float)((bd >> 10) & 31) / 31.0f));
    }
    SET(g->bright, glUniform2f(g->bright, (!cap && (mode == 1 || mode == 2)) ? (float)mode : 0.0f,
                               (float)f / 16.0f));
#undef SET
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

/* per s_hud_blocks entry: the box the HUD fills there (x1 <= x0: nothing) */
static int s_hud_box[4][4];

void video_set_hud_boxes(const int boxes[3][4])
{
    static const int block_of[3] = { 0, 2, 3 }; /* gpu2d's corners -> s_hud_blocks */
    int z;
    for (z = 0; z < 3; z++)
        memcpy(s_hud_box[block_of[z]], boxes[z], sizeof(s_hud_box[0]));
}

void video_set_hud_shrink(int on)
{
    s_hud_shrink = on != 0;
}

static void draw_composed(int screen)
{
    float v[16];
    const float hud = screen == s_inset ? 1.0f : s_hud_scale;
    const int shrink = s_hud_shrink && screen != s_inset && kh_config.hud_size < 100;
    screen_quad(screen, v);
    compose_pass(v, s_tex_cur(screen), 0, s_3d_tex, s_3d_bright, 0, hud, screen_alpha(screen),
                 FILTER_2D);
    if (shrink) {
        /* each block again, smaller, held to its corner of the screen */
        const ScreenRect *r = &s_rect[screen];
        const float k = (float)kh_config.hud_size / 100.0f;
        int b;
        for (b = 0; b < 4; b++) {
            const float *q = s_hud_blocks[b];
            const int *bx = s_hud_box[b];
            if (q[2] <= q[0] || bx[2] <= bx[0] || bx[3] <= bx[1])
                continue;
            /* the corner shrunk to its own edges, then only the box the HUD fills in it */
            const float zx0 = r->x + ((q[0] / DS_SCREEN_W - 0.5f) * hud + 0.5f) * r->w;
            const float zx1 = r->x + ((q[2] / DS_SCREEN_W - 0.5f) * hud + 0.5f) * r->w;
            const float zy0 = r->y + q[1] / DS_SCREEN_H * r->h, zy1 = r->y + q[3] / DS_SCREEN_H * r->h;
            const float sx = (zx1 - zx0) / (q[2] - q[0]) * k, sy = (zy1 - zy0) / (q[3] - q[1]) * k;
            const float ox = (b & 1) ? zx1 - (q[2] - q[0]) * sx : zx0; /* right ones keep their right edge */
            const float oy = (b & 2) ? zy1 - (q[3] - q[1]) * sy : zy0; /* bottom ones their bottom */
            const float u0 = (float)bx[0] / DS_SCREEN_W, u1 = (float)bx[2] / DS_SCREEN_W;
            const float v0 = (float)bx[1] / DS_SCREEN_H, v1 = (float)bx[3] / DS_SCREEN_H;
            const float X0 = ox + (bx[0] - q[0]) * sx, X1 = ox + (bx[2] - q[0]) * sx;
            const float Y0 = oy + (bx[1] - q[1]) * sy, Y1 = oy + (bx[3] - q[1]) * sy;
            {
                const float x0 = X0 / (DISPLAY_W / 2.0f) - 1.0f, x1 = X1 / (DISPLAY_W / 2.0f) - 1.0f;
                const float y0 = 1.0f - Y0 / (DISPLAY_H / 2.0f), y1 = 1.0f - Y1 / (DISPLAY_H / 2.0f);
                const float vq[16] = { x0, y0, u0, v0, x1, y0, u1, v0,
                                       x0, y1, u0, v1, x1, y1, u1, v1 };
                s_panel[0] = 3.0f; /* the block's 2D pixels only */
                /* the same picture, its HUD codes only */
                compose_pass(vq, s_tex_cur(screen), 0, 0, s_3d_bright, 0, 1.0f,
                             screen_alpha(screen), FILTER_TEXEL);
                s_panel[0] = 0.0f;
            }
        }
    }
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
/* Experimental single screen (config single_screen, game.c decides when): the top screen alone
 * over the display, and the config's panels -- rectangles of the bottom screen -- over it. */
static int s_single;
/* the bottom screen is all black (the field's conversations): as the small screen, not shown */
static int s_inset_blank;
static int s_tutorial; /* the bottom screen holds a tutorial page: shown whole, in the middle */
/* the tutorial page: the whole bottom screen, two Vita pixels per DS pixel, centred */
static const int s_tutorial_panel[KH_PANEL_FIELDS] = { 0, 0, 256, 192, KH_PANEL_TOP_CENTER, 0, 80,
                                                       200, KH_STYLE_PLAIN, 12, 0 };
static float s_panel_vis[KH_PANELS] = { 1, 1, 1, 1 }; /* 0 hidden .. 1 shown (autohide panels) */

void video_set_tutorial(int on)
{
    s_tutorial = on != 0;
}

void video_set_panel_visibility(int i, float vis)
{
    if (i >= 0 && i < KH_PANELS)
        s_panel_vis[i] = vis < 0 ? 0 : vis > 1 ? 1 : vis;
}

static ScreenRect panel_rect(const int *p);

/* where panel i is now: an autohide one slides in from the edge it is anchored to */
static ScreenRect panel_rect_now(int i)
{
    const int *p = kh_config.panel[i];
    ScreenRect r = panel_rect(p);
    if (p[KH_PANEL_AUTOHIDE]) {
        const float t = s_panel_vis[i], e = t * t * (3.0f - 2.0f * t); /* eased */
        const int bottom = p[KH_PANEL_ANCHOR] == KH_PANEL_BOTTOM_LEFT ||
                           p[KH_PANEL_ANCHOR] == KH_PANEL_BOTTOM_RIGHT ||
                           p[KH_PANEL_ANCHOR] == KH_PANEL_BOTTOM_CENTER;
        const int away = bottom ? DISPLAY_H - r.y : r.y + r.h;
        r.y += (int)((1.0f - e) * (float)away) * (bottom ? 1 : -1);
        if (t <= 0.0f)
            r.w = r.h = 0;
    }
    return r;
}

static ScreenRect panel_rect(const int *p)
{
    /* the panels always on screen (the map, the target) follow config hud_size, as the HUD */
    const int hud = p != s_tutorial_panel && (p[KH_PANEL_AUTOHIDE] == KH_SHOW_ALWAYS ||
                                              p[KH_PANEL_AUTOHIDE] == KH_SHOW_ON_RED)
                        ? kh_config.hud_size : 100;
    const int w = p[KH_PANEL_SW] * p[KH_PANEL_SCALE] / 100 * hud / 100;
    const int h = p[KH_PANEL_SH] * p[KH_PANEL_SCALE] / 100 * hud / 100;
    const int dx = p[KH_PANEL_DX] * hud / 100, dy = p[KH_PANEL_DY];
    switch (p[KH_PANEL_ANCHOR]) {
    case KH_PANEL_TOP_RIGHT: return (ScreenRect){ DISPLAY_W - w - dx, dy, w, h };
    case KH_PANEL_BOTTOM_LEFT: return (ScreenRect){ dx, DISPLAY_H - h - dy, w, h };
    case KH_PANEL_BOTTOM_RIGHT: return (ScreenRect){ DISPLAY_W - w - dx, DISPLAY_H - h - dy, w, h };
    case KH_PANEL_TOP_CENTER: return (ScreenRect){ (DISPLAY_W - w) / 2 + dx, dy, w, h };
    case KH_PANEL_BOTTOM_CENTER: return (ScreenRect){ (DISPLAY_W - w) / 2 + dx, DISPLAY_H - h - dy, w, h };
    default: return (ScreenRect){ dx, dy, w, h };
    }
}

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
    if (s_single) {
        /* the top screen as in the top-main layout; the bottom one only through its panels */
        s_rect[0] = kh_config.aspect == KH_ASPECT_4_3
                        ? (ScreenRect){ (DISPLAY_W - 725) / 2, 0, 725, 544 }
                        : (ScreenRect){ 0, 0, DISPLAY_W, DISPLAY_H };
        s_rect[1] = (ScreenRect){ 0, 0, DISPLAY_W, DISPLAY_H };
        s_inset = -1;
    }
}

void video_set_single_screen(int on)
{
    on = on != 0;
    if (on == s_single)
        return;
    s_single = on;
    compute_layout();
}

int video_single_screen(void)
{
    return s_single;
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

void video_set_inset_blank(int blank)
{
    s_inset_blank = blank != 0;
}

int video_on_inset(int px, int py)
{
    const ScreenRect *r;
    if (s_inset < 0 || s_pending >= 0 || (s_inset == 1 && s_inset_blank))
        return 0;
    r = &s_rect[s_inset];
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

int video_map_touch(int px, int py, int *x, int *y)
{
    const ScreenRect *r = &s_rect[1];
    if (s_single) {
        /* a panel is the bottom screen under it: a touch there is a touch on the DS's */
        int i;
        for (i = 0; i < KH_PANELS; i++) {
            const int *p = s_tutorial ? s_tutorial_panel : kh_config.panel[i];
            const ScreenRect q = s_tutorial ? panel_rect(p) : panel_rect_now(i);
            if (s_tutorial && i)
                break;
            if (!p[KH_PANEL_SW] || !q.w || !q.h || px < q.x || px >= q.x + q.w || py < q.y ||
                py >= q.y + q.h)
                continue;
            *x = p[KH_PANEL_SX] + (px - q.x) * p[KH_PANEL_SW] / q.w;
            *y = p[KH_PANEL_SY] + (py - q.y) * p[KH_PANEL_SH] / q.h;
            return 1;
        }
        return 0;
    }
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

    for (i = 0; i < 2 * TEX_RING; i++) {
        GLuint *t = &s_tex_ring[i / TEX_RING][i % TEX_RING];
        glGenTextures(1, t);
        glBindTexture(GL_TEXTURE_2D, *t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, DS_SCREEN_W, DS_SCREEN_H, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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

/* the single-screen panels: each its rectangle of the bottom screen, over the top one */
static void draw_panels(void)
{
    const float alpha = (float)kh_config.panel_opacity / 100.0f;
    int i;
    for (i = 0; i < KH_PANELS; i++) {
        const int *p = s_tutorial ? s_tutorial_panel : kh_config.panel[i];
        const ScreenRect q = s_tutorial ? panel_rect(p) : panel_rect_now(i);
        float x0, x1, y0, y1, u0, u1, v0, v1;
        if (s_tutorial && i)
            break; /* the tutorial page alone */
        if (!p[KH_PANEL_SW] || !p[KH_PANEL_SH] || q.w <= 0 || q.h <= 0)
            continue;
        x0 = q.x / (DISPLAY_W / 2.0f) - 1.0f, x1 = (q.x + q.w) / (DISPLAY_W / 2.0f) - 1.0f;
        y0 = 1.0f - q.y / (DISPLAY_H / 2.0f), y1 = 1.0f - (q.y + q.h) / (DISPLAY_H / 2.0f);
        u0 = (float)p[KH_PANEL_SX] / DS_SCREEN_W, u1 = (float)(p[KH_PANEL_SX] + p[KH_PANEL_SW]) / DS_SCREEN_W;
        v0 = (float)p[KH_PANEL_SY] / DS_SCREEN_H, v1 = (float)(p[KH_PANEL_SY] + p[KH_PANEL_SH]) / DS_SCREEN_H;
        {
            const float v[16] = { x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1, x1, y1, u1, v1 };
            s_panel[0] = (float)p[KH_PANEL_STYLE];
            s_panel[1] = (float)p[KH_PANEL_RADIUS];
            s_panel[2] = (float)q.w, s_panel[3] = (float)q.h;
            s_panel_rect[0] = u0, s_panel_rect[1] = v0, s_panel_rect[2] = u1, s_panel_rect[3] = v1;
            compose_pass(v, s_tex_cur(1), 0, 0, 0, 0, 1.0f, alpha, FILTER_2D);
            memset(s_panel, 0, sizeof(s_panel));
        }
    }
}

void video_present(const uint32_t *top, const uint32_t *bottom)
{
    const uint32_t *src[2] = { top, bottom };
    int k;
    uint64_t t_swap;

    s_upload_us = 0;
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
        if (src[i]) {
            const uint64_t t = sceKernelGetProcessTimeWide();
            s_tex_at[i] = (s_tex_at[i] + 1) % TEX_RING;
            glBindTexture(GL_TEXTURE_2D, s_tex_cur(i));
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DS_SCREEN_W, DS_SCREEN_H, GL_RGBA,
                            GL_UNSIGNED_BYTE, src[i]);
            s_upload_us += (uint32_t)(sceKernelGetProcessTimeWide() - t);
            s_upload_total += (uint32_t)(sceKernelGetProcessTimeWide() - t);
        }
        glBindTexture(GL_TEXTURE_2D, s_tex_cur(i));
        if (i == s_inset && i == 1 && s_inset_blank)
            continue; /* the small screen is the bottom one, all black: not shown */
        if (i == 1 && s_single) {
            if (s_compose)
                draw_panels();
            continue;
        }
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
            compose_pass(v, s_tex_cur(i), 0, 0, 0, 0, 1.0f, screen_alpha(i), FILTER_2D);
        } else {
            draw_quad(&s_rect[i]);
        }
    }
    if (s_overlay) {
        /* only the overlay's drawn rows and columns: a frame-rate counter is a few dozen
         * pixels, and the whole display drawn over (and uploaded) for it cost the GPU about
         * as much as composing the screen once more (0.1.14) */
        static int bx0, by0, bx1, by1;
        glBindTexture(GL_TEXTURE_2D, s_overlay_tex);
        if (s_overlay_dirty) {
            int x, y;
            s_overlay_dirty = 0;
            bx0 = VIDEO_OVERLAY_W, by0 = VIDEO_OVERLAY_H, bx1 = by1 = 0;
            for (y = 0; y < VIDEO_OVERLAY_H; y++)
                for (x = 0; x < VIDEO_OVERLAY_W; x++)
                    if (s_overlay[y * VIDEO_OVERLAY_W + x] >> 24) {
                        if (x < bx0) bx0 = x;
                        if (x >= bx1) bx1 = x + 1;
                        if (y < by0) by0 = y;
                        if (y >= by1) by1 = y + 1;
                    }
            if (by1 > by0)
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, by0, VIDEO_OVERLAY_W, by1 - by0, GL_RGBA,
                                GL_UNSIGNED_BYTE, s_overlay + by0 * VIDEO_OVERLAY_W);
        }
        if (bx1 > bx0 && by1 > by0) {
            const float sx = (float)DISPLAY_W / VIDEO_OVERLAY_W, sy = (float)DISPLAY_H / VIDEO_OVERLAY_H;
            const float x0 = bx0 * sx, y0 = by0 * sy, x1 = bx1 * sx, y1 = by1 * sy;
            const float pos[] = { x0, y0, x1, y0, x0, y1, x1, y1 };
            const float u0 = (float)bx0 / VIDEO_OVERLAY_W, u1 = (float)bx1 / VIDEO_OVERLAY_W;
            const float v0 = (float)by0 / VIDEO_OVERLAY_H, v1 = (float)by1 / VIDEO_OVERLAY_H;
            const float uv[] = { u0, v0, u1, v0, u0, v1, u1, v1 };
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glVertexPointer(2, GL_FLOAT, 0, pos);
            glTexCoordPointer(2, GL_FLOAT, 0, uv);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDisable(GL_BLEND);
        }
    }
    if (s_gpu_probing) {
        const uint64_t t = sceKernelGetProcessTimeWide();
        uint32_t d;
        glFinish();
        d = (uint32_t)(sceKernelGetProcessTimeWide() - t);
        s_gpu_total += d;
        s_gpu_n++;
        if (d > s_gpu_max)
            s_gpu_max = d;
        s_gpu_probing = 0;
    }
    {
        const uint64_t c = threadstat_self_us();
        t_swap = sceKernelGetProcessTimeWide();
        vglSwapBuffers(GL_FALSE);
        s_swap_us = (uint32_t)(sceKernelGetProcessTimeWide() - t_swap);
        s_swap_wall_total += s_swap_us;
        s_swap_cpu_total += threadstat_self_us() - c;
    }
}

void video_take_gpu_probe(uint32_t *avg_us, uint32_t *max_us)
{
    *avg_us = s_gpu_n ? s_gpu_total / s_gpu_n : 0;
    *max_us = s_gpu_max;
    s_gpu_total = s_gpu_max = s_gpu_n = 0;
}

void video_take_swap_cpu(uint64_t *cpu_us, uint64_t *wall_us, uint64_t *upload_us)
{
    *cpu_us = s_swap_cpu_total;
    *wall_us = s_swap_wall_total;
    *upload_us = s_upload_total;
    s_swap_cpu_total = s_swap_wall_total = s_upload_total = 0;
}

void video_present_times(uint32_t *upload_us, uint32_t *swap_us)
{
    *upload_us = s_upload_us;
    *swap_us = s_swap_us;
}
