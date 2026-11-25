/* m26: a JSRF stage read whole: see jstage.h */
#include "jstage.h"
#include "jtex.h"
#include "model.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "stb_image_write.h"

const JSSurf jst_surf[JST_NSURF] = {
    { 0, "Pass through", 90, 45, 109 },
    { 1, "Floor", 65, 138, 214 },
    { 2, "Unknown (2)", 126, 147, 147 },
    { 4, "Wall", 214, 138, 65 },
    { 8, "Unknown (8)", 150, 160, 120 },
    { 16, "Stairs", 218, 225, 40 },
    { 32, "Billboard", 44, 225, 40 },
    { 128, "Half-pipe", 60, 60, 200 },
    { 256, "Ceiling", 0, 254, 232 },
    { 512, "Untouchable", 225, 40, 40 },
    { 8192, "Ramp", 205, 40, 225 },
    { 0xFFFF, "Other", 200, 200, 200 },
};

int jst_surf_index(uint16_t flag)
{
    int i;
    for (i = 0; i < JST_NSURF - 1; i++) if (jst_surf[i].flag == flag) return i;
    return JST_NSURF - 1;
}

static uint32_t u32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static float f32(const uint8_t *b) { uint32_t v = u32(b); float f; memcpy(&f, &v, 4); return f; }

/* ---- growing arrays ---- */

typedef struct { void *p; size_t n, cap, sz; } Arr;
static void *arr_add(Arr *a, size_t k)
{
    if (a->n + k > a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 1024;
        void *np;
        while (nc < a->n + k) nc *= 2;
        np = realloc(a->p, nc * a->sz);
        if (!np) return NULL;
        a->p = np;
        a->cap = nc;
    }
    a->n += k;
    return (char *)a->p + (a->n - k) * a->sz;
}

typedef struct {
    Arr p, uv, col, ix, ttex, tkind, tg;    /* m29g: tg a triangle's group in its model (-1 a prop's) */
    Arr cp, cix, cs;
    Arr rs, rn, rp, rnrm;
} Build;

/* ---- the .dat files: models by id, textures ---- */

typedef struct {
    int id, kind, ntex;
    const uint8_t *ids;                 /* ntex texture ids */
    const uint8_t *d;                   /* its data */
    int dat;                            /* m29: its file */
    size_t doff;                        /* m29: d's offset in it */
    size_t boff;                        /* m29g: its block's */
    size_t len;
} Model;

typedef struct { char path[2048]; uint8_t *buf; size_t size; } Dat;

static int tex_index(const JStage *s, uint32_t id)
{
    int i;
    for (i = 0; i < s->ntex; i++) if (s->tex[i].id == id) return i;
    return -1;
}

/* the blocks of an indexed file (jtex.c's walk); models listed */
static void dat_models(const Dat *f, int fi, Model **ms, int *n, int *cap)
{
    size_t off = 0;
    int guard = 0;
    while (off + 28 < f->size && guard++ < 100000) {
        uint32_t t = u32(f->buf + off);
        size_t end;
        if (t == 1) {
            size_t nt = u32(f->buf + off + 16), sz = u32(f->buf + off + 8);
            end = off + sz + 20 + nt * 4;
            if (end <= f->size && off + 20 + nt * 4 <= end) {
                Model *m;
                if (*n == *cap) { *cap = *cap ? *cap * 2 : 256; *ms = (Model *)realloc(*ms, sizeof(Model) * (size_t)*cap); }
                m = &(*ms)[(*n)++];
                m->id = (int)u32(f->buf + off + 4);
                m->kind = (int)u32(f->buf + off + 12);
                m->ntex = (int)nt;
                m->ids = f->buf + off + 20;
                m->d = f->buf + off + 20 + nt * 4;
                m->dat = fi;
                m->doff = off + 20 + nt * 4;
                m->boff = off;
                m->len = sz;
            }
        } else if (t == 0) end = off + u32(f->buf + off + 4) + 8;
        else end = off + t + 4;
        if (end <= off) break;
        off = end;
    }
}

/* where the position, colour and first texture coordinates are in a vertex of format fvf; 0 known */
static int fvf_layout(uint32_t fvf, int vs, int *col, int *uv)
{
    static const int pos[8] = { 0, 12, 16, 16, 20, 24, 28, 32 };
    int o = pos[(fvf >> 1) & 7], tex = (int)((fvf >> 8) & 0xF);
    *col = -1; *uv = -1;
    if (!o) return -1;
    if (fvf & 0x10) o += 12;
    if (fvf & 0x20) o += 4;
    if (fvf & 0x40) { *col = o; o += 4; }
    if (fvf & 0x80) o += 4;
    if (tex) *uv = o;
    o += 8 * tex;
    return o == vs ? 0 : -1;
}

/* a stage model (kinds 1, 2) into the build; 0 done */
static int add_stage_model(JStage *s, Build *b, const Model *m)
{
    const uint8_t *d = m->d, *mh, *base = m->d + 0x10;
    uint32_t voff, vc, fvf, vs, ioff, ic, strip, goff, gc, g, k;
    int col, uv, v0 = (int)b->p.n;
    if (m->len < 0x90) return -1;
    if (u32(d + 0x10) + 0x10 + 32 > m->len) return -1;
    mh = base + u32(d + 0x10);
    voff = u32(mh); vc = u32(mh + 4); fvf = u32(mh + 8); vs = u32(mh + 12);
    ioff = u32(mh + 16); ic = u32(mh + 20); strip = u32(mh + 24);
    goff = u32(d + 0x80); gc = u32(d + 0x84);
    if (!vs || vs > 256 || (size_t)voff + 0x10 + (size_t)vc * vs > m->len || (size_t)ioff + 0x10 + (size_t)ic * 2 > m->len) return -1;
    if (gc > 4096 || (size_t)goff + 0x10 + (size_t)gc * 32 > m->len) return -1;
    if (fvf_layout(fvf, (int)vs, &col, &uv)) return -1;
    {
        float *p = (float *)arr_add(&b->p, vc), *t = (float *)arr_add(&b->uv, vc);
        uint32_t *c = (uint32_t *)arr_add(&b->col, vc);
        if (!p || !t || !c) return -1;
        for (k = 0; k < vc; k++) {
            const uint8_t *v = base + voff + (size_t)k * vs;
            p[k * 3] = f32(v); p[k * 3 + 1] = f32(v + 4); p[k * 3 + 2] = f32(v + 8);
            t[k * 2] = uv >= 0 ? f32(v + uv) : 0;
            t[k * 2 + 1] = uv >= 0 ? f32(v + uv + 4) : 0;
            if (col >= 0) {                     /* D3DCOLOR: B G R A */
                const uint8_t *q = v + col;
                c[k] = (uint32_t)q[2] | (uint32_t)q[1] << 8 | (uint32_t)q[0] << 16 | (uint32_t)q[3] << 24;
            } else c[k] = 0xFFFFFFFFu;
        }
    }
    {
        const uint8_t *ix = base + ioff;
        uint32_t ng = gc ? gc : 1;
        for (g = 0; g < ng; g++) {
            uint32_t cnt, first, ti, i;
            int tex = -1;
            if (gc) {
                const uint8_t *e = base + goff + (size_t)g * 32;
                cnt = u32(e); first = u32(e + 4); ti = u32(e + 12);
            } else { cnt = strip ? (ic >= 2 ? ic - 2 : 0) : ic / 3; first = 0; ti = 0; }
            if (ti < (uint32_t)m->ntex) tex = tex_index(s, u32(m->ids + 4 * ti));
            for (i = 0; i < cnt; i++) {
                uint32_t a, bb, c;
                if (strip) {
                    if (first + i + 2 >= ic) break;
                    a = ix[(first + i) * 2] | ix[(first + i) * 2 + 1] << 8;
                    bb = ix[(first + i + 1) * 2] | ix[(first + i + 1) * 2 + 1] << 8;
                    c = ix[(first + i + 2) * 2] | ix[(first + i + 2) * 2 + 1] << 8;
                    if (i & 1) { uint32_t x = a; a = bb; bb = x; }
                    if (a == bb || bb == c || a == c) continue;   /* a strip's joins */
                } else {
                    uint32_t q = first + i * 3;
                    if (q + 2 >= ic) break;
                    a = ix[q * 2] | ix[q * 2 + 1] << 8;
                    bb = ix[(q + 1) * 2] | ix[(q + 1) * 2 + 1] << 8;
                    c = ix[(q + 2) * 2] | ix[(q + 2) * 2 + 1] << 8;
                }
                if (a >= vc || bb >= vc || c >= vc) continue;
                {
                    uint32_t *t = (uint32_t *)arr_add(&b->ix, 3);
                    int *tt = (int *)arr_add(&b->ttex, 1), *tgp = (int *)arr_add(&b->tg, 1);
                    uint8_t *tk = (uint8_t *)arr_add(&b->tkind, 1);
                    if (!t || !tt || !tk) return -1;
                    t[0] = (uint32_t)v0 + a; t[1] = (uint32_t)v0 + bb; t[2] = (uint32_t)v0 + c;
                    *tt = tex;
                    if (tgp) *tgp = (int)g;
                    *tk = m->kind == 2 ? (uv < 0 ? JST_T_SHADOW : JST_T_SEE) : JST_T_STAGE;
                }
            }
        }
    }
    return 0;
}

