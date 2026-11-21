/* m26: the stage viewer: see stage3d.h */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#include "nuklear.h"

#include "stage3d.h"
#include "util.h"

/* what is under a pixel: the layer in the top bits, the triangle below */
enum { ID_NONE = 0, ID_STAGE = 1, ID_SEE = 2, ID_PROP = 3, ID_COLL = 4 };
#define ID_OF(l, i) ((uint32_t)(l) << 28 | ((uint32_t)(i) & 0x0FFFFFFFu))

typedef struct { int n; int w[14], h[14]; uint8_t *px[14]; } Mip;

typedef struct {
    float x[3], y[3], iz[3], u[3], v[3];   /* u, v already divided by z */
    float miny, maxy;
    const uint8_t *px;                     /* the texture's level, or NULL: plain colour */
    int tw, th;
    uint8_t r, g, b, mode;                 /* mode 0 opaque, 1 cut-out (alpha test), 2 laid over (half) */
    uint32_t id;
} STri;

static struct {
    JStage s;
    int open;
    Mip *mip;
    /* the camera */
    float pos[3], yaw, pitch, speed;
    int cam_set;
    /* the layers */
    int vis, see, prop, coll, rail, surf[JST_NSURF], scount[JST_NSURF];
    /* the picture */
    SDL_Texture *rt;
    int tw, th;                            /* the texture's size */
    int rw, rh;                            /* drawn this time (smaller while moving) */
    uint8_t *fb;
    float *zb;
    uint32_t *idb;
    int need, moving, still;
    float *vv, *cv;                        /* the vertices in the camera's space */
    STri *st;
    int nst, cst, nover;                   /* the laid-over ones come after nover */
    int nthreads;
    uint64_t last_t;
    uint32_t hover;
    float ms;                              /* the last picture's time */
} G;

/* ---- textures: smaller levels, so far surfaces don't shimmer ---- */

static void make_mips(void)
{
    int k, l;
    G.mip = (Mip *)calloc((size_t)(G.s.ntex ? G.s.ntex : 1), sizeof(Mip));
    for (k = 0; k < G.s.ntex; k++) {
        Mip *m = &G.mip[k];
        m->n = 1;
        m->w[0] = G.s.tex[k].w; m->h[0] = G.s.tex[k].h; m->px[0] = G.s.tex[k].rgba;
        for (l = 1; l < 14 && (m->w[l - 1] > 1 || m->h[l - 1] > 1); l++) {
            int w = m->w[l - 1] > 1 ? m->w[l - 1] / 2 : 1, h = m->h[l - 1] > 1 ? m->h[l - 1] / 2 : 1, x, y, c;
            const uint8_t *s = m->px[l - 1];
            int sw = m->w[l - 1], sh = m->h[l - 1];
            uint8_t *d = (uint8_t *)malloc((size_t)w * h * 4);
            if (!d) break;
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++)
                    for (c = 0; c < 4; c++) {
                        int x0 = x * 2 < sw ? x * 2 : sw - 1, x1 = x * 2 + 1 < sw ? x * 2 + 1 : sw - 1;
                        int y0 = y * 2 < sh ? y * 2 : sh - 1, y1 = y * 2 + 1 < sh ? y * 2 + 1 : sh - 1;
                        d[((size_t)y * w + x) * 4 + c] = (uint8_t)((s[((size_t)y0 * sw + x0) * 4 + c] + s[((size_t)y0 * sw + x1) * 4 + c]
                                                                   + s[((size_t)y1 * sw + x0) * 4 + c] + s[((size_t)y1 * sw + x1) * 4 + c] + 2) / 4);
                    }
            m->w[l] = w; m->h[l] = h; m->px[l] = d;
            m->n = l + 1;
        }
    }
}

static void free_mips(void)
{
    int k, l;
    if (!G.mip) return;
    for (k = 0; k < G.s.ntex; k++)
        for (l = 1; l < G.mip[k].n; l++) free(G.mip[k].px[l]);
    free(G.mip);
    G.mip = NULL;
}

/* ---- opening ---- */

