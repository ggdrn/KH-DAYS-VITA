/* The DS geometry engine (see gx3d.h). Fixed point throughout, as the hardware computes it
 * (matrices 20.12, vertices 4.12, normals and light vectors 1.9), so that matrix read-back and
 * the box/position tests give the game the values it expects; vertices leave as floats for the
 * GPU. Command semantics follow GBATEK's "DS 3D" chapters. */
#include <string.h>

#include "hw/gx3d.h"
#include "hw/gpu3d.h"
#include "hw/io.h"
#include "log.h"

typedef int32_t Mtx[16]; /* row-major; a row vector times the matrix, as the DS does it */

/* parameter words per command; -1: not a command */
static const int8_t s_params[256] = {
    [0x00] = 0,
    [0x10] = 1, [0x11] = 0, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 0, [0x16] = 16,
    [0x17] = 12, [0x18] = 16, [0x19] = 12, [0x1a] = 9, [0x1b] = 3, [0x1c] = 3,
    [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 2, [0x24] = 1, [0x25] = 1, [0x26] = 1,
    [0x27] = 1, [0x28] = 1, [0x29] = 1, [0x2a] = 1, [0x2b] = 1,
    [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1, [0x34] = 32,
    [0x40] = 1, [0x41] = 0, [0x50] = 1, [0x60] = 1, [0x70] = 3, [0x71] = 2, [0x72] = 1,
};
static uint8_t s_valid[256];

/* ---- engine state ---------------------------------------------------------------------- */

static Mtx s_proj, s_pos, s_vec, s_tex, s_clip;
volatile float kh_gx3d_wide_x = 1.0f;

/* a perspective projection (w taken from z): the field's camera, not a flat 2D-like layout */
static int wide_now(void)
{
    return kh_gx3d_wide_x < 0.999f && s_proj[11] != 0;
}
static Mtx s_proj_stack[1], s_pos_stack[32], s_vec_stack[32], s_tex_stack[1];
static int s_proj_sp, s_pos_sp, s_tex_sp, s_mtx_mode, s_stack_error;
static int s_clip_dirty = 1;

static int16_t s_vtx[3];          /* last vertex, 4.12 */
static int16_t s_raw_st[2];       /* TEXCOORD as given, 1/16 texel */
static int32_t s_st[2];           /* after texture-coordinate generation */
static int16_t s_normal[3];       /* 1.9 */
static uint8_t s_color[3];        /* 5 bits */
static uint32_t s_attr_pending, s_attr, s_teximage, s_pltt;
static uint16_t s_diffuse, s_ambient, s_specular, s_emission;
static int s_use_shine;
static uint8_t s_shine[128];
static int16_t s_light_dir[4][3]; /* transformed, 1.9 */
static uint16_t s_light_color[4];
static int s_vp_x1, s_vp_y1, s_vp_w = 256, s_vp_h = 192;

/* primitive assembly */
static int s_prim = -1, s_prim_n, s_strip_odd;
static int s_prim_vtx[4];        /* frame indices of the vertices collected so far */

/* command decoding */
static uint32_t s_fifo_cmds; /* remaining packed commands, low byte first */
static int s_fifo_left;      /* commands left in the packed word */
static int s_fifo_param, s_fifo_total;
static int s_exec_cmd = -1, s_exec_n, s_exec_total;
static uint32_t s_exec_params[32];

/* frames: triple buffered between this (game) side and the renderer */
static KhGxFrame s_frames[3];
static int s_build;
static int s_shown = 1;
static volatile int s_ready = 2; /* index | 4 when it holds a frame the renderer has not taken */
static KhGxFrame *s_f;
static uint32_t s_serial;

static KhGx3dStats s_stats;
static uint8_t s_unknown_logged[256];

/* ---- fixed-point helpers --------------------------------------------------------------- */

static inline int32_t fx_mul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 12); }

/* d = a * b (each 4x4, row-major) */
static void mtx_mul(Mtx d, const Mtx a, const Mtx b)
{
    Mtx r;
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            r[i * 4 + j] = (int32_t)(((int64_t)a[i * 4] * b[j] + (int64_t)a[i * 4 + 1] * b[4 + j] +
                                      (int64_t)a[i * 4 + 2] * b[8 + j] +
                                      (int64_t)a[i * 4 + 3] * b[12 + j]) >> 12);
    memcpy(d, r, sizeof(Mtx));
}

static void mtx_identity(Mtx m)
{
    memset(m, 0, sizeof(Mtx));
    m[0] = m[5] = m[10] = m[15] = 0x1000;
}

static inline int sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