/* a prop (kind 0, an MDLB) placed by a placement's rows r (12 floats: three rows, the position) */
static int add_prop(JStage *s, Build *b, const Model *m, const float *r, float **local, int *nvl)
{
    JMesh me;
    int i, v0 = (int)b->p.n;
    if (jm_parse_mdlb(m->d, m->len, &me)) {
        free(me.p); free(me.n); free(me.t); free(me.ix); free(me.tmat); free(me.bi); free(me.bw); free(me.bonepos);
        return -1;
    }
    {
        float *p = (float *)arr_add(&b->p, (size_t)me.nv), *t = (float *)arr_add(&b->uv, (size_t)me.nv);
        uint32_t *c = (uint32_t *)arr_add(&b->col, (size_t)me.nv);
        if (p && t && c)
            for (i = 0; i < me.nv; i++) {
                const float *q = me.p + i * 3;
                p[i * 3] = r[9] + q[0] * r[0] + q[1] * r[3] + q[2] * r[6];
                p[i * 3 + 1] = r[10] + q[0] * r[1] + q[1] * r[4] + q[2] * r[7];
                p[i * 3 + 2] = r[11] + q[0] * r[2] + q[1] * r[5] + q[2] * r[8];
                t[i * 2] = me.t[i * 2]; t[i * 2 + 1] = me.t[i * 2 + 1];
                c[i] = 0xFFFFFFFFu;
            }
    }
    for (i = 0; i < me.nt; i++) {
        uint32_t *t = (uint32_t *)arr_add(&b->ix, 3);
        int *tt = (int *)arr_add(&b->ttex, 1), mt = me.tmat ? me.tmat[i] : 0, *tgp = (int *)arr_add(&b->tg, 1);
        if (tgp) *tgp = -1;
        uint8_t *tk = (uint8_t *)arr_add(&b->tkind, 1);
        if (!t || !tt || !tk) break;
        t[0] = (uint32_t)v0 + me.ix[i * 3]; t[1] = (uint32_t)v0 + me.ix[i * 3 + 1]; t[2] = (uint32_t)v0 + me.ix[i * 3 + 2];
        *tt = mt >= 0 && mt < m->ntex ? tex_index(s, u32(m->ids + 4 * mt)) : (m->ntex ? tex_index(s, u32(m->ids)) : -1);
        *tk = JST_T_PROP;
    }
    *local = me.p;                              /* m28: kept, to place it again */
    *nvl = me.nv;
    free(me.n); free(me.t); free(me.ix); free(me.tmat); free(me.bi); free(me.bw); free(me.bonepos);
    (void)v0;
    return 0;
}

static void build_pieces(JStage *s);

/* ---- the .bin ---- */

static void read_collision(JStage *s, Build *b, const uint8_t *B, size_t n, int *nmodels)
{
    uint32_t toff, tcnt, i, j, k;
    if (n < 8) return;
    toff = u32(B); tcnt = u32(B + 4);
    if (tcnt > 100000 || (size_t)toff + (size_t)tcnt * 32 > n) return;
    for (i = 0; i < tcnt; i++) {
        const uint8_t *e = B + toff + (size_t)i * 32;
        uint32_t mo = u32(e + 24), mc = u32(e + 28);
        if (mc > 100000 || (size_t)mo + (size_t)mc * 112 > n) continue;
        for (j = 0; j < mc; j++) {
            const uint8_t *m = B + mo + (size_t)j * 112;
            float r[12];
            uint32_t vo = u32(m + 88), vc = u32(m + 92), to = u32(m + 96), tc = u32(m + 100);
            int v0 = (int)(b->cp.n), t0c = (int)(b->cix.n / 3);
            float *p;
            JSCModel *cmr;
            /* m29: every model recorded, in the file's order (the editor's numbering) */
            s->cmodels = (JSCModel *)realloc(s->cmodels, sizeof(JSCModel) * (size_t)(s->ncmodel + 1));
            if (!s->cmodels) { s->ncmodel = 0; return; }
            cmr = &s->cmodels[s->ncmodel++];
            cmr->area = (int)i; cmr->v0 = v0; cmr->nv = 0; cmr->t0 = t0c; cmr->nt = 0; cmr->src = -1;
            if ((size_t)vo + (size_t)vc * 16 > n || (size_t)to + (size_t)tc * 8 > n || vc > 1024) continue;
            for (k = 0; k < 3; k++) {          /* rows at 24, 40, 56; the position at 72 */
                r[k * 3] = f32(m + 24 + 16 * k); r[k * 3 + 1] = f32(m + 28 + 16 * k); r[k * 3 + 2] = f32(m + 32 + 16 * k);
            }
            r[9] = f32(m + 72); r[10] = f32(m + 76); r[11] = f32(m + 80);
            p = (float *)arr_add(&b->cp, vc);
            if (!p) return;
            for (k = 0; k < vc; k++) {
                const uint8_t *v = B + vo + (size_t)k * 16;
                float x = f32(v), y = f32(v + 4), z = f32(v + 8);
                p[k * 3] = r[9] + x * r[0] + y * r[3] + z * r[6];
                p[k * 3 + 1] = r[10] + x * r[1] + y * r[4] + z * r[7];
                p[k * 3 + 2] = r[11] + x * r[2] + y * r[5] + z * r[8];
            }
            for (k = 0; k < tc; k++) {
                uint32_t a = u32(B + to + (size_t)k * 8), o = u32(B + to + (size_t)k * 8 + 4);
                uint32_t i0 = a & 0x3FF, i1 = (a >> 10) & 0x3FF, i2 = (a >> 20) & 0x3FF;
                uint32_t *t;
                uint16_t *cs;
                if (i0 >= vc || i1 >= vc || i2 >= vc) continue;
                t = (uint32_t *)arr_add(&b->cix, 3);
                cs = (uint16_t *)arr_add(&b->cs, 1);
                if (!t || !cs) return;
                t[0] = (uint32_t)v0 + i0; t[1] = (uint32_t)v0 + i1; t[2] = (uint32_t)v0 + i2;
                *cs = (uint16_t)(o & 0xFFFF);
            }
            cmr->nv = (int)(b->cp.n) - v0;
            cmr->nt = (int)(b->cix.n / 3) - t0c;
            (*nmodels)++;
        }
    }
}

