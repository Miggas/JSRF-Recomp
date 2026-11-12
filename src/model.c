/* JSRF models: see model.h */
#include "model.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static uint32_t u32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static float f32(const uint8_t *b) { float f; uint32_t v = u32(b); memcpy(&f, &v, 4); return f; }

typedef struct { size_t s, e; } Item;
typedef struct { Item *it; int n; size_t s, e; } Norm;

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

/* m9: the material of the triangle starting at index i of part header h
 * (its triangle groups: +112 their offset - 16, +116 their count; a group:
 * +0 its triangles, +4 its first index, +16 its material) */
static int group_mat(const uint8_t *m, size_t len, const uint8_t *h, size_t i, int strip)
{
    size_t go = (size_t)u32(h + 112) + 16, gn = u32(h + 116), g;
    int best = -1;
    size_t best_start = 0;
    if (!u32(h + 112) || gn > 64 || go + gn * 32 > len) return 0;
    for (g = 0; g < gn; g++) {
        const uint8_t *e = m + go + g * 32;
        size_t st = u32(e + 4), cnt = u32(e);
        if (strip) {                        /* the last group starting at or before it */
            if (st <= i && (best < 0 || st >= best_start)) { best = (int)u32(e + 16); best_start = st; }
        } else if (i >= st && i < st + cnt * 3) return (int)u32(e + 16);
    }
    return best < 0 ? 0 : best;
}

/* m9: part p's vertices and indices are the MDLB's last bytes (nothing of
 * another part, no groups or materials after them): it can be rewritten */
static int data_is_last(const uint8_t *m, size_t len, uint32_t p, uint32_t parts)
{
    const uint8_t *h = m + 16 + p * 128;
    size_t vb = (size_t)u32(h) + 16, vo, to, vc, vs, q;
    if (vb + 32 > len) return 0;
    vo = (size_t)u32(m + vb) + 16; vc = u32(m + vb + 4); vs = u32(m + vb + 12); to = (size_t)u32(m + vb + 16) + 16;
    if (vo < vb + 32 || to < vo + vc * vs || to > len) return 0;
    for (q = 0; q < parts; q++) {
        const uint8_t *k = m + 16 + q * 128;
        if (q != p && ((size_t)u32(k) + 16 + 32 > vo)) return 0;
        if (u32(k + 4) && (size_t)u32(k + 4) + 16 > vo) return 0;
        if (u32(k + 112) && (size_t)u32(k + 112) + 16 > vo) return 0;
    }
    return 1;
}

/* the visible parts of the MDLB at m (len bytes) into one mesh; 0 done */
static int parse_mdlb(const uint8_t *m, size_t len, JMesh *out)
{
    uint32_t parts, p;
    int any_visible = 0, pass, vis_n = 0, vis_p = -1;
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
            out->tmat = (int *)malloc(nt * sizeof(int) + 1);
            if (!out->p || !out->n || !out->t || !out->ix || !out->tmat) return -1;
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
            if (pass) { vis_n++; vis_p = (int)p; }
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
                        out->tmat[nt] = group_mat(m, len, h, i, 1);
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
                        out->tmat[nt] = group_mat(m, len, h, i, 0);
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
    out->nparts = (int)parts;
    out->part = vis_n == 1 && data_is_last(m, len, (uint32_t)vis_p, parts) ? vis_p : -1;
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
    n->s = start;
    n->e = end;
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
            else norms[nn].it = NULL, norms[nn].n = 0, norms[nn].s = s, norms[nn].e = e;
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
            if (parse_mdlb(b + it->s, it->e - it->s, &m)) { free(m.p); free(m.n); free(m.t); free(m.ix); free(m.tmat); continue; }
            m.index = count;
            m.item = it->s;
            m.len = it->e - it->s;
            m.norm = norms[i].s;
            m.norm_end = norms[i].e;
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
        free(ms->m[i].tmat);
    }
    free(ms->m);
    ms->m = NULL;
    ms->n = 0;
}

/* the face winding that agrees with the vertex normals (mirroring x for the
 * view turned it round): 1 when a, b, c is right, 0 for a, c, b */