static void update_clip(void)
{
    int i;
    if (!s_clip_dirty)
        return;
    mtx_mul(s_clip, s_pos, s_proj);
    for (i = 0; i < 16; i++)
        KH_IO32(0x04000640 + i * 4) = (uint32_t)s_clip[i];
    s_clip_dirty = 0;
}

static void publish_vec(void)
{
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            KH_IO32(0x04000680 + (i * 3 + j) * 4) = (uint32_t)s_vec[i * 4 + j];
}

static void publish_counts(void)
{
    KH_IO32(0x04000604) = (uint32_t)s_f->npoly | ((uint32_t)s_f->nvtx << 16);
}

/* ---- matrices -------------------------------------------------------------------------- */

/* The matrices a matrix command applies to in the current mode: 0 projection, 1 position,
 * 2 position and vector, 3 texture. */
static void mtx_apply(const Mtx m, int vector_too)
{
    switch (s_mtx_mode) {
    case 0: mtx_mul(s_proj, m, s_proj); s_clip_dirty = 1; break;
    case 1: mtx_mul(s_pos, m, s_pos); s_clip_dirty = 1; break;
    case 2:
        mtx_mul(s_pos, m, s_pos);
        s_clip_dirty = 1;
        if (vector_too) {
            mtx_mul(s_vec, m, s_vec);
            publish_vec();
        }
        break;
    default: mtx_mul(s_tex, m, s_tex); break;
    }
}

static void mtx_load(const Mtx m)
{
    switch (s_mtx_mode) {
    case 0: memcpy(s_proj, m, sizeof(Mtx)); s_clip_dirty = 1; break;
    case 1: memcpy(s_pos, m, sizeof(Mtx)); s_clip_dirty = 1; break;
    case 2:
        memcpy(s_pos, m, sizeof(Mtx));
        memcpy(s_vec, m, sizeof(Mtx));
        s_clip_dirty = 1;
        publish_vec();
        break;
    default: memcpy(s_tex, m, sizeof(Mtx)); break;
    }
}

static void params_to_mtx(Mtx m, const uint32_t *p, int rows, int cols)
{
    int i, j;
    mtx_identity(m);
    for (i = 0; i < rows; i++)
        for (j = 0; j < cols; j++)
            m[i * 4 + j] = (int32_t)*p++;
}

static void mtx_push(void)
{
    switch (s_mtx_mode) {
    case 0:
        if (s_proj_sp > 0)
            s_stack_error = 1;
        memcpy(s_proj_stack[0], s_proj, sizeof(Mtx));
        s_proj_sp = 1;
        break;
    case 1:
    case 2:
        if (s_pos_sp >= 31)
            s_stack_error = 1;
        memcpy(s_pos_stack[s_pos_sp & 31], s_pos, sizeof(Mtx));
        memcpy(s_vec_stack[s_pos_sp & 31], s_vec, sizeof(Mtx));
        s_pos_sp = (s_pos_sp + 1) & 63;
        break;
    default:
        if (s_tex_sp > 0)
            s_stack_error = 1;
        memcpy(s_tex_stack[0], s_tex, sizeof(Mtx));
        s_tex_sp = 1;
        break;
    }
}

static void mtx_pop(uint32_t param)
{
    switch (s_mtx_mode) {
    case 0:
        if (s_proj_sp == 0)
            s_stack_error = 1;
        s_proj_sp = 0;
        memcpy(s_proj, s_proj_stack[0], sizeof(Mtx));
        s_clip_dirty = 1;
        break;
    case 1:
    case 2:
        s_pos_sp = (s_pos_sp - sext(param, 6)) & 63;
        if (s_pos_sp >= 31)
            s_stack_error = 1;
        memcpy(s_pos, s_pos_stack[s_pos_sp & 31], sizeof(Mtx));
        memcpy(s_vec, s_vec_stack[s_pos_sp & 31], sizeof(Mtx));
        s_clip_dirty = 1;
        publish_vec();
        break;
    default:
        if (s_tex_sp == 0)
            s_stack_error = 1;
        s_tex_sp = 0;
        memcpy(s_tex, s_tex_stack[0], sizeof(Mtx));
        break;
    }
}

static void mtx_store(uint32_t param)
{
    int i = param & 31;
    switch (s_mtx_mode) {
    case 0: memcpy(s_proj_stack[0], s_proj, sizeof(Mtx)); break;
    case 1:
    case 2:
        if (i == 31)
            s_stack_error = 1;
        memcpy(s_pos_stack[i], s_pos, sizeof(Mtx));
        memcpy(s_vec_stack[i], s_vec, sizeof(Mtx));
        break;
    default: memcpy(s_tex_stack[0], s_tex, sizeof(Mtx)); break;
    }
}

