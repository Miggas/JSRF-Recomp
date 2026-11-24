/* m27: a stage's files read into their parts and written back: see jsbuild.h */
#include "jsbuild.h"
#include "util.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t u32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static void p32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }

static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return !*a && !*b;
}

static int fail(char *err, size_t errn, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errn, fmt, ap);
    va_end(ap);
    return -1;
}

static int dup_bytes(JBytes *o, const uint8_t *p, size_t n)
{
    o->n = n;
    o->p = (uint8_t *)malloc(n ? n : 1);
    if (!o->p) return -1;
    if (n) memcpy(o->p, p, n);
    return 0;
}

/* ---- a growing output ---- */

typedef struct { uint8_t *p; size_t n, cap; int bad; } Out;

static size_t o_room(Out *o, size_t k)          /* k zero bytes added; returns where they start */
{
    size_t at = o->n;
    if (o->n + k > o->cap) {
        size_t nc = o->cap ? o->cap : 65536;
        uint8_t *np;
        while (nc < o->n + k) nc *= 2;
        np = (uint8_t *)realloc(o->p, nc);
        if (!np) { o->bad = 1; return at; }
        o->p = np;
        o->cap = nc;
    }
    memset(o->p + o->n, 0, k);
    o->n += k;
    return at;
}
static size_t o_put(Out *o, const void *d, size_t k)
{
    size_t at = o_room(o, k);
    if (!o->bad && k) memcpy(o->p + at, d, k);
    return at;
}
static void o_set(Out *o, size_t at, uint32_t v) { if (!o->bad) p32(o->p + at, v); }

/* ---- block 0: collision ---- */

typedef struct { uint32_t off, cnt; } Key;
static int key_find(const Key *k, int n, uint32_t off, uint32_t cnt)
{
    int i;
    for (i = 0; i < n; i++) if (k[i].off == off && k[i].cnt == cnt) return i;
    return -1;
}

static int parse0(const uint8_t *B, size_t n, JSBin *s, char *err, size_t errn)
{
    uint32_t toff, tcnt, i, mbase, run = 0;
    Key *vk = NULL, *tk = NULL;
    int nv = 0, nt = 0, cap = 0, ret = -1;
    if (n < 8) return fail(err, errn, "collision block too small");
    toff = u32(B); tcnt = u32(B + 4);
    if (toff < 8 || toff > 4096 || tcnt > 100000 || (size_t)toff + (size_t)tcnt * 32 > n)
        return fail(err, errn, "collision areas out of place");
    if (dup_bytes(&s->chead, B, toff)) return fail(err, errn, "out of memory");
    mbase = toff + tcnt * 32;
    s->nca = (int)tcnt;
    s->ca = (JCArea *)calloc(tcnt ? tcnt : 1, sizeof(JCArea));
    if (!s->ca) return fail(err, errn, "out of memory");
    for (i = 0; i < tcnt; i++) {
        const uint8_t *e = B + toff + (size_t)i * 32;
        uint32_t mo = u32(e + 24), mc = u32(e + 28);
        memcpy(s->ca[i].box, e, 24);
        if (mc && mo != mbase + run * 112)
            return fail(err, errn, "collision area %u's models aren't where expected", i);
        if ((size_t)mbase + (size_t)(run + mc) * 112 > n) return fail(err, errn, "collision models past the block");
        s->ca[i].first = (int)run;
        s->ca[i].count = (int)mc;
        run += mc;
    }
    s->ncm = (int)run;
    s->cm = (JCModel *)calloc(run ? run : 1, sizeof(JCModel));
    if (!s->cm) return fail(err, errn, "out of memory");
    for (i = 0; i < run; i++) {
        const uint8_t *m = B + mbase + (size_t)i * 112;
        uint32_t vo = u32(m + 88), vc = u32(m + 92), to = u32(m + 96), tc = u32(m + 100);
        int k;
        if ((size_t)vo + (size_t)vc * 16 > n || (size_t)to + (size_t)tc * 8 > n)
            { ret = fail(err, errn, "collision model %u's data past the block", i); goto out; }
        memcpy(s->cm[i].head, m, 88);
        memcpy(s->cm[i].tail, m + 104, 8);
        if (nv == cap || nt == cap) {
            cap = cap ? cap * 2 : 256;
            vk = (Key *)realloc(vk, sizeof(Key) * (size_t)cap);
            tk = (Key *)realloc(tk, sizeof(Key) * (size_t)cap);
            s->vs = (JBytes *)realloc(s->vs, sizeof(JBytes) * (size_t)cap);
            s->ts = (JBytes *)realloc(s->ts, sizeof(JBytes) * (size_t)cap);
            if (!vk || !tk || !s->vs || !s->ts) { ret = fail(err, errn, "out of memory"); goto out; }
        }
        if ((k = key_find(vk, nv, vo, vc)) < 0) {
            k = nv++;
            vk[k].off = vo; vk[k].cnt = vc;
            if (dup_bytes(&s->vs[k], B + vo, (size_t)vc * 16)) { ret = fail(err, errn, "out of memory"); goto out; }
            s->nvs = nv;
        }
        s->cm[i].vset = k;
        if ((k = key_find(tk, nt, to, tc)) < 0) {
            k = nt++;
            tk[k].off = to; tk[k].cnt = tc;
            if (dup_bytes(&s->ts[k], B + to, (size_t)tc * 8)) { ret = fail(err, errn, "out of memory"); goto out; }
            s->nts = nt;
        }
        s->cm[i].tset = k;
    }
    ret = 0;
out:
    free(vk);
    free(tk);
    return ret;
}