static void read_rails(Build *b, const uint8_t *B, size_t n)
{
    uint32_t *seen = NULL;
    int nseen = 0, cseen = 0, i;
    uint32_t k, q;
    if (n < 4 + 1024 * 8) return;
    for (i = 0; i < 1024; i++) {
        uint32_t so = u32(B + 4 + 8 * i), cnt = u32(B + 4 + 8 * i + 4);
        if (!cnt || cnt > 100000 || (size_t)so + (size_t)cnt * 4 > n) continue;
        for (k = 0; k < cnt; k++) {
            uint32_t h = u32(B + so + 4 * k), po, pc;
            int j, dup = 0;
            if ((size_t)h + 40 > n) continue;
            po = u32(B + h); pc = u32(B + h + 4);
            if (!pc || pc > 100000 || (size_t)po + (size_t)pc * 24 > n) continue;
            for (j = 0; j < nseen && !dup; j++) dup = seen[j] == po;
            if (dup) continue;
            if (nseen == cseen) { cseen = cseen ? cseen * 2 : 64; seen = (uint32_t *)realloc(seen, sizeof(uint32_t) * (size_t)cseen); }
            seen[nseen++] = po;
            {
                int *rs = (int *)arr_add(&b->rs, 1), *rn = (int *)arr_add(&b->rn, 1);
                float *p = (float *)arr_add(&b->rp, pc), *nn = (float *)arr_add(&b->rnrm, pc);
                if (!rs || !rn || !p || !nn) { free(seen); return; }
                *rs = (int)(b->rp.n - pc);
                *rn = (int)pc;
                for (q = 0; q < pc; q++) {
                    const uint8_t *pt = B + po + (size_t)q * 24;
                    p[q * 3] = f32(pt); p[q * 3 + 1] = f32(pt + 4); p[q * 3 + 2] = f32(pt + 8);
                    nn[q * 3] = f32(pt + 12); nn[q * 3 + 1] = f32(pt + 16); nn[q * 3 + 2] = f32(pt + 20);
                }
            }
        }
    }
    free(seen);
}

static int ci_ncmp(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}
static int ci_cmp(const char *a, const char *b) { return ci_ncmp(a, b, (size_t)-1); }

/* ---- finding the files ---- */

typedef struct { const char *name; char (*found)[260]; int n, cap; } Find;
static void find_cb(const char *nm, bool isdir, void *u)
{
    Find *f = (Find *)u;
    size_t l = strlen(f->name);
    if (isdir || strlen(nm) < l || ci_ncmp(nm, f->name, l)) return;
    if (f->n < f->cap) snprintf(f->found[f->n++], 260, "%s", nm);
}

int jst_name_of(const char *file, char *name, size_t n)
{
    const char *b = strrchr(file, '/'), *c = strrchr(file, '\\');
    if (c > b) b = c;
    b = b ? b + 1 : file;
    if (strlen(b) < 6 || tolower((unsigned char)b[0]) != 's' || tolower((unsigned char)b[1]) != 't' || tolower((unsigned char)b[2]) != 'g'
        || !isdigit((unsigned char)b[3]) || !isdigit((unsigned char)b[4]) || b[5] != '_')
        return 0;
    snprintf(name, n, "Stg%c%c", b[3], b[4]);
    return 1;
}

static int cmp_name(const void *a, const void *b) { return ci_cmp((const char *)a, (const char *)b); }