static void mtx_restore(uint32_t param)
{
    int i = param & 31;
    switch (s_mtx_mode) {
    case 0: memcpy(s_proj, s_proj_stack[0], sizeof(Mtx)); s_clip_dirty = 1; break;
    case 1:
    case 2:
        if (i == 31)
            s_stack_error = 1;
        memcpy(s_pos, s_pos_stack[i], sizeof(Mtx));
        memcpy(s_vec, s_vec_stack[i], sizeof(Mtx));
        s_clip_dirty = 1;
        publish_vec();
        break;
    default: memcpy(s_tex, s_tex_stack[0], sizeof(Mtx)); break;
    }
}

static void mtx_scale(const uint32_t *p)
{
    Mtx *m = s_mtx_mode == 0 ? &s_proj : s_mtx_mode == 3 ? &s_tex : &s_pos;
    int i, j;
    /* MTX_SCALE leaves the vector matrix alone, in mode 2 as well */
    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++)
            (*m)[i * 4 + j] = fx_mul((*m)[i * 4 + j], (int32_t)p[i]);
    if (s_mtx_mode != 3)
        s_clip_dirty = 1;
}

static void mtx_trans_one(Mtx m, const uint32_t *p)
{
    int j;
    for (j = 0; j < 4; j++)
        m[12 + j] += (int32_t)(((int64_t)(int32_t)p[0] * m[j] + (int64_t)(int32_t)p[1] * m[4 + j] +
                                (int64_t)(int32_t)p[2] * m[8 + j]) >> 12);
}

static void mtx_trans(const uint32_t *p)
{
    switch (s_mtx_mode) {
    case 0: mtx_trans_one(s_proj, p); s_clip_dirty = 1; break;
    case 1: mtx_trans_one(s_pos, p); s_clip_dirty = 1; break;
    case 2:
        mtx_trans_one(s_pos, p);
        mtx_trans_one(s_vec, p);
        s_clip_dirty = 1;
        publish_vec();
        break;
    default: mtx_trans_one(s_tex, p); break;
    }
}

/* ---- vertices and lighting ------------------------------------------------------------- */

static inline uint8_t c5to6(int c) { return (uint8_t)(c ? c * 2 + 1 : 0); }

static void set_color555(uint32_t c)
{
    s_color[0] = c & 31;
    s_color[1] = (c >> 5) & 31;
    s_color[2] = (c >> 10) & 31;
}

static void cmd_normal(uint32_t p)
{
    int32_t n[3], t[3];
    int i, c[3];
    n[0] = sext(p, 10);
    n[1] = sext(p >> 10, 10);
    n[2] = sext(p >> 20, 10);
    s_normal[0] = (int16_t)n[0];
    s_normal[1] = (int16_t)n[1];
    s_normal[2] = (int16_t)n[2];

    if ((s_teximage >> 30) == 2 && kh_gpu3d_debug != 3) {
        /* texture coordinates from the normal (environment mapping) */
        s_st[0] = (int32_t)(((int64_t)n[0] * s_tex[0] + (int64_t)n[1] * s_tex[4] +
                             (int64_t)n[2] * s_tex[8]) >> 21) + s_raw_st[0];
        s_st[1] = (int32_t)(((int64_t)n[0] * s_tex[1] + (int64_t)n[1] * s_tex[5] +
                             (int64_t)n[2] * s_tex[9]) >> 21) + s_raw_st[1];
    }

    for (i = 0; i < 3; i++)
        t[i] = (n[0] * s_vec[i] + n[1] * s_vec[4 + i] + n[2] * s_vec[8 + i]) >> 12;

    c[0] = s_emission & 31;
    c[1] = (s_emission >> 5) & 31;
    c[2] = (s_emission >> 10) & 31;
    for (i = 0; i < 4; i++) {
        int32_t diff, shine;
        int k;
        if (!(s_attr & (1u << i)))
            continue;
        diff = -(s_light_dir[i][0] * t[0] + s_light_dir[i][1] * t[1] + s_light_dir[i][2] * t[2]) >> 9;
        if (diff < 0)
            diff = 0;
        else if (diff > 255)
            diff = 255;
        /* the half vector between the light and the line of sight (0, 0, -1) */
        shine = -((((s_light_dir[i][0] >> 1) * t[0]) + ((s_light_dir[i][1] >> 1) * t[1]) +
                   (((s_light_dir[i][2] - 0x200) >> 1) * t[2])) >> 10);
        if (shine < 0)
            shine = 0;
        else if (shine > 255)
            shine = 255;
        shine = ((shine * shine) >> 7) - 0x100;
        if (shine < 0)
            shine = 0;
        else if (shine > 255)
            shine = 255;
        if (s_use_shine)
            shine = s_shine[shine >> 1];
        for (k = 0; k < 3; k++) {
            int lc = (s_light_color[i] >> (5 * k)) & 31;
            int sp = (s_specular >> (5 * k)) & 31;
            int df = (s_diffuse >> (5 * k)) & 31;
            int am = (s_ambient >> (5 * k)) & 31;
            c[k] += (((sp * lc * shine) >> 13) + ((df * lc * diff) >> 13) + am * lc) >> 5;
        }
    }
    for (i = 0; i < 3; i++)
        s_color[i] = (uint8_t)(c[i] > 31 ? 31 : c[i]);
}

