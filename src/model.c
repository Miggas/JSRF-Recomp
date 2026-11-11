/* JSRF models: see model.h */
#include "model.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t u32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static float f32(const uint8_t *b) { float f; uint32_t v = u32(b); memcpy(&f, &v, 4); return f; }

typedef struct { size_t s, e; } Item;
typedef struct { Item *it; int n; } Norm;

static int is_texture(const uint8_t *b, size_t len)
{
    uint32_t r;
    if (len < 40) return 0;
    r = u32(b + 20);
    return (b[24] == 5 || b[24] == 6 || b[24] == 7) && r >= 4 && r <= 4096 && !(r & (r - 1));
}

/* a vertex's normal and texture coordinates, by its size (MDLB.cs's table) */
static void layout(int vs, int *nrm, int *uv)
{
    *nrm = -1;
    *uv = -1;
    switch (vs) {
    case 20: case 24: case 28: *uv = 12; break;
    case 32: case 36: *nrm = 12; *uv = 24; break;
    case 40: case 44: case 48: *nrm = 20; *uv = 32; break;
    case 56: *nrm = 20; *uv = 40; break;
    }
}

/* the visible parts of the MDLB at m (len bytes) into one mesh; 0 done */
static int parse_mdlb(const uint8_t *m, size_t len, JMesh *out)
{
    uint32_t parts, p;
    int any_visible = 0, pass;
    size_t nv = 0, nt = 0;
    memset(out, 0, sizeof *out);
    if (len < 16 || memcmp(m, "MDLB", 4)) return -1;
    parts = u32(m + 4);
    if (!parts || parts > 256 || 16 + (size_t)parts * 128 > len) return -1;
    for (p = 0; p < parts; p++) any_visible |= u32(m + 16 + p * 128 + 12) == 0;
    for (pass = 0; pass < 2; pass++) {          /* count, then fill */
        size_t vbase = 0;
        if (pass) {
            out->p = (float *)malloc(nv * 3 * sizeof(float) + 1);
            out->n = (float *)malloc(nv * 3 * sizeof(float) + 1);
            out->t = (float *)malloc(nv * 2 * sizeof(float) + 1);
            out->ix = (uint32_t *)malloc(nt * 3 * sizeof(uint32_t) + 1);
            if (!out->p || !out->n || !out->t || !out->ix) return -1;
            out->has_n = 1;
        }
        nv = nt = 0;
        for (p = 0; p < parts; p++) {
            const uint8_t *h = m + 16 + p * 128;
            size_t vb = (size_t)u32(h) + 16, vo, to, vc, vs, tc, i;
            int strip, no, uo;
            if (any_visible ? u32(h + 12) != 0 : p != parts - 1) continue;
            if (vb + 32 > len) continue;
            vo = (size_t)u32(m + vb) + 16;
            vc = u32(m + vb + 4);
            vs = u32(m + vb + 12);
            to = (size_t)u32(m + vb + 16) + 16;
            tc = u32(m + vb + 20);
            strip = u32(m + vb + 24) == 1;
            if (vs < 12 || vs > 128 || vc > 65536 || vo + vc * vs > len || to + tc * 2 > len) continue;
            layout((int)vs, &no, &uo);
            if (no < 0 && pass) out->has_n = 0;
            if (pass) {
                for (i = 0; i < vc; i++) {
                    const uint8_t *v = m + vo + i * vs;
                    float *pp = out->p + (vbase + i) * 3, *nn = out->n + (vbase + i) * 3, *tt = out->t + (vbase + i) * 2;
                    pp[0] = -f32(v); pp[1] = f32(v + 4); pp[2] = f32(v + 8);   /* x mirrored, as the game shows it */
                    if (no >= 0) { nn[0] = -f32(v + no); nn[1] = f32(v + no + 4); nn[2] = f32(v + no + 8); }
                    else nn[0] = nn[1] = nn[2] = 0;
                    if (uo >= 0) { tt[0] = f32(v + uo); tt[1] = f32(v + uo + 4); }
                    else tt[0] = tt[1] = 0;
                }
            }
            if (strip) {
                for (i = 0; i + 2 < tc; i++) {
                    uint32_t a = m[to + i * 2] | m[to + i * 2 + 1] << 8, b = m[to + i * 2 + 2] | m[to + i * 2 + 3] << 8,
                             c = m[to + i * 2 + 4] | m[to + i * 2 + 5] << 8;
                    if (a == b || b == c || a == c || a >= vc || b >= vc || c >= vc) continue;
                    if (pass) {
                        uint32_t *t = out->ix + nt * 3;
                        if (i & 1) { t[0] = (uint32_t)vbase + c; t[1] = (uint32_t)vbase + b; t[2] = (uint32_t)vbase + a; }
                        else { t[0] = (uint32_t)vbase + a; t[1] = (uint32_t)vbase + b; t[2] = (uint32_t)vbase + c; }
                    }
                    nt++;
                }
            } else {
                for (i = 0; i + 2 < tc; i += 3) {
                    uint32_t a = m[to + i * 2] | m[to + i * 2 + 1] << 8, b = m[to + i * 2 + 2] | m[to + i * 2 + 3] << 8,
                             c = m[to + i * 2 + 4] | m[to + i * 2 + 5] << 8;
                    if (a >= vc || b >= vc || c >= vc) continue;
                    if (pass) {
                        uint32_t *t = out->ix + nt * 3;
                        t[0] = (uint32_t)vbase + a; t[1] = (uint32_t)vbase + b; t[2] = (uint32_t)vbase + c;
                    }
                    nt++;
                }
            }
            nv += vc;
            vbase += vc;
        }
        if (!nv || !nt) return -1;
    }
    out->nv = (int)nv;
    out->nt = (int)nt;
    {   /* its centre and size, for the camera */
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, r = 0;
        int i, k;
        for (i = 0; i < out->nv; i++)
            for (k = 0; k < 3; k++) {
                if (out->p[i * 3 + k] < lo[k]) lo[k] = out->p[i * 3 + k];
                if (out->p[i * 3 + k] > hi[k]) hi[k] = out->p[i * 3 + k];
            }
        for (k = 0; k < 3; k++) out->c[k] = (lo[k] + hi[k]) / 2;
        for (i = 0; i < out->nv; i++) {
            float dx = out->p[i * 3] - out->c[0], dy = out->p[i * 3 + 1] - out->c[1], dz = out->p[i * 3 + 2] - out->c[2];
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > r) r = d;
        }
        out->r = r > 0 ? r : 1;
    }
    return 0;
}