int jst_load(const char *dir, const char *name, JStage *s, char *err, size_t errn)
{
    char (*found)[260] = (char (*)[260])calloc(128, 260);
    char prefix[16], path[2048];
    Find fd;
    Dat dats[64];
    int ndat = 0, i, j, nm = 0, cm = 0, binat = -1;
    Model *ms = NULL;
    Build b;
    uint8_t *bin = NULL;
    size_t binsz = 0;
    memset(s, 0, sizeof *s);
    memset(&b, 0, sizeof b);
    b.p.sz = 12; b.uv.sz = 8; b.col.sz = 4; b.ix.sz = 4; b.ttex.sz = sizeof(int); b.tkind.sz = 1; b.tg.sz = sizeof(int);
    b.cp.sz = 12; b.cix.sz = 4; b.cs.sz = 2; b.rs.sz = sizeof(int); b.rn.sz = sizeof(int); b.rp.sz = 12; b.rnrm.sz = 12;
    snprintf(s->name, sizeof s->name, "%s", name);
    snprintf(prefix, sizeof prefix, "%s_", name);
    fd.name = prefix; fd.found = found; fd.n = 0; fd.cap = 128;
    if (!found) { snprintf(err, errn, "Out of memory."); return -1; }
    list_dir(dir, find_cb, &fd);
    qsort(found, (size_t)fd.n, 260, cmp_name);
    for (i = 0; i < fd.n; i++) {
        const char *nmf = found[i];
        size_t l = strlen(nmf);
        pjoin(path, dir, nmf);
        if (l == strlen(prefix) + 4 && !ci_cmp(nmf + l - 4, ".bin")) {
            bin = (uint8_t *)read_file(path, &binsz);
            snprintf(s->binfile, sizeof s->binfile, "%s", nmf);
            binat = i;
        } else if (l == strlen(prefix) + 6 && !ci_cmp(nmf + l - 4, ".dat") && isdigit((unsigned char)nmf[l - 6]) && ndat < 64) {
            Dat *d = &dats[ndat];
            snprintf(d->path, sizeof d->path, "%s", path);
            snprintf(s->datfile[ndat], sizeof s->datfile[ndat], "%s", nmf);
            d->buf = (uint8_t *)read_file(path, &d->size);
            if (d->buf) ndat++;
        }
    }
    (void)binat;
    free(found);
    if (bin && binsz < 0x40) { free(bin); bin = NULL; }
    s->has_bin = bin != NULL;                    /* without it: the models only (no collision, rails, props) */
    if (!ndat) {
        snprintf(err, errn, "No %s_NN.dat files in %s.", name, dir);
        free(bin);
        return -1;
    }
    s->ndat = ndat;
    /* the textures, from every .dat */
    for (i = 0; i < ndat; i++) {
        JFile f;
        uint8_t *cp = (uint8_t *)malloc(dats[i].size);
        if (!cp) continue;
        memcpy(cp, dats[i].buf, dats[i].size);
        if (jf_parse(&f, dats[i].path, cp, dats[i].size)) continue;
        for (j = 0; j < f.ntex; j++) {
            JSTex *t;
            uint8_t *px;
            if (tex_index(s, f.tex[j].id) >= 0 || f.tex[j].w <= 0 || f.tex[j].h <= 0 || f.tex[j].w > 4096 || f.tex[j].h > 4096) continue;
            px = (uint8_t *)malloc((size_t)f.tex[j].w * f.tex[j].h * 4);
            if (!px) continue;
            if (jt_decode(&f, &f.tex[j], px)) { free(px); continue; }
            s->tex = (JSTex *)realloc(s->tex, sizeof(JSTex) * (size_t)(s->ntex + 1));
            t = &s->tex[s->ntex++];
            t->id = f.tex[j].id; t->w = f.tex[j].w; t->h = f.tex[j].h; t->rgba = px;
        }
        jf_free(&f);
    }
    /* the models, by id */
    for (i = 0; i < ndat; i++) dat_models(&dats[i], i, &ms, &nm, &cm);
    for (i = 0; i < nm; i++)
        if (ms[i].kind == 1 || ms[i].kind == 2) {
            int v0m = (int)b.p.n, t0m = (int)(b.ix.n / 3);
            if (!add_stage_model(s, &b, &ms[i])) {
                JSModel *mm;
                s->models = (JSModel *)realloc(s->models, sizeof(JSModel) * (size_t)(s->nmodel + 1));
                if (s->models) {
                    mm = &s->models[s->nmodel++];
                    mm->id = ms[i].id; mm->kind = ms[i].kind; mm->dat = ms[i].dat; mm->doff = ms[i].doff; mm->boff = ms[i].boff;
                    mm->v0 = v0m; mm->nv = (int)b.p.n - v0m; mm->t0 = t0m; mm->nt = (int)(b.ix.n / 3) - t0m;
                } else s->nmodel = 0;
                int col, uv;
                s->nmodels++;
                if (ms[i].kind == 2) {
                    const uint8_t *mh = ms[i].d + 0x10 + u32(ms[i].d + 0x10);
                    fvf_layout(u32(mh + 8), (int)u32(mh + 12), &col, &uv);
                    if (uv < 0) s->nshadow++; else s->nsee++;
                }
            }
        }
    /* the .bin: collision, rails, placements */
    if (bin) {
        uint32_t o0 = u32(bin), s0 = u32(bin + 4), o1 = u32(bin + 8), m26 = u32(bin + 12), o2 = u32(bin + 16), s2 = u32(bin + 20);
        if ((size_t)o0 + s0 <= binsz) read_collision(s, &b, bin + o0, s0, &s->ncmodels);
        if ((size_t)o1 + m26 <= binsz) read_rails(&b, bin + o1, m26);
        if ((size_t)o2 + s2 <= binsz && s2 >= 12) {
            /* m28: the areas' placement lists: (offset, count) an area from +12, as many as the areas (+8);
             * m26 read only the first 12 (props in later areas were missing) */
            const uint8_t *B = bin + o2;
            uint32_t na = u32(B + 8), blk;
            if (na > 4096 || 12 + (size_t)na * 8 > s2) na = 0;
            for (blk = 0; blk < na; blk++) {
                uint32_t so = u32(B + 12 + 8 * blk), cnt = u32(B + 16 + 8 * blk), k;
                if (cnt > 90000) continue;
                for (k = 0; k < cnt; k++) {
                    const uint8_t *e = B + so + (size_t)k * 80;
                    float r[12], *local = NULL;
                    int id, q, nvl = 0, v0 = (int)b.p.n, t0 = (int)(b.ix.n / 3);
                    if ((size_t)so + (size_t)(k + 1) * 80 > s2) break;
                    id = (int)u32(e + 64);
                    for (q = 0; q < nm && ms[q].id != id; q++) ;
                    if (q == nm || ms[q].kind != 0) continue;   /* stage models are in place already */
                    for (j = 0; j < 3; j++) { r[j * 3] = f32(e + 16 * j); r[j * 3 + 1] = f32(e + 16 * j + 4); r[j * 3 + 2] = f32(e + 16 * j + 8); }
                    r[9] = f32(e + 48); r[10] = f32(e + 52); r[11] = f32(e + 56);
                    if (!add_prop(s, &b, &ms[q], r, &local, &nvl)) {
                        JSProp *pp;
                        s->nprops++;
                        s->props = (JSProp *)realloc(s->props, sizeof(JSProp) * (size_t)(s->nprop + 1));
                        if (!s->props) { s->nprop = 0; free(local); continue; }
                        pp = &s->props[s->nprop++];
                        memset(pp, 0, sizeof *pp);
                        pp->area = (int)blk; pp->entry = (int)k; pp->model_id = id;
                        memcpy(pp->bytes, e, 80);
                        memcpy(pp->r, r, sizeof r);
                        pp->v0 = v0; pp->nv = (int)b.p.n - v0;
                        pp->t0 = t0; pp->nt = (int)(b.ix.n / 3) - t0;
                        pp->local = local;
                        if (nvl != pp->nv) pp->nv = 0;          /* not placeable again */
                    }
                }
            }
        }
    }
    free(ms);
    for (i = 0; i < ndat; i++) free(dats[i].buf);
    free(bin);
    s->nv = (int)b.p.n; s->p = (float *)b.p.p; s->uv = (float *)b.uv.p; s->col = (uint32_t *)b.col.p;
    s->nt = (int)b.ix.n / 3; s->ix = (uint32_t *)b.ix.p; s->ttex = (int *)b.ttex.p; s->tkind = (uint8_t *)b.tkind.p; s->tgroup = (int *)b.tg.p;
    s->ncv = (int)b.cp.n; s->cp = (float *)b.cp.p; s->nct = (int)b.cix.n / 3; s->cix = (uint32_t *)b.cix.p; s->csurf = (uint16_t *)b.cs.p;
    s->nrail = (int)b.rs.n; s->rs = (int *)b.rs.p; s->rn = (int *)b.rn.p; s->nrp = (int)b.rp.n; s->rp = (float *)b.rp.p; s->rnrm = (float *)b.rnrm.p;
    for (i = 0; i < 3; i++) { s->lo[i] = 1e30f; s->hi[i] = -1e30f; }
    for (i = 0; i < s->ncv; i++)                 /* the box: the collision's (the skyline's far pieces aside) */
        for (j = 0; j < 3; j++) {
            float v = s->cp[i * 3 + j];
            if (v < s->lo[j]) s->lo[j] = v;
            if (v > s->hi[j]) s->hi[j] = v;
        }
    if (!s->ncv)
        for (i = 0; i < s->nv; i++)
            for (j = 0; j < 3; j++) {
                float v = s->p[i * 3 + j];
                if (v < s->lo[j]) s->lo[j] = v;
                if (v > s->hi[j]) s->hi[j] = v;
            }
    build_pieces(s);                             /* m29 */
    if (!s->nt && !s->nct) {
        snprintf(err, errn, "Nothing could be read from %s's files.", name);
        jst_free(s);
        return -1;
    }
    return 0;
}

void jst_free(JStage *s)
{
    int i;
    for (i = 0; i < s->ntex; i++) free(s->tex[i].rgba);
    free(s->tex); free(s->p); free(s->uv); free(s->col); free(s->ix); free(s->ttex); free(s->tkind); free(s->tgroup);
    free(s->cp); free(s->cix); free(s->csurf); free(s->rs); free(s->rn); free(s->rp); free(s->rnrm);
    for (i = 0; i < s->nprop; i++) free(s->props[i].local);
    free(s->props);
    for (i = 0; i < s->npiece; i++) { free(s->pieces[i].v); free(s->pieces[i].t); free(s->pieces[i].cm); free(s->pieces[i].rail); }
    free(s->rp0); free(s->rnrm0); free(s->rpiece); free(s->rsrc);
    free(s->pieces); free(s->models); free(s->tpiece); free(s->vpiece); free(s->p0); free(s->cp0);
    free(s->cmodels); free(s->cpiece);
    memset(s, 0, sizeof *s);
}


/* ---- m29: the map in pieces ---- */

static const float *s_sortp;
static int cmp_pos(const void *a, const void *b)
{
    const float *x = s_sortp + (size_t)*(const int *)a * 3, *y = s_sortp + (size_t)*(const int *)b * 3;
    int k;
    for (k = 0; k < 3; k++) if (x[k] != y[k]) return x[k] < y[k] ? -1 : 1;
    return 0;
}
static int uf_find(int *par, int x)
{
    while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; }
    return x;
}
static void uf_join(int *par, int a, int b)
{
    a = uf_find(par, a); b = uf_find(par, b);
    if (a != b) par[a] = b;
}

/* each stage model split into its connected parts (vertices at the same
 * place count as joined); each collision model given to the smallest
 * opaque piece whose box holds it */