static void write0(const JSBin *s, int moved, Out *o)
{
    size_t toff, mbase, *vo, *to;
    int i, k;
    toff = o_put(o, s->chead.p, s->chead.n);
    toff = o->n;
    o_set(o, 0, (uint32_t)toff);
    o_set(o, 4, (uint32_t)s->nca);
    o_room(o, (size_t)s->nca * 32);
    mbase = o_room(o, (size_t)s->ncm * 112);
    vo = (size_t *)calloc((size_t)s->nvs + 1, sizeof(size_t));
    to = (size_t *)calloc((size_t)s->nts + 1, sizeof(size_t));
    if (!vo || !to) { o->bad = 1; free(vo); free(to); return; }
    for (k = 0; k < s->nvs; k++) {
        int q = moved ? s->nvs - 1 - k : k;
        vo[q] = o_put(o, s->vs[q].p, s->vs[q].n);
    }
    for (k = 0; k < s->nts; k++) {
        int q = moved ? s->nts - 1 - k : k;
        to[q] = o_put(o, s->ts[q].p, s->ts[q].n);
    }
    if (!o->bad) {
        uint32_t run = 0;
        for (i = 0; i < s->nca; i++) {
            size_t e = toff + (size_t)i * 32;
            memcpy(o->p + e, s->ca[i].box, 24);
            if (s->ca[i].count) run = (uint32_t)s->ca[i].first;
            o_set(o, e + 24, (uint32_t)(mbase + (size_t)run * 112));   /* an empty one: where the next run starts */
            o_set(o, e + 28, (uint32_t)s->ca[i].count);
            run += (uint32_t)s->ca[i].count;
        }
        for (i = 0; i < s->ncm; i++) {
            size_t m = mbase + (size_t)i * 112;
            const JCModel *c = &s->cm[i];
            memcpy(o->p + m, c->head, 88);
            o_set(o, m + 88, (uint32_t)vo[c->vset]);
            o_set(o, m + 92, (uint32_t)(s->vs[c->vset].n / 16));
            o_set(o, m + 96, (uint32_t)to[c->tset]);
            o_set(o, m + 100, (uint32_t)(s->ts[c->tset].n / 8));
            memcpy(o->p + m + 104, c->tail, 8);
        }
    }
    free(vo);
    free(to);
}

/* ---- block 1: grind rails ---- */