static void overview(void)
{
    const float *lo = G.s.lo, *hi = G.s.hi;
    float c[3] = { (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f };
    float size = fmaxf(hi[0] - lo[0], hi[2] - lo[2]);
    G.pos[0] = c[0];
    G.pos[1] = hi[1] + size * 0.45f;
    G.pos[2] = lo[2] - size * 0.35f;
    G.yaw = 0;                                  /* towards +z */
    G.pitch = -atan2f(G.pos[1] - c[1], c[2] - G.pos[2]);
    G.speed = size * 0.25f;                     /* a quarter of the stage a second */
    G.cam_set = 1;
    G.need = 1;
}

int s3d_open(const char *dir, const char *name, char *err, size_t errn)
{
    int k;
    s3d_close();
    if (jst_load(dir, name, &G.s, err, errn)) return -1;
    G.open = 1;
    make_mips();
    G.vis = G.see = G.prop = 1;
    G.coll = 0;
    G.rail = 1;
    memset(G.scount, 0, sizeof G.scount);
    for (k = 0; k < G.s.nct; k++) G.scount[jst_surf_index(G.s.csurf[k])]++;
    for (k = 0; k < JST_NSURF; k++) G.surf[k] = 1;
    G.vv = (float *)malloc(sizeof(float) * 3 * (size_t)(G.s.nv + 1));
    G.cv = (float *)malloc(sizeof(float) * 3 * (size_t)(G.s.ncv + 1));
    G.nthreads = cpu_count();
    if (G.nthreads < 1) G.nthreads = 1;
    if (G.nthreads > 16) G.nthreads = 16;
    overview();
    return 0;
}

void s3d_close(void)
{
    if (!G.open) return;
    free_mips();
    jst_free(&G.s);
    free(G.vv); free(G.cv); free(G.st);
    G.vv = G.cv = NULL; G.st = NULL; G.cst = G.nst = 0;
    G.open = 0;
}

int s3d_is_open(void) { return G.open; }
const JStage *s3d_stage(void) { return G.open ? &G.s : NULL; }

void s3d_set_camera(float x, float y, float z, float yaw_deg, float pitch_deg)
{
    G.pos[0] = x; G.pos[1] = y; G.pos[2] = z;
    G.yaw = yaw_deg * 3.14159265f / 180.0f;
    G.pitch = pitch_deg * 3.14159265f / 180.0f;
    G.cam_set = 1;
    G.need = 1;
}

void s3d_set_layers(const char *l)
{
    G.vis = G.see = !!strchr(l, 'v');
    G.prop = !!strchr(l, 'p');
    G.coll = !!strchr(l, 'c');
    G.rail = !!strchr(l, 'r');
    G.need = 1;
}

/* ---- drawing ---- */

typedef struct { float x, y, z, u, v; } CV;   /* a vertex in the camera's space */

static float s_focal, s_cx, s_cy;
#define NEAR_Z 2.0f

static STri *new_tri(void)
{
    if (G.nst == G.cst) {
        int nc = G.cst ? G.cst * 2 : 65536;
        STri *n = (STri *)realloc(G.st, sizeof(STri) * (size_t)nc);
        if (!n) return NULL;
        G.st = n;
        G.cst = nc;
    }
    return &G.st[G.nst++];
}

/* a triangle in the camera's space, cut at the near plane, onto the screen */
static void emit(const CV *in, const Mip *mip, uint8_t r, uint8_t g, uint8_t b, int mode, uint32_t id)
{
    CV poly[4];
    int n = 0, i, k, level = 0;
    for (i = 0; i < 3; i++) {                   /* Sutherland-Hodgman against z >= NEAR_Z */
        const CV *a = &in[i], *c = &in[(i + 1) % 3];
        int ain = a->z >= NEAR_Z, cin = c->z >= NEAR_Z;
        if (ain) poly[n++] = *a;
        if (ain != cin) {
            float t = (NEAR_Z - a->z) / (c->z - a->z);
            CV q;
            q.x = a->x + (c->x - a->x) * t; q.y = a->y + (c->y - a->y) * t; q.z = NEAR_Z;
            q.u = a->u + (c->u - a->u) * t; q.v = a->v + (c->v - a->v) * t;
            poly[n++] = q;
        }
    }
    if (n < 3) return;
    {
        float sx[4], sy[4], iz[4], minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
        for (i = 0; i < n; i++) {
            iz[i] = 1.0f / poly[i].z;
            sx[i] = s_cx + poly[i].x * s_focal * iz[i];
            sy[i] = s_cy - poly[i].y * s_focal * iz[i];
            if (sx[i] < minx) minx = sx[i];
            if (sx[i] > maxx) maxx = sx[i];
            if (sy[i] < miny) miny = sy[i];
            if (sy[i] > maxy) maxy = sy[i];
        }
        if (maxx < 0 || minx >= (float)G.rw || maxy < 0 || miny >= (float)G.rh) return;
        if (maxx - minx > 1e5f || maxy - miny > 1e5f) return;
        if (mip && mip->n) {                    /* the level: texels per pixel */
            float sa = fabsf((sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]));
            float ta = fabsf((poly[1].u - poly[0].u) * (poly[2].v - poly[0].v) - (poly[1].v - poly[0].v) * (poly[2].u - poly[0].u))
                       * (float)mip->w[0] * (float)mip->h[0];
            if (sa > 1e-3f && ta > 0) {
                float r2 = ta / sa;
                level = r2 > 1 ? (int)floorf(0.5f * log2f(r2)) : 0;
                if (level >= mip->n) level = mip->n - 1;
                if (level < 0) level = 0;
            } else if (sa <= 1e-3f) level = mip->n - 1;
        }
        for (k = 1; k + 1 < n; k++) {
            STri *t = new_tri();
            int vi[3] = { 0, k, k + 1 };
            if (!t) return;
            for (i = 0; i < 3; i++) {
                t->x[i] = sx[vi[i]]; t->y[i] = sy[vi[i]]; t->iz[i] = iz[vi[i]];
                t->u[i] = poly[vi[i]].u * iz[vi[i]]; t->v[i] = poly[vi[i]].v * iz[vi[i]];
            }
            t->miny = fminf(t->y[0], fminf(t->y[1], t->y[2]));
            t->maxy = fmaxf(t->y[0], fmaxf(t->y[1], t->y[2]));
            t->px = mip && mip->n ? mip->px[level] : NULL;
            t->tw = mip && mip->n ? mip->w[level] : 0;
            t->th = mip && mip->n ? mip->h[level] : 0;
            t->r = r; t->g = g; t->b = b; t->mode = (uint8_t)mode;
            t->id = id;
        }
    }
}