static void build_pieces(JStage *s)
{
    int m, i, k, *par = NULL, *ord = NULL, *slot = NULL;
    s->p0 = (float *)malloc(sizeof(float) * 3 * (size_t)(s->nv + 1));
    s->cp0 = (float *)malloc(sizeof(float) * 3 * (size_t)(s->ncv + 1));
    s->tpiece = (int *)malloc(sizeof(int) * (size_t)(s->nt + 1));
    s->vpiece = (int *)malloc(sizeof(int) * (size_t)(s->nv + 1));
    s->cpiece = (int *)malloc(sizeof(int) * (size_t)(s->ncmodel + 1));
    if (!s->p0 || !s->cp0 || !s->tpiece || !s->vpiece || !s->cpiece) return;
    memcpy(s->p0, s->p, sizeof(float) * 3 * (size_t)s->nv);
    memcpy(s->cp0, s->cp, sizeof(float) * 3 * (size_t)s->ncv);
    for (i = 0; i < s->nt; i++) s->tpiece[i] = -1;
    for (i = 0; i < s->nv; i++) s->vpiece[i] = -1;
    for (i = 0; i < s->ncmodel; i++) s->cpiece[i] = -1;
    for (m = 0; m < s->nmodel; m++) {
        const JSModel *md = &s->models[m];
        int nv = md->nv, first = s->npiece;
        if (nv <= 0) continue;
        par = (int *)realloc(par, sizeof(int) * (size_t)nv);
        ord = (int *)realloc(ord, sizeof(int) * (size_t)nv);
        slot = (int *)realloc(slot, sizeof(int) * (size_t)nv);
        if (!par || !ord || !slot) break;
        for (i = 0; i < nv; i++) { par[i] = i; ord[i] = i; slot[i] = -1; }
        s_sortp = s->p + (size_t)md->v0 * 3;
        qsort(ord, (size_t)nv, sizeof(int), cmp_pos);
        for (i = 1; i < nv; i++) if (!cmp_pos(&ord[i - 1], &ord[i])) uf_join(par, ord[i - 1], ord[i]);
        for (i = 0; i < md->nt; i++) {
            const uint32_t *t = s->ix + (size_t)(md->t0 + i) * 3;
            uf_join(par, (int)t[0] - md->v0, (int)t[1] - md->v0);
            uf_join(par, (int)t[1] - md->v0, (int)t[2] - md->v0);
        }
        for (i = 0; i < nv; i++) {               /* a piece a root */
            int r = uf_find(par, i);
            if (slot[r] < 0) {
                JSPiece *np = (JSPiece *)realloc(s->pieces, sizeof(JSPiece) * (size_t)(s->npiece + 1));
                if (!np) break;
                s->pieces = np;
                memset(&np[s->npiece], 0, sizeof(JSPiece));
                np[s->npiece].model = m;
                np[s->npiece].src = -1;
                for (k = 0; k < 3; k++) { np[s->npiece].lo[k] = 1e30f; np[s->npiece].hi[k] = -1e30f; }
                slot[r] = s->npiece++;
            }
            s->vpiece[md->v0 + i] = slot[r];
            s->pieces[slot[r]].nv++;
        }
        for (i = 0; i < md->nt; i++) {
            int pk = s->vpiece[s->ix[(size_t)(md->t0 + i) * 3]];
            s->tpiece[md->t0 + i] = pk;
            if (pk >= 0) s->pieces[pk].nt++;
        }
        for (k = first; k < s->npiece; k++) {
            JSPiece *pc = &s->pieces[k];
            pc->v = (int *)malloc(sizeof(int) * (size_t)(pc->nv + 1));
            pc->t = (int *)malloc(sizeof(int) * (size_t)(pc->nt + 1));
            pc->nv = pc->nt = 0;
        }
        for (i = 0; i < nv; i++) {
            JSPiece *pc = &s->pieces[s->vpiece[md->v0 + i]];
            const float *q = s->p + (size_t)(md->v0 + i) * 3;
            if (!pc->v) continue;
            pc->v[pc->nv++] = md->v0 + i;
            for (k = 0; k < 3; k++) { if (q[k] < pc->lo[k]) pc->lo[k] = q[k]; if (q[k] > pc->hi[k]) pc->hi[k] = q[k]; }
        }
        for (i = 0; i < md->nt; i++) {
            JSPiece *pc = &s->pieces[s->tpiece[md->t0 + i]];
            if (pc->t) pc->t[pc->nt++] = md->t0 + i;
        }
        for (k = first; k < s->npiece; k++) {
            JSPiece *pc = &s->pieces[k];
            int q;
            for (q = 0; q < 3; q++) pc->pv[q] = pc->c[q] = (pc->lo[q] + pc->hi[q]) * 0.5f;
        }
    }
    free(par); free(ord); free(slot);
    /* the rails: to the smallest opaque piece holding all their points */
    s->rp0 = (float *)malloc(sizeof(float) * 3 * (size_t)(s->nrp + 1));
    s->rnrm0 = (float *)malloc(sizeof(float) * 3 * (size_t)(s->nrp + 1));
    s->rpiece = (int *)malloc(sizeof(int) * (size_t)(s->nrail + 1));
    s->rsrc = (int *)malloc(sizeof(int) * (size_t)(s->nrail + 1));
    if (s->rsrc) for (i = 0; i < s->nrail; i++) s->rsrc[i] = -1;
    if (s->rp0 && s->rnrm0 && s->rpiece) {
        memcpy(s->rp0, s->rp, sizeof(float) * 3 * (size_t)s->nrp);
        memcpy(s->rnrm0, s->rnrm, sizeof(float) * 3 * (size_t)s->nrp);
        for (i = 0; i < s->nrail; i++) {
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, best = 1e38f;
            int q, j, bk = -1;
            for (j = 0; j < s->rn[i]; j++)
                for (q = 0; q < 3; q++) {
                    float v = s->rp[(size_t)(s->rs[i] + j) * 3 + q];
                    if (v < lo[q]) lo[q] = v;
                    if (v > hi[q]) hi[q] = v;
                }
            for (k = 0; k < s->npiece; k++) {
                const JSPiece *pc = &s->pieces[k];
                float vol;
                if (s->models[pc->model].kind != 1) continue;
                for (q = 0; q < 3; q++) if (lo[q] < pc->lo[q] - 1.0f || hi[q] > pc->hi[q] + 1.0f) break;
                if (q < 3) continue;
                vol = (pc->hi[0] - pc->lo[0] + 1) * (pc->hi[1] - pc->lo[1] + 1) * (pc->hi[2] - pc->lo[2] + 1);
                if (vol < best) { best = vol; bk = k; }
            }
            s->rpiece[i] = bk;
            if (bk >= 0) {
                JSPiece *pc = &s->pieces[bk];
                int *nr = (int *)realloc(pc->rail, sizeof(int) * (size_t)(pc->nrail + 1));
                if (nr) { pc->rail = nr; pc->rail[pc->nrail++] = i; }
            }
        }
    }
    /* the collision: to the smallest opaque piece holding its box */
    for (i = 0; i < s->ncmodel; i++) {
        const JSCModel *cm = &s->cmodels[i];
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, best = 1e38f;
        int q, j, bk = -1;
        if (cm->nv <= 0) continue;
        for (j = 0; j < cm->nv; j++)
            for (q = 0; q < 3; q++) {
                float v = s->cp[(size_t)(cm->v0 + j) * 3 + q];
                if (v < lo[q]) lo[q] = v;
                if (v > hi[q]) hi[q] = v;
            }
        for (k = 0; k < s->npiece; k++) {
            const JSPiece *pc = &s->pieces[k];
            float vol;
            if (s->models[pc->model].kind != 1) continue;
            for (q = 0; q < 3; q++) if (lo[q] < pc->lo[q] - 0.5f || hi[q] > pc->hi[q] + 0.5f) break;
            if (q < 3) continue;
            vol = (pc->hi[0] - pc->lo[0] + 1) * (pc->hi[1] - pc->lo[1] + 1) * (pc->hi[2] - pc->lo[2] + 1);
            if (vol < best) { best = vol; bk = k; }
        }
        s->cpiece[i] = bk;
        if (bk >= 0) {
            JSPiece *pc = &s->pieces[bk];
            int *nc = (int *)realloc(pc->cm, sizeof(int) * (size_t)(pc->ncm + 1));
            if (nc) { pc->cm = nc; pc->cm[pc->ncm++] = i; }
        }
    }
}

