/* m26: the stage viewer: see stage3d.h */
#include <ctype.h>
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
#include "jsbuild.h"
#include "util.h"

/* what is under a pixel: the layer in the top bits, the triangle below */
enum { ID_NONE = 0, ID_STAGE = 1, ID_SEE = 2, ID_PROP = 3, ID_COLL = 4, ID_SHADOW = 5 };
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
    int vis, see, prop, coll, rail, shadow, shadows, surf[JST_NSURF], scount[JST_NSURF];
    /* the picture */
    SDL_Texture *rt;
    int tw, th;                            /* the texture's size */
    int rw, rh;                            /* drawn this time (smaller while moving) */
    uint8_t *fb;
    float *zb;
    uint32_t *idb;
    int8_t *sb;                            /* m26e: the shadow volumes' count */
    int shadow_on_surf;                    /* this picture has shadows cast */
    int need, moving, still;
    float *vv, *cv;                        /* the vertices in the camera's space */
    STri *st;
    int nst, cst, nover;                   /* the laid-over ones come after nover */
    int nthreads;
    uint64_t last_t;
    uint32_t hover;
    float ms;                              /* the last picture's time */
    /* m28: the editor (stage editor, level 3: props) */
    char dir[2048];
    JSBin ed;                              /* the .bin as read, its placements kept */
    int ed_ok;
    int sel, grab, dirty, leave_ask;
    float grab_r[12], grab_dy;
    int press, pmoved;
    float px0, py0;
    uint8_t kprev[SDL_NUM_SCANCODES];
    struct Undo { int n; float (*r)[12]; uint8_t *rm; int np; float (*pc)[4]; uint8_t *prm; } undo[64];
    int psel;                              /* m29: a piece picked (-1 none), the first of ... */
    int *pset, npset, cpset;               /* ... the pieces picked together */
    uint8_t *inset;                        /* a piece's picked or not */
    float grab_pos[3], grab_yaw;
    int *ctm;                              /* m29: a collision triangle's model */
    float (*cm0)[28];                      /* m29: the collision models' heads as read (rows, position, box) */
    int *tset0;                            /* their triangle sets as read */
    int *tset_own;                         /* m29d: a moved model's own copy of its triangles (-1) */
    uint8_t (*cabox0)[24];                 /* the collision areas' boxes as read */
    float *pyaw_saved;                     /* a piece's turn as last saved (normals in the .dat) */
    int *rmap;                             /* m29: a rail's number in G.ed (-1) */
    int **list0, *nlist0;                  /* G.ed's rail lists as read */
    int slot0[1024], nlists0;              /* G.ed's slots as read */
    int nundo;
} G;