/* the light: from above, a little from the side; both faces lit */
static float shade(const float *a, const float *b, const float *c, float lo, float hi)
{
    float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), d;
    if (l < 1e-12f) return hi;
    d = fabsf(n[0] * 0.35f + n[1] * 0.85f + n[2] * 0.40f) / (l * 1.0124f);
    return lo + (hi - lo) * d;
}

typedef struct { int y0, y1; } Band;

static int raster_band(void *u)
{
    const Band *bd = (const Band *)u;
    int i;
    for (i = 0; i < G.nst; i++) {
        const STri *t = &G.st[i];
        float area, inv, minx, maxx;
        int x0, x1, y0, y1, x, y;
        if (t->maxy < (float)bd->y0 || t->miny >= (float)bd->y1) continue;
        area = (t->x[1] - t->x[0]) * (t->y[2] - t->y[0]) - (t->y[1] - t->y[0]) * (t->x[2] - t->x[0]);
        if (fabsf(area) < 1e-8f) continue;
        inv = 1.0f / area;
        minx = fminf(t->x[0], fminf(t->x[1], t->x[2]));
        maxx = fmaxf(t->x[0], fmaxf(t->x[1], t->x[2]));
        x0 = (int)floorf(minx); x1 = (int)ceilf(maxx);
        y0 = (int)floorf(t->miny); y1 = (int)ceilf(t->maxy);
        if (x0 < 0) x0 = 0;
        if (x1 > G.rw - 1) x1 = G.rw - 1;
        if (y0 < bd->y0) y0 = bd->y0;
        if (y1 > bd->y1 - 1) y1 = bd->y1 - 1;
        for (y = y0; y <= y1; y++) {
            float py = (float)y + 0.5f;
            for (x = x0; x <= x1; x++) {
                float px = (float)x + 0.5f;
                float w0 = ((t->x[1] - px) * (t->y[2] - py) - (t->y[1] - py) * (t->x[2] - px)) * inv;
                float w1 = ((t->x[2] - px) * (t->y[0] - py) - (t->y[2] - py) * (t->x[0] - px)) * inv;
                float w2 = 1.0f - w0 - w1, iz;
                size_t pi = (size_t)y * G.rw + x;
                uint8_t *o, cr, cg, cb;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                iz = w0 * t->iz[0] + w1 * t->iz[1] + w2 * t->iz[2];
                if (t->mode == 2) { if (iz * 1.004f < G.zb[pi]) continue; }
                else if (iz <= G.zb[pi]) continue;
                if (t->px) {
                    float uz = w0 * t->u[0] + w1 * t->u[1] + w2 * t->u[2];
                    float vz = w0 * t->v[0] + w1 * t->v[1] + w2 * t->v[2];
                    float uu = uz / iz, vv = vz / iz;
                    int tx, ty;
                    const uint8_t *s;
                    if (!(fabsf(uu) < 1e6f && fabsf(vv) < 1e6f)) uu = vv = 0;
                    tx = (int)floorf((uu - floorf(uu)) * (float)t->tw);
                    ty = (int)floorf((vv - floorf(vv)) * (float)t->th);
                    if (tx >= t->tw) tx = t->tw - 1;
                    if (ty >= t->th) ty = t->th - 1;
                    if (tx < 0) tx = 0;
                    if (ty < 0) ty = 0;
                    s = t->px + ((size_t)ty * t->tw + tx) * 4;
                    if (t->mode == 1 && s[3] < 100) continue;
                    cr = (uint8_t)((s[0] * t->r) >> 8); cg = (uint8_t)((s[1] * t->g) >> 8); cb = (uint8_t)((s[2] * t->b) >> 8);
                } else { cr = t->r; cg = t->g; cb = t->b; }
                o = G.fb + pi * 4;
                if (t->mode == 2) {
                    o[0] = (uint8_t)((o[0] * 2 + cr * 3) / 5); o[1] = (uint8_t)((o[1] * 2 + cg * 3) / 5); o[2] = (uint8_t)((o[2] * 2 + cb * 3) / 5);
                } else {
                    o[0] = cr; o[1] = cg; o[2] = cb;
                    G.zb[pi] = iz;
                }
                o[3] = 255;
                G.idb[pi] = t->id;
            }
        }
    }
    return 0;
}