/* piece k's vertices (and its collision) placed again: turned about its middle, then moved */
void jst_piece_update(JStage *s, int k)
{
    JSPiece *pc;
    float a, c, sn;
    int i, j;
    if (k < 0 || k >= s->npiece || !s->p0) return;
    pc = &s->pieces[k];
    a = pc->yaw * 3.14159265f / 180.0f; c = cosf(a); sn = sinf(a);
    for (i = 0; i < pc->nv; i++) {
        const float *q = s->p0 + (size_t)pc->v[i] * 3;
        float *o = s->p + (size_t)pc->v[i] * 3, dx = q[0] - pc->pv[0], dz = q[2] - pc->pv[2];
        o[0] = pc->pv[0] + dx * c + dz * sn + pc->t3[0];
        o[1] = q[1] + pc->t3[1];
        o[2] = pc->pv[2] - dx * sn + dz * c + pc->t3[2];
    }
    for (j = 0; j < pc->nrail && s->rp0; j++) {    /* its rails, points and normals */
        int r = pc->rail[j];
        for (i = 0; i < s->rn[r]; i++) {
            size_t q = (size_t)(s->rs[r] + i) * 3;
            const float *a0 = s->rp0 + q, *n0 = s->rnrm0 + q;
            float dx = a0[0] - pc->pv[0], dz = a0[2] - pc->pv[2];
            s->rp[q] = pc->pv[0] + dx * c + dz * sn + pc->t3[0];
            s->rp[q + 1] = a0[1] + pc->t3[1];
            s->rp[q + 2] = pc->pv[2] - dx * sn + dz * c + pc->t3[2];
            s->rnrm[q] = n0[0] * c + n0[2] * sn;
            s->rnrm[q + 1] = n0[1];
            s->rnrm[q + 2] = -n0[0] * sn + n0[2] * c;
        }
    }
    for (j = 0; j < pc->ncm; j++) {
        const JSCModel *cm = &s->cmodels[pc->cm[j]];
        for (i = 0; i < cm->nv; i++) {
            const float *q = s->cp0 + (size_t)(cm->v0 + i) * 3;
            float *o = s->cp + (size_t)(cm->v0 + i) * 3, dx = q[0] - pc->pv[0], dz = q[2] - pc->pv[2];
            o[0] = pc->pv[0] + dx * c + dz * sn + pc->t3[0];
            o[1] = q[1] + pc->t3[1];
            o[2] = pc->pv[2] - dx * sn + dz * c + pc->t3[2];
        }
    }
}


/* ---- m29g: map pieces copied ---- */

static int grow(void *pp, size_t bytes)
{
    void **p = (void **)pp, *n = realloc(*p, bytes ? bytes : 1);
    if (!n) return -1;
    *p = n;
    return 0;
}

