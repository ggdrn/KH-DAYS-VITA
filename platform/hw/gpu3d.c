/* The DS 3D rendering engine on the GPU (see gpu3d.h).
 *
 * The geometry engine's vertices go up as they are (clip space with the viewport folded in) in
 * one vertex buffer per frame; polygons are drawn in the frame's order (opaque, then the
 * sorted translucent ones) as indexed triangles, batched while the GL state stays the same.
 * The fragment shader does the DS polygon modes (modulation, decal, toon, highlight) with the
 * toon table as a 32x1 texture, the alpha test, and writes premultiplied alpha so that the
 * composition can lay the layer over the 2D ones below it.
 *
 * Not yet: shadow polygons (skipped), wireframe (alpha 0, skipped), fog, edge marking,
 * anti-aliasing, the rear-plane bitmap, w-buffering (drawn with the z-buffer). */
#include "hw/gpu3d.h"

#include "hw/io.h"
#include "hw/textures.h"
#include "hw/vram.h"
#include "config.h"
#include "log.h"
#include "video.h"
#include "workers.h"

#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <psp2/io/stat.h>
#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

static const char s_vs[] =
    "void main(float4 aPos, float2 aTex, float4 aCol,\n"
    "          uniform float2 uTexScale,\n"
    "          out float4 vPos : POSITION, out float4 vCol : COLOR, out float2 vTex : TEXCOORD0)\n"
    "{\n"
    "    vPos = aPos;\n"
    "    vCol = float4(aCol.rgb * (255.0 / 63.0), aCol.a * (255.0 / 31.0));\n"
    "    vTex = aTex * uTexScale;\n"
    "}\n";

/* uMode: 0 modulation, 1 decal, 2 toon, 3 highlight */
static const char s_fs[] =
    "float4 main(float4 vCol : COLOR, float2 vTex : TEXCOORD0,\n"
    "            uniform sampler2D uTex, uniform sampler2D uToon,\n"
    "            uniform float uMode, uniform float uTextured, uniform float uAlphaRef,\n"
    "            uniform float uCutout, uniform float uSplit) : COLOR\n"
    "{\n"
    "    float4 t = float4(1.0, 1.0, 1.0, 1.0);\n"
    "    float4 r;\n"
    "    if (uTextured > 0.5)\n"
    "        t = tex2D(uTex, vTex);\n"
    /* a filtered cut-out's edge (uCutout): on an opaque polygon, where the DS's own texel
     * edge is (drawn wherever alpha was above 0, the edge was half a texel too thick and
     * dark); blended, a third of a texel wide instead of a whole one */
    "#ifndef OPAQUE\n"
    "    if (uCutout > 1.5) {\n"
    "        if (t.a < 0.5)\n"
    "            discard;\n"
    "        t.a = 1.0;\n"
    "    } else if (uCutout > 0.5) {\n"
    "        t.a = saturate((t.a - 0.5) * 3.0 + 0.5);\n"
    "    }\n"
    "#endif\n"
    "    if (uMode < 0.5) {\n"
    "        r = vCol * t;\n"
    "    } else if (uMode < 1.5) {\n"
    "        r = float4(lerp(vCol.rgb, t.rgb, t.a), vCol.a);\n"
    "    } else {\n"
    "        float idx = floor(vCol.r * 31.5);\n"
    "        float3 toon = tex2D(uToon, float2((idx + 0.5) / 32.0, 0.5)).rgb;\n"
    "        if (uMode < 2.5)\n"
    "            r = float4(toon, vCol.a) * t;\n"
    "        else\n"
    "            r = float4(min(t.rgb * vCol.rrr + toon, 1.0), vCol.a * t.a);\n"
    "    }\n"
    /* OPAQUE: the build for opaque polygons with no texture or one with no clear texel, which
     * can never be discarded: a shader with a discard keeps the GPU from skipping the pixels a
     * nearer polygon hides (the Vita's GPU does that before shading), and the whole scene paid
     * for every layer of it */
    "#ifndef OPAQUE\n"
    "    if (r.a <= uAlphaRef)\n"
    "        discard;\n"
    /* uSplit: a polygon translucent only through its texture's alpha, in two passes (see
     * flush): 1 its opaque texels alone, 2 the others */
    "    if (uSplit > 1.5) {\n"
    "        if (r.a > 0.984)\n"
    "            discard;\n"
    "    } else if (uSplit > 0.5) {\n"
    "        if (r.a <= 0.984)\n"
    "            discard;\n"
    "    }\n"
    "#endif\n"
    "    return float4(r.rgb * r.a, r.a);\n"
    "}\n";

enum { A_POS, A_TEX, A_COL };

static int s_scale, s_w, s_h;
static GLuint s_prog, s_fbo, s_color, s_depth, s_vbo, s_toon_tex;
/* the two builds of the polygon shader: [0] with the alpha test, [1] OPAQUE without */
typedef struct {
    GLuint id;
    GLint tex_scale, mode, textured, alpha_ref, tex, toon, cutout, split;
} Prog3d;
static Prog3d s_p3[2];
static const Prog3d *s_cur3; /* the one in use while a frame is drawn */
static uint32_t s_frame;
static uint32_t s_last_serial;
static KhGpu3dStats s_stats;
volatile int kh_gpu3d_debug;
volatile uint32_t kh_gpu3d_mixes;

/* ---- texture cache --------------------------------------------------------------------- */

#define TEX_SLOTS 1024 /* open addressing, power of two */
#define TEX_MAX_LIVE 700

typedef struct {
    uint32_t key_img, key_pltt; /* key_img 0: empty */
    uint32_t hash, gen, used; /* gen: the texture-VRAM generation hash was taken at */
    uint32_t checked;         /* the frame it was last hashed in */
    GLuint tex;
    float sx, sy;
    float cutout; /* 1: every texel opaque or clear (no partial alpha), see texels_smooth */
    GLint ws, wt;     /* the DS's wrap modes */
    int clamped;      /* the GL texture clamps instead (see wrap_for_frame) */
    uint32_t wrapped; /* the last frame a polygon used it beyond its edges */
    int opaque;       /* no texel less than opaque: never discarded (the OPAQUE shader) */
} TexEntry;