static int parse1(const uint8_t *B, size_t n, JSBin *s, char *err, size_t errn)
{
    uint32_t rbase = 0xFFFFFFFFu, rmax = 0, so, c;
    uint32_t lofs[1024];
    int i, k;
    if (n < 4 + 1024 * 8) return fail(err, errn, "rails block too small");
    s->rid = u32(B);
    for (i = 0; i < 1024; i++) {
        so = u32(B + 4 + 8 * i); c = u32(B + 8 + 8 * i);
        s->slot[i] = -1;
        if (!c) continue;
        if (c > 100000 || (size_t)so + (size_t)c * 4 > n) return fail(err, errn, "rail slot %d past the block", i);
        for (k = 0; k < (int)c; k++) {
            uint32_t r = u32(B + so + 4 * k);
            if (r < rbase) rbase = r;
            if (r > rmax) rmax = r;
        }
    }
    if (rbase != 0xFFFFFFFFu) {
        if ((rmax - rbase) % 36 || (size_t)rmax + 36 > n) return fail(err, errn, "rails not 36 bytes apart");
        s->nrail = (int)((rmax - rbase) / 36 + 1);
        s->rail = (JRail *)calloc((size_t)s->nrail, sizeof(JRail));
        if (!s->rail) return fail(err, errn, "out of memory");
        for (k = 0; k < s->nrail; k++) {
            const uint8_t *r = B + rbase + (size_t)k * 36;
            uint32_t po = u32(r), pc = u32(r + 4);
            if (pc > 100000 || (size_t)po + (size_t)pc * 24 > n) return fail(err, errn, "rail %d's points past the block", k);
            memcpy(s->rail[k].rest, r + 8, 28);
            if (dup_bytes(&s->rail[k].pts, B + po, (size_t)pc * 24)) return fail(err, errn, "out of memory");
        }
    }
    s->list = (int **)calloc(1024, sizeof(int *));
    s->nlist = (int *)calloc(1024, sizeof(int));
    if (!s->list || !s->nlist) return fail(err, errn, "out of memory");
    for (i = 0; i < 1024; i++) {
        so = u32(B + 4 + 8 * i); c = u32(B + 8 + 8 * i);
        if (!c) continue;
        for (k = 0; k < s->nlists; k++)
            if (lofs[k] == so && s->nlist[k] == (int)c) break;
        if (k == s->nlists) {
            int q;
            lofs[k] = so;
            s->nlist[k] = (int)c;
            s->list[k] = (int *)malloc(sizeof(int) * c);
            if (!s->list[k]) return fail(err, errn, "out of memory");
            for (q = 0; q < (int)c; q++) {
                uint32_t r = u32(B + so + 4 * q);
                if ((r - rbase) % 36) return fail(err, errn, "a rail list names no rail");
                s->list[k][q] = (int)((r - rbase) / 36);
            }
            s->nlists++;
        }
        s->slot[i] = k;
    }
    return 0;
}

static void write1(const JSBin *s, int moved, Out *o)
{
    size_t lists, rbase, *lo;
    int i, k;
    o_set(o, o_room(o, 4), s->rid);
    o_room(o, 1024 * 8);
    lists = o->n;
    lo = (size_t *)calloc((size_t)s->nlists + 1, sizeof(size_t));
    if (!lo) { o->bad = 1; return; }
    for (k = 0; k < s->nlists; k++) lo[k] = o_room(o, (size_t)s->nlist[k] * 4);
    rbase = o_room(o, (size_t)s->nrail * 36);
    for (k = 0; k < s->nrail; k++) {
        int q = moved ? s->nrail - 1 - k : k;
        size_t at = o_put(o, s->rail[q].pts.p, s->rail[q].pts.n);
        o_set(o, rbase + (size_t)q * 36, (uint32_t)at);
        o_set(o, rbase + (size_t)q * 36 + 4, (uint32_t)(s->rail[q].pts.n / 24));
        if (!o->bad) memcpy(o->p + rbase + (size_t)q * 36 + 8, s->rail[q].rest, 28);
    }
    for (i = 0; i < 1024; i++) {
        k = s->slot[i];
        o_set(o, 4 + 8 * (size_t)i, (uint32_t)(k >= 0 ? lo[k] : lists));
        o_set(o, 8 + 8 * (size_t)i, (uint32_t)(k >= 0 ? s->nlist[k] : 0));
    }
    for (k = 0; k < s->nlists; k++)
        for (i = 0; i < s->nlist[k]; i++)
            o_set(o, lo[k] + 4 * (size_t)i, (uint32_t)(rbase + (size_t)s->list[k][i] * 36));
    free(lo);
}