static int winding(const JMesh *m)
{
    int i, agree = 0, against = 0;
    if (!m->has_n) return 0;
    for (i = 0; i < m->nt; i++) {
        const uint32_t *t = m->ix + i * 3;
        const float *a = m->p + t[0] * 3, *b = m->p + t[1] * 3, *c = m->p + t[2] * 3;
        float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        float fn[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] }, d = 0;
        int k, j;
        for (k = 0; k < 3; k++)
            for (j = 0; j < 3; j++) d += fn[j] * m->n[t[k] * 3 + j];
        if (d > 0) agree++;
        else if (d < 0) against++;
    }
    return agree >= against;
}

int jm_write_obj(const JMesh *m, const char *obj_path, const char *mtl_path, const char *mtl_file,
                 const char *tex_png, const char *name)
{
    FILE *f;
    int i, fwd = winding(m), mats[64], nm = 0, k;
    for (i = 0; i < m->nt; i++) {               /* the materials used, in order */
        int t = m->tmat ? m->tmat[i] : 0;
        for (k = 0; k < nm && mats[k] != t; k++) ;
        if (k == nm && nm < 64) mats[nm++] = t;
    }
    for (i = 1; i < nm; i++)
        for (k = i; k > 0 && mats[k] < mats[k - 1]; k--) { int x = mats[k]; mats[k] = mats[k - 1]; mats[k - 1] = x; }
    /* m9: one material a game material (mat<N>), all wearing the texture:
     * the names bring each part back to its material on import */
    if ((f = fopen(mtl_path, "w"))) {
        fprintf(f, "# JSRF Mod Tool: mat<N> is the model's material N; keep the names to keep them on import\n");
        for (k = 0; k < nm; k++) {
            fprintf(f, "newmtl mat%d\nKa 1 1 1\nKd 1 1 1\nKs 0 0 0\nd 1\nillum 1\n", mats[k]);
            if (tex_png) fprintf(f, "map_Kd %s\nmap_d %s\n", tex_png, tex_png);
        }
        fclose(f);
    } else return -1;
    f = fopen(obj_path, "w");
    if (!f) return -1;
    fprintf(f, "# JSRF Mod Tool: %d vertices, %d triangles\nmtllib %s\no %s\n", m->nv, m->nt, mtl_file, name);
    for (i = 0; i < m->nv; i++) fprintf(f, "v %.6f %.6f %.6f\n", m->p[i * 3], m->p[i * 3 + 1], m->p[i * 3 + 2]);
    for (i = 0; i < m->nv; i++) fprintf(f, "vt %.6f %.6f\n", m->t[i * 2], 1.0f - m->t[i * 2 + 1]);
    if (m->has_n)
        for (i = 0; i < m->nv; i++) fprintf(f, "vn %.6f %.6f %.6f\n", m->n[i * 3], m->n[i * 3 + 1], m->n[i * 3 + 2]);
    fprintf(f, "s off\n");
    for (k = 0; k < nm; k++) {
        fprintf(f, "usemtl mat%d\n", mats[k]);
        for (i = 0; i < m->nt; i++) {
            uint32_t a = m->ix[i * 3] + 1, b = m->ix[i * 3 + 1] + 1, c = m->ix[i * 3 + 2] + 1;
            if ((m->tmat ? m->tmat[i] : 0) != mats[k]) continue;
            if (!fwd) { uint32_t x = b; b = c; c = x; }
            if (m->has_n) fprintf(f, "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a, a, a, b, b, b, c, c, c);
            else fprintf(f, "f %u/%u %u/%u %u/%u\n", a, a, b, b, c, c);
        }
    }
    return fclose(f) ? -1 : 0;
}

/* ---- m9: replacing a model's visible mesh from an .obj ---- */

#include "jcont.h"

static void put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static void putf(uint8_t *b, float f) { uint32_t v; memcpy(&v, &f, 4); put32(b, v); }

typedef struct { float *p, *t, *n; int np, nt, nn, cp, ct, cn; } ObjPools;
typedef struct { int v, t, n, mat; } Corner;

static int obj_index(const char *s, int count)   /* 1-based, or negative from the end; -1 none */
{
    int i = atoi(s);
    if (i > 0) return i - 1 < count ? i - 1 : -1;
    if (i < 0) return count + i >= 0 ? count + i : -1;
    return -1;
}

static void pool_add(float **a, int *n, int *cap, int k, const float *v)
{
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 1024; *a = (float *)realloc(*a, sizeof(float) * (size_t)k * (size_t)*cap); }
    memcpy(*a + (size_t)*n * k, v, sizeof(float) * (size_t)k);
    (*n)++;
}