static void norm_children(const uint8_t *b, size_t size, size_t start, size_t end, Norm *n)
{
    size_t off = start + 16;
    int cap = 0, guard = 0;
    n->it = NULL;
    n->n = 0;
    while (off + 8 <= end && off + 8 <= size && guard++ < 100000) {
        size_t s = u32(b + off), e = u32(b + off + 4);
        if (!e || start + e <= off) break;
        if (n->n == cap) { cap = cap ? cap * 2 : 16; n->it = (Item *)realloc(n->it, sizeof(Item) * (size_t)cap); }
        n->it[n->n].s = s && s <= e && start + e <= size ? start + s : 0;
        n->it[n->n].e = s && s <= e && start + e <= size ? start + e : 0;
        n->n++;
        off = start + e;
    }
}

int jm_load(const uint8_t *b, size_t size, JModels *out)
{
    Norm *norms = NULL;
    int nn = 0, cap = 0, i, j, k, mcap = 0;
    uint32_t *texids = NULL;
    int ntex = 0, tcap = 0, count = 0;
    memset(out, 0, sizeof *out);
    if (size < 32) return -1;
    if (!memcmp(b, "MULT", 4)) {
        size_t off = 16;
        int guard = 0;
        while (off + 8 <= size && guard++ < 100000) {
            size_t s = u32(b + off), e = u32(b + off + 4);
            if (e <= off || s >= size) break;
            if (nn == cap) { cap = cap ? cap * 2 : 16; norms = (Norm *)realloc(norms, sizeof(Norm) * (size_t)cap); }
            if (s + 4 <= size && !memcmp(b + s, "NORM", 4)) norm_children(b, size, s, e < size ? e : size, &norms[nn]);
            else norms[nn].it = NULL, norms[nn].n = 0;
            nn++;
            off = e;
        }
    } else if (!memcmp(b, "NORM", 4)) {
        norms = (Norm *)malloc(sizeof(Norm));
        norm_children(b, size, 0, size, &norms[0]);
        nn = 1;
    } else return -1;
    for (i = 0; i < nn; i++)                    /* the file's textures */
        for (j = 0; j < norms[i].n; j++) {
            const Item *t = &norms[i].it[j];
            if (t->e > t->s && is_texture(b + t->s, t->e - t->s)) {
                if (ntex == tcap) { tcap = tcap ? tcap * 2 : 16; texids = (uint32_t *)realloc(texids, sizeof(uint32_t) * (size_t)tcap); }
                texids[ntex++] = u32(b + t->s);
            }
        }
    for (i = 0; i < nn; i++)
        for (j = 0; j < norms[i].n; j++) {
            const Item *it = &norms[i].it[j];
            JMesh m;
            if (it->e <= it->s || it->e - it->s < 16 || memcmp(b + it->s, "MDLB", 4)) continue;
            count++;
            if (parse_mdlb(b + it->s, it->e - it->s, &m)) { free(m.p); free(m.n); free(m.t); free(m.ix); continue; }
            m.index = count;
            m.texid = 0;
            if (i > 0 && norms[i - 1].n == norms[i].n) {
                const Item *q = &norms[i - 1].it[j];
                size_t ql = q->e - q->s;
                if (q->e > q->s && is_texture(b + q->s, ql)) m.texid = u32(b + q->s);
                else if (q->e > q->s && ql >= 8 && u32(b + q->s) >= 1 && u32(b + q->s) <= 16 && 4 + 4 * (size_t)u32(b + q->s) <= ql) {
                    /* a material list: its first texture that is in this file */
                    uint32_t c = u32(b + q->s);
                    for (k = 0; k < (int)c && !m.texid; k++) {
                        uint32_t id = u32(b + q->s + 4 + 4 * (size_t)k);
                        int t;
                        for (t = 0; t < ntex; t++) if (texids[t] == id) m.texid = id;
                    }
                }
            }
            if (out->n == mcap) { mcap = mcap ? mcap * 2 : 16; out->m = (JMesh *)realloc(out->m, sizeof(JMesh) * (size_t)mcap); }
            out->m[out->n++] = m;
        }
    for (i = 0; i < nn; i++) free(norms[i].it);
    free(norms);
    free(texids);
    return 0;
}

void jm_free(JModels *ms)
{
    int i;
    for (i = 0; i < ms->n; i++) {
        free(ms->m[i].p);
        free(ms->m[i].n);
        free(ms->m[i].t);
        free(ms->m[i].ix);
    }
    free(ms->m);
    ms->m = NULL;
    ms->n = 0;
}