int jst_piece_copy(JStage *s, const int *set, int n)
{
    int j, first = s->npiece;
    if (!s->p0 || !s->cp0 || !s->rp0 || !s->rsrc) return -1;
    for (j = 0; j < n; j++) {
        int k = set[j], i, q, nv0 = s->nv, nt0 = s->nt, np;
        JSPiece *src, *pc;
        int *map;
        if (k < 0 || k >= s->npiece || s->pieces[k].removed) continue;
        src = &s->pieces[k];
        /* room */
        if (grow(&s->p, sizeof(float) * 3 * (size_t)(nv0 + src->nv)) || grow(&s->uv, sizeof(float) * 2 * (size_t)(nv0 + src->nv))
            || grow(&s->col, sizeof(uint32_t) * (size_t)(nv0 + src->nv)) || grow(&s->p0, sizeof(float) * 3 * (size_t)(nv0 + src->nv))
            || grow(&s->vpiece, sizeof(int) * (size_t)(nv0 + src->nv))
            || grow(&s->ix, sizeof(uint32_t) * 3 * (size_t)(nt0 + src->nt)) || grow(&s->ttex, sizeof(int) * (size_t)(nt0 + src->nt))
            || grow(&s->tkind, (size_t)(nt0 + src->nt)) || grow(&s->tgroup, sizeof(int) * (size_t)(nt0 + src->nt))
            || grow(&s->tpiece, sizeof(int) * (size_t)(nt0 + src->nt)) || grow(&s->pieces, sizeof(JSPiece) * (size_t)(s->npiece + 1)))
            return -1;
        src = &s->pieces[k];
        np = s->npiece++;
        pc = &s->pieces[np];
        memset(pc, 0, sizeof *pc);
        pc->model = src->model;
        pc->src = k;
        pc->touched = 1;
        pc->yaw_base = src->yaw + src->yaw_base;   /* how far turned from the file's, where it starts */
        pc->v = (int *)malloc(sizeof(int) * (size_t)(src->nv + 1));
        pc->t = (int *)malloc(sizeof(int) * (size_t)(src->nt + 1));
        map = (int *)malloc(sizeof(int) * (size_t)(src->nv + 1));
        if (!pc->v || !pc->t || !map) { free(map); return -1; }
        for (q = 0; q < 3; q++) { pc->lo[q] = 1e30f; pc->hi[q] = -1e30f; }
        for (i = 0; i < src->nv; i++) {             /* its corners, where the source's are now */
            int sv = src->v[i], nvx = nv0 + i;
            memcpy(s->p + (size_t)nvx * 3, s->p + (size_t)sv * 3, 12);
            memcpy(s->p0 + (size_t)nvx * 3, s->p + (size_t)sv * 3, 12);
            memcpy(s->uv + (size_t)nvx * 2, s->uv + (size_t)sv * 2, 8);
            s->col[nvx] = s->col[sv];
            s->vpiece[nvx] = np;
            pc->v[i] = nvx;
            for (q = 0; q < 3; q++) {
                float v = s->p[(size_t)nvx * 3 + q];
                if (v < pc->lo[q]) pc->lo[q] = v;
                if (v > pc->hi[q]) pc->hi[q] = v;
            }
        }
        for (q = 0; q < 3; q++) pc->pv[q] = pc->c[q] = (pc->lo[q] + pc->hi[q]) * 0.5f;
        pc->nv = src->nv;
        s->nv = nv0 + src->nv;
        for (i = 0; i < src->nt; i++) {
            int st = src->t[i], nti = nt0 + i, c3;
            for (c3 = 0; c3 < 3; c3++) {
                int sv = (int)s->ix[(size_t)st * 3 + c3], lv = 0, z;
                for (z = 0; z < src->nv; z++) if (src->v[z] == sv) { lv = z; break; }
                s->ix[(size_t)nti * 3 + c3] = (uint32_t)(nv0 + lv);
            }
            s->ttex[nti] = s->ttex[st];
            s->tkind[nti] = s->tkind[st];
            s->tgroup[nti] = s->tgroup[st];
            s->tpiece[nti] = np;
            pc->t[i] = nti;
        }
        pc->nt = src->nt;
        s->nt = nt0 + src->nt;
        free(map);
        /* its collision */
        for (i = 0; i < src->ncm; i++) {
            int ci = src->cm[i], ncv0 = s->ncv, nct0 = s->nct, nci = s->ncmodel;
            const JSCModel *cm = &s->cmodels[ci];
            JSCModel *ncm;
            if (grow(&s->cp, sizeof(float) * 3 * (size_t)(ncv0 + cm->nv)) || grow(&s->cp0, sizeof(float) * 3 * (size_t)(ncv0 + cm->nv))
                || grow(&s->cix, sizeof(uint32_t) * 3 * (size_t)(nct0 + cm->nt)) || grow(&s->csurf, sizeof(uint16_t) * (size_t)(nct0 + cm->nt))
                || grow(&s->cmodels, sizeof(JSCModel) * (size_t)(nci + 1)) || grow(&s->cpiece, sizeof(int) * (size_t)(nci + 1)))
                return -1;
            cm = &s->cmodels[ci];
            memcpy(s->cp + (size_t)ncv0 * 3, s->cp + (size_t)cm->v0 * 3, sizeof(float) * 3 * (size_t)cm->nv);
            memcpy(s->cp0 + (size_t)ncv0 * 3, s->cp + (size_t)cm->v0 * 3, sizeof(float) * 3 * (size_t)cm->nv);
            for (q = 0; q < cm->nt * 3; q++) s->cix[(size_t)nct0 * 3 + q] = s->cix[(size_t)cm->t0 * 3 + q] - (uint32_t)cm->v0 + (uint32_t)ncv0;
            memcpy(s->csurf + nct0, s->csurf + cm->t0, sizeof(uint16_t) * (size_t)cm->nt);
            ncm = &s->cmodels[nci];
            ncm->area = cm->area; ncm->v0 = ncv0; ncm->nv = cm->nv; ncm->t0 = nct0; ncm->nt = cm->nt; ncm->src = ci;
            s->ncv = ncv0 + cm->nv;
            s->nct = nct0 + cm->nt;
            s->ncmodel = nci + 1;
            s->cpiece[nci] = np;
            pc = &s->pieces[np];
            if (grow(&pc->cm, sizeof(int) * (size_t)(pc->ncm + 1))) return -1;
            pc->cm[pc->ncm++] = nci;
        }
        /* its rails */
        for (i = 0; i < src->nrail; i++) {
            int r = src->rail[i], nr = s->nrail, nrp0 = s->nrp, cnt = s->rn[r];
            if (grow(&s->rs, sizeof(int) * (size_t)(nr + 1)) || grow(&s->rn, sizeof(int) * (size_t)(nr + 1))
                || grow(&s->rpiece, sizeof(int) * (size_t)(nr + 1)) || grow(&s->rsrc, sizeof(int) * (size_t)(nr + 1))
                || grow(&s->rp, sizeof(float) * 3 * (size_t)(nrp0 + cnt)) || grow(&s->rnrm, sizeof(float) * 3 * (size_t)(nrp0 + cnt))
                || grow(&s->rp0, sizeof(float) * 3 * (size_t)(nrp0 + cnt)) || grow(&s->rnrm0, sizeof(float) * 3 * (size_t)(nrp0 + cnt)))
                return -1;
            memcpy(s->rp + (size_t)nrp0 * 3, s->rp + (size_t)s->rs[r] * 3, sizeof(float) * 3 * (size_t)cnt);
            memcpy(s->rnrm + (size_t)nrp0 * 3, s->rnrm + (size_t)s->rs[r] * 3, sizeof(float) * 3 * (size_t)cnt);
            memcpy(s->rp0 + (size_t)nrp0 * 3, s->rp + (size_t)s->rs[r] * 3, sizeof(float) * 3 * (size_t)cnt);
            memcpy(s->rnrm0 + (size_t)nrp0 * 3, s->rnrm + (size_t)s->rs[r] * 3, sizeof(float) * 3 * (size_t)cnt);
            s->rs[nr] = nrp0; s->rn[nr] = cnt; s->rpiece[nr] = np; s->rsrc[nr] = r;
            s->nrail = nr + 1;
            s->nrp = nrp0 + cnt;
            pc = &s->pieces[np];
            if (grow(&pc->rail, sizeof(int) * (size_t)(pc->nrail + 1))) return -1;
            pc->rail[pc->nrail++] = nr;
        }
    }
    return s->npiece > first ? first : -1;
}

/* ---- m28: props moved, removed, copied ---- */

void jst_prop_update(JStage *s, int k)
{
    JSProp *pp;
    int i;
    if (k < 0 || k >= s->nprop) return;
    pp = &s->props[k];
    for (i = 0; i < pp->nt; i++) s->tkind[pp->t0 + i] = (uint8_t)(pp->removed ? JST_T_GONE : JST_T_PROP);
    if (!pp->local || !pp->nv) return;
    for (i = 0; i < pp->nv; i++) {
        const float *q = pp->local + i * 3, *r = pp->r;
        float *o = s->p + (size_t)(pp->v0 + i) * 3;
        o[0] = r[9] + q[0] * r[0] + q[1] * r[3] + q[2] * r[6];
        o[1] = r[10] + q[0] * r[1] + q[1] * r[4] + q[2] * r[7];
        o[2] = r[11] + q[0] * r[2] + q[1] * r[5] + q[2] * r[8];
    }
}

int jst_prop_dup(JStage *s, int k)
{
    JSProp *src, *pp, *np;
    int nv, nt, i;
    float *p, *uv, *loc;
    uint32_t *col, *ix;
    int *tt;
    uint8_t *tk;
    if (k < 0 || k >= s->nprop || !s->props[k].nv || !s->props[k].local) return -1;
    src = &s->props[k];
    nv = s->nv + src->nv;
    nt = s->nt + src->nt;
    p = (float *)realloc(s->p, sizeof(float) * 3 * (size_t)nv); if (p) s->p = p;
    uv = (float *)realloc(s->uv, sizeof(float) * 2 * (size_t)nv); if (uv) s->uv = uv;
    col = (uint32_t *)realloc(s->col, sizeof(uint32_t) * (size_t)nv); if (col) s->col = col;
    ix = (uint32_t *)realloc(s->ix, sizeof(uint32_t) * 3 * (size_t)nt); if (ix) s->ix = ix;
    tt = (int *)realloc(s->ttex, sizeof(int) * (size_t)nt); if (tt) s->ttex = tt;
    tk = (uint8_t *)realloc(s->tkind, (size_t)nt); if (tk) s->tkind = tk;
    { int *tg = (int *)realloc(s->tgroup, sizeof(int) * (size_t)nt); if (tg) s->tgroup = tg; else return -1; }
    np = (JSProp *)realloc(s->props, sizeof(JSProp) * (size_t)(s->nprop + 1)); if (np) s->props = np;
    loc = (float *)malloc(sizeof(float) * 3 * (size_t)s->props[k].nv);
    if (!p || !uv || !col || !ix || !tt || !tk || !np || !loc) { free(loc); return -1; }
    src = &s->props[k];
    pp = &s->props[s->nprop];
    *pp = *src;
    pp->entry = -1;                             /* a new entry, in the same area */
    pp->removed = 0;
    pp->v0 = s->nv;
    pp->t0 = s->nt;
    memcpy(loc, src->local, sizeof(float) * 3 * (size_t)src->nv);
    pp->local = loc;
    memcpy(s->p + (size_t)pp->v0 * 3, s->p + (size_t)src->v0 * 3, sizeof(float) * 3 * (size_t)src->nv);
    memcpy(s->uv + (size_t)pp->v0 * 2, s->uv + (size_t)src->v0 * 2, sizeof(float) * 2 * (size_t)src->nv);
    memcpy(s->col + pp->v0, s->col + src->v0, sizeof(uint32_t) * (size_t)src->nv);
    for (i = 0; i < src->nt; i++) {
        int q;
        for (q = 0; q < 3; q++) s->ix[(size_t)(pp->t0 + i) * 3 + q] = s->ix[(size_t)(src->t0 + i) * 3 + q] - (uint32_t)src->v0 + (uint32_t)pp->v0;
        s->ttex[pp->t0 + i] = s->ttex[src->t0 + i];
        s->tgroup[pp->t0 + i] = -1;
        s->tkind[pp->t0 + i] = JST_T_PROP;
    }
    s->nv = nv;
    s->nt = nt;
    s->nprop++;
    s->nprops++;
    return s->nprop - 1;
}