static void basis(float f[3], float r[3], float u[3])
{
    float cp = cosf(G.pitch);
    f[0] = sinf(G.yaw) * cp; f[1] = sinf(G.pitch); f[2] = cosf(G.yaw) * cp;
    r[0] = cosf(G.yaw); r[1] = 0; r[2] = -sinf(G.yaw);          /* (0,1,0) x f: x to the right, as the game has it */
    u[0] = f[1] * r[2] - f[2] * r[1]; u[1] = f[2] * r[0] - f[0] * r[2]; u[2] = f[0] * r[1] - f[1] * r[0];
}

static void to_cam(const float *p, int n, float *out, const float f[3], const float r[3], const float u[3])
{
    int i;
    for (i = 0; i < n; i++) {
        float d0 = p[i * 3] - G.pos[0], d1 = p[i * 3 + 1] - G.pos[1], d2 = p[i * 3 + 2] - G.pos[2];
        out[i * 3] = d0 * r[0] + d1 * r[1] + d2 * r[2];
        out[i * 3 + 1] = d0 * u[0] + d1 * u[1] + d2 * u[2];
        out[i * 3 + 2] = d0 * f[0] + d1 * f[1] + d2 * f[2];
    }
}

static void line3(const float *a, const float *b, const float f[3], const float r[3], const float u[3], uint8_t cr, uint8_t cg, uint8_t cb, int thick)
{
    float pa[3], pb[3], ia, ib, ax, ay, bx, by, dx, dy;
    int n, k;
    to_cam(a, 1, pa, f, r, u);
    to_cam(b, 1, pb, f, r, u);
    if (pa[2] < NEAR_Z && pb[2] < NEAR_Z) return;
    if (pa[2] < NEAR_Z || pb[2] < NEAR_Z) {
        float t = (NEAR_Z - pa[2]) / (pb[2] - pa[2]), q[3];
        for (k = 0; k < 3; k++) q[k] = pa[k] + (pb[k] - pa[k]) * t;
        if (pa[2] < NEAR_Z) memcpy(pa, q, sizeof q); else memcpy(pb, q, sizeof q);
    }
    ia = 1.0f / pa[2]; ib = 1.0f / pb[2];
    ax = s_cx + pa[0] * s_focal * ia; ay = s_cy - pa[1] * s_focal * ia;
    bx = s_cx + pb[0] * s_focal * ib; by = s_cy - pb[1] * s_focal * ib;
    dx = bx - ax; dy = by - ay;
    n = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy)));
    if (n > 20000) return;
    for (k = 0; k <= n; k++) {
        float s = n ? (float)k / (float)n : 0, iz = ia + (ib - ia) * s;
        int x = (int)(ax + dx * s), y = (int)(ay + dy * s), ox, oy;
        for (oy = 0; oy < thick; oy++)
            for (ox = 0; ox < thick; ox++) {
                int xx = x + ox - thick / 2, yy = y + oy - thick / 2;
                size_t i;
                uint8_t *o;
                if (xx < 0 || yy < 0 || xx >= G.rw || yy >= G.rh) continue;
                i = (size_t)yy * G.rw + xx;
                if (iz < G.zb[i] * 0.97f) continue;   /* hidden (a little slack: rails lie on their edges) */
                o = G.fb + i * 4;
                o[0] = cr; o[1] = cg; o[2] = cb;
            }
    }
}