/* ---- block 2: areas and placements ---- */

static const int f_extra[4] = { 240, 248, 256, 264 };

static int parse2(const uint8_t *B, size_t n, JSBin *s, char *err, size_t errn)
{
    uint32_t ao, na, i;
    int f;
    if (n < 12) return fail(err, errn, "areas block too small");
    s->dw0 = u32(B); ao = u32(B + 4); na = u32(B + 8);
    if (na > 4096 || 12 + (size_t)na * 8 > n || (size_t)ao + (size_t)na * 340 > n)
        return fail(err, errn, "areas out of place");
    s->nva = (int)na;
    s->pl = (JBytes *)calloc(na ? na : 1, sizeof(JBytes));
    s->va = (JVArea *)calloc(na ? na : 1, sizeof(JVArea));
    if (!s->pl || !s->va) return fail(err, errn, "out of memory");
    for (i = 0; i < na; i++) {
        uint32_t o = u32(B + 12 + 8 * i), c = u32(B + 16 + 8 * i);
        if (c > 100000 || (size_t)o + (size_t)c * 80 > n) return fail(err, errn, "area %u's placements past the block", i);
        if (dup_bytes(&s->pl[i], B + o, (size_t)c * 80)) return fail(err, errn, "out of memory");
    }
    for (i = 0; i < na; i++) {
        const uint8_t *a = B + ao + (size_t)i * 340;
        uint32_t po = u32(a + 144), pc = u32(a + 148), vo = u32(a + 152), vc = u32(a + 156);
        for (f = 0; f < 4; f++)
            if (u32(a + f_extra[f] + 4)) return fail(err, errn, "area %u has a list at +%d (never seen before)", i, f_extra[f]);
        if (pc > 100000 || (size_t)po + (size_t)pc * 144 > n || vc > 100000 || (size_t)vo + (size_t)vc * 4 > n)
            return fail(err, errn, "area %u's lists past the block", i);
        memcpy(s->va[i].rec, a, 340);
        if (dup_bytes(&s->va[i].port, B + po, (size_t)pc * 144) || dup_bytes(&s->va[i].vis, B + vo, (size_t)vc * 4))
            return fail(err, errn, "out of memory");
    }
    return 0;
}

static void write2(const JSBin *s, int moved, Out *o)
{
    size_t ao;
    int i, f;
    o_room(o, 12 + (size_t)s->nva * 8);
    o_set(o, 0, s->dw0);
    o_set(o, 8, (uint32_t)s->nva);
    for (i = 0; i < s->nva; i++) {
        size_t at = o_put(o, s->pl[i].p, s->pl[i].n);
        o_set(o, 12 + 8 * (size_t)i, (uint32_t)at);
        o_set(o, 16 + 8 * (size_t)i, (uint32_t)(s->pl[i].n / 80));
    }
    ao = o->n;
    o_set(o, 4, (uint32_t)ao);
    for (i = 0; i < s->nva; i++) o_put(o, s->va[i].rec, 340);
    for (i = 0; i < s->nva; i++) {
        int q = moved ? s->nva - 1 - i : i;
        size_t at = o_put(o, s->va[q].port.p, s->va[q].port.n);
        o_set(o, ao + (size_t)q * 340 + 144, (uint32_t)at);
        o_set(o, ao + (size_t)q * 340 + 148, (uint32_t)(s->va[q].port.n / 144));
    }
    for (i = 0; i < s->nva; i++) {
        int q = moved ? s->nva - 1 - i : i;
        size_t at = o_put(o, s->va[q].vis.p, s->va[q].vis.n);
        o_set(o, ao + (size_t)q * 340 + 152, (uint32_t)at);
        o_set(o, ao + (size_t)q * 340 + 156, (uint32_t)(s->va[q].vis.n / 4));
    }
    for (i = 0; i < s->nva; i++)
        for (f = 0; f < 4; f++) {
            o_set(o, ao + (size_t)i * 340 + (size_t)f_extra[f], (uint32_t)o->n);
            o_set(o, ao + (size_t)i * 340 + (size_t)f_extra[f] + 4, 0);
        }
}

/* ---- the whole .bin ---- */