int jm_import_obj(const uint8_t *buf, size_t size, const JMesh *m, const char *path,
                  uint8_t **nb, size_t *nsize, char *err, size_t errn)
{
    FILE *f;
    char line[4096], names[64][64];
    ObjPools P;
    Corner *cs = NULL;
    int ncs = 0, ccs = 0, cur_mat = -1, nnames = 0, i, k;
    const uint8_t *m0 = buf + m->item, *h, *ov;
    size_t vb, vo, vs, ovc, go, gn;
    int nrm_off, uv_off, gmat[64], ngmat = 0, nmat_total;
    *nb = NULL;
    memset(&P, 0, sizeof P);
    if (m->part < 0) {
        snprintf(err, errn, "Model %d can't be replaced yet: its visible mesh isn't a single part at the end of it.", m->index);
        return -1;
    }
    h = m0 + 16 + (size_t)m->part * 128;
    vb = (size_t)u32(h) + 16;
    vo = (size_t)u32(m0 + vb) + 16;
    ovc = u32(m0 + vb + 4);
    vs = u32(m0 + vb + 12);
    layout((int)vs, &nrm_off, &uv_off);
    if (uv_off < 0) { snprintf(err, errn, "Model %d's vertices (%u bytes) aren't a kind this tool can write.", m->index, (unsigned)vs); return -1; }
    ov = m0 + vo;
    go = u32(h + 112) ? (size_t)u32(h + 112) + 16 : 0;
    gn = go ? u32(h + 116) : 0;
    if (gn > 64) gn = 64;
    for (k = 0; k < (int)gn; k++) {             /* the materials its groups use, in order */
        int mt = (int)u32(m0 + go + (size_t)k * 32 + 16), j;
        for (j = 0; j < ngmat && gmat[j] != mt; j++) ;
        if (j == ngmat) gmat[ngmat++] = mt;
    }
    if (!ngmat) gmat[ngmat++] = 0;
    nmat_total = (int)u32(m0 + 8) ? (int)u32(m0 + 8) : (int)u32(h + 108);

    /* the .obj */
    f = fopen(path, "r");
    if (!f) { snprintf(err, errn, "Can't open %s", path); return -1; }
    while (fgets(line, sizeof line, f)) {
        float v[3] = { 0, 0, 0 };
        if (line[0] == 'v' && line[1] == ' ') { sscanf(line + 2, "%f %f %f", &v[0], &v[1], &v[2]); pool_add(&P.p, &P.np, &P.cp, 3, v); }
        else if (line[0] == 'v' && line[1] == 't') { sscanf(line + 3, "%f %f", &v[0], &v[1]); pool_add(&P.t, &P.nt, &P.ct, 2, v); }
        else if (line[0] == 'v' && line[1] == 'n') { sscanf(line + 3, "%f %f %f", &v[0], &v[1], &v[2]); pool_add(&P.n, &P.nn, &P.cn, 3, v); }
        else if (!strncmp(line, "usemtl ", 7)) {
            char nm[64] = "";
            sscanf(line + 7, "%63s", nm);
            if (!strncmp(nm, "mat", 3) && nm[3] >= '0' && nm[3] <= '9') cur_mat = atoi(nm + 3);   /* the game's material N */
            else {                              /* another name: the original's groups in turn */
                for (k = 0; k < nnames && strcmp(names[k], nm); k++) ;
                if (k == nnames && nnames < 64) snprintf(names[nnames++], 64, "%s", nm);
                cur_mat = gmat[k % ngmat];
            }
        } else if (line[0] == 'f' && line[1] == ' ') {   /* a polygon: a fan of triangles */
            Corner poly[64];
            int np = 0;
            char *tok = strtok(line + 2, " \t\r\n");
            while (tok && np < 64) {
                char *a = tok, *b = strchr(tok, '/'), *c = NULL;
                Corner q;
                if (b) { *b++ = 0; c = strchr(b, '/'); if (c) *c++ = 0; }
                q.v = obj_index(a, P.np);
                q.t = b && *b ? obj_index(b, P.nt) : -1;
                q.n = c && *c ? obj_index(c, P.nn) : -1;
                q.mat = cur_mat;
                if (q.v >= 0) poly[np++] = q;
                tok = strtok(NULL, " \t\r\n");
            }
            for (k = 1; k + 1 < np; k++) {
                if (ncs + 3 > ccs) { ccs = ccs ? ccs * 2 : 3072; cs = (Corner *)realloc(cs, sizeof(Corner) * (size_t)ccs); }
                cs[ncs++] = poly[0]; cs[ncs++] = poly[k]; cs[ncs++] = poly[k + 1];
            }
        }
    }
    fclose(f);
    if (!ncs) { snprintf(err, errn, "No triangles in %s", path); free(P.p); free(P.t); free(P.n); free(cs); return -1; }

    {
        /* the new vertices: one for each different corner (position,
         * texture coordinate, normal), in the game's space: x back, v down */
        int nv = 0, nt = ncs / 3, hsz = 1, *hash, *vidx = (int *)malloc(sizeof(int) * (size_t)ncs);
        Corner *uv = (Corner *)malloc(sizeof(Corner) * (size_t)ncs);
        float *pos, *nrm, *tex, lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, cen[3], rad = 0;
        int *tri = (int *)malloc(sizeof(int) * (size_t)nt), *order;
        size_t to, nlen, il;
        uint8_t *o;
        int used[64], nused = 0, agree = 0, against = 0, flip, r;
        while (hsz < ncs * 2) hsz <<= 1;
        hash = (int *)malloc(sizeof(int) * (size_t)hsz);
        for (i = 0; i < hsz; i++) hash[i] = -1;
        for (i = 0; i < ncs; i++) {
            unsigned hk = ((unsigned)cs[i].v * 73856093u) ^ ((unsigned)(cs[i].t + 1) * 19349663u) ^ ((unsigned)(cs[i].n + 1) * 83492791u);
            unsigned slot = hk & (unsigned)(hsz - 1);
            while (hash[slot] >= 0 && !(uv[hash[slot]].v == cs[i].v && uv[hash[slot]].t == cs[i].t && uv[hash[slot]].n == cs[i].n))
                slot = (slot + 1) & (unsigned)(hsz - 1);
            if (hash[slot] < 0) { hash[slot] = nv; uv[nv++] = cs[i]; }
            vidx[i] = hash[slot];
        }
        free(hash);
        if (nv > 65535) {
            snprintf(err, errn, "%d vertices: the game takes at most 65535 in one model.", nv);
            free(vidx); free(uv); free(tri); free(P.p); free(P.t); free(P.n); free(cs);
            return -1;
        }
        pos = (float *)calloc((size_t)nv * 3, sizeof(float));
        nrm = (float *)calloc((size_t)nv * 3, sizeof(float));
        tex = (float *)calloc((size_t)nv * 2, sizeof(float));
        for (i = 0; i < nv; i++) {
            const float *p = P.p + (size_t)uv[i].v * 3;
            pos[i * 3] = -p[0]; pos[i * 3 + 1] = p[1]; pos[i * 3 + 2] = p[2];
            if (uv[i].t >= 0) { tex[i * 2] = P.t[(size_t)uv[i].t * 2]; tex[i * 2 + 1] = 1.0f - P.t[(size_t)uv[i].t * 2 + 1]; }
            if (uv[i].n >= 0) { const float *n = P.n + (size_t)uv[i].n * 3; nrm[i * 3] = -n[0]; nrm[i * 3 + 1] = n[1]; nrm[i * 3 + 2] = n[2]; }
            for (k = 0; k < 3; k++) { if (pos[i * 3 + k] < lo[k]) lo[k] = pos[i * 3 + k]; if (pos[i * 3 + k] > hi[k]) hi[k] = pos[i * 3 + k]; }
        }
        if (!P.nn) {                            /* no normals: from the faces around each vertex */
            for (i = 0; i < nt; i++) {
                const float *a = pos + vidx[i * 3] * 3, *b = pos + vidx[i * 3 + 1] * 3, *c = pos + vidx[i * 3 + 2] * 3;
                float u1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, u2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
                float fn[3] = { u1[1] * u2[2] - u1[2] * u2[1], u1[2] * u2[0] - u1[0] * u2[2], u1[0] * u2[1] - u1[1] * u2[0] };
                for (k = 0; k < 3; k++) { int j; for (j = 0; j < 3; j++) nrm[vidx[i * 3 + k] * 3 + j] -= fn[j]; }   /* mirrored: the other side */
            }
        }
        for (i = 0; i < nv; i++) {
            float *n = nrm + i * 3, l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (l > 1e-12f) { n[0] /= l; n[1] /= l; n[2] /= l; } else n[1] = 1;
        }
        for (k = 0; k < 3; k++) cen[k] = (lo[k] + hi[k]) / 2;
        for (i = 0; i < nv; i++) {
            float dx = pos[i * 3] - cen[0], dy = pos[i * 3 + 1] - cen[1], dz = pos[i * 3 + 2] - cen[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > rad) rad = d;
        }
        /* the winding the original has against its normals, in the game's space */
        for (i = 0; i < nt; i++) {
            const float *a = pos + vidx[i * 3] * 3, *b = pos + vidx[i * 3 + 1] * 3, *c = pos + vidx[i * 3 + 2] * 3;
            float u1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, u2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            float fn[3] = { u1[1] * u2[2] - u1[2] * u2[1], u1[2] * u2[0] - u1[0] * u2[2], u1[0] * u2[1] - u1[1] * u2[0] }, d = 0;
            for (k = 0; k < 3; k++) { int j; for (j = 0; j < 3; j++) d += fn[j] * nrm[vidx[i * 3 + k] * 3 + j]; }
            if (d > 0) agree++; else if (d < 0) against++;
        }
        flip = m->has_n ? ((agree >= against) != !winding(m)) : 1;
        /* each triangle's material: the game's, else the first group's */
        for (i = 0; i < nt; i++) {
            int mt = cs[i * 3].mat;
            if (mt < 0 || mt >= nmat_total) mt = gmat[0];
            for (k = 0; k < nused && used[k] != mt; k++) ;
            if (k == nused) { if (nused < (int)(gn ? gn : 1)) used[nused++] = mt; else mt = used[0]; }
            tri[i] = mt;
        }
        for (i = 1; i < nused; i++)
            for (k = i; k > 0 && used[k] < used[k - 1]; k--) { int x = used[k]; used[k] = used[k - 1]; used[k - 1] = x; }
        order = (int *)malloc(sizeof(int) * (size_t)nt);
        { int q = 0, u; for (u = 0; u < nused; u++) for (i = 0; i < nt; i++) if (tri[i] == used[u]) order[q++] = i; }

        /* the MDLB: as it was up to the part's vertices, then the new ones
         * and their triangles (a list), padded */
        to = vo + (size_t)nv * vs;
        il = (size_t)nt * 6;
        nlen = ((to + il + 15) & ~(size_t)15) + 16;
        o = (uint8_t *)calloc(1, nlen);
        memcpy(o, m0, vo);
        for (i = 0; i < nv; i++) {
            uint8_t *d = o + vo + (size_t)i * vs;
            size_t best = 0, j;
            float bd = 1e30f;
            for (j = 0; j < ovc; j++) {         /* the nearest old vertex: its bone weights and the rest */
                float dx = f32(ov + j * vs) - pos[i * 3], dy = f32(ov + j * vs + 4) - pos[i * 3 + 1], dz = f32(ov + j * vs + 8) - pos[i * 3 + 2];
                float dd = dx * dx + dy * dy + dz * dz;
                if (dd < bd) { bd = dd; best = j; }
            }
            if (ovc) memcpy(d, ov + best * vs, vs);
            putf(d, pos[i * 3]); putf(d + 4, pos[i * 3 + 1]); putf(d + 8, pos[i * 3 + 2]);
            if (nrm_off >= 0) { putf(d + nrm_off, nrm[i * 3]); putf(d + nrm_off + 4, nrm[i * 3 + 1]); putf(d + nrm_off + 8, nrm[i * 3 + 2]); }
            putf(d + uv_off, tex[i * 2]); putf(d + uv_off + 4, tex[i * 2 + 1]);
        }
        for (i = 0; i < nt; i++) {
            int t = order[i], a = vidx[t * 3], b = vidx[t * 3 + 1], c = vidx[t * 3 + 2];
            uint8_t *d = o + to + (size_t)i * 6;
            if (flip) { int x = b; b = c; c = x; }
            d[0] = (uint8_t)a; d[1] = (uint8_t)(a >> 8); d[2] = (uint8_t)b; d[3] = (uint8_t)(b >> 8); d[4] = (uint8_t)c; d[5] = (uint8_t)(c >> 8);
        }
        put32(o + vb + 4, (uint32_t)nv);                /* the vertex block */
        put32(o + vb + 16, (uint32_t)(to - 16));
        put32(o + vb + 20, (uint32_t)(nt * 3));
        put32(o + vb + 24, 0);                          /* a list, not a strip */
        if (gn) {                                       /* a triangle group a material */
            int q = 0, u;
            uint8_t g0[32];
            memcpy(g0, m0 + go, 32);
            for (u = 0; u < nused; u++) {
                int cnt = 0;
                uint8_t *g = o + go + (size_t)u * 32;
                for (i = 0; i < nt; i++) cnt += tri[order[i]] == used[u];
                memcpy(g, g0, 32);
                put32(g, (uint32_t)cnt);
                put32(g + 4, (uint32_t)(q * 3));
                put32(g + 16, (uint32_t)used[u]);
                q += cnt;
            }
            put32(o + 16 + (size_t)m->part * 128 + 116, (uint32_t)nused);
        }
        {   /* its bounding sphere, so the game doesn't hide it too early */
            uint8_t *ph = o + 16 + (size_t)m->part * 128;
            putf(ph + 16, cen[0]); putf(ph + 20, cen[1]); putf(ph + 24, cen[2]); putf(ph + 28, rad);
        }
        r = jc_replace(buf, size, m->item, m->len, m->norm, m->norm_end, o, nlen, nb, nsize);
        if (r) snprintf(err, errn, "Couldn't rebuild the file.");
        else snprintf(err, errn, "%d vertices, %d triangles, %d material%s", nv, nt, nused, nused == 1 ? "" : "s");
        free(o); free(order); free(tri); free(pos); free(nrm); free(tex); free(vidx); free(uv);
        free(P.p); free(P.t); free(P.n); free(cs);
        return r;
    }
}

int jm_materials(const uint8_t *buf, size_t size, const JMesh *m, size_t *at, int *count)
{
    const uint8_t *m0 = buf + m->item, *h;
    int parts, vis;
    size_t o;
    *count = 0;
    if (m->item + m->len > size || m->len < 16) return -1;
    parts = (int)u32(m0 + 4);
    if (parts < 1) return -1;
    vis = m->part >= 0 ? m->part : parts - 1;
    if (u32(m0 + 8)) {                          /* the model's list, after the last part's groups */
        h = m0 + 16 + (size_t)(parts - 1) * 128;
        o = (size_t)u32(h + 4) + 16;
        *count = (int)u32(m0 + 8);
    } else {                                    /* none: the visible part's, after its vertex block's header */
        h = m0 + 16 + (size_t)vis * 128;
        o = (size_t)u32(h) + 16 + 32;
        *count = (int)u32(h + 108);
    }
    if (*count < 1 || *count > 64 || o + (size_t)*count * 20 > m->len) { *count = 0; return -1; }
    *at = m->item + o;
    return 0;
}