static void render(void)
{
    float f[3], r[3], u[3];
    int i, y;
    uint64_t t0 = SDL_GetPerformanceCounter();
    basis(f, r, u);
    s_focal = (float)G.rh * 0.5f / tanf(0.6f);  /* about 69 degrees high */
    s_cx = (float)G.rw * 0.5f;
    s_cy = (float)G.rh * 0.5f;
    for (y = 0; y < G.rh; y++) {                /* the sky */
        float k = (float)y / (float)(G.rh ? G.rh : 1);
        uint8_t cr = (uint8_t)(70 + 60 * k), cg = (uint8_t)(110 + 60 * k), cb = (uint8_t)(170 + 50 * k);
        uint8_t *o = G.fb + (size_t)y * G.rw * 4;
        for (i = 0; i < G.rw; i++, o += 4) { o[0] = cr; o[1] = cg; o[2] = cb; o[3] = 255; }
    }
    memset(G.zb, 0, sizeof(float) * (size_t)G.rw * G.rh);
    memset(G.idb, 0, sizeof(uint32_t) * (size_t)G.rw * G.rh);
    G.nst = 0;
    to_cam(G.s.p, G.s.nv, G.vv, f, r, u);
    to_cam(G.s.cp, G.s.ncv, G.cv, f, r, u);
    {
        int over = G.coll && (G.vis || G.see || G.prop);
        /* the models and props */
        for (i = 0; i < G.s.nt; i++) {
            int kind = G.s.tkind[i], k;
            CV c[3];
            const uint32_t *ix = G.s.ix + i * 3;
            const Mip *m;
            uint32_t col = 0;
            float l;
            int cr = 0, cg = 0, cb = 0;
            if ((kind == JST_T_STAGE && !G.vis) || (kind == JST_T_SEE && !G.see) || (kind == JST_T_PROP && !G.prop)) continue;
            for (k = 0; k < 3; k++) {
                const float *p = G.vv + ix[k] * 3;
                c[k].x = p[0]; c[k].y = p[1]; c[k].z = p[2];
                c[k].u = G.s.uv[ix[k] * 2]; c[k].v = G.s.uv[ix[k] * 2 + 1];
                col = G.s.col[ix[k]];
                cr += (int)(col & 255); cg += (int)(col >> 8 & 255); cb += (int)(col >> 16 & 255);
            }
            if (c[0].z < NEAR_Z && c[1].z < NEAR_Z && c[2].z < NEAR_Z) continue;
            m = G.s.ttex[i] >= 0 ? &G.mip[G.s.ttex[i]] : NULL;
            l = shade(G.s.p + ix[0] * 3, G.s.p + ix[1] * 3, G.s.p + ix[2] * 3, 0.78f, 1.0f);
            if (m) {
                int rr = (int)(cr / 3 * l), gg = (int)(cg / 3 * l), bb = (int)(cb / 3 * l);
                emit(c, m, (uint8_t)(rr > 255 ? 255 : rr), (uint8_t)(gg > 255 ? 255 : gg), (uint8_t)(bb > 255 ? 255 : bb),
                     kind == JST_T_STAGE ? 0 : 1, ID_OF(kind == JST_T_STAGE ? ID_STAGE : kind == JST_T_SEE ? ID_SEE : ID_PROP, i));
            } else
                emit(c, NULL, (uint8_t)(150 * l), (uint8_t)(150 * l), (uint8_t)(155 * l), 0,
                     ID_OF(kind == JST_T_STAGE ? ID_STAGE : kind == JST_T_SEE ? ID_SEE : ID_PROP, i));
        }
        G.nover = G.nst;
        /* the collision, by surface: laid over the models, or on its own */
        if (G.coll)
            for (i = 0; i < G.s.nct; i++) {
                int s = jst_surf_index(G.s.csurf[i]), k;
                CV c[3];
                const uint32_t *ix = G.s.cix + i * 3;
                float l;
                if (!G.surf[s]) continue;
                for (k = 0; k < 3; k++) {
                    const float *p = G.cv + ix[k] * 3;
                    c[k].x = p[0]; c[k].y = p[1]; c[k].z = p[2]; c[k].u = c[k].v = 0;
                }
                if (c[0].z < NEAR_Z && c[1].z < NEAR_Z && c[2].z < NEAR_Z) continue;
                l = shade(G.s.cp + ix[0] * 3, G.s.cp + ix[1] * 3, G.s.cp + ix[2] * 3, 0.45f, 1.0f);
                emit(c, NULL, (uint8_t)(jst_surf[s].r * l), (uint8_t)(jst_surf[s].g * l), (uint8_t)(jst_surf[s].b * l),
                     over ? 2 : 0, ID_OF(ID_COLL, i));
            }
    }
    /* the triangles, a band of rows a thread (the laid-over ones last in each) */
    {
        SDL_Thread *th[16];
        Band bd[16];
        int n = G.nthreads, k;
        if (n > G.rh / 8) n = G.rh / 8 > 0 ? G.rh / 8 : 1;
        for (k = 0; k < n; k++) {
            bd[k].y0 = G.rh * k / n;
            bd[k].y1 = G.rh * (k + 1) / n;
            th[k] = k ? SDL_CreateThread(raster_band, "stage", &bd[k]) : NULL;
            if (k && !th[k]) raster_band(&bd[k]);
        }
        raster_band(&bd[0]);
        for (k = 1; k < n; k++) if (th[k]) SDL_WaitThread(th[k], NULL);
    }
    /* the rails, over it all where they're seen */
    if (G.rail) {
        int k, q, thick = G.rw >= 900 ? 3 : 2;
        for (k = 0; k < G.s.nrail; k++)
            for (q = 0; q + 1 < G.s.rn[k]; q++) {
                const float *a = G.s.rp + (G.s.rs[k] + q) * 3;
                line3(a, a + 3, f, r, u, 255, 196, 30, thick);
            }
    }
    G.ms = (float)((double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
    if (getenv("MT_WINDING")) {                  /* (tests) which way the triangles seen turn on the screen */
        int pos = 0, neg = 0, q;
        for (q = 0; q < G.nst; q++) {
            const STri *t = &G.st[q];
            float a = (t->x[1] - t->x[0]) * (t->y[2] - t->y[0]) - (t->y[1] - t->y[0]) * (t->x[2] - t->x[0]);
            int seen = 0;
            size_t pi;
            for (pi = 0; pi < (size_t)G.rw * G.rh && !seen; pi += 7) seen = G.idb[pi] == t->id;
            if (!seen || (t->id >> 28) != ID_STAGE) continue;
            if (a > 0) pos++; else neg++;
        }
        fprintf(stderr, "[winding] seen stage triangles: %d clockwise on the screen, %d anticlockwise\n", pos, neg);
    }
}

/* ---- the panel ---- */

static void hover_text(char *out, size_t n)
{
    uint32_t id = G.hover, l = id >> 28, i = id & 0x0FFFFFFFu;
    if (l == ID_COLL && (int)i < G.s.nct) {
        int s = jst_surf_index(G.s.csurf[i]);
        snprintf(out, n, "Collision: %s (flags %u)", jst_surf[s].name, G.s.csurf[i]);
    } else if ((l == ID_STAGE || l == ID_SEE || l == ID_PROP) && (int)i < G.s.nt) {
        int t = G.s.ttex[i];
        const char *what = l == ID_STAGE ? "Stage model" : l == ID_SEE ? "Stage model (see-through)" : "Prop";
        if (t >= 0) snprintf(out, n, "%s, texture %08X (%dx%d)", what, G.s.tex[t].id, G.s.tex[t].w, G.s.tex[t].h);
        else snprintf(out, n, "%s, no texture known", what);
    } else out[0] = 0;
}

static int check(struct nk_context *ctx, const char *label, int *v)
{
    nk_bool b = (nk_bool)*v;
    nk_checkbox_label(ctx, label, &b);
    if ((int)b != *v) { *v = b; G.need = 1; return 1; }
    return 0;
}

int s3d_draw(struct nk_context *ctx, SDL_Renderer *ren, float x, float y, float w, float h, float row,
             char *msg, size_t msgn, const char *export_dir, void (*more)(struct nk_context *ctx, float row))
{
    const float cw = 300;
    int back = 0, k;
    char t[256];
    if (!G.open) return 1;
    if (nk_begin(ctx, "Stage", nk_rect(x, y, cw, h), NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        nk_layout_row_dynamic(ctx, row, 1);
        if (nk_button_label(ctx, "Back to the stages")) back = 1;
        nk_layout_row_dynamic(ctx, row - 6, 1);
        nk_labelf(ctx, NK_TEXT_LEFT, "%s: %d .dat files", G.s.name, G.s.ndat);
        nk_labelf(ctx, NK_TEXT_LEFT, "%d stage models, %d props", G.s.nmodels, G.s.nprops);
        nk_labelf(ctx, NK_TEXT_LEFT, "%d triangles, %d textures", G.s.nt, G.s.ntex);
        nk_layout_row_dynamic(ctx, row, 1);
        nk_label(ctx, "Show", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, row - 4, 1);
        check(ctx, "Stage models", &G.vis);
        check(ctx, "See-through parts", &G.see);
        check(ctx, "Props", &G.prop);
        snprintf(t, sizeof t, "Grind rails (%d)", G.s.nrail);
        check(ctx, t, &G.rail);
        snprintf(t, sizeof t, "Collision (%d triangles)", G.s.nct);
        check(ctx, t, &G.coll);
        if (G.coll) {
            for (k = 0; k < JST_NSURF; k++) {
                struct nk_rect b;
                if (!G.scount[k]) continue;
                nk_layout_row_template_begin(ctx, row - 6);
                nk_layout_row_template_push_static(ctx, 14);
                nk_layout_row_template_push_dynamic(ctx);
                nk_layout_row_template_end(ctx);
                b = nk_widget_bounds(ctx);
                nk_fill_rect(nk_window_get_canvas(ctx), nk_rect(b.x + 2, b.y + 5, 10, b.h - 10), 2,
                             nk_rgb(jst_surf[k].r, jst_surf[k].g, jst_surf[k].b));
                nk_spacing(ctx, 1);
                snprintf(t, sizeof t, "%s (%d)", jst_surf[k].name, G.scount[k]);
                check(ctx, t, &G.surf[k]);
            }
        }
        nk_layout_row_dynamic(ctx, row, 1);
        nk_label(ctx, "Camera", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, row, 2);
        if (nk_button_label(ctx, "Overview")) overview();
        if (nk_button_label(ctx, "From above")) {
            float c[3] = { (G.s.lo[0] + G.s.hi[0]) * 0.5f, 0, (G.s.lo[2] + G.s.hi[2]) * 0.5f };
            float size = fmaxf(G.s.hi[0] - G.s.lo[0], G.s.hi[2] - G.s.lo[2]);
            G.pos[0] = c[0]; G.pos[1] = G.s.hi[1] + size * 0.75f; G.pos[2] = c[2];
            G.yaw = 0; G.pitch = -1.5607f;
            G.need = 1;
        }
        nk_layout_row_dynamic(ctx, row - 4, 1);
        {
            float size = fmaxf(G.s.hi[0] - G.s.lo[0], G.s.hi[2] - G.s.lo[2]), sp = G.speed;
            nk_labelf(ctx, NK_TEXT_LEFT, "Speed: %.0f a second", G.speed);
            nk_slider_float(ctx, size * 0.01f, &sp, size * 1.0f, size * 0.005f);
            G.speed = sp;
        }
        nk_layout_row_dynamic(ctx, row - 8, 1);
        nk_label_colored(ctx, "Left drag: look around", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        nk_label_colored(ctx, "W A S D: move, Q E: down, up", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        nk_label_colored(ctx, "Shift: faster. Wheel: forward, back", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        nk_label_colored(ctx, "Right drag: move sideways, up", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160), "At %.0f, %.0f, %.0f", G.pos[0], G.pos[1], G.pos[2]);
        nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160), "Drawn in %.0f ms", G.ms);
        nk_layout_row_dynamic(ctx, row, 1);
        if (nk_button_label(ctx, "Export as .obj")) {
            char dir[LPATH], err[512];
            pjoin(dir, export_dir, G.s.name);
            if (jst_write_obj(&G.s, dir, err, sizeof err)) snprintf(msg, msgn, "%s", err);
            else snprintf(msg, msgn, "%s exported to %s: %s_visual.obj (with its textures), %s_collision.obj, %s_rails.obj.",
                          G.s.name, dir, G.s.name, G.s.name, G.s.name);
        }
        if (more) more(ctx, row);
    }
    nk_end(ctx);

    if (nk_begin(ctx, "Stage view", nk_rect(x + cw, y, w - cw, h), NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        struct nk_input *in = &ctx->input;
        struct nk_rect b;
        char hv[160];
        nk_layout_row_dynamic(ctx, row - 6, 1);
        hover_text(hv, sizeof hv);
        nk_label(ctx, hv[0] ? hv : "Point at something to see what it is.", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, h - row - 30, 1);
        if (nk_widget(&b, ctx) != NK_WIDGET_INVALID) {
            int bw = (int)b.w, bh = (int)b.h, hover = nk_input_is_mouse_hovering_rect(in, b), moved = 0, scale;
            uint64_t now = SDL_GetTicks64();
            float dt = G.last_t ? (float)(now - G.last_t) / 1000.0f : 0;
            float f[3], r[3], u[3];
            G.last_t = now;
            if (dt > 0.1f) dt = 0.1f;
            if (bw < 16) bw = 16;
            if (bh < 16) bh = 16;
            if (bw > 3840) bw = 3840;
            if (bh > 2160) bh = 2160;
            if (bw != G.tw || bh != G.th || !G.rt) {
                if (G.rt) SDL_DestroyTexture(G.rt);
                G.rt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, bw, bh);
                free(G.fb); free(G.zb); free(G.idb);
                G.fb = (uint8_t *)malloc((size_t)bw * bh * 4);
                G.zb = (float *)malloc(sizeof(float) * (size_t)bw * bh);
                G.idb = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)bw * bh);
                G.tw = bw; G.th = bh;
                G.need = 1;
            }
            basis(f, r, u);
            if (nk_input_has_mouse_click_down_in_rect(in, NK_BUTTON_LEFT, b, nk_true) && (in->mouse.delta.x || in->mouse.delta.y)) {
                G.yaw += in->mouse.delta.x * 0.005f;
                G.pitch -= in->mouse.delta.y * 0.005f;
                if (G.pitch > 1.56f) G.pitch = 1.56f;
                if (G.pitch < -1.56f) G.pitch = -1.56f;
                moved = 1;
            }
            if (nk_input_has_mouse_click_down_in_rect(in, NK_BUTTON_RIGHT, b, nk_true) && (in->mouse.delta.x || in->mouse.delta.y)) {
                float k2 = G.speed * 0.004f;
                for (k = 0; k < 3; k++) G.pos[k] += -in->mouse.delta.x * k2 * r[k] + in->mouse.delta.y * k2 * u[k];
                moved = 1;
            }
            if (hover && in->mouse.scroll_delta.y != 0) {
                for (k = 0; k < 3; k++) G.pos[k] += f[k] * in->mouse.scroll_delta.y * G.speed * 0.15f;
                in->mouse.scroll_delta.y = 0;
                moved = 1;
            }
            if (hover && dt > 0) {
                const Uint8 *ks = SDL_GetKeyboardState(NULL);
                float mv[3] = { 0, 0, 0 }, sp = G.speed * dt * ((ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT]) ? 4.0f : 1.0f);
                float fl[3] = { f[0], 0, f[2] }, fl_l = sqrtf(f[0] * f[0] + f[2] * f[2]);
                if (fl_l > 1e-4f) { fl[0] /= fl_l; fl[2] /= fl_l; }
                if (ks[SDL_SCANCODE_W] || ks[SDL_SCANCODE_UP]) for (k = 0; k < 3; k++) mv[k] += f[k];
                if (ks[SDL_SCANCODE_S] || ks[SDL_SCANCODE_DOWN]) for (k = 0; k < 3; k++) mv[k] -= f[k];
                if (ks[SDL_SCANCODE_D] || ks[SDL_SCANCODE_RIGHT]) for (k = 0; k < 3; k++) mv[k] += r[k];
                if (ks[SDL_SCANCODE_A] || ks[SDL_SCANCODE_LEFT]) for (k = 0; k < 3; k++) mv[k] -= r[k];
                if (ks[SDL_SCANCODE_E]) mv[1] += 1;
                if (ks[SDL_SCANCODE_Q]) mv[1] -= 1;
                if (mv[0] || mv[1] || mv[2]) {
                    for (k = 0; k < 3; k++) G.pos[k] += mv[k] * sp;
                    moved = 1;
                }
            }
            if (moved) { G.need = 1; G.moving = 1; G.still = 0; }
            else if (G.moving && ++G.still > 2) { G.moving = 0; G.need = 1; }   /* sharp again once it stops */
            scale = G.moving ? 2 : 1;
            if (G.need && G.fb) {
                G.rw = G.tw / scale; G.rh = G.th / scale;
                render();
                {
                    SDL_Rect ur = { 0, 0, G.rw, G.rh };
                    SDL_UpdateTexture(G.rt, &ur, G.fb, G.rw * 4);
                }
                G.need = 0;
            }
            if (G.rt) {
                struct nk_image im = nk_subimage_ptr(G.rt, (nk_ushort)G.tw, (nk_ushort)G.th, nk_rect(0, 0, (float)G.rw, (float)G.rh));
                nk_draw_image(nk_window_get_canvas(ctx), nk_rect(b.x, b.y, (float)G.tw, (float)G.th), &im, nk_rgb(255, 255, 255));
            }
            G.hover = 0;
            if (hover && G.idb && G.rw > 0) {
                int mx = (int)((in->mouse.pos.x - b.x) / (float)G.tw * (float)G.rw), my = (int)((in->mouse.pos.y - b.y) / (float)G.th * (float)G.rh);
                if (mx >= 0 && my >= 0 && mx < G.rw && my < G.rh) G.hover = G.idb[(size_t)my * G.rw + mx];
            }
        }
    }
    nk_end(ctx);
    return back;
}