static void cmd_texcoord(uint32_t p)
{
    s_raw_st[0] = (int16_t)(p & 0xffff);
    s_raw_st[1] = (int16_t)(p >> 16);
    if ((s_teximage >> 30) == 1 && kh_gpu3d_debug != 3) {
        int32_t s = s_raw_st[0], t = s_raw_st[1];
        s_st[0] = (int32_t)(((int64_t)s * s_tex[0] + (int64_t)t * s_tex[4] + s_tex[8] + s_tex[12]) >> 12);
        s_st[1] = (int32_t)(((int64_t)s * s_tex[1] + (int64_t)t * s_tex[5] + s_tex[9] + s_tex[13]) >> 12);
    } else {
        s_st[0] = s_raw_st[0];
        s_st[1] = s_raw_st[1];
    }
}

static void transform(const int16_t v[3], int32_t out[4])
{
    int j;
    update_clip();
    for (j = 0; j < 4; j++)
        out[j] = (int32_t)(((int64_t)v[0] * s_clip[j] + (int64_t)v[1] * s_clip[4 + j] +
                            (int64_t)v[2] * s_clip[8 + j] + ((int64_t)s_clip[12 + j] << 12)) >> 12);
}

/* A polygon from the frame vertices idx[0..n-1], unless culled. */
static void emit_polygon(const int *idx, int n, int odd)
{
    KhGxFrame *f = s_f;
    const KhGxVertex *v0 = &f->vtx[idx[0]], *v1 = &f->vtx[idx[1]], *v2 = &f->vtx[idx[2]];
    KhGxPolygon *poly;
    double nx, ny, nz, dot;
    int i, alpha, fmt, render;
    float ymin = 1e30f, ymax = -1e30f;

    /* facing: the sign of the triangle's orientation in homogeneous screen space (x, y, w) */
    nx = (double)(v0->y - v1->y) * (v2->w - v1->w) - (double)(v0->w - v1->w) * (v2->y - v1->y);
    ny = (double)(v0->w - v1->w) * (v2->x - v1->x) - (double)(v0->x - v1->x) * (v2->w - v1->w);
    nz = (double)(v0->x - v1->x) * (v2->y - v1->y) - (double)(v0->y - v1->y) * (v2->x - v1->x);
    dot = v1->x * nx + v1->y * ny + v1->w * nz;
    if (odd)
        dot = -dot;
    if (dot < 0)
        render = s_attr & (1u << 7); /* front: counter-clockwise with y up */
    else if (dot > 0)
        render = s_attr & (1u << 6); /* back */
    else
        render = s_attr & (3u << 6);
    if (!render) {
        s_stats.culled++;
        return;
    }

    {
        /* wholly outside one side of the view volume: the DS stores nothing of it */
        int out_l = 1, out_r = 1, out_b = 1, out_t = 1, out_n = 1;
        for (i = 0; i < n; i++) {
            const KhGxVertex *v = &f->vtx[idx[i]];
            out_l &= v->x < -v->w;
            out_r &= v->x > v->w;
            out_b &= v->y < -v->w;
            out_t &= v->y > v->w;
            out_n &= v->z < -v->w;
        }
        if (out_l | out_r | out_b | out_t | out_n) {
            s_stats.culled++;
            return;
        }
    }
    for (i = 0; i < n; i++) {
        const KhGxVertex *v = &f->vtx[idx[i]];
        /* far-plane crossing polygons are dropped unless attr bit 12 says to keep them */
        if (v->z > v->w && !(s_attr & (1u << 12)))
            return;
        if (v->w > 0) {
            float y = v->y / v->w;
            if (y < ymin) ymin = y;
            if (y > ymax) ymax = y;
        }
    }
    if (f->npoly >= KH_GX_MAX_POLYGONS) {
        s_stats.overflows++;
        return;
    }
    poly = &f->poly[f->npoly++];
    for (i = 0; i < n; i++)
        poly->v[i] = (uint16_t)idx[i];
    poly->count = (uint8_t)n;
    poly->attr = s_attr;
    poly->teximage = s_teximage;
    poly->pltt = s_pltt;
    alpha = (s_attr >> 16) & 31;
    fmt = (s_teximage >> 26) & 7;
    poly->translucent = (alpha != 0 && alpha != 31) || fmt == 1 || fmt == 6;
    if (ymin > ymax)
        ymin = ymax = 0;
    poly->ytop = (int16_t)(96.0f - ymax * 96.0f);
    poly->ybottom = (int16_t)(96.0f - ymin * 96.0f);
    s_stats.polygons++;
    publish_counts();
}