static TexEntry s_tex[TEX_SLOTS];
static uint32_t s_tex_gen; /* kh_vram_tex_generation() for this frame */
static int s_tex_live;
static uint32_t *s_decode;
static size_t s_decode_size;

static uint32_t tex_key_img(uint32_t teximage)
{
    /* everything but the texture-coordinate generation mode (bits 30-31); bit 31 marks used */
    return (teximage & 0x3fffffffu) | 0x80000000u;
}

static uint32_t tex_slot(uint32_t ki, uint32_t kp)
{
    return ((ki * 0x9e3779b1u) ^ (kp * 0x85ebca6bu)) >> 22; /* 10 bits */
}

static void tex_evict_old(void)
{
    int i;
    for (i = 0; i < TEX_SLOTS; i++) {
        TexEntry *e = &s_tex[i];
        if (e->key_img && s_frame - e->used > 120) {
            glDeleteTextures(1, &e->tex);
            memset(e, 0, sizeof(*e));
            s_tex_live--;
        }
    }
    /* open addressing: re-insert the survivors so that no probe chain has a hole */
    {
        static TexEntry tmp[TEX_SLOTS];
        memcpy(tmp, s_tex, sizeof(tmp));
        memset(s_tex, 0, sizeof(s_tex));
        for (i = 0; i < TEX_SLOTS; i++) {
            uint32_t j;
            if (!tmp[i].key_img)
                continue;
            j = tex_slot(tmp[i].key_img, tmp[i].key_pltt);
            while (s_tex[j].key_img)
                j = (j + 1) & (TEX_SLOTS - 1);
            s_tex[j] = tmp[i];
        }
    }
}

/* For the smoothed filter (config texture_filter): a clear texel's colour is black, and a
 * bilinear sample between it and its opaque neighbour came out half dark, the dark outline
 * round every cut-out (leaves, hair, sprites' edges). Clear texels next to opaque ones take
 * their neighbours' average colour (their alpha stays 0). Returns 1 when the texture has no
 * partial alpha: its edges are then sharpened in the shader, not left a texel wide. */
static int texels_smooth(uint32_t *px, int w, int h)
{
    int x, y, cutout = 1;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            const uint32_t a = px[y * w + x] >> 24;
            if (a && a != 0xff)
                cutout = 0;
        }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t *p = &px[y * w + x], r = 0, g = 0, b = 0, n = 0;
            int dx, dy;
            if (*p >> 24)
                continue;
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++) {
                    const int xx = x + dx, yy = y + dy;
                    uint32_t q;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h)
                        continue;
                    q = px[yy * w + xx];
                    if (!(q >> 24))
                        continue;
                    r += q & 0xff, g += (q >> 8) & 0xff, b += (q >> 16) & 0xff, n++;
                }
            if (n)
                *p = r / n | (g / n) << 8 | (b / n) << 16;
        }
    return cutout;
}

/* the decoded texels into the entry's GL texture */
static void tex_put(TexEntry *e, uint32_t teximage, const uint32_t *px, int cutout)
{
    const int w = kh_tex_width(teximage), h = kh_tex_height(teximage);
    GLint ws, wt;
    s_stats.fmt[kh_tex_format(teximage)]++;
    if (kh_tex_source_empty(teximage)) {
        static uint32_t logged;
        s_stats.empty_src++;
        if (kh_log_verbose && logged++ < 40)
            LOG("gpu3d: frame %u: texture %08x (pltt %04x) from all-zero VRAM; VRAMCNT %08x %08x",
                (unsigned)s_frame, (unsigned)teximage, (unsigned)0, (unsigned)KH_IO32(0x04000240),
                (unsigned)KH_IO32(0x04000244));
    }
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, kh_config.texture_filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, kh_config.texture_filter ? GL_LINEAR : GL_NEAREST);
    ws = !(teximage & (1u << 16)) ? GL_CLAMP_TO_EDGE : (teximage & (1u << 18)) ? GL_MIRRORED_REPEAT : GL_REPEAT;
    wt = !(teximage & (1u << 17)) ? GL_CLAMP_TO_EDGE : (teximage & (1u << 19)) ? GL_MIRRORED_REPEAT : GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
    e->ws = ws, e->wt = wt, e->clamped = 0;
    {
        int i;
        e->opaque = 1;
        for (i = 0; i < w * h; i++)
            if ((px[i] >> 24) != 0xff) {
                e->opaque = 0;
                break;
            }
    }
    e->cutout = cutout ? 1.0f : 0.0f;
    e->sx = 1.0f / (float)w;
    e->sy = 1.0f / (float)h;
    s_stats.textures_decoded++;
}

static void tex_upload(TexEntry *e, uint32_t teximage, uint32_t pltt)
{
    const size_t need = (size_t)kh_tex_width(teximage) * kh_tex_height(teximage) * 4;
    if (need > s_decode_size) {
        free(s_decode);
        s_decode = malloc(need);
        s_decode_size = s_decode ? need : 0;
        if (!s_decode)
            return;
    }
    kh_tex_decode(teximage, pltt, s_decode);
    tex_put(e, teximage, s_decode,
            kh_config.texture_filter
                ? texels_smooth(s_decode, kh_tex_width(teximage), kh_tex_height(teximage)) : 0);
}

/* Textures to decode before the frame is drawn (kh_gpu3d_prepare): a burst of new ones (a
 * scene's models, an enemy appearing) is decoded on two cores, the helper's and this one,
 * instead of one after the other inside the draw. The GL uploads stay on this thread. */
#define DEFER_MAX 96
#define DEFER_ARENA (4u * 1024 * 1024)

typedef struct {
    TexEntry *e;
    uint32_t teximage, pltt;
    uint32_t *px; /* in the arena */
    int cutout;
} TexJob;

static TexJob s_jobs[DEFER_MAX];
static int s_njobs, s_deferring;
static uint32_t *s_arena;
static size_t s_arena_used;