static size_t pad16(size_t x) { return (x / 16 + 1) * 16; }   /* as the game's files: the next multiple of 16 past it */

int jsb_parse(const uint8_t *b, size_t n, JSBin *s, char *err, size_t errn)
{
    uint32_t o[3], z[3];
    int k;
    memset(s, 0, sizeof *s);
    if (n < 0xA0) return fail(err, errn, "the .bin is too small");
    memcpy(s->head, b, 0xA0);
    for (k = 0; k < 3; k++) {
        o[k] = u32(b + 8 * k);
        z[k] = u32(b + 8 * k + 4);
        if ((size_t)o[k] + z[k] > n) return fail(err, errn, "block %d past the file", k);
    }
    if (o[0] != 0xA0 || o[1] < o[0] + z[0] || o[2] < o[1] + z[1])
        return fail(err, errn, "blocks not laid out as expected");
    for (k = 0; k < 3; k++) {                   /* the filler after each block, kept when short */
        size_t end = o[k] + z[k], next = k < 2 ? o[k + 1] : n;
        s->padn[k] = next - end <= sizeof s->pad[k] ? (int)(next - end) : -1;
        if (s->padn[k] > 0) memcpy(s->pad[k], b + end, next - end);
    }
    if (parse0(b + o[0], z[0], s, err, errn) || parse1(b + o[1], z[1], s, err, errn) || parse2(b + o[2], z[2], s, err, errn)) {
        jsb_free(s);
        return -1;
    }
    return 0;
}

uint8_t *jsb_write(const JSBin *s, int moved, size_t *n)
{
    Out o, blk[3];
    int k;
    memset(&o, 0, sizeof o);
    memset(blk, 0, sizeof blk);
    write0(s, moved, &blk[0]);
    write1(s, moved, &blk[1]);
    write2(s, moved, &blk[2]);
    o_put(&o, s->head, 0xA0);
    for (k = 0; k < 3; k++) {
        size_t at = o.n, end, padded;
        o_put(&o, blk[k].p, blk[k].n);
        end = o.n;
        padded = pad16(end) + (moved ? 16 : 0);
        o_room(&o, padded - end);
        if (!o.bad && (int)(padded - end) == s->padn[k]) memcpy(o.p + end, s->pad[k], padded - end);   /* the same filler */
        o_set(&o, 8 * (size_t)k, (uint32_t)at);
        o_set(&o, 8 * (size_t)k + 4, (uint32_t)blk[k].n);
        if (blk[k].bad) o.bad = 1;
        free(blk[k].p);
    }
    if (o.bad) { free(o.p); return NULL; }
    *n = o.n;
    return o.p;
}

void jsb_free(JSBin *s)
{
    int i;
    free(s->chead.p);
    free(s->ca);
    free(s->cm);
    for (i = 0; i < s->nvs; i++) free(s->vs[i].p);
    for (i = 0; i < s->nts; i++) free(s->ts[i].p);
    free(s->vs);
    free(s->ts);
    if (s->list) for (i = 0; i < s->nlists; i++) free(s->list[i]);
    free(s->list);
    free(s->nlist);
    for (i = 0; i < s->nrail; i++) free(s->rail[i].pts.p);
    free(s->rail);
    for (i = 0; i < s->nva; i++) {
        if (s->pl) free(s->pl[i].p);
        if (s->va) { free(s->va[i].port.p); free(s->va[i].vis.p); }
    }
    free(s->pl);
    free(s->va);
    memset(s, 0, sizeof *s);
}

/* ---- the .dat ---- */

int jsd_parse(const uint8_t *b, size_t n, JSDat *d, char *err, size_t errn)
{
    size_t off = 0;
    int cap = 0;
    memset(d, 0, sizeof *d);
    while (off + 8 <= n) {
        uint32_t t = u32(b + off);
        size_t end;
        if (t == 1) {
            if (off + 20 > n) break;
            end = off + 20 + (size_t)u32(b + off + 16) * 4 + u32(b + off + 8);
        } else if (t == 0) end = off + 8 + u32(b + off + 4);
        else end = off + 4 + (size_t)t;
        if (end <= off || end > n) break;
        if (d->nblk == cap) {
            cap = cap ? cap * 2 : 64;
            d->blk = (JBytes *)realloc(d->blk, sizeof(JBytes) * (size_t)cap);
            if (!d->blk) return fail(err, errn, "out of memory");
        }
        if (dup_bytes(&d->blk[d->nblk], b + off, end - off)) return fail(err, errn, "out of memory");
        d->nblk++;
        off = end;
    }
    if (n - off > 16) { jsd_free(d); return fail(err, errn, "%zu bytes after the last block", n - off); }
    if (dup_bytes(&d->tail, b + off, n - off)) return fail(err, errn, "out of memory");
    return 0;
}