static void ed_box(int k, float lo[3], float hi[3]);
static int obj_ok(void);
static void pset_clean(void);
static void obj_box(float lo[3], float hi[3]);
static int (*s_saver)(const char *file, const uint8_t *data, size_t n, void *u);
static void *s_saver_u;
void s3d_set_saver(int (*save)(const char *file, const uint8_t *data, size_t n, void *u), void *u)
{
    s_saver = save;
    s_saver_u = u;
}

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
    snprintf(G.dir, sizeof G.dir, "%s", dir);
    G.sel = -1; G.grab = 0; G.dirty = 0; G.leave_ask = 0; G.nundo = 0;
    G.ed_ok = 0;
    if (G.s.binfile[0]) {                       /* m28: the .bin in its parts, for saving edits */
        char p[2048], e2[300];
        size_t n = 0;
        uint8_t *b;
        pjoin(p, dir, G.s.binfile);
        b = (uint8_t *)read_file(p, &n);
        if (b && !jsb_parse(b, n, &G.ed, e2, sizeof e2)) G.ed_ok = 1;
        free(b);
    }
    G.psel = -1;                                /* m29 */
    G.npset = 0;
    free(G.inset);
    G.inset = (uint8_t *)calloc((size_t)G.s.npiece + 1, 1);
    G.ctm = (int *)malloc(sizeof(int) * (size_t)(G.s.nct + 1));
    if (G.ctm) {
        int i, j;
        for (i = 0; i < G.s.nct; i++) G.ctm[i] = -1;
        for (i = 0; i < G.s.ncmodel; i++)
            for (j = 0; j < G.s.cmodels[i].nt; j++) G.ctm[G.s.cmodels[i].t0 + j] = i;
    }
    G.pyaw_saved = (float *)calloc((size_t)G.s.npiece + 1, sizeof(float));
    if (G.ed_ok) {                              /* the viewer's rails to the file's (by their points), and the lists as read */
        int i, j;
        G.rmap = (int *)malloc(sizeof(int) * (size_t)(G.s.nrail + 1));
        for (i = 0; G.rmap && i < G.s.nrail; i++) {
            G.rmap[i] = -1;
            for (j = 0; j < G.ed.nrail; j++) {   /* m29e: every point the same, and each file rail once (stages have rails twice over) */
                int q2, same = (int)(G.ed.rail[j].pts.n / 24) == G.s.rn[i] && G.s.rn[i] > 0, k2;
                for (q2 = 0; same && q2 < G.s.rn[i]; q2++)
                    same = !memcmp(G.ed.rail[j].pts.p + (size_t)q2 * 24, G.s.rp0 + (size_t)(G.s.rs[i] + q2) * 3, 12);
                for (k2 = 0; same && k2 < i; k2++) if (G.rmap[k2] == j) same = 0;
                if (same) { G.rmap[i] = j; break; }
            }
        }
        memcpy(G.slot0, G.ed.slot, sizeof G.slot0);
        G.nlists0 = G.ed.nlists;
        G.list0 = (int **)calloc((size_t)G.ed.nlists + 1, sizeof(int *));
        G.nlist0 = (int *)calloc((size_t)G.ed.nlists + 1, sizeof(int));
        for (i = 0; G.list0 && G.nlist0 && i < G.ed.nlists; i++) {
            G.list0[i] = (int *)malloc(sizeof(int) * (size_t)(G.ed.nlist[i] + 1));
            if (G.list0[i]) memcpy(G.list0[i], G.ed.list[i], sizeof(int) * (size_t)G.ed.nlist[i]);
            G.nlist0[i] = G.ed.nlist[i];
        }
    }
    if (G.ed_ok && G.ed.ncm == G.s.ncmodel) {   /* the collision as read, to move it with the pieces */
        int i;
        G.cm0 = (float (*)[28])calloc((size_t)G.ed.ncm + 1, sizeof *G.cm0);
        G.tset0 = (int *)calloc((size_t)G.ed.ncm + 1, sizeof(int));
        G.cabox0 = (uint8_t (*)[24])calloc((size_t)G.ed.nca + 1, 24);
        if (G.cm0 && G.tset0 && G.cabox0) {
            for (i = 0; i < G.ed.ncm; i++) { memcpy(G.cm0[i], G.ed.cm[i].head, 88); G.tset0[i] = G.ed.cm[i].tset; }
            G.tset_own = (int *)malloc(sizeof(int) * (size_t)(G.ed.ncm + 1));
            if (G.tset_own) for (i = 0; i < G.ed.ncm; i++) G.tset_own[i] = -1;
            for (i = 0; i < G.ed.nca; i++) memcpy(G.cabox0[i], G.ed.ca[i].box, 24);
        } else { free(G.cm0); free(G.tset0); free(G.cabox0); G.cm0 = NULL; G.tset0 = NULL; G.cabox0 = NULL; }
    }
    make_mips();
    G.vis = G.see = G.prop = 1;
    G.shadow = 0;
    G.shadows = 1;                              /* m26e: cast on the surfaces, as in the game */
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
    {
        int k;
        for (k = 0; k < G.nundo; k++) { free(G.undo[k].r); free(G.undo[k].rm); free(G.undo[k].pc); free(G.undo[k].prm); }
        free(G.ctm); free(G.cm0); free(G.tset0); free(G.tset_own); free(G.cabox0); free(G.pyaw_saved); free(G.rmap);
        G.tset_own = NULL;
        if (G.list0) for (k = 0; k < G.nlists0; k++) free(G.list0[k]);
        free(G.list0); free(G.nlist0);
        G.rmap = NULL; G.list0 = NULL; G.nlist0 = NULL;
        free(G.pset); free(G.inset); G.pset = NULL; G.inset = NULL; G.npset = G.cpset = 0;
        G.ctm = NULL; G.cm0 = NULL; G.tset0 = NULL; G.cabox0 = NULL; G.pyaw_saved = NULL;
        G.nundo = 0;
    }
    if (G.ed_ok) jsb_free(&G.ed);
    G.ed_ok = 0;
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
    G.shadow = !!strchr(l, 's');
    G.shadows = !strchr(l, 'n');   /* n: no shadows cast */
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
    /* m26f: cut at the near plane and at a band around the screen's edges
     * (four times its size). Before, only the near plane: a triangle close
     * by reached screen coordinates past 1e5 and was dropped whole, so
     * surfaces flickered out up close (and the ground showed the sky), and a
     * shadow volume's face dropped that way left its count off (Miggas) */
    CV poly[2][12];
    int n = 3, i, k, level = 0, pl, cur = 0;
    float gx = s_cx * 4.0f / s_focal, gy = s_cy * 4.0f / s_focal;
    for (i = 0; i < 3; i++) poly[0][i] = in[i];
    for (pl = 0; pl < 5 && n >= 3; pl++) {
        const CV *src = poly[cur];
        CV *dst = poly[cur ^ 1];
        int m = 0;
        for (i = 0; i < n; i++) {
            const CV *a = &src[i], *c = &src[(i + 1) % n];
            /* d >= 0: inside this plane */
            float da = pl == 0 ? a->z - NEAR_Z : pl == 1 ? gx * a->z - a->x : pl == 2 ? gx * a->z + a->x
                     : pl == 3 ? gy * a->z - a->y : gy * a->z + a->y;
            float dc = pl == 0 ? c->z - NEAR_Z : pl == 1 ? gx * c->z - c->x : pl == 2 ? gx * c->z + c->x
                     : pl == 3 ? gy * c->z - c->y : gy * c->z + c->y;
            if (da >= 0) dst[m++] = *a;
            if ((da >= 0) != (dc >= 0) && m < 12) {
                float t = da / (da - dc);
                CV q;
                q.x = a->x + (c->x - a->x) * t; q.y = a->y + (c->y - a->y) * t; q.z = a->z + (c->z - a->z) * t;
                q.u = a->u + (c->u - a->u) * t; q.v = a->v + (c->v - a->v) * t;
                if (pl == 0) q.z = NEAR_Z;
                dst[m++] = q;
            }
            if (m >= 11) break;
        }
        n = m;
        cur ^= 1;
    }
    if (n < 3) return;
    {
        float sx[12], sy[12], iz[12], minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
        for (i = 0; i < n; i++) {
            iz[i] = 1.0f / poly[cur][i].z;
            sx[i] = s_cx + poly[cur][i].x * s_focal * iz[i];
            sy[i] = s_cy - poly[cur][i].y * s_focal * iz[i];
            if (sx[i] < minx) minx = sx[i];
            if (sx[i] > maxx) maxx = sx[i];
            if (sy[i] < miny) miny = sy[i];
            if (sy[i] > maxy) maxy = sy[i];
        }
        if (maxx < 0 || minx >= (float)G.rw || maxy < 0 || miny >= (float)G.rh) return;
        if (mip && mip->n) {                    /* the level: texels per pixel */
            /* m26f: the whole cut polygon's areas (its first three corners can be a sliver) */
            float sa = 0, ta = 0;
            for (i = 0; i < n; i++) {
                int j = (i + 1) % n;
                sa += sx[i] * sy[j] - sx[j] * sy[i];
                ta += poly[cur][i].u * poly[cur][j].v - poly[cur][j].u * poly[cur][i].v;
            }
            sa = fabsf(sa);
            ta = fabsf(ta) * (float)mip->w[0] * (float)mip->h[0];
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
                t->u[i] = poly[cur][vi[i]].u * iz[vi[i]]; t->v[i] = poly[cur][vi[i]].v * iz[vi[i]];
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
    int i, pass;
    /* pass 0: the surfaces; 1: the shadow volumes, counted behind them
     * (m26f, z-fail: a pixel left counted is in the shadow, and is
     * darkened; counting in front went wrong with the camera inside a
     * volume, whose near faces are cut away); 2: what is laid over (the collision) */
    for (pass = 0; pass < 3; pass++) {
    if (pass == 2 && G.shadow_on_surf) {
        int x, y;
        for (y = bd->y0; y < bd->y1; y++)
            for (x = 0; x < G.rw; x++) {
                size_t pi = (size_t)y * G.rw + x;
                if (G.sb[pi]) {
                    uint8_t *o = G.fb + pi * 4;
                    o[0] = (uint8_t)(o[0] * 11 / 20); o[1] = (uint8_t)(o[1] * 11 / 20); o[2] = (uint8_t)(o[2] * 12 / 20);
                    G.sb[pi] = 0;
                }
            }
    }
    for (i = 0; i < G.nst; i++) {
        const STri *t = &G.st[i];
        float area, inv, minx, maxx;
        int x0, x1, y0, y1, x, y, sgn;
        if ((pass == 0) != (t->mode <= 1) || (pass == 1) != (t->mode == 3) || (pass == 2) != (t->mode == 2)) continue;
        if (t->maxy < (float)bd->y0 || t->miny >= (float)bd->y1) continue;
        area = (t->x[1] - t->x[0]) * (t->y[2] - t->y[0]) - (t->y[1] - t->y[0]) * (t->x[2] - t->x[0]);
        if (fabsf(area) < 1e-8f) continue;
        sgn = area > 0 ? 1 : -1;
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
                if (t->mode == 3) { if (iz < G.zb[pi]) G.sb[pi] = (int8_t)(G.sb[pi] + sgn); continue; }   /* m26f: faces behind the surface (z-fail): right with the camera inside a volume too */
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
            int kind = G.s.tkind[i], k, hl = 0;
            CV c[3];
            const uint32_t *ix = G.s.ix + i * 3;
            const Mip *m;
            uint32_t col = 0;
            float l;
            int cr = 0, cg = 0, cb = 0;
            if (kind == JST_T_GONE) continue;          /* m28: deleted in the editor */
            {                                           /* m29: a removed piece's; the picked one stands out */
                int pk = G.s.tpiece ? G.s.tpiece[i] : -1;
                if (pk >= 0 && G.s.pieces[pk].removed) continue;
                hl = pk >= 0 && G.inset && G.inset[pk];
            }
            if ((kind == JST_T_STAGE && !G.vis) || (kind == JST_T_SEE && !G.see) || (kind == JST_T_PROP && !G.prop)
                || (kind == JST_T_SHADOW && !G.shadow && !G.shadows)) continue;
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
                if (hl || (G.sel >= 0 && i >= G.s.props[G.sel].t0 && i < G.s.props[G.sel].t0 + G.s.props[G.sel].nt)) {
                    rr = rr / 2 + 150; gg = gg / 2 + 110; bb = bb / 3 + 20;   /* m28: the picked prop, orange */
                }
                emit(c, m, (uint8_t)(rr > 255 ? 255 : rr), (uint8_t)(gg > 255 ? 255 : gg), (uint8_t)(bb > 255 ? 255 : bb),
                     kind == JST_T_STAGE ? 0 : 1, ID_OF(kind == JST_T_STAGE ? ID_STAGE : kind == JST_T_SEE ? ID_SEE : ID_PROP, i));
            } else if (kind == JST_T_SHADOW) {      /* m26e: cast on the surfaces; and/or the volumes laid over, dark */
                if (G.shadows) emit(c, NULL, 0, 0, 0, 3, 0);
                if (G.shadow) emit(c, NULL, hl ? 230 : 20, hl ? 150 : 20, hl ? 50 : 30, 2, ID_OF(ID_SHADOW, i));
            }
            else if (hl)
                emit(c, NULL, (uint8_t)(230 * l), (uint8_t)(160 * l), (uint8_t)(60 * l), 0,
                     ID_OF(kind == JST_T_STAGE ? ID_STAGE : kind == JST_T_SEE ? ID_SEE : ID_PROP, i));
            else
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
                {                                       /* m29: a removed piece's collision is gone */
                    int cmi = G.ctm ? G.ctm[i] : -1, pk = cmi >= 0 && G.s.cpiece ? G.s.cpiece[cmi] : -1;
                    if (pk >= 0 && G.s.pieces[pk].removed) continue;
                    if (pk >= 0 && G.inset && G.inset[pk]) {
                        for (k = 0; k < 3; k++) { const float *p = G.cv + ix[k] * 3; c[k].x = p[0]; c[k].y = p[1]; c[k].z = p[2]; c[k].u = c[k].v = 0; }
                        if (!(c[0].z < NEAR_Z && c[1].z < NEAR_Z && c[2].z < NEAR_Z))
                            emit(c, NULL, 255, 140, 40, over ? 2 : 0, ID_OF(ID_COLL, i));
                        continue;
                    }
                }
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
    G.shadow_on_surf = G.shadows && G.s.nshadow && G.sb;
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
                if (G.s.rpiece && G.s.rpiece[k] >= 0 && G.s.pieces[G.s.rpiece[k]].removed) break;   /* m29 */
                const float *a = G.s.rp + (G.s.rs[k] + q) * 3;
                if (G.inset && G.s.rpiece && G.s.rpiece[k] >= 0 && G.inset[G.s.rpiece[k]])
                    line3(a, a + 3, f, r, u, 255, 110, 20, thick + 1);   /* m29e: going with what's picked */
                else line3(a, a + 3, f, r, u, 255, 196, 30, thick);
            }
    }
    if (obj_ok()) {                                /* m28: the picked prop's box (m29: or piece's) */
        float lo[3], hi[3], c[8][3];
        static const int e[12][2] = { {0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7} };
        int q;
        obj_box(lo, hi);
        for (q = 0; q < 8; q++) { c[q][0] = q & 1 ? hi[0] : lo[0]; c[q][1] = q & 2 ? hi[1] : lo[1]; c[q][2] = q & 4 ? hi[2] : lo[2]; }
        for (q = 0; q < 12; q++) line3(c[e[q][0]], c[e[q][1]], f, r, u, 80, 230, 255, G.rw >= 900 ? 2 : 1);
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


/* ---- m28: the editor (stage editor, level 3): props picked, moved,
 * turned, copied, deleted, with undo; saved through the rebuild (m27) ---- */

static void ed_snap(void)                       /* the props as they are, before a change */
{
    struct Undo *u;
    int k;
    if (G.nundo == 64) {                        /* the oldest goes */
        free(G.undo[0].r); free(G.undo[0].rm); free(G.undo[0].pc); free(G.undo[0].prm);
        memmove(&G.undo[0], &G.undo[1], sizeof G.undo[0] * 63);
        G.nundo--;
    }
    u = &G.undo[G.nundo];
    u->n = G.s.nprop;
    u->r = (float (*)[12])malloc(sizeof(float) * 12 * (size_t)(u->n + 1));
    u->rm = (uint8_t *)malloc((size_t)u->n + 1);
    u->np = G.s.npiece;
    u->pc = (float (*)[4])malloc(sizeof(float) * 4 * (size_t)(u->np + 1));
    u->prm = (uint8_t *)malloc((size_t)u->np + 1);
    if (!u->r || !u->rm || !u->pc || !u->prm) { free(u->r); free(u->rm); free(u->pc); free(u->prm); return; }
    for (k = 0; k < u->n; k++) { memcpy(u->r[k], G.s.props[k].r, sizeof u->r[k]); u->rm[k] = (uint8_t)G.s.props[k].removed; }
    for (k = 0; k < u->np; k++) {
        const JSPiece *pc = &G.s.pieces[k];
        u->pc[k][0] = pc->yaw; u->pc[k][1] = pc->t3[0]; u->pc[k][2] = pc->t3[1]; u->pc[k][3] = pc->t3[2];
        u->prm[k] = (uint8_t)pc->removed;
    }
    G.nundo++;
}

static void ed_changed(int k)
{
    jst_prop_update(&G.s, k);
    G.dirty = 1;
    G.leave_ask = 0;
    G.need = 1;
}

static int ed_undo(void)
{
    struct Undo *u;
    int k;
    if (!G.nundo) return 0;
    u = &G.undo[--G.nundo];
    for (k = 0; k < G.s.nprop; k++) {
        JSProp *pp = &G.s.props[k];
        if (k < u->n) { memcpy(pp->r, u->r[k], sizeof pp->r); pp->removed = u->rm[k]; }
        else pp->removed = 1;                   /* a copy made after: gone again */
        jst_prop_update(&G.s, k);
    }
    for (k = 0; k < G.s.npiece && k < u->np; k++) {     /* m29: the pieces that differ */
        JSPiece *pc = &G.s.pieces[k];
        if (pc->yaw != u->pc[k][0] || pc->t3[0] != u->pc[k][1] || pc->t3[1] != u->pc[k][2] || pc->t3[2] != u->pc[k][3]
                || pc->removed != u->prm[k]) {
            pc->yaw = u->pc[k][0]; pc->t3[0] = u->pc[k][1]; pc->t3[1] = u->pc[k][2]; pc->t3[2] = u->pc[k][3];
            pc->removed = u->prm[k];
            pc->touched = 1;
            jst_piece_update(&G.s, k);
        }
    }
    free(u->r); free(u->rm); free(u->pc); free(u->prm);
    if (G.sel >= 0 && G.s.props[G.sel].removed) G.sel = -1;
    pset_clean();
    G.grab = 0;
    G.dirty = 1;
    G.need = 1;
    return 1;
}

static void ed_turn(int k, float deg)           /* about the world's up, round its own place */
{
    JSProp *pp = &G.s.props[k];
    float a = deg * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
    int i;
    for (i = 0; i < 3; i++) {
        float x = pp->r[i * 3], z = pp->r[i * 3 + 2];
        pp->r[i * 3] = x * c + z * sn;
        pp->r[i * 3 + 2] = -x * sn + z * c;
    }
    ed_changed(k);
}

static float ed_heading(int k)                  /* the way its x row points, degrees */
{
    const float *r = G.s.props[k].r;
    return atan2f(-r[2], r[0]) * 180.0f / 3.14159265f;
}

/* the point on the level y under the mouse (mx, my in the view's pixels); 0 found */
static int ed_ray_y(float mx, float my, float y, float out[3])
{
    float f[3], r[3], u[3], d[3], foc = (float)G.th * 0.5f / tanf(0.6f), t;
    int k;
    basis(f, r, u);
    for (k = 0; k < 3; k++) d[k] = f[k] + r[k] * ((mx - (float)G.tw * 0.5f) / foc) - u[k] * ((my - (float)G.th * 0.5f) / foc);
    if (fabsf(d[1]) < 1e-5f) return -1;
    t = (y - G.pos[1]) / d[1];
    if (t <= 0 || t > 1e6f) return -1;
    for (k = 0; k < 3; k++) out[k] = G.pos[k] + d[k] * t;
    return 0;
}

static void ed_box(int k, float lo[3], float hi[3])
{
    const JSProp *pp = &G.s.props[k];
    int i, q;
    for (q = 0; q < 3; q++) { lo[q] = 1e30f; hi[q] = -1e30f; }
    for (i = 0; i < pp->nv; i++)
        for (q = 0; q < 3; q++) {
            float v = G.s.p[(size_t)(pp->v0 + i) * 3 + q];
            if (v < lo[q]) lo[q] = v;
            if (v > hi[q]) hi[q] = v;
        }
}

static void ed_delete(int k)
{
    ed_snap();
    G.s.props[k].removed = 1;
    ed_changed(k);
    if (G.sel == k) G.sel = -1;
    G.grab = 0;
}

static int ed_copy(int k)                       /* a copy beside it, picked and grabbed */
{
    int n, nv0 = G.s.nv;
    float lo[3], hi[3];
    ed_snap();
    n = jst_prop_dup(&G.s, k);
    if (n < 0) return -1;
    if (G.s.nv > nv0) {                         /* the camera-space buffer grows with it */
        float *vv = (float *)realloc(G.vv, sizeof(float) * 3 * (size_t)(G.s.nv + 1));
        if (vv) G.vv = vv;
    }
    ed_box(k, lo, hi);
    G.s.props[n].r[9] += (hi[0] - lo[0]) + 2.0f;   /* beside the first */
    ed_changed(n);
    G.sel = n;
    return n;
}


/* ---- m29: pieces of the map, and one way to edit a prop or a piece ---- */

static void piece_changed(int k)
{
    JSPiece *pc = &G.s.pieces[k];
    pc->touched = 1;
    jst_piece_update(&G.s, k);
    G.dirty = 1;
    G.leave_ask = 0;
    G.need = 1;
}

/* m29b: what lies inside the picked pieces together goes with them: a
 * collision model or rail (as read) inside their boxes taken together, not
 * yet with one of them, is given to the first one, when none of them has
 * moved yet (Miggas: a platform's ramp kept its collision and its rails
 * where they were; each was bigger than any one of the platform's pieces) */
static void pset_claim(void)
{
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    int j, i, q, to;
    if (G.npset < 1) return;
    for (j = 0; j < G.npset; j++) {
        const JSPiece *pc = &G.s.pieces[G.pset[j]];
        if (pc->touched && (pc->yaw != 0 || pc->t3[0] != 0 || pc->t3[1] != 0 || pc->t3[2] != 0)) return;
        for (q = 0; q < 3; q++) { if (pc->lo[q] < lo[q]) lo[q] = pc->lo[q]; if (pc->hi[q] > hi[q]) hi[q] = pc->hi[q]; }
    }
    to = G.pset[0];
    for (i = 0; i < G.s.ncmodel; i++) {
        const JSCModel *cm = &G.s.cmodels[i];
        float clo[3] = { 1e30f, 1e30f, 1e30f }, chi[3] = { -1e30f, -1e30f, -1e30f };
        int k, owner = G.s.cpiece[i];
        if (cm->nv <= 0 || (owner >= 0 && G.inset[owner])) continue;
        if (owner >= 0 && G.s.pieces[owner].touched) continue;
        for (k = 0; k < cm->nv; k++)
            for (q = 0; q < 3; q++) {
                float v = G.s.cp0[(size_t)(cm->v0 + k) * 3 + q];
                if (v < clo[q]) clo[q] = v;
                if (v > chi[q]) chi[q] = v;
            }
        for (q = 0; q < 3; q++) if (clo[q] < lo[q] - 1.0f || chi[q] > hi[q] + 1.0f) break;
        if (q < 3) continue;
        if (owner >= 0) {                        /* out of its old piece's list */
            JSPiece *op = &G.s.pieces[owner];
            for (k = 0; k < op->ncm; k++) if (op->cm[k] == i) { op->cm[k] = op->cm[--op->ncm]; break; }
        }
        {
            JSPiece *tp = &G.s.pieces[to];
            int *nc = (int *)realloc(tp->cm, sizeof(int) * (size_t)(tp->ncm + 1));
            if (!nc) continue;
            tp->cm = nc;
            tp->cm[tp->ncm++] = i;
            G.s.cpiece[i] = to;
        }
    }
    for (i = 0; i < G.s.nrail && G.s.rpiece; i++) {
        float rlo[3] = { 1e30f, 1e30f, 1e30f }, rhi[3] = { -1e30f, -1e30f, -1e30f };
        int k, owner = G.s.rpiece[i];
        if (owner >= 0 && (G.inset[owner] || G.s.pieces[owner].touched)) continue;
        for (k = 0; k < G.s.rn[i]; k++)
            for (q = 0; q < 3; q++) {
                float v = G.s.rp0[(size_t)(G.s.rs[i] + k) * 3 + q];
                if (v < rlo[q]) rlo[q] = v;
                if (v > rhi[q]) rhi[q] = v;
            }
        /* m29e: a rail only within one of the pieces' own boxes (not anywhere in their joint box:
         * Miggas: rails beside a moved platform went with it, away from their bars) */
        {
            int j2, in = 0;
            for (j2 = 0; j2 < G.npset && !in; j2++) {
                const JSPiece *sp = &G.s.pieces[G.pset[j2]];
                for (q = 0; q < 3; q++) if (rlo[q] < sp->lo[q] - 1.0f || rhi[q] > sp->hi[q] + 1.0f) break;
                if (q == 3) in = 1;
            }
            if (!in) continue;
        }
        if (owner >= 0) {
            JSPiece *op = &G.s.pieces[owner];
            for (k = 0; k < op->nrail; k++) if (op->rail[k] == i) { op->rail[k] = op->rail[--op->nrail]; break; }
        }
        {
            JSPiece *tp = &G.s.pieces[to];
            int *nr = (int *)realloc(tp->rail, sizeof(int) * (size_t)(tp->nrail + 1));
            if (!nr) continue;
            tp->rail = nr;
            tp->rail[tp->nrail++] = i;
            G.s.rpiece[i] = to;
        }
    }
}

/* the pieces picked: a set (m29) */
static void pset_clear(void)
{
    int i;
    for (i = 0; i < G.npset; i++) if (G.inset) G.inset[G.pset[i]] = 0;
    G.npset = 0;
    G.psel = -1;
}
static void pset_add(int k)
{
    if (k < 0 || k >= G.s.npiece || !G.inset || G.inset[k] || G.s.pieces[k].removed) return;
    if (G.npset == G.cpset) {
        int nc = G.cpset ? G.cpset * 2 : 64, *np = (int *)realloc(G.pset, sizeof(int) * (size_t)nc);
        if (!np) return;
        G.pset = np;
        G.cpset = nc;
    }
    G.pset[G.npset++] = k;
    G.inset[k] = 1;
    G.psel = G.pset[0];
}
static void pset_remove(int k)
{
    int i;
    for (i = 0; i < G.npset; i++)
        if (G.pset[i] == k) { G.pset[i] = G.pset[--G.npset]; G.inset[k] = 0; break; }
    G.psel = G.npset ? G.pset[0] : -1;
}
static void pset_clean(void)                    /* removed ones out */
{
    int i;
    for (i = 0; i < G.npset; ) {
        if (G.s.pieces[G.pset[i]].removed) { G.inset[G.pset[i]] = 0; G.pset[i] = G.pset[--G.npset]; }
        else i++;
    }
    G.psel = G.npset ? G.pset[0] : -1;
}

/* what's picked: 1 a prop, 2 pieces, 0 nothing */
static int obj_ok(void)
{
    if (G.sel >= 0 && G.sel < G.s.nprop && !G.s.props[G.sel].removed) return 1;
    if (G.npset > 0) return 2;
    return 0;
}

static void obj_box(float lo[3], float hi[3])
{
    int q, i, j;
    if (obj_ok() == 1) { ed_box(G.sel, lo, hi); return; }
    for (q = 0; q < 3; q++) { lo[q] = 1e30f; hi[q] = -1e30f; }
    for (j = 0; j < G.npset; j++) {
        const JSPiece *pc = &G.s.pieces[G.pset[j]];
        for (i = 0; i < pc->nv; i++)
            for (q = 0; q < 3; q++) {
                float v = G.s.p[(size_t)pc->v[i] * 3 + q];
                if (v < lo[q]) lo[q] = v;
                if (v > hi[q]) hi[q] = v;
            }
    }
}

static void obj_get(float pos[3], float *yaw)
{
    int k;
    if (obj_ok() == 1) {
        for (k = 0; k < 3; k++) pos[k] = G.s.props[G.sel].r[9 + k];
        *yaw = ed_heading(G.sel);
    } else if (obj_ok() == 2) {                 /* the pieces' middle, as they are now; the first one's turn */
        float lo[3], hi[3];
        obj_box(lo, hi);
        for (k = 0; k < 3; k++) pos[k] = (lo[k] + hi[k]) * 0.5f;
        *yaw = G.s.pieces[G.pset[0]].yaw;
    } else { pos[0] = pos[1] = pos[2] = 0; *yaw = 0; }
}

static void obj_set(const float pos[3], float yaw)
{
    int k, j;
    if (obj_ok() == 1) {
        float h = ed_heading(G.sel);
        for (k = 0; k < 3; k++) G.s.props[G.sel].r[9 + k] = pos[k];
        if (yaw != h) ed_turn(G.sel, yaw - h);
        else ed_changed(G.sel);
    } else if (obj_ok() == 2) {
        float cur[3], cy, d[3], a, c, sn;
        pset_claim();                           /* m29b: before the first move */
        obj_get(cur, &cy);
        for (k = 0; k < 3; k++) d[k] = pos[k] - cur[k];
        a = (yaw - cy) * 3.14159265f / 180.0f; c = cosf(a); sn = sinf(a);
        for (j = 0; j < G.npset; j++) {
            JSPiece *pc = &G.s.pieces[G.pset[j]];
            if (yaw != cy) {                    /* turned about the pieces' middle: t' = Ra (pv + t - W) + W - pv */
                float x = pc->pv[0] + pc->t3[0] - cur[0], z = pc->pv[2] + pc->t3[2] - cur[2];
                pc->t3[0] = x * c + z * sn + cur[0] - pc->pv[0];
                pc->t3[2] = -x * sn + z * c + cur[2] - pc->pv[2];
                pc->yaw = fmodf(pc->yaw + (yaw - cy), 360.0f);
            }
            for (k = 0; k < 3; k++) pc->t3[k] += d[k];
            piece_changed(G.pset[j]);
        }
    }
}

static void obj_turn(float deg)
{
    float p[3], y;
    obj_get(p, &y);
    obj_set(p, y + deg);
}

static void obj_delete(void)
{
    int j;
    if (obj_ok() == 1) { ed_delete(G.sel); return; }
    if (obj_ok() == 2) {
        ed_snap();
        pset_claim();                           /* m29b: what's inside them goes too */
        for (j = 0; j < G.npset; j++) {
            G.s.pieces[G.pset[j]].removed = 1;
            piece_changed(G.pset[j]);
        }
        pset_clear();
        G.grab = 0;
    }
}

static void piece_box_now(int k, float lo[3], float hi[3])
{
    const JSPiece *pc = &G.s.pieces[k];
    int i, q;
    for (q = 0; q < 3; q++) { lo[q] = 1e30f; hi[q] = -1e30f; }
    for (i = 0; i < pc->nv; i++)
        for (q = 0; q < 3; q++) {
            float v = G.s.p[(size_t)pc->v[i] * 3 + q];
            if (v < lo[q]) lo[q] = v;
            if (v > hi[q]) hi[q] = v;
        }
}

/* the parts touching the picked pieces (at most four times as big as all of
 * them together: the ground and the big walls stay out); pressed again, the
 * next ring */
static int obj_add_touching(void)
{
    float lo[3], hi[3], vol;
    int k, q, j, n = 0, *add;
    if (obj_ok() != 2) return 0;
    obj_box(lo, hi);
    vol = (hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1);
    add = (int *)malloc(sizeof(int) * (size_t)(G.s.npiece + 1));
    if (!add) return 0;
    for (k = 0; k < G.s.npiece; k++) {
        float plo[3], phi[3];
        if (G.inset[k] || G.s.pieces[k].removed) continue;
        if (!G.shadow && G.s.pieces[k].nt && G.s.tkind[G.s.pieces[k].t[0]] == JST_T_SHADOW) continue;   /* shadows only when shown */
        piece_box_now(k, plo, phi);
        for (q = 0; q < 3; q++) if (phi[q] < lo[q] - 1.0f || plo[q] > hi[q] + 1.0f) break;
        if (q < 3) continue;
        if ((phi[0] - plo[0] + 1) * (phi[1] - plo[1] + 1) * (phi[2] - plo[2] + 1) > vol * 4.0f) continue;
        for (j = 0; j < G.npset; j++) {          /* touching one of them, not only the whole box */
            float slo[3], shi[3];
            piece_box_now(G.pset[j], slo, shi);
            for (q = 0; q < 3; q++) if (phi[q] < slo[q] - 1.0f || plo[q] > shi[q] + 1.0f) break;
            if (q == 3) break;
        }
        if (j < G.npset) add[n++] = k;
    }
    for (j = 0; j < n; j++) pset_add(add[j]);
    free(add);
    pset_claim();
    G.need = 1;
    return n;
}

static void obj_add_inside(void)                /* the parts inside the picked pieces' box */
{
    float lo[3], hi[3];
    int k, q, n = 0;
    if (obj_ok() != 2) return;
    obj_box(lo, hi);
    for (k = 0; k < G.s.npiece; k++) {
        const JSPiece *pc = &G.s.pieces[k];
        float plo[3], phi[3];
        int i;
        if (G.inset[k] || pc->removed) continue;
        for (q = 0; q < 3; q++) { plo[q] = 1e30f; phi[q] = -1e30f; }
        for (i = 0; i < pc->nv; i++)
            for (q = 0; q < 3; q++) {
                float v = G.s.p[(size_t)pc->v[i] * 3 + q];
                if (v < plo[q]) plo[q] = v;
                if (v > phi[q]) phi[q] = v;
            }
        for (q = 0; q < 3; q++) if (plo[q] < lo[q] - 1.0f || phi[q] > hi[q] + 1.0f) break;
        if (q == 3) { pset_add(k); n++; }
    }
    pset_claim();
    G.need = 1;
}

static void obj_unpick(void) { G.sel = -1; pset_clear(); G.grab = 0; G.need = 1; }

static void obj_grab(void)
{
    if (!obj_ok() || G.grab) return;
    ed_snap();
    obj_get(G.grab_pos, &G.grab_yaw);
    G.grab = 1;
}

static void obj_ungrab(void)                    /* cancelled: back where it was */
{
    if (!G.grab) return;
    obj_set(G.grab_pos, G.grab_yaw);
    if (G.nundo) { G.nundo--; free(G.undo[G.nundo].r); free(G.undo[G.nundo].rm); free(G.undo[G.nundo].pc); free(G.undo[G.nundo].prm); }
    G.grab = 0;
}

static const char *kind_name(int k)
{
    const JSPiece *pc = &G.s.pieces[k];
    const JSModel *md = &G.s.models[pc->model];
    int t = pc->nt ? pc->t[0] : -1;
    if (t >= 0 && G.s.tkind[t] == JST_T_SHADOW) return "shadow volume";
    return md->kind == 2 ? "see-through part" : "map piece";
}

/* the pieces' edits into the .dat files (vertex positions, normals, the
 * spheres the game culls by) and the collision moved with them into G.ed;
 * the .dat files written by the saver. 0 done */
static int pieces_save(int *files, int *pieces_n, char *msg, size_t msgn)
{
    int m, any = 0, k;
    uint8_t *buf[64] = { 0 };
    size_t bn[64] = { 0 };
    int dirtyf[64] = { 0 };
    *files = 0; *pieces_n = 0;
    for (k = 0; k < G.s.npiece; k++) if (G.s.pieces[k].touched) { any = 1; (*pieces_n)++; }
    if (!any) return 0;
    for (m = 0; m < G.s.nmodel; m++) {
        const JSModel *md = &G.s.models[m];
        uint8_t *b, *D, *base, *mh;
        uint32_t vo, vc, fvf, vs, io, ic, strip, go, gc, g, i;
        int mt = 0;
        if (md->dat < 0 || md->dat >= 64) continue;
        for (k = 0; k < G.s.npiece && !mt; k++) if (G.s.pieces[k].model == m && G.s.pieces[k].touched) mt = 1;
        if (!mt) continue;
        if (!buf[md->dat]) {
            char p[2048];
            pjoin(p, G.dir, G.s.datfile[md->dat]);
            buf[md->dat] = (uint8_t *)read_file(p, &bn[md->dat]);
            if (!buf[md->dat]) { snprintf(msg, msgn, "Couldn't read %s.", G.s.datfile[md->dat]); goto fail; }
        }
        b = buf[md->dat];
        if (md->doff + 0x90 > bn[md->dat]) continue;
        D = b + md->doff; base = D + 0x10;
#define U32(x) ((uint32_t)(x)[0] | (uint32_t)(x)[1] << 8 | (uint32_t)(x)[2] << 16 | (uint32_t)(x)[3] << 24)
        mh = base + U32(D + 0x10);
        vo = U32(mh); vc = U32(mh + 4); fvf = U32(mh + 8); vs = U32(mh + 12);
        io = U32(mh + 16); ic = U32(mh + 20); strip = U32(mh + 24);
        go = U32(D + 0x80); gc = U32(D + 0x84);
        if ((int)vc != md->nv) continue;           /* not as read: leave it */
        for (i = 0; i < vc; i++) {
            int gv = md->v0 + (int)i, pk = G.s.vpiece[gv];
            uint8_t *v = base + vo + (size_t)i * vs;
            float pos[3];
            if (pk < 0 || !G.s.pieces[pk].touched) continue;
            if (G.s.pieces[pk].removed) {          /* gone: every corner at one point, nothing to see */
                for (k = 0; k < 3; k++) pos[k] = G.s.pieces[pk].c[k];
            } else memcpy(pos, G.s.p + (size_t)gv * 3, 12);
            memcpy(v, pos, 12);
            if (fvf & 0x10) {                      /* its normal turned as much as it was since the last save */
                float a = (G.s.pieces[pk].yaw - G.pyaw_saved[pk]) * 3.14159265f / 180.0f, n[3];
                memcpy(n, v + 12, 12);
                if (a != 0) {
                    float c = cosf(a), sn = sinf(a), x = n[0], z = n[2];
                    n[0] = x * c + z * sn; n[2] = -x * sn + z * c;
                    memcpy(v + 12, n, 12);
                }
            }
        }
        /* the spheres: each group's that has a piece changed, and the model's */
        for (g = 0; g < gc; g++) {
            uint8_t *e = base + go + (size_t)g * 32;
            uint32_t cnt = U32(e), first = U32(e + 4), so = U32(e + 28), n = strip ? cnt + 2 : cnt * 3, q;
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, c[3], r2 = 0;
            int touched = 0;
            if ((size_t)(io + (first + n) * 2) > (size_t)(bn[md->dat] - (size_t)(base - b))) continue;
            for (q = 0; q < n; q++) {
                uint32_t vi = (uint32_t)base[io + (first + q) * 2] | (uint32_t)base[io + (first + q) * 2 + 1] << 8;
                int pk;
                const float *pp;
                if (vi >= vc) continue;
                pk = G.s.vpiece[md->v0 + (int)vi];
                if (pk >= 0 && G.s.pieces[pk].touched) touched = 1;
                if (pk >= 0 && G.s.pieces[pk].removed) continue;
                pp = G.s.p + (size_t)(md->v0 + (int)vi) * 3;
                for (k = 0; k < 3; k++) { if (pp[k] < lo[k]) lo[k] = pp[k]; if (pp[k] > hi[k]) hi[k] = pp[k]; }
            }
            if (!touched || lo[0] > hi[0]) continue;
            for (k = 0; k < 3; k++) c[k] = (lo[k] + hi[k]) * 0.5f;
            for (q = 0; q < n; q++) {
                uint32_t vi = (uint32_t)base[io + (first + q) * 2] | (uint32_t)base[io + (first + q) * 2 + 1] << 8;
                const float *pp;
                float d2;
                if (vi >= vc) continue;
                pp = G.s.p + (size_t)(md->v0 + (int)vi) * 3;
                d2 = (pp[0] - c[0]) * (pp[0] - c[0]) + (pp[1] - c[1]) * (pp[1] - c[1]) + (pp[2] - c[2]) * (pp[2] - c[2]);
                if (d2 > r2) r2 = d2;
            }
            {
                float sp[4] = { c[0], c[1], c[2], sqrtf(r2) * 1.001f + 0.01f };
                if (so + 16 <= (uint32_t)(bn[md->dat] - (size_t)(base - b))) memcpy(base + so, sp, 16);
            }
        }
        {
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, c[3], r2 = 0, sp[4];
            for (i = 0; i < vc; i++) {
                const float *pp = G.s.p + (size_t)(md->v0 + (int)i) * 3;
                for (k = 0; k < 3; k++) { if (pp[k] < lo[k]) lo[k] = pp[k]; if (pp[k] > hi[k]) hi[k] = pp[k]; }
            }
            for (k = 0; k < 3; k++) c[k] = (lo[k] + hi[k]) * 0.5f;
            for (i = 0; i < vc; i++) {
                const float *pp = G.s.p + (size_t)(md->v0 + (int)i) * 3;
                float d2 = (pp[0] - c[0]) * (pp[0] - c[0]) + (pp[1] - c[1]) * (pp[1] - c[1]) + (pp[2] - c[2]) * (pp[2] - c[2]);
                if (d2 > r2) r2 = d2;
            }
            sp[0] = c[0]; sp[1] = c[1]; sp[2] = c[2]; sp[3] = sqrtf(r2) * 1.001f + 0.01f;
            memcpy(D + 0x20, sp, 16);
        }
#undef U32
        dirtyf[md->dat] = 1;
    }
    /* the collision moved with them */
    if (G.cm0) {
        int empty = -1, i;
        for (i = 0; i < G.ed.nts; i++) if (!G.ed.ts[i].n) empty = i;
        for (k = 0; k < G.s.npiece; k++) {
            const JSPiece *pc = &G.s.pieces[k];
            float a = pc->yaw * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
            int j;
            if (!pc->touched) continue;
            for (j = 0; j < pc->ncm; j++) {
                int ci = pc->cm[j], q, r;
                JCModel *cm = &G.ed.cm[ci];
                const JSCModel *cr = &G.s.cmodels[ci];
                float head[22], lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
                memcpy(head, G.cm0[ci], 88);
                for (r = 0; r < 3; r++) {          /* the rows turned */
                    float x = head[6 + r * 4], z = head[6 + r * 4 + 2];
                    head[6 + r * 4] = x * c + z * sn;
                    head[6 + r * 4 + 2] = -x * sn + z * c;
                }
                {                                  /* the position turned about the piece's middle, then moved */
                    float dx = head[18] - pc->pv[0], dz = head[20] - pc->pv[2];
                    head[18] = pc->pv[0] + dx * c + dz * sn + pc->t3[0];
                    head[19] += pc->t3[1];
                    head[20] = pc->pv[2] - dx * sn + dz * c + pc->t3[2];
                }
                for (q = 0; q < cr->nv; q++)
                    for (r = 0; r < 3; r++) {
                        float v = G.s.cp[(size_t)(cr->v0 + q) * 3 + r];
                        if (v < lo[r]) lo[r] = v;
                        if (v > hi[r]) hi[r] = v;
                    }
                if (cr->nv) for (r = 0; r < 3; r++) { head[r] = lo[r]; head[3 + r] = hi[r]; }
                memcpy(cm->head, head, 88);
                if (pc->removed) {                 /* gone: no triangles left to stand on */
                    if (empty < 0) {
                        JBytes *nt = (JBytes *)realloc(G.ed.ts, sizeof(JBytes) * (size_t)(G.ed.nts + 1));
                        if (nt) { G.ed.ts = nt; nt[G.ed.nts].p = (uint8_t *)malloc(1); nt[G.ed.nts].n = 0; empty = G.ed.nts++; }
                    }
                    if (empty >= 0) cm->tset = empty;
                } else {
                    cm->tset = G.tset0[ci];
                    /* m29d: each triangle notes which corner is lowest and highest on each axis, from
                     * its world place (bits 16..27 of its second word: min x, max x, min y, max y,
                     * min z, max z, two bits each, the first corner on a tie; matches every triangle
                     * of the stages read). The game builds the triangle's box from it, so a turned
                     * model's notes are worked out again (Miggas: a turned platform's floor and ramp
                     * didn't hold him). Its own copy of the triangles, so others sharing them keep theirs */
                    if ((pc->yaw != 0 || pc->t3[0] != 0 || pc->t3[1] != 0 || pc->t3[2] != 0) && G.tset_own) {
                        int own = G.tset_own[ci];
                        const JBytes *vsb = &G.ed.vs[cm->vset];
                        if (own < 0) {
                            JBytes *nt = (JBytes *)realloc(G.ed.ts, sizeof(JBytes) * (size_t)(G.ed.nts + 1));
                            if (nt) {
                                G.ed.ts = nt;
                                nt[G.ed.nts].n = G.ed.ts[G.tset0[ci]].n;
                                nt[G.ed.nts].p = (uint8_t *)malloc(nt[G.ed.nts].n + 1);
                                if (nt[G.ed.nts].p) {
                                    memcpy(nt[G.ed.nts].p, G.ed.ts[G.tset0[ci]].p, nt[G.ed.nts].n);
                                    own = G.tset_own[ci] = G.ed.nts++;
                                }
                            }
                        }
                        if (own >= 0) {
                            JBytes *tb = &G.ed.ts[own];
                            size_t t, nv = vsb->n / 16;
                            cm->tset = own;
                            memcpy(tb->p, G.ed.ts[G.tset0[ci]].p, tb->n);   /* from the original each time */
                            for (t = 0; t + 8 <= tb->n; t += 8) {
                                uint32_t w0 = tb->p[t] | tb->p[t + 1] << 8 | tb->p[t + 2] << 16 | (uint32_t)tb->p[t + 3] << 24;
                                uint32_t w1 = tb->p[t + 4] | tb->p[t + 5] << 8 | tb->p[t + 6] << 16 | (uint32_t)tb->p[t + 7] << 24;
                                float wv[3][3];
                                int cn, ax, bits = 0;
                                for (cn = 0; cn < 3; cn++) {
                                    uint32_t vi = (w0 >> (10 * cn)) & 1023u;
                                    float lv[3];
                                    if (vi >= nv) break;
                                    memcpy(lv, vsb->p + vi * 16, 12);
                                    for (ax = 0; ax < 3; ax++)   /* world = position + x row0 + y row1 + z row2 */
                                        wv[cn][ax] = (float)((double)head[18 + ax] + (double)lv[0] * head[6 + ax]
                                                             + (double)lv[1] * head[10 + ax] + (double)lv[2] * head[14 + ax]);
                                }
                                if (cn < 3) continue;
                                for (ax = 0; ax < 3; ax++) {
                                    int mn = 0, mx = 0;
                                    for (cn = 1; cn < 3; cn++) {
                                        if (wv[cn][ax] < wv[mn][ax]) mn = cn;
                                        if (wv[cn][ax] > wv[mx][ax]) mx = cn;
                                    }
                                    bits |= mn << (4 * ax) | mx << (4 * ax + 2);
                                }
                                w1 = (w1 & ~(0xFFFu << 16)) | (uint32_t)bits << 16;
                                tb->p[t + 4] = (uint8_t)w1; tb->p[t + 5] = (uint8_t)(w1 >> 8);
                                tb->p[t + 6] = (uint8_t)(w1 >> 16); tb->p[t + 7] = (uint8_t)(w1 >> 24);
                            }
                        }
                    }
                }
            }
        }
        /* each area's box as read, grown round its models as they are now */
        for (i = 0; i < G.ed.nca; i++) {
            float bx[6];
            int j, r;
            memcpy(bx, G.cabox0[i], 24);
            for (j = 0; j < G.ed.ca[i].count; j++) {
                const float *h = (const float *)G.ed.cm[G.ed.ca[i].first + j].head;
                float hb[6];
                memcpy(hb, h, 24);
                for (r = 0; r < 3; r++) { if (hb[r] < bx[r]) bx[r] = hb[r]; if (hb[3 + r] > bx[3 + r]) bx[3 + r] = hb[3 + r]; }
            }
            memcpy(G.ed.ca[i].box, bx, 24);
        }
    }
    /* the rails moved with them; a removed piece's rails taken out of the lists that find them */
    if (G.rmap && G.list0) {
        int r, i, j, q;
        for (r = 0; r < G.s.nrail; r++) {
            int pk = G.s.rpiece ? G.s.rpiece[r] : -1, ri = G.rmap[r];
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
            if (pk < 0 || ri < 0 || !G.s.pieces[pk].touched) continue;
            for (i = 0; i < G.s.rn[r] && (size_t)(i + 1) * 24 <= G.ed.rail[ri].pts.n; i++) {
                const float *pp = G.s.rp + (size_t)(G.s.rs[r] + i) * 3, *nn = G.s.rnrm + (size_t)(G.s.rs[r] + i) * 3;
                memcpy(G.ed.rail[ri].pts.p + (size_t)i * 24, pp, 12);
                memcpy(G.ed.rail[ri].pts.p + (size_t)i * 24 + 12, nn, 12);
                for (q = 0; q < 3; q++) { if (pp[q] < lo[q]) lo[q] = pp[q]; if (pp[q] > hi[q]) hi[q] = pp[q]; }
            }
            memcpy(G.ed.rail[ri].rest + 4, lo, 12);   /* its box, after its flags */
            memcpy(G.ed.rail[ri].rest + 16, hi, 12);
        }
        /* m29b: the 1024 slots are a grid of 128-unit squares (x, z; wrapping every 32): a rail is
         * listed in every square its box (a unit wider) covers, which is how the game finds what to
         * grind (Miggas: moved rails couldn't be ground). The slots are made again: as read, less the
         * rails that changed, plus those (not removed) in the squares where they are now */
        {
            int *chg = (int *)calloc((size_t)G.ed.nrail + 1, sizeof(int));   /* 1 changed, 2 removed */
            int **nl = (int **)calloc(1025, sizeof(int *)), *nn = (int *)calloc(1025, sizeof(int)), nlists = 0, sl;
            int *cap = (int *)calloc(1025, sizeof(int)), *tmp = (int *)malloc(sizeof(int) * (size_t)(G.ed.nrail + 1));
            if (chg && nl && nn && cap && tmp) {
                for (r = 0; r < G.s.nrail; r++) {
                    int pk = G.s.rpiece ? G.s.rpiece[r] : -1, ri = G.rmap[r];
                    if (pk < 0 || ri < 0 || !G.s.pieces[pk].touched) continue;
                    chg[ri] = G.s.pieces[pk].removed ? 2 : 1;
                }
                for (sl = 0; sl < 1024; sl++) {
                    int n = 0, l0 = G.slot0[sl], cx = sl & 31, cz = sl >> 5, a2, b2;
                    if (l0 >= 0) for (j = 0; j < G.nlist0[l0]; j++) if (!chg[G.list0[l0][j]]) tmp[n++] = G.list0[l0][j];
                    for (r = 0; r < G.ed.nrail; r++) {
                        float bx[6];
                        int x0, x1, z0, z1, xx, zz, in = 0;
                        if (chg[r] != 1) continue;
                        memcpy(bx, G.ed.rail[r].rest + 4, 24);
                        x0 = (int)floorf((bx[0] - 1.0f) / 128.0f); x1 = (int)floorf((bx[3] + 1.0f) / 128.0f);
                        z0 = (int)floorf((bx[2] - 1.0f) / 128.0f); z1 = (int)floorf((bx[5] + 1.0f) / 128.0f);
                        for (xx = x0; xx <= x1 && !in; xx++)
                            for (zz = z0; zz <= z1 && !in; zz++)
                                if ((xx & 31) == cx && (zz & 31) == cz) in = 1;
                        if (in) tmp[n++] = r;
                    }
                    for (a2 = 1; a2 < n; a2++)          /* in rail order, as the game's are */
                        for (b2 = a2; b2 > 0 && tmp[b2 - 1] > tmp[b2]; b2--) { int t = tmp[b2]; tmp[b2] = tmp[b2 - 1]; tmp[b2 - 1] = t; }
                    if (!n) { G.ed.slot[sl] = -1; continue; }
                    for (j = 0; j < nlists; j++)        /* the same list as another slot's: shared */
                        if (nn[j] == n && !memcmp(nl[j], tmp, sizeof(int) * (size_t)n)) break;
                    if (j == nlists) {
                        nl[j] = (int *)malloc(sizeof(int) * (size_t)n);
                        if (!nl[j]) continue;
                        memcpy(nl[j], tmp, sizeof(int) * (size_t)n);
                        nn[j] = n;
                        nlists++;
                    }
                    G.ed.slot[sl] = j;
                }
                for (j = 0; j < G.ed.nlists; j++) free(G.ed.list[j]);
                free(G.ed.list); free(G.ed.nlist);
                G.ed.list = nl; G.ed.nlist = nn; G.ed.nlists = nlists;
                nl = NULL; nn = NULL;
            }
            free(chg); free(nl); free(nn); free(cap); free(tmp);
        }
    }
    for (k = 0; k < 64; k++)
        if (dirtyf[k]) {
            if (s_saver(G.s.datfile[k], buf[k], bn[k], s_saver_u)) { snprintf(msg, msgn, "Couldn't write %s.", G.s.datfile[k]); goto fail; }
            (*files)++;
        }
    for (k = 0; k < G.s.npiece; k++) G.pyaw_saved[k] = G.s.pieces[k].yaw;
    for (k = 0; k < 64; k++) free(buf[k]);
    return 0;
fail:
    for (k = 0; k < 64; k++) free(buf[k]);
    return -1;
}

/* the edits into the .bin's placements, written by the saver; 0 done */
static int ed_save(char *msg, size_t msgn)
{
    int a, k, moved = 0, removed = 0, added = 0;
    uint8_t *w;
    size_t wn = 0;
    JBytes *npl;
    int *newe, dfiles = 0, npc = 0;
    if (!G.ed_ok || !s_saver) { snprintf(msg, msgn, "This stage's .bin couldn't be read in its parts: can't save."); return -1; }
    if (pieces_save(&dfiles, &npc, msg, msgn)) return -1;   /* m29: the map's pieces first (their collision goes in G.ed) */
    npl = (JBytes *)calloc((size_t)G.ed.nva + 1, sizeof(JBytes));
    newe = (int *)malloc(sizeof(int) * (size_t)(G.s.nprop + 1));
    if (!npl || !newe) { free(npl); free(newe); return -1; }
    for (k = 0; k < G.s.nprop; k++) newe[k] = -2;
    for (a = 0; a < G.ed.nva; a++) {
        size_t ne = G.ed.pl[a].n / 80, j;
        uint8_t *o = (uint8_t *)malloc((ne + (size_t)G.s.nprop) * 80 + 80);
        size_t on = 0;
        if (!o) { for (k = 0; k < a; k++) free(npl[k].p); free(npl); free(newe); return -1; }
        for (j = 0; j < ne; j++) {
            const uint8_t *e = G.ed.pl[a].p + j * 80;
            int pk = -1;
            for (k = 0; k < G.s.nprop; k++)
                if (G.s.props[k].area == a && G.s.props[k].entry == (int)j) { pk = k; break; }
            if (pk < 0) { memcpy(o + on, e, 80); on += 80; continue; }   /* a stage model's, as it was */
            if (G.s.props[pk].removed) { removed++; continue; }
            newe[pk] = (int)(on / 80);
            memcpy(o + on, e, 80);
            for (k = 0; k < 3; k++) memcpy(o + on + 16 * k, &G.s.props[pk].r[k * 3], 12);
            memcpy(o + on + 48, &G.s.props[pk].r[9], 12);
            if (memcmp(o + on, e, 80)) moved++;
            on += 80;
        }
        for (k = 0; k < G.s.nprop; k++) {          /* copies made here, at the end of their area's list */
            JSProp *pp = &G.s.props[k];
            int q;
            if (pp->area != a || pp->entry >= 0 || pp->removed) continue;
            newe[k] = (int)(on / 80);
            memcpy(o + on, pp->bytes, 80);
            for (q = 0; q < 3; q++) memcpy(o + on + 16 * q, &pp->r[q * 3], 12);
            memcpy(o + on + 48, &pp->r[9], 12);
            on += 80;
            added++;
        }
        npl[a].p = o;
        npl[a].n = on;
    }
    {
        JBytes *old = G.ed.pl;
        G.ed.pl = npl;
        w = jsb_write(&G.ed, 0, &wn);
        if (!w || s_saver(G.s.binfile, w, wn, s_saver_u)) {
            G.ed.pl = old;
            for (a = 0; a < G.ed.nva; a++) free(npl[a].p);
            free(npl);
            free(w);
            free(newe);
            snprintf(msg, msgn, "Couldn't write %s.", G.s.binfile);
            return -1;
        }
        for (a = 0; a < G.ed.nva; a++) free(old[a].p);
        free(old);
        free(w);
    }
    /* the props' entries as now written (a deleted one has none: -2; undone, it's added as a copy) */
    for (k = 0; k < G.s.nprop; k++) G.s.props[k].entry = newe[k];
    free(newe);
    if (npc)
        snprintf(msg, msgn, "%s saved: %d map piece%s changed (%d .dat file%s); props: %d moved, %d deleted, %d copied. The originals are in mod_backups.",
                 G.s.name, npc, npc == 1 ? "" : "s", dfiles, dfiles == 1 ? "" : "s", moved, removed, added);
    else
        snprintf(msg, msgn, "%s saved: %d prop%s moved, %d deleted, %d copied (the original in mod_backups).",
                 G.s.binfile, moved, moved == 1 ? "" : "s", removed, added);
    G.dirty = 0;
    return 0;
}

/* m28 (tests): edits by script: "move K dx dy dz; turn K deg; del K; copy K; undo; save" */
void s3d_test_edit(const char *script, char *msg, size_t msgn)
{
    char buf[512], *tok, *save = NULL;
    snprintf(buf, sizeof buf, "%s", script);
    for (tok = strtok_r(buf, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        int k = -1;
        float a = 0, b2 = 0, c = 0;
        while (*tok == ' ') tok++;
        if (sscanf(tok, "move %d %f %f %f", &k, &a, &b2, &c) == 4 && k >= 0 && k < G.s.nprop) {
            ed_snap(); G.s.props[k].r[9] += a; G.s.props[k].r[10] += b2; G.s.props[k].r[11] += c; ed_changed(k); G.sel = k;
        } else if (sscanf(tok, "turn %d %f", &k, &a) == 2 && k >= 0 && k < G.s.nprop) { ed_snap(); ed_turn(k, a); G.sel = k; }
        else if (sscanf(tok, "del %d", &k) == 1 && k >= 0 && k < G.s.nprop) ed_delete(k);
        else if (sscanf(tok, "copy %d", &k) == 1 && k >= 0 && k < G.s.nprop) ed_copy(k);
        else if (!strncmp(tok, "undo", 4)) ed_undo();
        else if (!strncmp(tok, "save", 4)) ed_save(msg, msgn);
        else if (sscanf(tok, "pick %d", &k) == 1) G.sel = k;
        else if (sscanf(tok, "pmove %d %f %f %f", &k, &a, &b2, &c) == 4 && k >= 0 && k < G.s.npiece) {
            float p[3], y; G.sel = -1; pset_clear(); pset_add(k); ed_snap(); obj_get(p, &y); p[0] += a; p[1] += b2; p[2] += c; obj_set(p, y);
        } else if (sscanf(tok, "pturn %d %f", &k, &a) == 2 && k >= 0 && k < G.s.npiece) { G.sel = -1; pset_clear(); pset_add(k); ed_snap(); obj_turn(a); }
        else if (sscanf(tok, "pdel %d", &k) == 1 && k >= 0 && k < G.s.npiece) { G.sel = -1; pset_clear(); pset_add(k); obj_delete(); }
        else if (sscanf(tok, "ppick %d", &k) == 1) { G.sel = -1; pset_clear(); pset_add(k); }
        else if (sscanf(tok, "padd %d", &k) == 1) pset_add(k);
        else if (!strncmp(tok, "inside", 6)) obj_add_inside();
        else if (!strncmp(tok, "touch", 5)) obj_add_touching();
        else if (!strncmp(tok, "near", 4)) {      /* (tests) pieces within 20 of the picked ones' box */
            float lo[3], hi[3]; int q2, kk;
            obj_box(lo, hi);
            printf("set box %.1f %.1f %.1f .. %.1f %.1f %.1f\n", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
            for (kk = 0; kk < G.s.npiece; kk++) {
                float plo[3], phi[3];
                if (G.inset[kk]) continue;
                piece_box_now(kk, plo, phi);
                for (q2 = 0; q2 < 3; q2++) if (phi[q2] < lo[q2] - 20 || plo[q2] > hi[q2] + 20) break;
                if (q2 == 3) printf("  piece %d model %d kind %d: %.1f %.1f %.1f .. %.1f %.1f %.1f\n", kk, G.s.models[G.s.pieces[kk].model].id,
                                    G.s.models[G.s.pieces[kk].model].kind, plo[0], plo[1], plo[2], phi[0], phi[1], phi[2]);
            }
        }
        else if (sscanf(tok, "gmove %f %f %f", &a, &b2, &c) == 3 && obj_ok()) { float p[3], y; ed_snap(); obj_get(p, &y); p[0] += a; p[1] += b2; p[2] += c; obj_set(p, y); }
        else if (sscanf(tok, "gturn %f", &a) == 1 && obj_ok()) { ed_snap(); obj_turn(a); }
        else if (!strncmp(tok, "count", 5)) {
            int nc = 0, nr = 0, j2;
            for (j2 = 0; j2 < G.npset; j2++) { nc += G.s.pieces[G.pset[j2]].ncm; nr += G.s.pieces[G.pset[j2]].nrail; }
            printf("picked: %d pieces, %d collision models, %d rails\n", G.npset, nc, nr);
        }
        else if (!strncmp(tok, "ramps", 5)) {     /* (tests) the pieces round the ramps' collision */
            int t2;
            for (t2 = 0; t2 < G.s.nct; t2++)
                if (G.s.csurf[t2] == 8192) {
                    int cmi = G.ctm[t2];
                    printf("ramp tri %d: collision model %d, piece %d, at %.0f %.0f %.0f\n", t2, cmi, cmi >= 0 ? G.s.cpiece[cmi] : -1,
                           G.s.cp[(size_t)G.s.cix[t2 * 3] * 3], G.s.cp[(size_t)G.s.cix[t2 * 3] * 3 + 1], G.s.cp[(size_t)G.s.cix[t2 * 3] * 3 + 2]);
                    t2 += 3;
                }
        }
        else if (!strncmp(tok, "plist", 5)) {     /* the biggest pieces with collision */
            int q;
            for (q = 0; q < G.s.npiece; q++)
                if (G.s.pieces[q].nrail > 0)
                    printf("piece %d rails %d model %d verts %d coll %d at %.0f %.0f %.0f size %.0f %.0f %.0f\n", q, G.s.pieces[q].nrail, G.s.models[G.s.pieces[q].model].id,
                           G.s.pieces[q].nv, G.s.pieces[q].ncm, G.s.pieces[q].c[0], G.s.pieces[q].c[1], G.s.pieces[q].c[2],
                           G.s.pieces[q].hi[0] - G.s.pieces[q].lo[0], G.s.pieces[q].hi[1] - G.s.pieces[q].lo[1], G.s.pieces[q].hi[2] - G.s.pieces[q].lo[2]);
        }
    }
    G.need = 1;
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
    } else if (l == ID_SHADOW) snprintf(out, n, "Shadow volume (the game darkens what is inside it)");
    else out[0] = 0;
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
        if (nk_button_label(ctx, "Back to the stages")) {
            if (G.dirty && !G.leave_ask) {       /* m28: not without asking */
                G.leave_ask = 1;
                snprintf(msg, msgn, "Unsaved changes to %s: Save them, or press Back again to leave without saving.", G.s.name);
            } else back = 1;
        }
        nk_layout_row_dynamic(ctx, row - 6, 1);
        nk_labelf(ctx, NK_TEXT_LEFT, "%s: %d .dat files", G.s.name, G.s.ndat);
        nk_labelf(ctx, NK_TEXT_LEFT, "%d stage models, %d props", G.s.nmodels, G.s.nprops);
        nk_labelf(ctx, NK_TEXT_LEFT, "%d triangles, %d textures", G.s.nt, G.s.ntex);
        if (!G.s.has_bin) nk_label_colored(ctx, "No .bin: no collision, rails, props", NK_TEXT_LEFT, nk_rgb(230, 170, 90));
        nk_layout_row_dynamic(ctx, row, 1);
        nk_label(ctx, "Show", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, row - 4, 1);
        check(ctx, "Stage models", &G.vis);
        check(ctx, "See-through parts", &G.see);
        check(ctx, "Props", &G.prop);
        if (G.s.nshadow) {
            check(ctx, "Shadows", &G.shadows);
            snprintf(t, sizeof t, "Shadow volumes (%d)", G.s.nshadow);
            check(ctx, t, &G.shadow);
        }
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
        /* m28: the editor */
        nk_layout_row_dynamic(ctx, row, 1);
        nk_label(ctx, G.dirty ? "Edit (unsaved changes)" : "Edit", NK_TEXT_LEFT);
        if (obj_ok()) {
            float v[3], hd, v0[3], hd0;
            int q, ch = 0;
            obj_get(v, &hd);
            memcpy(v0, v, sizeof v0);
            hd0 = hd;
            nk_layout_row_dynamic(ctx, row - 6, 1);
            if (obj_ok() == 1) nk_labelf(ctx, NK_TEXT_LEFT, "Prop %d: model %d, area %d", G.sel, G.s.props[G.sel].model_id, G.s.props[G.sel].area);
            else {
                const JSPiece *pc = &G.s.pieces[G.psel];
                int ncm = 0, nr = 0, j;
                for (j = 0; j < G.npset; j++) { ncm += G.s.pieces[G.pset[j]].ncm; nr += G.s.pieces[G.pset[j]].nrail; }
                if (G.npset == 1)
                    nk_labelf(ctx, NK_TEXT_LEFT, "%c%s %d (model %d)", toupper((unsigned char)kind_name(G.psel)[0]), kind_name(G.psel) + 1,
                              G.psel, G.s.models[pc->model].id);
                else nk_labelf(ctx, NK_TEXT_LEFT, "%d pieces picked together", G.npset);
                nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160), "With it: %d collision piece%s, %d rail%s",
                                  ncm, ncm == 1 ? "" : "s", nr, nr == 1 ? "" : "s");
            }
            for (q = 0; q < 3; q++) {
                static const char *nmx[3] = { "#X:", "#Y:", "#Z:" };
                nk_property_float(ctx, nmx[q], -100000.0f, &v[q], 100000.0f, 1.0f, 0.5f);
            }
            nk_property_float(ctx, "#Facing:", -360.0f, &hd, 360.0f, 15.0f, 1.0f);
            for (q = 0; q < 3; q++) ch |= v[q] != v0[q];
            if (ch || hd != hd0) { if (!G.grab) ed_snap(); obj_set(v, hd); }
            nk_layout_row_dynamic(ctx, row - 4, 2);
            if (nk_button_label(ctx, G.grab ? "Grabbed: click to drop" : "Grab (G)")) obj_grab();
            if (obj_ok() == 1) {
                if (nk_button_label(ctx, "Copy (Ctrl+D)")) ed_copy(G.sel);
            } else nk_spacing(ctx, 1);
            if (nk_button_label(ctx, "Turn -15 ([)")) { ed_snap(); obj_turn(-15); }
            if (nk_button_label(ctx, "Turn +15 (])")) { ed_snap(); obj_turn(15); }
            if (nk_button_label(ctx, "Delete (Del)")) obj_delete();
            if (nk_button_label(ctx, "Unpick (Esc)")) obj_unpick();
            if (obj_ok() == 2) {
                nk_layout_row_dynamic(ctx, row - 4, 1);
                nk_layout_row_dynamic(ctx, row - 4, 2);
                if (nk_button_label(ctx, "Add touching (T)")) obj_add_touching();
                if (nk_button_label(ctx, "Add inside (I)")) obj_add_inside();
                nk_layout_row_dynamic(ctx, row - 8, 1);
                nk_label_colored(ctx, "Shift+click adds or takes away a piece", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
            }
        } else {
            nk_layout_row_dynamic(ctx, row - 8, 1);
            nk_label_colored(ctx, "Click a prop or a piece of the map.", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        }
        nk_layout_row_dynamic(ctx, row - 4, 2);
        if (nk_button_label(ctx, G.nundo ? "Undo (Ctrl+Z)" : "Undo (nothing)")) ed_undo();
        if (nk_button_label(ctx, G.dirty ? "Save the stage" : "Saved")) { if (G.dirty) ed_save(msg, msgn); }
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
                free(G.fb); free(G.zb); free(G.idb); free(G.sb);
                G.fb = (uint8_t *)malloc((size_t)bw * bh * 4);
                G.zb = (float *)malloc(sizeof(float) * (size_t)bw * bh);
                G.idb = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)bw * bh);
                G.sb = (int8_t *)calloc((size_t)bw * bh, 1);
                G.tw = bw; G.th = bh;
                G.need = 1;
            }
            basis(f, r, u);
            if (!G.grab && nk_input_has_mouse_click_down_in_rect(in, NK_BUTTON_LEFT, b, nk_true) && (in->mouse.delta.x || in->mouse.delta.y)) {
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
            /* m28: the editor's keys and clicks */
            if (hover) {
                const Uint8 *ks = SDL_GetKeyboardState(NULL);
                int ctrl = ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL], shift = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
                int sel_ok = obj_ok() != 0;
#define KEYHIT(sc) (ks[sc] && !G.kprev[sc])
                if (ctrl && KEYHIT(SDL_SCANCODE_Z)) ed_undo();
                if (obj_ok() == 1 && ctrl && KEYHIT(SDL_SCANCODE_D)) ed_copy(G.sel);
                if (sel_ok && !ctrl && KEYHIT(SDL_SCANCODE_G)) obj_grab();
                if (sel_ok && KEYHIT(SDL_SCANCODE_DELETE)) obj_delete();
                if (obj_ok() == 2 && !ctrl && KEYHIT(SDL_SCANCODE_I)) obj_add_inside();
                if (obj_ok() == 2 && !ctrl && KEYHIT(SDL_SCANCODE_T)) obj_add_touching();
                if (sel_ok && KEYHIT(SDL_SCANCODE_LEFTBRACKET)) { ed_snap(); obj_turn(shift ? -90.0f : -15.0f); }
                if (sel_ok && KEYHIT(SDL_SCANCODE_RIGHTBRACKET)) { ed_snap(); obj_turn(shift ? 90.0f : 15.0f); }
                if (sel_ok && (KEYHIT(SDL_SCANCODE_PAGEUP) || KEYHIT(SDL_SCANCODE_PAGEDOWN))) {
                    float pp[3], yy;
                    ed_snap();
                    obj_get(pp, &yy);
                    pp[1] += (ks[SDL_SCANCODE_PAGEUP] ? 1.0f : -1.0f) * (shift ? 10.0f : 1.0f);
                    obj_set(pp, yy);
                }
                if (KEYHIT(SDL_SCANCODE_ESCAPE)) {
                    if (G.grab) obj_ungrab();
                    else obj_unpick();
                    G.need = 1;
                }
#undef KEYHIT
                memcpy(G.kprev, ks, sizeof G.kprev);
                if (G.grab && obj_ok()) {             /* it follows the mouse, at its height */
                    float pt[3], cur[3], yy;
                    obj_get(cur, &yy);
                    if (!ed_ray_y(in->mouse.pos.x - b.x, in->mouse.pos.y - b.y, cur[1], pt) && (pt[0] != cur[0] || pt[2] != cur[2])) {
                        cur[0] = pt[0];
                        cur[2] = pt[2];
                        obj_set(cur, yy);
                    }
                    if (nk_input_is_mouse_pressed(in, NK_BUTTON_RIGHT)) { obj_ungrab(); G.need = 1; }
                }
                if (nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT)) { G.press = 1; G.pmoved = 0; G.px0 = in->mouse.pos.x; G.py0 = in->mouse.pos.y; }
                if (G.press && (fabsf(in->mouse.pos.x - G.px0) > 4 || fabsf(in->mouse.pos.y - G.py0) > 4)) G.pmoved = 1;
                if (G.press && nk_input_is_mouse_released(in, NK_BUTTON_LEFT)) {
                    G.press = 0;
                    if (G.grab) G.grab = 0;          /* dropped there */
                    else if (!G.pmoved) {             /* a click: the prop or piece under it, or none */
                        uint32_t id = G.hover, l = id >> 28, ti = id & 0x0FFFFFFFu;
                        int pk = -1;
                        if ((l == ID_STAGE || l == ID_SEE || l == ID_SHADOW) && G.s.tpiece && (int)ti < G.s.nt) pk = G.s.tpiece[ti];
                        else if (l == ID_COLL && G.ctm && G.s.cpiece && (int)ti < G.s.nct && G.ctm[ti] >= 0) pk = G.s.cpiece[G.ctm[ti]];
                        if (shift && pk >= 0) {         /* m29: more pieces, or fewer */
                            G.sel = -1;
                            if (G.inset[pk]) pset_remove(pk); else pset_add(pk);
                            pset_claim();
                        } else {
                            G.sel = l == ID_PROP ? jst_prop_of_tri(&G.s, (int)ti) : -1;
                            pset_clear();
                            if (pk >= 0) pset_add(pk);
                            pset_claim();
                        }
                        G.need = 1;
                    }
                }
            } else G.press = 0;
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