static void decode_job(int i, void *arg)
{
    (void)arg;
    TexJob *j = &s_jobs[i];
    kh_tex_decode(j->teximage, j->pltt, j->px);
    j->cutout = kh_config.texture_filter
                    ? texels_smooth(j->px, kh_tex_width(j->teximage), kh_tex_height(j->teximage)) : 0;
}

/* the upload, or a place in the batch decoded by kh_gpu3d_prepare */
static void tex_refresh(TexEntry *e, uint32_t teximage, uint32_t pltt)
{
    if (s_deferring && s_njobs < DEFER_MAX) {
        const size_t words = (size_t)kh_tex_width(teximage) * kh_tex_height(teximage);
        if (s_arena && s_arena_used + words <= DEFER_ARENA / 4) {
            TexJob *j = &s_jobs[s_njobs++];
            j->e = e, j->teximage = teximage, j->pltt = pltt;
            j->px = s_arena + s_arena_used;
            s_arena_used += words;
            return;
        }
    }
    tex_upload(e, teximage, pltt);
}

/* The cache entry for a polygon's texture, decoded or re-decoded when its VRAM changed. */
static TexEntry *tex_get(uint32_t teximage, uint32_t pltt)
{
    const uint32_t ki = tex_key_img(teximage);
    const uint32_t kp = kh_tex_format(teximage) == 7 ? 0 : pltt;
    uint32_t j = tex_slot(ki, kp);
    TexEntry *e;

    while (s_tex[j].key_img && (s_tex[j].key_img != ki || s_tex[j].key_pltt != kp))
        j = (j + 1) & (TEX_SLOTS - 1);
    e = &s_tex[j];
    if (!e->key_img) {
        if (s_tex_live >= TEX_MAX_LIVE)
            return NULL; /* full this frame; eviction runs between frames */
        e->key_img = ki;
        e->key_pltt = kp;
        glGenTextures(1, &e->tex);
        e->gen = s_tex_gen - 1;
        e->hash = 0;
        s_tex_live++;
    }
    e->used = s_frame;
    /* the VRAM generation says when bytes can have changed; besides, every entry is checked
     * again every 32 frames in turn, in case some path writes texture VRAM unseen */
    if (e->checked != s_frame &&
        (e->gen != s_tex_gen || ((uint32_t)(e - s_tex) & 31) == (s_frame & 31) ||
         kh_gpu3d_debug == 2)) {
        /* only after a bank A-G was remapped can the bytes have changed */
        uint32_t hv = kh_tex_hash(teximage, kp);
        e->gen = s_tex_gen;
        e->checked = s_frame;
        if (hv != e->hash || !e->sx) {
            e->hash = hv;
            /* the game drawing while it uploads: VRAM still empty where a texture that is
             * already decoded lives, keep that one rather than flash a blank one */
            if (!(e->sx && kh_tex_source_empty(teximage)))
                tex_refresh(e, teximage, kp);
        }
    }
    return e;
}

/* ---- set-up ---------------------------------------------------------------------------- */

/* the off-screen target the 3D is drawn into, s_w x s_h */
static void make_target(void)
{
    glGenTextures(1, &s_color);
    glBindTexture(GL_TEXTURE_2D, s_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s_w, s_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_color, 0);
    glGenRenderbuffers(1, &s_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, s_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, s_w, s_h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_depth);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static volatile int s_want_scale, s_want_reload;

void kh_gpu3d_set_scale(int scale)
{
    s_want_scale = scale < 1 ? 1 : scale > 4 ? 4 : scale;
}

void kh_gpu3d_reload_textures(void)
{
    s_want_reload = 1;
}

/* requests from the port menu, applied on the thread that draws */
static uint32_t s_batches_serial; /* below, with flush */

static void apply_requests(void)
{
    if (s_want_scale && s_want_scale != s_scale && s_prog) {
        glDeleteFramebuffers(1, &s_fbo);
        glDeleteRenderbuffers(1, &s_depth);
        glDeleteTextures(1, &s_color);
        s_scale = s_want_scale;
        s_w = 256 * s_scale;
        s_h = 192 * s_scale;
        make_target();
        s_last_serial = 0; /* draw the frame again into the new target */
        LOG("gpu3d: now %dx%d (scale %d)", s_w, s_h, s_scale);
    }
    s_want_scale = 0;
    if (s_want_reload) {
        int i;
        s_want_reload = 0;
        s_batches_serial = 0xffffffffu; /* the kept draws would skip the textures' decoding */
        for (i = 0; i < TEX_SLOTS; i++)
            s_tex[i].sx = 0; /* decoded again, with the filter now chosen, when next used */
        for (i = 0; i < TEX_SLOTS; i++)
            s_tex[i].gen = s_tex_gen - 1;
    }
}

int kh_gpu3d_init(int scale)
{
    static const char *const attribs[] = { "aPos", "aTex", "aCol" };

    s_scale = scale < 1 ? 1 : scale > 4 ? 4 : scale;
    s_w = 256 * s_scale;
    s_h = 192 * s_scale;

    {
        static char opaque_fs[sizeof(s_fs) + 32];
        int k;
        snprintf(opaque_fs, sizeof(opaque_fs), "#define OPAQUE\n%s", s_fs);
        for (k = 0; k < 2; k++) {
            Prog3d *g = &s_p3[k];
            g->id = video_build_program(s_vs, k ? opaque_fs : s_fs, attribs, 3);
            if (!g->id) {
                LOG("gpu3d: shaders did not build: no 3D");
                return 0;
            }
            g->tex_scale = glGetUniformLocation(g->id, "uTexScale");
            g->mode = glGetUniformLocation(g->id, "uMode");
            g->textured = glGetUniformLocation(g->id, "uTextured");
            g->cutout = glGetUniformLocation(g->id, "uCutout");
            g->split = glGetUniformLocation(g->id, "uSplit");
            g->alpha_ref = glGetUniformLocation(g->id, "uAlphaRef");
            g->tex = glGetUniformLocation(g->id, "uTex");
            g->toon = glGetUniformLocation(g->id, "uToon");
        }
        s_prog = s_p3[0].id;
        s_cur3 = &s_p3[0];
    }

    make_target();

    glGenTextures(1, &s_toon_tex);
    glBindTexture(GL_TEXTURE_2D, s_toon_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenBuffers(1, &s_vbo);
    LOG("gpu3d: %dx%d (scale %d)", s_w, s_h, s_scale);
    return 1;
}

/* ---- drawing --------------------------------------------------------------------------- */

typedef struct {
    TexEntry *tex;  /* NULL: untextured */
    int mode;       /* POLYGON_ATTR 4-5 */
    int blend;      /* translucent with blending on */
    int depth_write;
    int depth_equal;
    int shadow;     /* 0 no, 1 shadow mask (polygon ID 0), 2 shadow colour */
    int id;         /* POLYGON_ATTR 24-29, the polygon ID */
    int opaque_prog; /* 1: drawn with the OPAQUE shader (nothing in it can be discarded) */
    int tex_alpha;  /* alpha 31, translucent only through its texture (A3I5, A5I3) */
    int pass;       /* flush's passes for those: 1 the opaque texels, 2 the others */
} DrawState;

static uint16_t s_idx[KH_GX_MAX_POLYGONS * 6];
static DrawState s_pstate[KH_GX_MAX_POLYGONS]; /* per polygon, in frame order */
static uint32_t s_pkey[KH_GX_MAX_POLYGONS];
static uint16_t s_popaque[KH_GX_MAX_POLYGONS];

/* Smoothed textures: a repeating texture that no polygon of the frame uses beyond its edges
 * (a flare, a glow, a sprite-like effect drawn once over a quad) clamps instead. Repeating, the
 * bilinear filter at its edge mixed in the opposite edge, a line round each effect. */
static void wrap_for_frame(TexEntry *t)
{
    const int clamp = kh_config.texture_filter && t->wrapped != s_frame &&
                      (t->ws != GL_CLAMP_TO_EDGE || t->wt != GL_CLAMP_TO_EDGE);
    if (clamp == t->clamped)
        return;
    t->clamped = clamp;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp ? GL_CLAMP_TO_EDGE : t->ws);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp ? GL_CLAMP_TO_EDGE : t->wt);
}