static void emit_vertex(void)
{
    KhGxFrame *f = s_f;
    KhGxVertex *out;
    int32_t c[4];
    int idx;

    if (s_prim < 0)
        return;
    if (f->nvtx >= KH_GX_MAX_VERTICES) {
        s_stats.overflows++;
        return;
    }
    if ((s_teximage >> 30) == 3 && kh_gpu3d_debug != 3) {
        /* texture coordinates from the vertex position */
        s_st[0] = (int32_t)(((int64_t)s_vtx[0] * s_tex[0] + (int64_t)s_vtx[1] * s_tex[4] +
                             (int64_t)s_vtx[2] * s_tex[8]) >> 24) + s_raw_st[0];
        s_st[1] = (int32_t)(((int64_t)s_vtx[0] * s_tex[1] + (int64_t)s_vtx[1] * s_tex[5] +
                             (int64_t)s_vtx[2] * s_tex[9]) >> 24) + s_raw_st[1];
    }
    transform(s_vtx, c);
    if (wide_now())
        c[0] = (int32_t)((float)c[0] * kh_gx3d_wide_x);
    idx = f->nvtx++;
    out = &f->vtx[idx];
    {
        /* fold the viewport in: NDC over the whole screen, still homogeneous */
        float x = (float)c[0], y = (float)c[1], w = (float)c[3];
        out->x = (x + w) * ((float)s_vp_w / 256.0f) + w * ((float)s_vp_x1 / 128.0f - 1.0f);
        out->y = (y + w) * ((float)s_vp_h / 192.0f) + w * ((float)s_vp_y1 / 96.0f - 1.0f);
        out->z = (float)c[2];
        out->w = w;
    }
    out->s = (float)s_st[0] * (1.0f / 16.0f);
    out->t = (float)s_st[1] * (1.0f / 16.0f);
    out->r = c5to6(s_color[0]);
    out->g = c5to6(s_color[1]);
    out->b = c5to6(s_color[2]);
    out->a = (uint8_t)((s_attr >> 16) & 31);
    s_stats.vertices++;

    s_prim_vtx[s_prim_n++] = idx;
    switch (s_prim) {
    case 0: /* separate triangles */
        if (s_prim_n == 3) {
            emit_polygon(s_prim_vtx, 3, 0);
            s_prim_n = 0;
        }
        break;
    case 1: /* separate quads */
        if (s_prim_n == 4) {
            emit_polygon(s_prim_vtx, 4, 0);
            s_prim_n = 0;
        }
        break;
    case 2: /* triangle strip */
        if (s_prim_n == 3) {
            emit_polygon(s_prim_vtx, 3, s_strip_odd);
            s_strip_odd ^= 1;
            s_prim_vtx[0] = s_prim_vtx[1];
            s_prim_vtx[1] = s_prim_vtx[2];
            s_prim_n = 2;
        }
        break;
    default: /* quad strip: v0 v1 v2 v3 make the quad v0 v1 v3 v2 */
        if (s_prim_n == 4) {
            int q[4] = { s_prim_vtx[0], s_prim_vtx[1], s_prim_vtx[3], s_prim_vtx[2] };
            emit_polygon(q, 4, 0);
            s_prim_vtx[0] = s_prim_vtx[2];
            s_prim_vtx[1] = s_prim_vtx[3];
            s_prim_n = 2;
        }
        break;
    }
}

/* ---- tests ----------------------------------------------------------------------------- */