uint8_t *jsd_write(const JSDat *d, size_t *n)
{
    Out o;
    int i;
    memset(&o, 0, sizeof o);
    for (i = 0; i < d->nblk; i++) o_put(&o, d->blk[i].p, d->blk[i].n);
    o_put(&o, d->tail.p, d->tail.n);
    if (o.bad) { free(o.p); return NULL; }
    *n = o.n;
    return o.p;
}

void jsd_free(JSDat *d)
{
    int i;
    for (i = 0; i < d->nblk; i++) free(d->blk[i].p);
    free(d->blk);
    free(d->tail.p);
    memset(d, 0, sizeof *d);
}

/* ---- a stage ---- */

typedef struct { const char *prefix; char (*names)[260]; int n, cap; } Find;
static void find_cb(const char *nm, bool isdir, void *u)
{
    Find *f = (Find *)u;
    size_t l = strlen(f->prefix), i;
    if (isdir || strlen(nm) < l) return;
    for (i = 0; i < l; i++) if (tolower((unsigned char)nm[i]) != tolower((unsigned char)f->prefix[i])) return;
    if (f->n < f->cap) snprintf(f->names[f->n++], 260, "%s", nm);
}

int jsb_rebuild(const char *dir, const char *name, int moved,
                int (*out)(const char *file, const uint8_t *data, size_t n, int same, void *u), void *u,
                char *err, size_t errn)
{
    char (*names)[260] = (char (*)[260])calloc(128, 260);
    char prefix[16], path[2048];
    Find fd;
    int i, nbin = 0, ret = 0;
    if (!names) return fail(err, errn, "out of memory");
    snprintf(prefix, sizeof prefix, "%s_", name);
    fd.prefix = prefix; fd.names = names; fd.n = 0; fd.cap = 128;
    list_dir(dir, find_cb, &fd);
    for (i = 0; i < fd.n && !ret; i++) {
        const char *nm = names[i];
        size_t l = strlen(nm), n = 0, wn = 0;
        int isbin = l == strlen(prefix) + 4 && ci_eq(nm + l - 4, ".bin");
        int isdat = l == strlen(prefix) + 6 && ci_eq(nm + l - 4, ".dat")
                    && isdigit((unsigned char)nm[l - 6]) && isdigit((unsigned char)nm[l - 5]);
        uint8_t *b, *w = NULL;
        if (!isbin && !isdat) continue;          /* the sky (_t) is another file's business */
        pjoin(path, dir, nm);
        b = (uint8_t *)read_file(path, &n);
        if (!b) { ret = fail(err, errn, "couldn't read %s", nm); break; }
        if (isbin) {
            JSBin s;
            char e2[300];
            if (jsb_parse(b, n, &s, e2, sizeof e2)) ret = fail(err, errn, "%s: %s", nm, e2);
            else { w = jsb_write(&s, moved, &wn); jsb_free(&s); nbin++; }
        } else {
            JSDat d;
            char e2[300];
            if (jsd_parse(b, n, &d, e2, sizeof e2)) ret = fail(err, errn, "%s: %s", nm, e2);
            else { w = jsd_write(&d, &wn); jsd_free(&d); }
        }
        if (!ret && !w) ret = fail(err, errn, "%s: out of memory", nm);
        if (!ret && out(nm, w, wn, wn == n && !memcmp(w, b, n), u)) ret = fail(err, errn, "couldn't write %s", nm);
        free(w);
        free(b);
    }
    free(names);
    if (!ret && !nbin) ret = fail(err, errn, "no %s_.bin in %s", name, dir);
    return ret;
}