/* a location the build leaves out (not used there) is -1 */
#define SET3(loc, call) do { if ((loc) >= 0) call; } while (0)

static void apply_state(const DrawState *st)
{
    const Prog3d *g = &s_p3[st->opaque_prog];
    if (g != s_cur3) {
        s_cur3 = g;
        glUseProgram(g->id);
    }
    if (st->tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, st->tex->tex);
        wrap_for_frame(st->tex);
        SET3(g->tex_scale, glUniform2f(g->tex_scale, st->tex->sx, st->tex->sy));
        SET3(g->textured, glUniform1f(g->textured, 1.0f));
        SET3(g->cutout, glUniform1f(g->cutout, st->tex->cutout > 0.5f ? (st->blend ? 1.0f : 2.0f) : 0.0f));
    } else {
        SET3(g->textured, glUniform1f(g->textured, 0.0f));
        SET3(g->cutout, glUniform1f(g->cutout, 0.0f));
    }
    SET3(g->mode, glUniform1f(g->mode, st->mode == 3 ? 0.0f : (float)st->mode));
    SET3(g->split, glUniform1f(g->split, (float)st->pass));
    if (st->blend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    glDepthMask(st->depth_write ? GL_TRUE : GL_FALSE);
    glDepthFunc(st->depth_equal ? GL_LEQUAL : GL_LESS);
    /* DS shadow polygons through the stencil: an ID-0 shadow polygon marks the pixels where it
     * lies behind the scene (its depth test fails) without drawing; a shadow polygon of
     * another ID draws its colour on marked pixels only and clears the mark */
    /* the stencil: bit 0 the shadow mark, bits 1-6 the polygon ID of the opaque pixel (the
     * DS's attribute buffer), so that a shadow does not fall on the model of its own ID */
    glEnable(GL_STENCIL_TEST);
    switch (st->shadow) {
    case 1:
        glStencilMask(0x01);
        glStencilFunc(GL_ALWAYS, 1, 0xff);
        glStencilOp(GL_KEEP, GL_REPLACE, GL_KEEP);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        break;
    case 2:
        glStencilMask(0x01);
        glStencilFunc(GL_EQUAL, 1, 0x01);
        glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        break;
    default:
        if (!st->blend && st->depth_write) {
            glStencilMask(0x7e);
            glStencilFunc(GL_ALWAYS, st->id << 1, 0xff);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        } else {
            glStencilMask(0);
            glStencilFunc(GL_ALWAYS, 0, 0);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        break;
    }
}

/* The frame's draws as last sent: the same frame is drawn twice at 60 fps (its halfway mix,
 * then itself), with other vertices only; the second time these are sent again as they are,
 * with no polygon sorted, grouped or indexed anew (that was half of the 3D's CPU time). */
#define BATCHES_MAX 4096
static struct {
    DrawState st;
    int first, count;
} s_batches[BATCHES_MAX];
static int s_nbatches;
static uint32_t s_batches_serial = 0xffffffffu;
static int s_replaying; /* sending the kept draws: not recorded again */

static void flush(const DrawState *st, int first, int count)
{
    if (!count)
        return;
    if (!s_replaying) {
        if (s_nbatches < BATCHES_MAX) {
            s_batches[s_nbatches].st = *st;
            s_batches[s_nbatches].first = first;
            s_batches[s_nbatches].count = count;
        }
        s_nbatches++;
    }
    if (st->tex_alpha) {
        /* a pixel of alpha 31 is opaque on the DS whatever its polygon: it writes depth and
         * hides what comes after it. Hair is drawn so (Marluxia's frame dump: 129 polygons,
         * A3I5, alpha 31, both sides, no depth write; their texels mostly fully opaque);
         * writing no depth here, its back layers, drawn later, came through the front ones.
         * Its opaque texels first, as opaque, then the translucent ones as before. */
        DrawState o = *st;
        o.tex_alpha = 0;
        o.pass = 1;
        o.blend = 0;
        o.depth_write = 1;
        apply_state(&o);
        glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, s_idx + first);
        o = *st;
        o.tex_alpha = 0;
        o.pass = 2;
        apply_state(&o);
        glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, s_idx + first);
        s_stats.batches += 2;
        return;
    }
    if (st->shadow == 2) {
        /* first unmark the pixels whose opaque polygon has this shadow's ID */
        apply_state(st);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glStencilFunc(GL_EQUAL, (st->id << 1) | 1, 0x7f);
        glStencilOp(GL_KEEP, GL_ZERO, GL_ZERO);
        glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, s_idx + first);
    }
    apply_state(st);
    glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, s_idx + first);
    s_stats.batches++;
}