int jst_prop_of_tri(const JStage *s, int tri)
{
    int i;
    for (i = 0; i < s->nprop; i++)
        if (tri >= s->props[i].t0 && tri < s->props[i].t0 + s->props[i].nt) return i;
    return -1;
}

/* ---- .obj export ---- */

int jst_write_obj(const JStage *s, const char *dir, char *err, size_t errn)
{
    char path[2048], tdir[2048], fn[300];
    FILE *f;
    int i, k, *used;
    if (!make_dirs(dir)) { snprintf(err, errn, "Couldn't make %s.", dir); return -1; }
    /* the textures used, as PNGs */
    snprintf(fn, sizeof fn, "%s_tex", s->name);
    pjoin(tdir, dir, fn);
    make_dirs(tdir);
    used = (int *)calloc((size_t)(s->ntex + 1), sizeof(int));
    for (i = 0; i < s->nt; i++) if (s->ttex[i] >= 0 && s->ttex[i] < s->ntex) used[s->ttex[i]] = 1;
    for (k = 0; k < s->ntex; k++)
        if (used[k]) {
            snprintf(fn, sizeof fn, "%08X.png", s->tex[k].id);
            pjoin(path, tdir, fn);
            stbi_write_png(path, s->tex[k].w, s->tex[k].h, 4, s->tex[k].rgba, s->tex[k].w * 4);
        }
    /* the visual: a material per texture */
    snprintf(fn, sizeof fn, "%s_visual.mtl", s->name);
    pjoin(path, dir, fn);
    if (!(f = ufopen(path, "w"))) { free(used); snprintf(err, errn, "Couldn't write %s.", path); return -1; }
    fprintf(f, "# JSRF Mod Tool: %s's textures\nnewmtl none\nKd 0.7 0.7 0.7\n", s->name);
    for (k = 0; k < s->ntex; k++)
        if (used[k]) fprintf(f, "newmtl t%08X\nKa 1 1 1\nKd 1 1 1\nKs 0 0 0\nillum 1\nmap_Kd %s_tex/%08X.png\nmap_d %s_tex/%08X.png\n",
                             s->tex[k].id, s->name, s->tex[k].id, s->name, s->tex[k].id);
    fclose(f);
    snprintf(fn, sizeof fn, "%s_visual.obj", s->name);
    pjoin(path, dir, fn);
    if (!(f = ufopen(path, "w"))) { free(used); snprintf(err, errn, "Couldn't write %s.", path); return -1; }
    fprintf(f, "# JSRF Mod Tool: %s, the stage models and props as the game places them (%d vertices, %d triangles)\nmtllib %s_visual.mtl\n",
            s->name, s->nv, s->nt, s->name);
    for (i = 0; i < s->nv; i++) fprintf(f, "v %.4f %.4f %.4f\n", s->p[i * 3], s->p[i * 3 + 1], s->p[i * 3 + 2]);
    for (i = 0; i < s->nv; i++) fprintf(f, "vt %.6f %.6f\n", s->uv[i * 2], 1.0f - s->uv[i * 2 + 1]);
    for (k = -1; k < s->ntex; k++) {
        static const char *kn[4] = { "stage", "see_through", "props", "shadows" };
        int kind;
        if (k >= 0 && !used[k]) continue;
        for (kind = 0; kind < 4; kind++) {
            int any = 0;
            for (i = 0; i < s->nt; i++) {
                uint32_t a, b, c;
                if (s->ttex[i] != k || s->tkind[i] != kind) continue;
                if (!any) {
                    if (k >= 0) fprintf(f, "g %s_%08X\nusemtl t%08X\n", kn[kind], s->tex[k].id, s->tex[k].id);
                    else fprintf(f, "g %s_none\nusemtl none\n", kn[kind]);
                    any = 1;
                }
                a = s->ix[i * 3] + 1; b = s->ix[i * 3 + 1] + 1; c = s->ix[i * 3 + 2] + 1;
                fprintf(f, "f %u/%u %u/%u %u/%u\n", a, a, c, c, b, b);
            }
        }
    }
    fclose(f);
    free(used);
    /* the collision: a material per surface */
    snprintf(fn, sizeof fn, "%s_collision.mtl", s->name);
    pjoin(path, dir, fn);
    if ((f = ufopen(path, "w"))) {
        for (k = 0; k < JST_NSURF; k++)
            fprintf(f, "newmtl surf_%u\n# %s\nKd %.3f %.3f %.3f\nillum 1\n", jst_surf[k].flag, jst_surf[k].name,
                    jst_surf[k].r / 255.0, jst_surf[k].g / 255.0, jst_surf[k].b / 255.0);
        fclose(f);
    }
    snprintf(fn, sizeof fn, "%s_collision.obj", s->name);
    pjoin(path, dir, fn);
    if (!(f = ufopen(path, "w"))) { snprintf(err, errn, "Couldn't write %s.", path); return -1; }
    fprintf(f, "# JSRF Mod Tool: %s's collision (%d triangles); materials surf_<flags>: "
            "1 floor, 4 wall, 16 stairs, 32 billboard, 128 half-pipe, 256 ceiling, 512 untouchable, 8192 ramp\nmtllib %s_collision.mtl\n",
            s->name, s->nct, s->name);
    for (i = 0; i < s->ncv; i++) fprintf(f, "v %.4f %.4f %.4f\n", s->cp[i * 3], s->cp[i * 3 + 1], s->cp[i * 3 + 2]);
    for (k = 0; k < JST_NSURF; k++) {
        int any = 0;
        for (i = 0; i < s->nct; i++) {
            uint32_t a, b, c;
            if (jst_surf_index(s->csurf[i]) != k) continue;
            if (!any) {
                if (k < JST_NSURF - 1) fprintf(f, "g %s\nusemtl surf_%u\n", jst_surf[k].name, jst_surf[k].flag);
                else fprintf(f, "g Other\nusemtl surf_65535\n");
                any = 1;
            }
            a = s->cix[i * 3] + 1; b = s->cix[i * 3 + 1] + 1; c = s->cix[i * 3 + 2] + 1;
            fprintf(f, "f %u %u %u\n", a, c, b);
        }
    }
    fclose(f);
    /* the rails: a line each */
    snprintf(fn, sizeof fn, "%s_rails.obj", s->name);
    pjoin(path, dir, fn);
    if (!(f = ufopen(path, "w"))) { snprintf(err, errn, "Couldn't write %s.", path); return -1; }
    fprintf(f, "# JSRF Mod Tool: %s's grind rails (%d), a line each\n", s->name, s->nrail);
    for (i = 0; i < s->nrp; i++) fprintf(f, "v %.4f %.4f %.4f\n", s->rp[i * 3], s->rp[i * 3 + 1], s->rp[i * 3 + 2]);
    for (k = 0; k < s->nrail; k++) {
        fprintf(f, "o rail_%d\nl", k);
        for (i = 0; i < s->rn[k]; i++) fprintf(f, " %d", s->rs[k] + i + 1);
        fprintf(f, "\n");
    }
    fclose(f);
    return 0;
}
