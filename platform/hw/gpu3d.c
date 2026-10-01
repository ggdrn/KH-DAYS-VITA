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

#include "hw/textures.h"
#include "log.h"
#include "video.h"

#include <psp2/kernel/processmgr.h>
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
    "            uniform float uMode, uniform float uTextured, uniform float uAlphaRef) : COLOR\n"
    "{\n"
    "    float4 t = float4(1.0, 1.0, 1.0, 1.0);\n"
    "    float4 r;\n"
    "    if (uTextured > 0.5)\n"
    "        t = tex2D(uTex, vTex);\n"
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
    "    if (r.a <= uAlphaRef)\n"
    "        discard;\n"
    "    return float4(r.rgb * r.a, r.a);\n"
    "}\n";

enum { A_POS, A_TEX, A_COL };

static int s_scale, s_w, s_h;
static GLuint s_prog, s_fbo, s_color, s_depth, s_vbo, s_toon_tex;
static GLint u_tex_scale, u_mode, u_textured, u_alpha_ref, u_tex, u_toon;
static uint32_t s_frame;
static uint32_t s_last_serial;
static KhGpu3dStats s_stats;

/* ---- texture cache --------------------------------------------------------------------- */

#define TEX_SLOTS 1024 /* open addressing, power of two */
#define TEX_MAX_LIVE 700

typedef struct {
    uint32_t key_img, key_pltt; /* key_img 0: empty */
    uint32_t hash, checked, used;
    GLuint tex;
    float sx, sy;
} TexEntry;

static TexEntry s_tex[TEX_SLOTS];
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

static void tex_upload(TexEntry *e, uint32_t teximage, uint32_t pltt)
{
    const int w = kh_tex_width(teximage), h = kh_tex_height(teximage);
    const size_t need = (size_t)w * h * 4;
    GLint ws, wt;
    if (need > s_decode_size) {
        free(s_decode);
        s_decode = malloc(need);
        s_decode_size = s_decode ? need : 0;
        if (!s_decode)
            return;
    }
    kh_tex_decode(teximage, pltt, s_decode);
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, s_decode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    ws = !(teximage & (1u << 16)) ? GL_CLAMP_TO_EDGE : (teximage & (1u << 18)) ? GL_MIRRORED_REPEAT : GL_REPEAT;
    wt = !(teximage & (1u << 17)) ? GL_CLAMP_TO_EDGE : (teximage & (1u << 19)) ? GL_MIRRORED_REPEAT : GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
    e->sx = 1.0f / (float)w;
    e->sy = 1.0f / (float)h;
    s_stats.textures_decoded++;
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
        e->checked = s_frame - 1;
        e->hash = 0;
        s_tex_live++;
    }
    e->used = s_frame;
    if (e->checked != s_frame) {
        uint32_t hv = kh_tex_hash(teximage, kp);
        e->checked = s_frame;
        if (hv != e->hash || !e->sx) {
            e->hash = hv;
            tex_upload(e, teximage, kp);
        }
    }
    return e;
}

/* ---- set-up ---------------------------------------------------------------------------- */

int kh_gpu3d_init(int scale)
{
    static const char *const attribs[] = { "aPos", "aTex", "aCol" };

    s_scale = scale < 1 ? 1 : scale > 3 ? 3 : scale;
    s_w = 256 * s_scale;
    s_h = 192 * s_scale;

    s_prog = video_build_program(s_vs, s_fs, attribs, 3);
    if (!s_prog) {
        LOG("gpu3d: shaders did not build: no 3D");
        return 0;
    }
    u_tex_scale = glGetUniformLocation(s_prog, "uTexScale");
    u_mode = glGetUniformLocation(s_prog, "uMode");
    u_textured = glGetUniformLocation(s_prog, "uTextured");
    u_alpha_ref = glGetUniformLocation(s_prog, "uAlphaRef");
    u_tex = glGetUniformLocation(s_prog, "uTex");
    u_toon = glGetUniformLocation(s_prog, "uToon");

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
} DrawState;