static inline uint32_t reg32(const KhGxFrame *f, uint32_t addr)
{
    uint32_t v;
    memcpy(&v, f->regs + (addr - 0x04000330), 4);
    return v;
}

static void upload_toon(const KhGxFrame *f)
{
    uint32_t px[32];
    int i;
    for (i = 0; i < 32; i++) {
        uint16_t c;
        uint32_t r, g, b;
        memcpy(&c, f->regs + (0x04000380 - 0x04000330) + i * 2, 2);
        r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        px[i] = (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16 | 0xff000000u;
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_toon_tex);
    {
        /* the same table as last time: no upload (vitaGL copies a texture drawn in the last
         * frames whole before writing it) */
        static uint32_t last[32];
        static int have;
        if (!have || memcmp(last, px, sizeof(px))) {
            memcpy(last, px, sizeof(px));
            have = 1;
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 32, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        }
    }
    glActiveTexture(GL_TEXTURE0);
}

/* ---- diagnosis dump -------------------------------------------------------------------- */

static volatile int s_dump_request;
static int s_dump_color; /* write the drawn 3D layer at the end of this frame */

void kh_gpu3d_request_dump(void)
{
    s_dump_request = 1;
}

void kh_gpu3d_dump_tga(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    uint8_t hdr[18] = { 0 };
    int y, x;
    if (!f)
        return;
    hdr[2] = 2; /* uncompressed true colour */
    hdr[12] = (uint8_t)w, hdr[13] = (uint8_t)(w >> 8);
    hdr[14] = (uint8_t)h, hdr[15] = (uint8_t)(h >> 8);
    hdr[16] = 32;
    hdr[17] = 0x28; /* top-left origin, 8 alpha bits */
    fwrite(hdr, 1, sizeof(hdr), f);
    for (y = 0; y < h; y++) {
        static uint8_t row[4096 * 4];
        for (x = 0; x < w && x < 4096; x++) {
            const uint32_t c = px[y * w + x];
            row[x * 4] = (uint8_t)(c >> 16), row[x * 4 + 1] = (uint8_t)(c >> 8);
            row[x * 4 + 2] = (uint8_t)c, row[x * 4 + 3] = (uint8_t)(c >> 24);
        }
        fwrite(row, 4, (size_t)x, f);
    }
    fclose(f);
}

static void dump_frame(const KhGxFrame *f)
{
    static const char dir[] = "ux0:data/khdays/dump";
    char path[128];
    FILE *list;
    int i, k, ntex = 0;
    static uint32_t seen_img[512], seen_pal[512];

    sceIoMkdir(dir, 0777);
    snprintf(path, sizeof(path), "%s/polygons.txt", dir);
    list = fopen(path, "w");
    if (!list)
        return;
    fprintf(list, "frame %u: %d polygons, %d vertices, DISP3DCNT %04x, swap %u\n",
            (unsigned)f->serial, f->npoly, f->nvtx, (unsigned)f->disp3dcnt, (unsigned)f->swap);
    fprintf(list, "VRAMCNT A-G %02x %02x %02x %02x %02x %02x %02x\n", kh_ds_io[0x240],
            kh_ds_io[0x241], kh_ds_io[0x242], kh_ds_io[0x243], kh_ds_io[0x244], kh_ds_io[0x245],
            kh_ds_io[0x246]);
    for (i = 0; i < f->npoly; i++) {
        const KhGxPolygon *p = &f->poly[f->order[i]];
        fprintf(list, "poly %d: attr %08x teximage %08x pltt %04x tr %d |", i, (unsigned)p->attr,
                (unsigned)p->teximage, (unsigned)p->pltt, p->translucent);
        for (k = 0; k < p->count; k++) {
            const KhGxVertex *v = &f->vtx[p->v[k]];
            fprintf(list, " (%.1f %.1f %.1f %.1f st %.2f %.2f rgb %d %d %d)", v->x / v->w,
                    v->y / v->w, v->z, v->w, v->s, v->t, v->r, v->g, v->b);
        }
        fputc('\n', list);
        if (kh_tex_format(p->teximage) && ntex < 512) {
            const uint32_t img = p->teximage & 0x3fffffffu, pal = p->pltt;
            int j;
            for (j = 0; j < ntex && (seen_img[j] != img || seen_pal[j] != pal); j++)
                ;
            if (j == ntex) {
                const int w = kh_tex_width(img), h = kh_tex_height(img);
                uint32_t *px = malloc((size_t)w * h * 4);
                seen_img[ntex] = img, seen_pal[ntex] = pal, ntex++;
                if (px) {
                    kh_tex_decode(img, pal, px);
                    snprintf(path, sizeof(path), "%s/tex_%08x_%04x.tga", dir, (unsigned)img,
                             (unsigned)pal);
                    kh_gpu3d_dump_tga(path, px, w, h);
                    free(px);
                }
            }
        }
    }
    fclose(list);
    LOG("gpu3d: dumped frame %u: %d polygons, %d textures to %s", (unsigned)f->serial,
        f->npoly, ntex, dir);
}

static uint32_t s_setup_serial;

/* once per new frame, by kh_gpu3d_prepare or else kh_gpu3d_render */
static void frame_setup(const KhGxFrame *f)
{
    if (f->serial == s_setup_serial)
        return;
    s_setup_serial = f->serial;
    s_frame++;
    if ((s_frame & 63) == 0 || s_tex_live >= TEX_MAX_LIVE)
        tex_evict_old();
    s_tex_gen = kh_vram_tex_generation() + kh_tex_map_slots();
}

void kh_gpu3d_prepare(const KhGxFrame *f)
{
    const int textures_on = f && (f->disp3dcnt & 1);
    int i;

    if (!s_prog || !f || !textures_on || f->serial == s_last_serial || f->serial == s_setup_serial)
        return;
    frame_setup(f);
    if (!s_arena)
        s_arena = malloc(DEFER_ARENA);
    s_njobs = 0;
    s_arena_used = 0;
    s_deferring = 1;
    for (i = 0; i < f->npoly; i++) {
        const KhGxPolygon *p = &f->poly[i];
        if (((p->attr >> 16) & 31) && kh_tex_format(p->teximage))
            tex_get(p->teximage, p->pltt);
    }
    s_deferring = 0;
    if (!s_njobs)
        return;
    {
        const uint64_t t = sceKernelGetProcessTimeWide();
        if (s_njobs > 1) {
            workers_begin(decode_job, s_njobs, NULL);
            workers_join();
        } else {
            decode_job(0, NULL);
        }
        for (i = 0; i < s_njobs; i++)
            tex_put(s_jobs[i].e, s_jobs[i].teximage, s_jobs[i].px, s_jobs[i].cutout);
        s_stats.prepare_us += (uint32_t)(sceKernelGetProcessTimeWide() - t);
        if ((uint32_t)s_njobs > s_stats.burst_max)
            s_stats.burst_max = (uint32_t)s_njobs;
    }
    s_njobs = 0;
}

/* The frame's polygons drawn with the vertices vtx (the frame's own, or a mix of two). */
static unsigned draw_frame(const KhGxFrame *f, const KhGxVertex *vtx)
{
    uint64_t t0;
    const int textures_on = f && (f->disp3dcnt & 1);
    const int blending_on = f && (f->disp3dcnt & 8);
    DrawState cur = { 0 }, st;
    int i, nidx = 0, first = 0, have = 0;

    s_stats.disp3dcnt = f->disp3dcnt;
    t0 = sceKernelGetProcessTimeWide();
    frame_setup(f);
    if (s_dump_request) {
        s_dump_request = 0;
        s_dump_color = 1;
        dump_frame(f);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, s_w, s_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    {
        const uint32_t cc = reg32(f, 0x04000350);
        const float a = (float)((cc >> 16) & 31) / 31.0f;
        const float r = (float)(cc & 31) / 31.0f, g = (float)((cc >> 5) & 31) / 31.0f,
                    b = (float)((cc >> 10) & 31) / 31.0f;
        const uint32_t cd = reg32(f, 0x04000354) & 0x7fff;
        glClearColor(r * a, g * a, b * a, a);
        glClearDepthf((float)(cd * 0x200 + 0x1ff) / 16777215.0f);
        glDepthMask(GL_TRUE);
        glStencilMask(0xff);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    if (!f->npoly)
        goto done;

    {
        /* the frame's uniforms in both builds; the first draw picks its own */
        int k;
        for (k = 1; k >= 0; k--) {
            const Prog3d *g = &s_p3[k];
            const int ref = reg32(f, 0x04000340) & 31;
            glUseProgram(g->id);
            SET3(g->tex, glUniform1i(g->tex, 0));
            SET3(g->toon, glUniform1i(g->toon, 1));
            SET3(g->alpha_ref, glUniform1f(g->alpha_ref, (f->disp3dcnt & 4)
                                                        ? ((float)ref + 0.5f) / 31.0f
                                                        : 0.5f / 31.0f));
        }
        s_cur3 = &s_p3[0];
    }
    upload_toon(f);
    glEnable(GL_DEPTH_TEST);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, f->nvtx * (int)sizeof(KhGxVertex), vtx, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(A_POS);
    glEnableVertexAttribArray(A_TEX);
    glEnableVertexAttribArray(A_COL);
    glVertexAttribPointer(A_POS, 4, GL_FLOAT, GL_FALSE, sizeof(KhGxVertex), (void *)0);
    glVertexAttribPointer(A_TEX, 2, GL_FLOAT, GL_FALSE, sizeof(KhGxVertex), (void *)16);
    glVertexAttribPointer(A_COL, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(KhGxVertex), (void *)24);

    if (f->serial == s_batches_serial) {
        /* this frame's draws again, with the vertices just uploaded */
        int b;
        s_replaying = 1;
        for (b = 0; b < s_nbatches; b++)
            flush(&s_batches[b].st, s_batches[b].first, s_batches[b].count);
        s_replaying = 0;
        goto drawn;
    }
    s_nbatches = 0;
    /* opaque polygons first, grouped by GL state (the z-buffer makes their order free; it
     * cuts the draw calls several times), then the translucent ones in the frame's order */
    {
        int nop = 0, k;
        for (i = 0; i < f->npoly; i++) {
            const KhGxPolygon *p = &f->poly[f->order[i]];
            const int alpha = (p->attr >> 16) & 31;
            const int fmt = kh_tex_format(p->teximage);
            DrawState *ps = &s_pstate[i];
            ps->mode = (p->attr >> 4) & 3;
            s_stats.modes[ps->mode]++;
            ps->shadow = ps->mode == 3 ? (((p->attr >> 24) & 63) == 0 ? 1 : 2) : 0;
            ps->id = (int)((p->attr >> 24) & 63);
            if (alpha == 0) {
                s_stats.skipped++; /* wireframe: not yet */
                s_pkey[i] = 0xffffffffu;
                continue;
            }
            ps->tex = (textures_on && fmt) ? tex_get(p->teximage, p->pltt) : NULL;
            if (fmt) {
                s_stats.tex_wanted++;
                if (!ps->tex)
                    s_stats.tex_none++;
            }
            if (ps->tex) {
                const KhGxVertex *a = &f->vtx[p->v[0]], *b = &f->vtx[p->v[1]], *c = &f->vtx[p->v[2]];
                const float tw = 1.0f / ps->tex->sx + 0.05f, th = 1.0f / ps->tex->sy + 0.05f;
                int k;
                for (k = 0; k < p->count; k++) {
                    const KhGxVertex *v = &f->vtx[p->v[k]];
                    if (v->s < -0.05f || v->t < -0.05f || v->s > tw || v->t > th)
                        ps->tex->wrapped = s_frame; /* repeats here: keeps the DS's wrap */
                }
                s_stats.texgen[p->teximage >> 30]++;
                if (a->s == b->s && a->s == c->s && a->t == b->t && a->t == c->t)
                    s_stats.flat_st++;
            }
            ps->blend = p->translucent && blending_on;
            /* opaque, and no texture or one with no clear texel: no alpha test can drop a
             * pixel of it */
            ps->opaque_prog = !p->translucent && alpha == 31 && kh_gpu3d_debug != 1 &&
                              (!ps->tex || ps->tex->opaque);
            ps->depth_write = !p->translucent || (p->attr & (1u << 11));
            ps->tex_alpha = p->translucent && alpha == 31 && ps->tex && !ps->tex->opaque &&
                            (fmt == 1 || fmt == 6);
            ps->pass = 0;
            ps->depth_equal = (p->attr >> 14) & 1;
            s_stats.depth_equal += ps->depth_equal;
            if (ps->shadow) {
                ps->opaque_prog = 0;
                ps->tex_alpha = 0;
                /* shadows draw over the finished opaque scene, in the frame's order, blended
                 * as translucent polygons are, never writing depth */
                ps->blend = blending_on;
                ps->depth_write = 0;
            }
            if (p->translucent || ps->depth_equal || ps->shadow || kh_gpu3d_debug == 1) {
                /* keeps its place, after the grouped opaque ones. Depth-equal polygons are
                 * second passes over geometry drawn before them (a field's textures over its
                 * lit base): grouped, they could come first and be covered (Tram Common's
                 * walls showed their lighting alone from 0.0.39 to 0.0.45) */
                s_pkey[i] = 0xfffffffeu;
            } else {
                s_pkey[i] = (ps->tex ? (uint32_t)(ps->tex - s_tex) + 1 : 0) << 4 |
                            (uint32_t)ps->id << 20 | (uint32_t)ps->mode << 1 |
                            (uint32_t)ps->depth_equal;
                s_popaque[nop++] = (uint16_t)i;
            }
        }
        /* group the opaque list by key, groups in order of first appearance and polygons
         * in frame order inside each: O(n), a hash of the keys to group numbers */
        {
            static uint32_t gkey[512];
            static uint16_t gcount[512], gstart[512];
            static uint16_t gof[KH_GX_MAX_POLYGONS];
            static uint16_t sorted[KH_GX_MAX_POLYGONS];
            static int16_t slot[1024];
            int ng = 0, overflow = 0;
            memset(slot, 0xff, sizeof(slot));
            for (k = 0; k < nop && !overflow; k++) {
                const uint32_t key = s_pkey[s_popaque[k]];
                uint32_t h = (key * 0x9e3779b1u) >> 22;
                while (slot[h] >= 0 && gkey[slot[h]] != key)
                    h = (h + 1) & 1023;
                if (slot[h] < 0) {
                    if (ng == 512) {
                        overflow = 1; /* that many states: leave the frame order */
                        break;
                    }
                    slot[h] = (int16_t)ng;
                    gkey[ng] = key;
                    gcount[ng++] = 0;
                }
                gof[k] = (uint16_t)slot[h];
                gcount[slot[h]]++;
            }
            if (!overflow) {
                int g, at = 0;
                for (g = 0; g < ng; g++) {
                    gstart[g] = (uint16_t)at;
                    at += gcount[g];
                }
                for (k = 0; k < nop; k++)
                    sorted[gstart[gof[k]]++] = s_popaque[k];
                memcpy(s_popaque, sorted, (size_t)nop * sizeof(s_popaque[0]));
            }
        }
        for (k = 0; k < f->npoly + nop; k++) {
            int idx;
            const KhGxPolygon *p;
            if (k < nop) {
                idx = s_popaque[k];
            } else {
                idx = k - nop;
                if (s_pkey[idx] != 0xfffffffeu)
                    continue; /* opaque (done) or skipped */
            }
            p = &f->poly[f->order[idx]];
            st = s_pstate[idx];
            if (have && memcmp(&st, &cur, sizeof(st))) {
                flush(&cur, first, nidx - first);
                first = nidx;
            }
            cur = st;
            have = 1;
            s_idx[nidx++] = p->v[0];
            s_idx[nidx++] = p->v[1];
            s_idx[nidx++] = p->v[2];
            if (p->count == 4) {
                s_idx[nidx++] = p->v[0];
                s_idx[nidx++] = p->v[2];
                s_idx[nidx++] = p->v[3];
            }
        }
    }
    if (have)
        flush(&cur, first, nidx - first);
    /* kept for the frame's second draw when every batch fitted */
    s_batches_serial = s_nbatches <= BATCHES_MAX ? f->serial : 0xffffffffu;
drawn:

    glDisableVertexAttribArray(A_POS);
    glDisableVertexAttribArray(A_TEX);
    glDisableVertexAttribArray(A_COL);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
done:
    if (s_dump_color) {
        /* the 3D layer as drawn, top row first */
        uint32_t *px = malloc((size_t)s_w * s_h * 4), *row = malloc((size_t)s_w * 4);
        s_dump_color = 0;
        if (px && row) {
            int y;
            glReadPixels(0, 0, s_w, s_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            for (y = 0; y < s_h / 2; y++) {
                memcpy(row, px + y * s_w, (size_t)s_w * 4);
                memcpy(px + y * s_w, px + (s_h - 1 - y) * s_w, (size_t)s_w * 4);
                memcpy(px + (s_h - 1 - y) * s_w, row, (size_t)s_w * 4);
            }
            kh_gpu3d_dump_tga("ux0:data/khdays/dump/layer_3d.tga", px, s_w, s_h);
        }
        free(px);
        free(row);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_stats.render_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    return s_color;
}

/* ---- 60 fps: frames in between ------------------------------------------------------------
 * The game draws its 3D at 30 fps, each frame shown for two Vita frames. With interpolation on
 * (config.ini), a new frame B is first shown as the halfway mix of the previous frame A and B,
 * and B itself one Vita frame later: the movement goes at 60 fps for one frame of latency. Only
 * the vertices both frames share from the start (mix_vertices), and when A was up for two
 * Vita frames; otherwise B is shown at once. */
static KhGxVertex *s_prev_vtx, *s_mix_vtx;
static int s_prev_nvtx = -1;
static uint32_t s_prev_disp3dcnt;
static int s_shown_count;  /* renders of the current serial so far */
static int s_final_pending; /* the mix (or A again) was shown: B itself next */
static int s_was_mixed;     /* that was a mix, for the statistics */

/* How many vertices from the start of the frame are the previous frame's again (same tag in
 * the same place): the scene is sent in the same order every frame -- the field, the
 * characters, then effects that come and go -- so this keeps the field and the characters
 * interpolated when the tail differs. Their halfway mix goes to s_mix_vtx, the rest is the
 * new frame's; 0 when too little matches or half of it jumps half the screen (a camera cut:
 * a camera turning fast moves everything a good way, 0.0.74 took that for a cut). */
static int mix_vertices(const KhGxFrame *f)
{
    const int n = f->nvtx < s_prev_nvtx ? f->nvtx : s_prev_nvtx;
    int i, p, jumps = 0;
    if (f->disp3dcnt != s_prev_disp3dcnt)
        return 0;
    for (p = 0; p < n && s_prev_vtx[p].tag == f->vtx[p].tag; p++)
        ;
    if (p * 2 < f->nvtx)
        return 0;
    for (i = 0; i < p; i++) {
        const KhGxVertex *a = &s_prev_vtx[i], *b = &f->vtx[i];
        KhGxVertex *m = &s_mix_vtx[i];
        /* a vertex that cannot be halved: one side of the camera to the other (the halfway w
         * near 0 projects it off to infinity) or a long jump (a particle reborn elsewhere with
         * the same tag) stays where the new frame has it. Halved, a Heartless's death burst
         * once drew a polygon over half the screen (0.1.6) */
        int snap = !(a->w > 0 && b->w > 0);
        if (!snap) {
            const float dx = a->x / a->w - b->x / b->w, dy = a->y / a->w - b->y / b->w;
            const float d2 = dx * dx + dy * dy;
            if (d2 > 1.0f) /* half the screen in one game frame */
                jumps++;
            snap = d2 > 0.25f || a->w > 4.0f * b->w || b->w > 4.0f * a->w;
        }
        if (snap) {
            *m = *b;
            continue;
        }
        m->x = (a->x + b->x) * 0.5f;
        m->y = (a->y + b->y) * 0.5f;
        m->z = (a->z + b->z) * 0.5f;
        m->w = (a->w + b->w) * 0.5f;
        m->s = (a->s + b->s) * 0.5f;
        m->t = (a->t + b->t) * 0.5f;
        m->r = (uint8_t)((a->r + b->r + 1) >> 1);
        m->g = (uint8_t)((a->g + b->g + 1) >> 1);
        m->b = (uint8_t)((a->b + b->b + 1) >> 1);
        m->a = b->a;
        m->tag = b->tag;
    }
    if (jumps * 2 >= p)
        return 0;
    memcpy(s_mix_vtx + p, f->vtx + p, sizeof(KhGxVertex) * (size_t)(f->nvtx - p));
    return 1;
}

volatile int kh_gpu3d_direct;

void kh_gpu3d_forget_previous(void)
{
    s_prev_nvtx = -1;
}

static void keep_as_previous(const KhGxFrame *f)
{
    if (!s_prev_vtx) {
        s_prev_vtx = malloc(sizeof(KhGxVertex) * KH_GX_MAX_VERTICES);
        s_mix_vtx = malloc(sizeof(KhGxVertex) * KH_GX_MAX_VERTICES);
        if (!s_prev_vtx || !s_mix_vtx)
            return;
    }
    memcpy(s_prev_vtx, f->vtx, sizeof(KhGxVertex) * (size_t)f->nvtx);
    s_prev_nvtx = f->nvtx;
    s_prev_disp3dcnt = f->disp3dcnt;
}

unsigned kh_gpu3d_render(const KhGxFrame *f)
{
    apply_requests();
    if (!s_prog || !f)
        return 0;
    if (f->serial == s_last_serial) {
        s_shown_count++;
        if (s_final_pending) {
            /* the frame itself, after its halfway mix */
            s_final_pending = 0;
            s_stats.interpolated += (uint32_t)s_was_mixed;
            return draw_frame(f, f->vtx);
        }
        return s_color; /* same frame as last time: the target still holds it */
    }
    s_last_serial = f->serial;
    {
        const int was_shown = s_shown_count;
        unsigned tex;
        s_shown_count = 0;
        if (kh_gpu3d_direct)
            s_prev_nvtx = -1; /* frames for the two screens in turn: none to mix with */
        if (kh_config.frame_interpolation && !kh_gpu3d_direct && s_prev_vtx && was_shown >= 1 &&
            !s_final_pending) {
            /* B one Vita frame from now either way, after the mix or after A once more: shown
             * at once when no mix is possible, B would come a frame early and the motion
             * stutter (0.0.74 alternated between the two in the field) */
            s_was_mixed = mix_vertices(f);
            if (s_was_mixed) {
                kh_gpu3d_mixes++;
                tex = draw_frame(f, s_mix_vtx);
            } else {
                tex = s_color; /* the target still holds A */
            }
            s_final_pending = 1;
        } else {
            s_final_pending = 0;
            tex = draw_frame(f, f->vtx);
        }
        if (kh_config.frame_interpolation)
            keep_as_previous(f);
        return tex;
    }
}

void kh_gpu3d_take_stats(KhGpu3dStats *out)
{
    *out = s_stats;
    out->textures_live = (uint32_t)s_tex_live;
    out->slots = kh_tex_slots_mapped();
    memset(&s_stats, 0, sizeof(s_stats));
}