static void box_test(const uint32_t *p)
{
    int16_t x = (int16_t)(p[0] & 0xffff), y = (int16_t)(p[0] >> 16);
    int16_t z = (int16_t)(p[1] & 0xffff), w = (int16_t)(p[1] >> 16);
    int16_t h = (int16_t)(p[2] & 0xffff), d = (int16_t)(p[2] >> 16);
    int32_t c[8][4];
    int i, plane, visible = 1;

    for (i = 0; i < 8; i++) {
        int16_t v[3] = { (int16_t)(x + ((i & 1) ? w : 0)), (int16_t)(y + ((i & 2) ? h : 0)),
                         (int16_t)(z + ((i & 4) ? d : 0)) };
        transform(v, c[i]);
        if (wide_now())
            c[i][0] = (int32_t)((float)c[i][0] * kh_gx3d_wide_x);
    }
    /* invisible when every corner lies outside one and the same frustum plane */
    for (plane = 0; plane < 6 && visible; plane++) {
        int axis = plane >> 1, out = 0;
        for (i = 0; i < 8; i++) {
            int32_t a = c[i][axis], wv = c[i][3];
            if ((plane & 1) ? (a > wv) : (a < -wv))
                out++;
        }
        if (out == 8)
            visible = 0;
    }
    if (visible)
        KH_IO32(0x04000600) |= 2u;
    else
        KH_IO32(0x04000600) &= ~2u;
}

static void pos_test(const uint32_t *p)
{
    int32_t c[4];
    int i;
    s_vtx[0] = (int16_t)(p[0] & 0xffff);
    s_vtx[1] = (int16_t)(p[0] >> 16);
    s_vtx[2] = (int16_t)(p[1] & 0xffff);
    transform(s_vtx, c);
    /* in widescreen too, so that 2D markers the game places from it stay on their models */
    if (wide_now())
        c[0] = (int32_t)((float)c[0] * kh_gx3d_wide_x);
    for (i = 0; i < 4; i++)
        KH_IO32(0x04000620 + i * 4) = (uint32_t)c[i];
}

static void vec_test(uint32_t p)
{
    int32_t n[3] = { sext(p, 10), sext(p >> 10, 10), sext(p >> 20, 10) };
    int i;
    for (i = 0; i < 3; i++) {
        int32_t r = (n[0] * s_vec[i] + n[1] * s_vec[4 + i] + n[2] * s_vec[8 + i]) >> 9;
        /* 4.12, with the sign bit repeated into bits 12-15 */
        r = sext((uint32_t)r, 13);
        KH_IO16(0x04000630 + i * 2) = (uint16_t)r;
    }
}

/* ---- frames ---------------------------------------------------------------------------- */

static void frame_begin(void)
{
    s_f = &s_frames[s_build];
    s_f->nvtx = 0;
    s_f->npoly = 0;
    s_prim_n = 0;
    publish_counts();
}

static void sort_frame(KhGxFrame *f)
{
    int i, n = 0, first_tr;
    for (i = 0; i < f->npoly; i++)
        if (!f->poly[i].translucent)
            f->order[n++] = (uint16_t)i;
    first_tr = n;
    for (i = 0; i < f->npoly; i++)
        if (f->poly[i].translucent)
            f->order[n++] = (uint16_t)i;
    if (!(f->swap & 1)) {
        /* translucent polygons by bottom row, then top row (insertion sort: stable) */
        for (i = first_tr + 1; i < n; i++) {
            uint16_t k = f->order[i];
            const KhGxPolygon *pk = &f->poly[k];
            int j = i - 1;
            while (j >= first_tr) {
                const KhGxPolygon *pj = &f->poly[f->order[j]];
                if (pj->ybottom < pk->ybottom ||
                    (pj->ybottom == pk->ybottom && pj->ytop <= pk->ytop))
                    break;
                f->order[j + 1] = f->order[j];
                j--;
            }
            f->order[j + 1] = k;
        }
    }
}

static void swap_buffers(uint32_t param)
{
    KhGxFrame *f = s_f;
    int old;
    f->swap = param & 3;
    f->disp3dcnt = KH_IO32(0x04000060);
    memcpy(f->regs, KH_IO_PTR(0x04000330), sizeof(f->regs));
    f->serial = ++s_serial;
    sort_frame(f);
    s_stats.frames++;

    old = __atomic_exchange_n(&s_ready, s_build | 4, __ATOMIC_ACQ_REL);
    s_build = old & 3;
    frame_begin();
}

const KhGxFrame *kh_gx3d_acquire(void)
{
    if (__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) & 4) {
        int old = __atomic_exchange_n(&s_ready, s_shown, __ATOMIC_ACQ_REL);
        s_shown = old & 3;
    }
    return s_frames[s_shown].serial ? &s_frames[s_shown] : NULL;
}

/* ---- command execution ----------------------------------------------------------------- */