static uint16_t s_idx[KH_GX_MAX_POLYGONS * 6];

static void apply_state(const DrawState *st)
{
    if (st->tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, st->tex->tex);
        glUniform2f(u_tex_scale, st->tex->sx, st->tex->sy);
        glUniform1f(u_textured, 1.0f);
    } else {
        glUniform1f(u_textured, 0.0f);
    }
    glUniform1f(u_mode, (float)st->mode);
    if (st->blend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    glDepthMask(st->depth_write ? GL_TRUE : GL_FALSE);
    glDepthFunc(st->depth_equal ? GL_LEQUAL : GL_LESS);
}

static void flush(const DrawState *st, int first, int count)
{
    if (!count)
        return;
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
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 32, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glActiveTexture(GL_TEXTURE0);
}

unsigned kh_gpu3d_render(const KhGxFrame *f)
{
    uint64_t t0;
    const int textures_on = f && (f->disp3dcnt & 1);
    const int blending_on = f && (f->disp3dcnt & 8);
    DrawState cur = { 0 }, st;
    int i, nidx = 0, first = 0, have = 0;

    if (!s_prog || !f)
        return 0;
    if (f->serial == s_last_serial)
        return s_color; /* same frame as last time: the target still holds it */
    s_last_serial = f->serial;
    t0 = sceKernelGetProcessTimeWide();
    s_frame++;
    if ((s_frame & 63) == 0 || s_tex_live >= TEX_MAX_LIVE)
        tex_evict_old();
    kh_tex_map_slots();

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
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    if (!f->npoly)
        goto done;

    glUseProgram(s_prog);
    glUniform1i(u_tex, 0);
    glUniform1i(u_toon, 1);
    {
        const int ref = reg32(f, 0x04000340) & 31;
        glUniform1f(u_alpha_ref, (f->disp3dcnt & 4) ? ((float)ref + 0.5f) / 31.0f : 0.5f / 31.0f);
    }
    upload_toon(f);
    glEnable(GL_DEPTH_TEST);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, f->nvtx * (int)sizeof(KhGxVertex), f->vtx, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(A_POS);
    glEnableVertexAttribArray(A_TEX);
    glEnableVertexAttribArray(A_COL);
    glVertexAttribPointer(A_POS, 4, GL_FLOAT, GL_FALSE, sizeof(KhGxVertex), (void *)0);
    glVertexAttribPointer(A_TEX, 2, GL_FLOAT, GL_FALSE, sizeof(KhGxVertex), (void *)16);
    glVertexAttribPointer(A_COL, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(KhGxVertex), (void *)24);

    for (i = 0; i < f->npoly; i++) {
        const KhGxPolygon *p = &f->poly[f->order[i]];
        const int alpha = (p->attr >> 16) & 31;
        const int fmt = kh_tex_format(p->teximage);
        st.mode = (p->attr >> 4) & 3;
        if (st.mode == 3 || alpha == 0) {
            s_stats.skipped++; /* shadow polygons, wireframe: not yet */
            continue;
        }
        st.tex = (textures_on && fmt) ? tex_get(p->teximage, p->pltt) : NULL;
        st.blend = p->translucent && blending_on;
        st.depth_write = !p->translucent || (p->attr & (1u << 11));
        st.depth_equal = (p->attr >> 14) & 1;
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
    if (have)
        flush(&cur, first, nidx - first);

    glDisableVertexAttribArray(A_POS);
    glDisableVertexAttribArray(A_TEX);
    glDisableVertexAttribArray(A_COL);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
done:
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_stats.render_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    return s_color;
}

void kh_gpu3d_take_stats(KhGpu3dStats *out)
{
    *out = s_stats;
    out->textures_live = (uint32_t)s_tex_live;
    memset(&s_stats, 0, sizeof(s_stats));
}