static void execute(int cmd, const uint32_t *p)
{
    Mtx m;
    s_stats.commands++;
    switch (cmd) {
    case 0x00: break;
    case 0x10: s_mtx_mode = p[0] & 3; break;
    case 0x11: mtx_push(); break;
    case 0x12: mtx_pop(p[0]); break;
    case 0x13: mtx_store(p[0]); break;
    case 0x14: mtx_restore(p[0]); break;
    case 0x15: mtx_identity(m); mtx_load(m); break;
    case 0x16: params_to_mtx(m, p, 4, 4); mtx_load(m); break;
    case 0x17: params_to_mtx(m, p, 4, 3); mtx_load(m); break;
    case 0x18: params_to_mtx(m, p, 4, 4); mtx_apply(m, 1); break;
    case 0x19: params_to_mtx(m, p, 4, 3); mtx_apply(m, 1); break;
    case 0x1a: params_to_mtx(m, p, 3, 3); mtx_apply(m, 1); break;
    case 0x1b: mtx_scale(p); break;
    case 0x1c: mtx_trans(p); break;
    case 0x20: set_color555(p[0]); break;
    case 0x21: cmd_normal(p[0]); break;
    case 0x22: cmd_texcoord(p[0]); break;
    case 0x23:
        s_vtx[0] = (int16_t)(p[0] & 0xffff);
        s_vtx[1] = (int16_t)(p[0] >> 16);
        s_vtx[2] = (int16_t)(p[1] & 0xffff);
        emit_vertex();
        break;
    case 0x24:
        s_vtx[0] = (int16_t)(sext(p[0], 10) << 6);
        s_vtx[1] = (int16_t)(sext(p[0] >> 10, 10) << 6);
        s_vtx[2] = (int16_t)(sext(p[0] >> 20, 10) << 6);
        emit_vertex();
        break;
    case 0x25: s_vtx[0] = (int16_t)(p[0] & 0xffff); s_vtx[1] = (int16_t)(p[0] >> 16); emit_vertex(); break;
    case 0x26: s_vtx[0] = (int16_t)(p[0] & 0xffff); s_vtx[2] = (int16_t)(p[0] >> 16); emit_vertex(); break;
    case 0x27: s_vtx[1] = (int16_t)(p[0] & 0xffff); s_vtx[2] = (int16_t)(p[0] >> 16); emit_vertex(); break;
    case 0x28:
        s_vtx[0] = (int16_t)(s_vtx[0] + sext(p[0], 10));
        s_vtx[1] = (int16_t)(s_vtx[1] + sext(p[0] >> 10, 10));
        s_vtx[2] = (int16_t)(s_vtx[2] + sext(p[0] >> 20, 10));
        emit_vertex();
        break;
    case 0x29: s_attr_pending = p[0]; break;
    case 0x2a: s_teximage = p[0]; break;
    case 0x2b: s_pltt = p[0] & 0x1fff; break;
    case 0x30:
        s_diffuse = p[0] & 0x7fff;
        s_ambient = (p[0] >> 16) & 0x7fff;
        if (p[0] & 0x8000)
            set_color555(s_diffuse);
        break;
    case 0x31:
        s_specular = p[0] & 0x7fff;
        s_emission = (p[0] >> 16) & 0x7fff;
        s_use_shine = (p[0] >> 15) & 1;
        break;
    case 0x32: {
        int l = p[0] >> 30, i;
        int32_t d[3] = { sext(p[0], 10), sext(p[0] >> 10, 10), sext(p[0] >> 20, 10) };
        for (i = 0; i < 3; i++)
            s_light_dir[l][i] = (int16_t)((d[0] * s_vec[i] + d[1] * s_vec[4 + i] + d[2] * s_vec[8 + i]) >> 12);
        break;
    }
    case 0x33: s_light_color[p[0] >> 30] = p[0] & 0x7fff; break;
    case 0x34: {
        int i;
        for (i = 0; i < 32; i++) {
            s_shine[i * 4] = p[i] & 0xff;
            s_shine[i * 4 + 1] = (p[i] >> 8) & 0xff;
            s_shine[i * 4 + 2] = (p[i] >> 16) & 0xff;
            s_shine[i * 4 + 3] = p[i] >> 24;
        }
        break;
    }
    case 0x40:
        s_prim = p[0] & 3;
        s_prim_n = 0;
        s_strip_odd = 0;
        s_attr = s_attr_pending;
        break;
    case 0x41: break; /* END_VTXS does nothing on the hardware */
    case 0x50: swap_buffers(p[0]); break;
    case 0x60: {
        int x1 = p[0] & 0xff, y1 = (p[0] >> 8) & 0xff, x2 = (p[0] >> 16) & 0xff, y2 = p[0] >> 24;
        s_vp_x1 = x1;
        s_vp_y1 = y1;
        s_vp_w = x2 - x1 + 1;
        s_vp_h = y2 - y1 + 1;
        break;
    }
    case 0x70: box_test(p); break;
    case 0x71: pos_test(p); break;
    case 0x72: vec_test(p[0]); break;
    default:
        break;
    }
    /* the clip matrix registers follow every matrix command, as on the hardware */
    if (s_clip_dirty && cmd < 0x20)
        update_clip();
}

/* One (command, parameter) entry, as the hardware queues them: a command takes the parameter
 * of its own entry and those of the entries after it (a parameterless command one dummy). */
static void push_entry(int cmd, uint32_t param)
{
    if (s_exec_cmd < 0) {
        if (!s_valid[cmd]) {
            s_stats.unknown++;
            if (!s_unknown_logged[cmd]++)
                LOG("gx3d: unknown command %02x (param %08x)", cmd, (unsigned)param);
            return;
        }
        s_exec_cmd = cmd;
        s_exec_n = 0;
        s_exec_total = s_params[cmd] ? s_params[cmd] : 1;
    }
    s_exec_params[s_exec_n++] = param;
    if (s_exec_n == s_exec_total) {
        int c = s_exec_cmd;
        s_exec_cmd = -1;
        execute(c, s_exec_params);
    }
}

/* A word to the packed FIFO: a command word (up to four command bytes, low first) then their
 * parameters in order. Zero command bytes are skipped, except that an all-zero word is a NOP. */
static void fifo_write(uint32_t w)
{
    if (s_fifo_left == 0) {
        s_fifo_cmds = w;
        s_fifo_left = 4;
        s_fifo_param = 0;
        s_fifo_total = s_valid[w & 0xff] ? s_params[w & 0xff] : 0;
        if (s_fifo_total > 0)
            return;
    } else {
        s_fifo_param++;
    }
    for (;;) {
        if ((s_fifo_cmds & 0xff) || (s_fifo_left == 4 && s_fifo_cmds == 0))
            push_entry(s_fifo_cmds & 0xff, w);
        if (s_fifo_param >= s_fifo_total) {
            s_fifo_cmds >>= 8;
            if (--s_fifo_left == 0)
                break;
            s_fifo_param = 0;
            s_fifo_total = s_valid[s_fifo_cmds & 0xff] ? s_params[s_fifo_cmds & 0xff] : 0;
        }
        if (s_fifo_param < s_fifo_total)
            break;
    }
}

int kh_gx3d_is_port(const volatile void *p)
{
    uintptr_t off = (uintptr_t)p - (uintptr_t)kh_ds_io;
    return off >= 0x400 && off < 0x5cc;
}

void kh_gx_cmd(volatile void *reg, unsigned long value)
{
    uintptr_t off = (uintptr_t)reg - (uintptr_t)kh_ds_io;
    if (off >= 0x400 && off < 0x440)
        fifo_write((uint32_t)value);
    else if (off >= 0x440 && off < 0x5cc)
        push_entry((int)((off - 0x400) >> 2), (uint32_t)value);
    else
        *(volatile uint32_t *)reg = (uint32_t)value;
}

void kh_gx3d_sync_gxstat(void)
{
    uint32_t v = KH_IO32(0x04000600);
    if (v & 0x8000)
        s_stack_error = 0; /* write 1 to acknowledge (the stored copy keeps bit 15 clear) */
    v &= 0xc0000003u;      /* IRQ mode (game's), box-test result (ours) */
    v |= (uint32_t)(s_pos_sp & 31) << 8;
    v |= (uint32_t)(s_proj_sp & 1) << 13;
    v |= 0x06000000u;      /* the FIFO is always drained: empty and under half full */
    KH_IO32(0x04000600) = v;
}

uint32_t kh_gx3d_serial(void)
{
    return __atomic_load_n(&s_serial, __ATOMIC_ACQUIRE);
}

void kh_gx3d_take_stats(KhGx3dStats *out)
{
    *out = s_stats;
    memset(&s_stats, 0, sizeof(s_stats));
}

void kh_gx3d_init(void)
{
    int i;
    for (i = 0; i < 256; i++)
        s_valid[i] = (i == 0 || s_params[i] != 0 || i == 0x11 || i == 0x15 || i == 0x41);
    mtx_identity(s_proj);
    mtx_identity(s_pos);
    mtx_identity(s_vec);
    mtx_identity(s_tex);
    s_clip_dirty = 1;
    update_clip();
    publish_vec();
    s_build = 0;
    s_shown = 1;
    s_ready = 2;
    frame_begin();
}
